"""协议层：JSON Lines 解析、命令组包、snapshot 合并。

固件侧协议见 docs/esp32/protocol.md。这里只做三件事：
  1. 把一串字节切成完整的 JSON 行（串口分片必然存在）
  2. 把 hello/net/telemetry/scan 合并进同一份 snapshot
  3. 给命令编号，便于 ack 配对
"""

from __future__ import annotations

import json
import threading
import time
from typing import Any, Dict, List, Optional

PROTO_VERSION = 1
SERIES_MAX = 120          # 保留最近 N 个遥测点给前端画趋势

# 上行行类型
T_HELLO = "hello"
T_TELEMETRY = "telemetry"
T_NET = "net"
T_SCAN = "scan"
T_ACK = "ack"
T_ERR = "err"
T_LOG = "log"


class LineBuffer:
    """把字节流切成行。串口一次 read() 可能只拿到半行，必须自己缓冲。"""

    def __init__(self, limit: int = 8192):
        self._buf = bytearray()
        self._limit = limit

    def feed(self, data: bytes) -> List[str]:
        self._buf.extend(data)
        if len(self._buf) > self._limit:      # 防垃圾数据把内存吃满
            del self._buf[: len(self._buf) - self._limit]
        out = []
        while True:
            idx = self._buf.find(b"\n")
            if idx < 0:
                break
            raw = bytes(self._buf[:idx])
            del self._buf[: idx + 1]
            line = raw.rstrip(b"\r").decode("utf-8", "replace").strip()
            if line:
                out.append(line)
        return out


def parse_line(line: str) -> Optional[Dict[str, Any]]:
    """能解析成 JSON 对象就返回，否则 None（不是协议行）。"""
    s = line.strip()
    if not s.startswith("{"):
        return None
    try:
        obj = json.loads(s)
    except json.JSONDecodeError:
        return None
    return obj if isinstance(obj, dict) else None


class Snapshot:
    """设备状态的唯一真相；线程安全（串口线程写、HTTP 线程读）。"""

    def __init__(self) -> None:
        self._lock = threading.RLock()
        self.source = "none"
        self.connected = False
        self.port = ""
        self.host = ""
        self.device: Dict[str, Any] = {}
        self.net: Dict[str, Any] = {}
        self.runtime: Dict[str, Any] = {}
        self.io: Dict[str, Any] = {"gpio": {}, "adc": {}}
        self.scan: Dict[str, Any] = {}
        self.series: List[Dict[str, Any]] = []
        self.logs: List[Dict[str, Any]] = []
        self.raw: List[Dict[str, Any]] = []
        self.last_seen: Optional[float] = None
        self.stats = {"lines": 0, "errors": 0, "reconnects": 0, "acks": 0}
        self.proto_mismatch = False

    # ---------- 写入 ----------
    def set_source(self, source: str) -> None:
        with self._lock:
            self.source = source
            self.connected = False
            self.device = {}
            self.net = {}
            self.runtime = {}
            self.io = {"gpio": {}, "adc": {}}
            self.scan = {}
            self.series = []
            self.logs = []
            self.last_seen = None

    def mark(self, connected: bool, note: str = "") -> None:
        with self._lock:
            if self.connected != connected:
                self.connected = connected
                if not connected:
                    self.stats["reconnects"] += 1
            if note:
                self.push_log("W" if not connected else "I", note)

    def push_log(self, level: str, msg: str) -> None:
        with self._lock:
            self.logs.append({"t": time.time(), "lvl": level, "msg": msg})
            if len(self.logs) > 300:
                del self.logs[:-300]

    def push_raw(self, obj: Dict[str, Any] | str) -> None:
        with self._lock:
            self.raw.append({"t": time.time(), "data": obj})
            if len(self.raw) > 300:
                del self.raw[:-300]

    def ingest(self, obj: Dict[str, Any]) -> None:
        """把一条协议行合并进 snapshot。"""
        with self._lock:
            self.stats["lines"] += 1
            self.last_seen = time.time()
            self.connected = True
            t = obj.get("t")
            if obj.get("v") not in (None, PROTO_VERSION):
                self.proto_mismatch = True

            if t == T_HELLO:
                self.device = {k: v for k, v in obj.items() if k not in ("v", "t", "ts")}
            elif t == T_NET:
                self.net = {k: v for k, v in obj.items() if k not in ("v", "t", "ts")}
            elif t == T_TELEMETRY:
                rt = {k: v for k, v in obj.items() if k not in ("v", "t", "ts", "gpio", "adc")}
                self.runtime = rt
                self.io = {"gpio": obj.get("gpio", {}), "adc": obj.get("adc", {})}
                self.series.append(
                    {
                        "t": time.time(),
                        "heap": rt.get("heap"),
                        "temp": rt.get("temp"),
                        "rssi": rt.get("rssi"),
                        "frag": rt.get("frag"),
                    }
                )
                if len(self.series) > SERIES_MAX:
                    del self.series[:-SERIES_MAX]
            elif t == T_SCAN:
                # 保留 ts：前端用它判断"是不是新的一次扫描"
                self.scan = {k: v for k, v in obj.items() if k not in ("v", "t")}
            elif t == T_LOG:
                self.logs.append(
                    {"t": time.time(), "lvl": obj.get("lvl", "I"), "msg": obj.get("msg", "")}
                )
                if len(self.logs) > 300:
                    del self.logs[:-300]
            elif t == T_ERR:
                self.stats["errors"] += 1
            elif t == T_ACK:
                self.stats["acks"] += 1

            self.raw.append({"t": time.time(), "data": obj})
            if len(self.raw) > 300:
                del self.raw[:-300]

    # ---------- 读取 ----------
    def read(self) -> Dict[str, Any]:
        with self._lock:
            age = None if self.last_seen is None else round(time.time() - self.last_seen, 2)
            return {
                "source": self.source,
                "connected": self.connected,
                "port": self.port,
                "host": self.host,
                "device": dict(self.device),
                "net": dict(self.net),
                "runtime": dict(self.runtime),
                "io": {"gpio": dict(self.io.get("gpio", {})), "adc": dict(self.io.get("adc", {}))},
                "scan": dict(self.scan),
                "series": list(self.series),
                "logs": list(self.logs[-60:]),
                "raw": list(self.raw[-80:]),
                "last_seen_age": age,
                "stats": dict(self.stats),
                "proto_mismatch": self.proto_mismatch,
                "ts": time.time(),
            }


def build_command(cmd_id: int, cmd: str) -> bytes:
    """组下行命令：'<id> <cmd>\\n'。id 让设备 ack 能配对。"""
    return f"{cmd_id} {cmd.strip()}\n".encode("utf-8")

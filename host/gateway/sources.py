"""三种数据源：串口 / WiFi(HTTP 轮询) / 内置模拟器。

三者对上层暴露同一套接口，网关和前端不需要知道数据从哪来：
    src.start() / src.stop() / src.send(cmd, cmd_id) -> bool / src.info()
"""

from __future__ import annotations

import json
import math
import queue
import random
import threading
import time
import urllib.error
import urllib.parse
import urllib.request
from typing import Any, Dict, Optional

from protocol import LineBuffer, Snapshot, build_command, parse_line

try:
    import serial
    from serial.tools import list_ports
    HAVE_SERIAL = True
except ImportError:                                    # 没装 pyserial 也能跑 mock/wifi
    HAVE_SERIAL = False


# ---------------------------------------------------------------- 端口枚举
def list_serial_ports() -> list[dict[str, Any]]:
    if not HAVE_SERIAL:
        return []
    out = []
    for p in list_ports.comports():
        out.append(
            {
                "device": p.device,
                "description": p.description or "",
                "hwid": p.hwid or "",
                "likely_esp32": any(
                    k in (p.description or "").upper() + (p.hwid or "").upper()
                    for k in ("CH340", "CH341", "CP210", "USB-SERIAL", "USB JTAG", "ESP32", "SILICON")
                ),
                "likely_stm32": any(
                    k in (p.description or "").upper() for k in ("STM32", "STLINK", "ST-LINK")
                ),
            }
        )
    out.sort(key=lambda x: (not x["likely_esp32"], x["device"]))
    return out


class Source:
    name = "none"

    def __init__(self, snap: Snapshot):
        self.snap = snap
        self.acks: Dict[str, "queue.Queue[Dict[str, Any]]"] = {}
        self._lock = threading.Lock()

    # ---- 生命周期 ----
    def start(self) -> None:
        raise NotImplementedError

    def stop(self) -> None:
        raise NotImplementedError

    def send(self, cmd: str, cmd_id: int) -> bool:
        raise NotImplementedError

    def info(self) -> Dict[str, Any]:
        return {"source": self.name}

    # ---- ack 配对 ----
    def wait_ack(self, cmd_id: int, timeout: float = 4.0) -> Optional[Dict[str, Any]]:
        q = None
        with self._lock:
            q = self.acks.get(str(cmd_id))
        if q is None:
            return None
        try:
            return q.get(timeout=timeout)
        except queue.Empty:
            return None
        finally:
            with self._lock:
                self.acks.pop(str(cmd_id), None)

    def _ingest(self, obj: Dict[str, Any]) -> None:
        t = obj.get("t")
        if t in ("ack", "err"):
            cid = str(obj.get("id") or "")
            if cid:
                with self._lock:
                    q = self.acks.get(cid)
                    if q is None:
                        q = queue.Queue()
                        self.acks[cid] = q
                q.put(obj)
        self.snap.ingest(obj)


# ---------------------------------------------------------------- 串口
class SerialSource(Source):
    name = "serial"

    def __init__(self, snap: Snapshot, port: str = "", baud: int = 115200):
        super().__init__(snap)
        self.port = port
        self.baud = baud
        self._ser: Optional[Any] = None
        self._stop = threading.Event()
        self._thread: Optional[threading.Thread] = None
        self._buf = LineBuffer()
        self._reconnect_at = 0.0

    def start(self) -> None:
        if not HAVE_SERIAL:
            raise RuntimeError("pyserial 未安装，无法使用串口数据源")
        if not self.port:
            ports = list_serial_ports()
            if not ports:
                raise RuntimeError("未发现任何串口，请先插上 ESP32（CH340/CP210x）")
            self.port = ports[0]["device"]
        self.snap.set_source(self.name)
        self.snap.port = self.port
        self._stop.clear()
        self._thread = threading.Thread(target=self._run, name="serial-reader", daemon=True)
        self._thread.start()

    def stop(self) -> None:
        self._stop.set()
        if self._thread:
            self._thread.join(timeout=2)
        self._close()
        self.snap.mark(False, "串口已断开")

    def _close(self) -> None:
        try:
            if self._ser and self._ser.is_open:
                self._ser.close()
        except Exception:
            pass
        self._ser = None

    def _open(self) -> bool:
        try:
            self._ser = serial.Serial(self.port, self.baud, timeout=0.2)
            self._buf = LineBuffer()
            self.snap.mark(True, f"串口已连接 {self.port} @ {self.baud}")
            # 先问一次身份，避免上位机干等
            self.send("info", 1)
            return True
        except Exception as e:                                    # noqa: BLE001
            self.snap.mark(False, f"打开 {self.port} 失败：{e}")
            return False

    def _run(self) -> None:
        while not self._stop.is_set():
            if self._ser is None or not self._ser.is_open:
                if time.time() < self._reconnect_at:
                    time.sleep(0.3)
                    continue
                if not self._open():
                    self._reconnect_at = time.time() + 3.0   # 3 s 重试，别刷日志
                    continue
            try:
                data = self._ser.read(512)
                if data:
                    for line in self._buf.feed(data):
                        obj = parse_line(line)
                        if obj is None:
                            self.snap.push_raw(line)
                            self.snap.push_log("W", f"非协议行：{line[:80]}")
                            continue
                        self._ingest(obj)
            except Exception as e:                                # noqa: BLE001
                self.snap.mark(False, f"串口读取异常：{e}")
                self._close()
                self._reconnect_at = time.time() + 2.0

    def send(self, cmd: str, cmd_id: int) -> bool:
        with self._lock:
            if cmd_id:
                self.acks[str(cmd_id)] = queue.Queue()
        try:
            if self._ser and self._ser.is_open:
                self._ser.write(build_command(cmd_id, cmd))
                self._ser.flush()
                return True
        except Exception as e:                                    # noqa: BLE001
            self.snap.mark(False, f"串口写入失败：{e}")
        return False

    def info(self) -> Dict[str, Any]:
        return {"source": self.name, "port": self.port, "baud": self.baud}


# ---------------------------------------------------------------- WiFi(HTTP)
class WifiSource(Source):
    """ESP32 开 HTTP 时用：轮询 /api/snapshot，命令走 /api/cmd。"""

    name = "wifi"

    def __init__(self, snap: Snapshot, host: str = "", interval: float = 1.0):
        super().__init__(snap)
        self.host = host or "192.168.1.50"
        self.interval = interval
        self._stop = threading.Event()
        self._thread: Optional[threading.Thread] = None
        self._fails = 0

    def _url(self, path: str) -> str:
        h = self.host.strip()
        if not h.startswith("http"):
            h = "http://" + h
        return h.rstrip("/") + path

    def _get(self, path: str, timeout: float = 3.0) -> Optional[Any]:
        try:
            with urllib.request.urlopen(self._url(path), timeout=timeout) as r:
                return json.loads(r.read().decode("utf-8", "replace"))
        except (urllib.error.URLError, TimeoutError, json.JSONDecodeError, OSError):
            return None

    def start(self) -> None:
        self.snap.set_source(self.name)
        self.snap.host = self.host
        self._stop.clear()
        self._thread = threading.Thread(target=self._run, name="wifi-poller", daemon=True)
        self._thread.start()
        self.snap.push_log("I", f"开始轮询 {self._url('/api/snapshot')}")

    def stop(self) -> None:
        self._stop.set()
        if self._thread:
            self._thread.join(timeout=2)
        self.snap.mark(False, "WiFi 数据源已停止")

    def _run(self) -> None:
        while not self._stop.is_set():
            data = self._get("/api/snapshot")
            if data is None:
                self._fails += 1
                if self._fails in (1, 5, 20):
                    self.snap.mark(False, f"无法访问 {self.host}（连续 {self._fails} 次）")
            else:
                self._fails = 0
                self.snap.mark(True)
                dev = data.get("device") or {}
                net = data.get("net") or {}
                rt = data.get("runtime") or {}
                io = data.get("io") or {}
                if dev:
                    self._ingest({"v": 1, "t": "hello", **dev})
                if net:
                    self._ingest({"v": 1, "t": "net", **net})
                self._ingest(
                    {
                        "v": 1,
                        "t": "telemetry",
                        **rt,
                        "gpio": io.get("gpio", {}),
                        "adc": io.get("adc", {}),
                    }
                )
            time.sleep(self.interval)

    def send(self, cmd: str, cmd_id: int) -> bool:
        path = f"/api/cmd?cmd={urllib.parse.quote(cmd)}&id={cmd_id}"
        data = self._get(path, timeout=5.0)
        if not data:
            return False
        for line in data.get("lines", []):
            self._ingest(line)
        return True

    def info(self) -> Dict[str, Any]:
        return {"source": self.name, "host": self.host, "interval": self.interval}


# ---------------------------------------------------------------- 模拟器
class MockSource(Source):
    """没有硬件时的替身：协议、命令、字段与真机一致。

    存在的意义：Web/App 的每一处渲染都能在插板子之前验完，
    插上真机只换数据源，界面代码零改动。
    """

    name = "mock"

    def __init__(self, snap: Snapshot, interval: float = 1.0):
        super().__init__(snap)
        self.interval = interval
        self._stop = threading.Event()
        self._thread: Optional[threading.Thread] = None
        self._t0 = time.time()
        self._gpio = {p: 0 for p in (2, 4, 5, 12, 13, 14, 15, 16, 17, 18, 19, 21, 22, 23, 25, 26, 27, 32, 33)}
        self._gpio.update({p: 0 for p in (34, 35, 36, 39)})
        self._adc_pins = [34, 35]
        self._heap_min = 236_000
        self._net = {
            "mode": "sta",
            "state": "connected",
            "ssid": "Lab-2.4G",
            "ip": "192.168.1.66",
            "gw": "192.168.1.1",
            "mask": "255.255.255.0",
            "dns": "192.168.1.1",
            "mac": "24:0A:C4:00:BE:EF",
            "bssid": "8C:53:C3:11:22:33",
            "ch": 6,
            "rssi": -55,
            "host": "esp32-info-node",
            "clients": 0,
        }
        self._aps = [
            ("Lab-2.4G", "8C:53:C3:11:22:33", -55, 6, "WPA2_PSK"),
            ("Lab-5G", "8C:53:C3:11:22:34", -62, 149, "WPA2_PSK"),
            ("CMCC-8fA2", "44:FE:3B:8F:A2:01", -71, 11, "WPA2_PSK"),
            ("TP-LINK_2.4G", "AC:84:C6:00:9B:11", -74, 1, "WPA2_PSK"),
            ("HUAWEI-9C31", "78:D7:52:9C:31:AA", -81, 3, "WPA2_PSK"),
            ("ChinaNet-7Tqk", "F4:83:CD:7T:AA:12", -88, 9, "WPA2_PSK"),
        ]
        self._interval_req = 1000

    def start(self) -> None:
        self.snap.set_source(self.name)
        self._stop.clear()
        self._thread = threading.Thread(target=self._run, name="mock-device", daemon=True)
        self._thread.start()
        self.snap.push_log("I", "模拟数据源已启动（无硬件模式）")

    def stop(self) -> None:
        self._stop.set()
        if self._thread:
            self._thread.join(timeout=2)
        self.snap.mark(False, "模拟数据源已停止")

    # ---- 造数据 ----
    def _uptime(self) -> int:
        return int(time.time() - self._t0)

    def _hello(self) -> Dict[str, Any]:
        return {
            "v": 1, "t": "hello", "ts": self._uptime() * 1000,
            "fw": "0.1.0", "build": "Oct 18 2026 18:40:00 (mock)",
            "model": "ESP32-D0WD-V3", "rev": 3, "cores": 2, "freq": 240,
            "flash_kb": 4096, "psram_kb": 0, "sdk": "v3.3.12",
            "mac": "24:0A:C4:00:BE:EF", "chip_id": "240ac400beef",
            "reset": "poweron", "features": ["wifi", "bt", "ble"],
        }

    def _telemetry(self) -> Dict[str, Any]:
        t = time.time() - self._t0
        heap = int(236_000 + 9_000 * math.sin(t / 37.0) + random.randint(-1500, 1500))
        self._heap_min = min(self._heap_min, heap)
        temp = round(44.5 + 2.6 * math.sin(t / 90.0) + random.uniform(-0.3, 0.3), 2)
        rssi = int(-55 + 6 * math.sin(t / 23.0) + random.randint(-3, 3))
        self._net["rssi"] = rssi
        self._gpio[2] = 1 if int(t) % 2 == 0 else 0       # LED 心跳
        adc = {}
        for p in self._adc_pins:
            raw = int(2048 + 300 * math.sin(t / 11.0 + p) + random.randint(-30, 30))
            raw = max(0, min(4095, raw))
            adc[str(p)] = {"raw": raw, "mv": int(raw * 3100 / 4095)}
        return {
            "v": 1, "t": "telemetry", "ts": self._uptime() * 1000,
            "up": self._uptime(), "heap": heap, "heap_min": self._heap_min,
            "heap_max": int(heap * 0.92), "frag": random.randint(3, 9),
            "cpu": 240, "tasks": 9, "temp": temp, "rssi": rssi,
            "gpio": dict(self._gpio), "adc": adc,
        }

    def _scan(self) -> Dict[str, Any]:
        aps = []
        for ssid, bssid, rssi, ch, enc in self._aps:
            if random.random() < 0.08:                       # 偶尔扫不到，贴近真实
                continue
            aps.append({
                "ssid": ssid, "bssid": bssid,
                "rssi": rssi + random.randint(-4, 4),
                "ch": ch, "enc": enc,
            })
        aps.sort(key=lambda a: a["rssi"], reverse=True)
        return {"v": 1, "t": "scan", "ts": self._uptime() * 1000,
                "n": len(aps), "dur_ms": random.randint(1500, 2300), "aps": aps}

    def _run(self) -> None:
        self._ingest(self._hello())
        self._ingest({"v": 1, "t": "net", "ts": self._uptime() * 1000, **self._net})
        while not self._stop.is_set():
            self._ingest(self._telemetry())
            self.snap.mark(True)
            time.sleep(self._interval_req / 1000.0)

    # ---- 命令（与固件同语义）----
    def send(self, cmd: str, cmd_id: int) -> bool:
        threading.Thread(target=self._exec, args=(cmd, cmd_id), daemon=True).start()
        return True

    def _ack(self, cmd_id: int, cmd: str, data: Optional[dict] = None) -> None:
        obj = {"v": 1, "t": "ack", "ts": self._uptime() * 1000, "id": str(cmd_id), "cmd": cmd, "ok": True}
        if data is not None:
            obj["data"] = data
        self._ingest(obj)

    def _err(self, cmd_id: int, cmd: str, code: str, msg: str) -> None:
        self._ingest({"v": 1, "t": "err", "ts": self._uptime() * 1000, "id": str(cmd_id),
                      "cmd": cmd, "code": code, "msg": msg})

    def _exec(self, raw: str, cmd_id: int) -> None:
        parts = raw.strip().split()
        if not parts:
            return
        verb, args = parts[0], parts[1:]
        if verb == "info":
            self._ingest(self._hello())
            self._ingest({"v": 1, "t": "net", "ts": self._uptime() * 1000, **self._net})
            self._ack(cmd_id, verb, {"refreshed": ["hello", "net"]})
        elif verb == "telemetry":
            self._ingest(self._telemetry())
        elif verb == "net":
            self._ingest({"v": 1, "t": "net", "ts": self._uptime() * 1000, **self._net})
        elif verb == "scan":
            s = self._scan()
            self._ingest(s)
            self._ack(cmd_id, verb, {"n": s["n"], "dur_ms": s["dur_ms"]})
        elif verb == "temp":
            self._ack(cmd_id, verb, {"temp": self._telemetry()["temp"]})
        elif verb == "gpio.get":
            if not args or args[0] == "all":
                self._ack(cmd_id, verb, {"gpio": dict(self._gpio)})
            else:
                p = int(args[0])
                self._ack(cmd_id, verb, {"pin": p, "value": self._gpio.get(p, 0)})
        elif verb == "gpio.set":
            p, v = int(args[0]), 1 if args[1] in ("1", "high", "on") else 0
            self._gpio[p] = v
            self._ack(cmd_id, verb, {"pin": p, "value": v, "mode": "out"})
        elif verb == "gpio.mode":
            self._ack(cmd_id, verb, {"pin": int(args[0]), "mode": args[1]})
        elif verb == "adc.get":
            p = int(args[0])
            raw = random.randint(0, 4095)
            if p not in self._adc_pins:
                self._adc_pins.append(p)
            self._ack(cmd_id, verb, {"pin": p, "raw": raw, "mv": int(raw * 3100 / 4095)})
        elif verb == "interval":
            if args:
                self._interval_req = max(100, min(60000, int(args[0])))
            self._ack(cmd_id, verb, {"interval_ms": self._interval_req})
        elif verb in ("wifi.sta", "wifi.ap", "wifi.off"):
            if verb == "wifi.sta" and len(args) >= 1:
                self._net.update({"mode": "sta", "state": "connected", "ssid": args[0],
                                  "ip": "192.168.1.66"})
                self._ack(cmd_id, verb, {"ssid": args[0], "ip": "192.168.1.66", "saved": True})
            elif verb == "wifi.ap":
                ssid = args[0] if args else "esp32-info-beef"
                self._net.update({"mode": "ap", "state": "ap_listening", "ssid": ssid,
                                  "ip": "192.168.4.1", "rssi": 0})
                self._ack(cmd_id, verb, {"ssid": ssid, "ip": "192.168.4.1", "open": False})
            else:
                self._net.update({"mode": "off", "state": "off", "ssid": "", "ip": "0.0.0.0", "rssi": 0})
                self._ack(cmd_id, verb, {"mode": "off"})
            self._ingest({"v": 1, "t": "net", "ts": self._uptime() * 1000, **self._net})
        elif verb == "ping":
            self._ack(cmd_id, verb, {"pong": self._uptime() * 1000})
        elif verb == "echo":
            self._ack(cmd_id, verb, {"text": " ".join(args)})
        elif verb == "reboot":
            self._ack(cmd_id, verb, {"restarting": True})
            time.sleep(0.4)
            self._t0 = time.time()
            self._ingest(self._hello())
            self._ingest({"v": 1, "t": "net", "ts": 0, **self._net})
        elif verb == "help":
            self._ack(cmd_id, verb, {"fw": "0.1.0 (mock)", "cmds": [
                "help", "info", "telemetry", "net", "scan", "temp", "gpio.get",
                "gpio.set", "gpio.mode", "adc.get", "interval", "wifi.sta",
                "wifi.ap", "wifi.off", "ping", "echo", "reboot"]})
        else:
            self._err(cmd_id, verb, "unknown_cmd", f"unknown command '{verb}' (mock)")

    def info(self) -> Dict[str, Any]:
        return {"source": self.name, "interval": self._interval_req}

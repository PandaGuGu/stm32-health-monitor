#!/usr/bin/env python3
"""ESP32 信息节点 · 上位机网关

一个进程同时提供：
  · 静态 Web 面板      GET  /                （host/web 下的单页应用）
  · REST 快照          GET  /api/snapshot
  · 命令下发           POST /api/cmd         {"cmd": "gpio.set 2 1"}
  · 数据源切换         POST /api/connect     {"source": "serial|wifi|mock", ...}
  · 实时推送           GET  /api/events      （SSE，1 Hz 推整份 snapshot）

只用标准库 + pyserial：国内网络装不了 FastAPI 也能跑。

    python server.py                 # 自动：有 ESP32 串口就串口，否则模拟器
    python server.py --source mock
    python server.py --source serial --port COM3
    python server.py --source wifi --wifi 192.168.1.66
"""

from __future__ import annotations

import argparse
import json
import mimetypes
import os
import sys
import threading
import time
import webbrowser
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from typing import Any, Dict, Optional, Tuple
from urllib.parse import parse_qs, urlparse

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from protocol import Snapshot                      # noqa: E402
from sources import (                              # noqa: E402
    MockSource,
    SerialSource,
    Source,
    WifiSource,
    list_serial_ports,
)

WEB_DIR = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "web"))
VERSION = "0.1.0"


class App:
    """持有当前数据源与快照，所有 HTTP 线程共享。"""

    def __init__(self) -> None:
        self.snap = Snapshot()
        self.lock = threading.RLock()
        self.src: Optional[Source] = None
        self.cmd_seq = 100

    def connect(self, source: str, **kw: Any) -> Dict[str, Any]:
        with self.lock:
            self.disconnect(silent=True)
            src: Source
            if source == "serial":
                src = SerialSource(self.snap, port=kw.get("port", ""), baud=int(kw.get("baud") or 115200))
            elif source == "wifi":
                src = WifiSource(self.snap, host=kw.get("host", ""), interval=float(kw.get("interval") or 1.0))
            elif source == "mock":
                src = MockSource(self.snap, interval=float(kw.get("interval") or 1.0))
            else:
                raise ValueError(f"未知数据源：{source}")
            src.start()
            self.src = src
            return {"ok": True, **src.info()}

    def disconnect(self, silent: bool = False) -> Dict[str, Any]:
        with self.lock:
            if self.src is not None:
                try:
                    self.src.stop()
                except Exception:                                  # noqa: BLE001
                    pass
                self.src = None
            if not silent:
                self.snap.set_source("none")
                self.snap.push_log("I", "已断开数据源")
            return {"ok": True}

    def command(self, cmd: str, timeout: float = 5.0) -> Dict[str, Any]:
        with self.lock:
            src = self.src
            if src is None:
                return {"ok": False, "error": "尚未连接数据源"}
            self.cmd_seq += 1
            cid = self.cmd_seq
        ok = src.send(cmd, cid)
        if not ok:
            return {"ok": False, "error": "命令发送失败", "id": cid}
        ack = src.wait_ack(cid, timeout=timeout)
        return {"ok": True, "id": cid, "cmd": cmd, "ack": ack}

    def status(self) -> Dict[str, Any]:
        seg = self.snap.read()
        seg["gateway"] = {
            "version": VERSION,
            "web_dir": WEB_DIR,
            "source": self.src.name if self.src else "none",
            "source_info": self.src.info() if self.src else {},
        }
        return seg


APP = App()


class Handler(BaseHTTPRequestHandler):
    server_version = f"esp32-info-gateway/{VERSION}"
    protocol_version = "HTTP/1.1"

    # ---------- 通用 ----------
    def log_message(self, fmt: str, *args: Any) -> None:      # 静音：只留自己的日志
        return

    def _cors(self) -> None:
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Access-Control-Allow-Headers", "Content-Type")
        self.send_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS")

    def _json(self, obj: Any, code: int = 200) -> None:
        body = json.dumps(obj, ensure_ascii=False).encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self._cors()
        self.end_headers()
        self.wfile.write(body)

    def _body(self) -> Dict[str, Any]:
        try:
            n = int(self.headers.get("Content-Length") or 0)
            if n <= 0:
                return {}
            raw = self.rfile.read(n)
            obj = json.loads(raw.decode("utf-8"))
            return obj if isinstance(obj, dict) else {}
        except Exception:                                       # noqa: BLE001
            return {}

    def do_OPTIONS(self) -> None:                               # noqa: N802
        self.send_response(204)
        self._cors()
        self.send_header("Content-Length", "0")
        self.end_headers()

    # ---------- GET ----------
    def do_GET(self) -> None:                                   # noqa: N802
        u = urlparse(self.path)
        path = u.path
        qs = parse_qs(u.query)

        if path == "/api/status" or path == "/api/snapshot":
            self._json(APP.status())
        elif path == "/api/ports":
            self._json({"ports": list_serial_ports()})
        elif path == "/api/log":
            n = int((qs.get("n") or ["200"])[0])
            snap = APP.status()
            self._json({"logs": snap["logs"][-n:], "raw": snap["raw"][-n:]})
        elif path == "/api/health":
            self._json({"ok": True, "version": VERSION})
        elif path == "/api/events":
            self._sse()
        elif path == "/api/cmd":
            cmd = (qs.get("cmd") or [""])[0]
            self._json(APP.command(cmd))
        else:
            self._static(path)

    # ---------- POST ----------
    def do_POST(self) -> None:                                  # noqa: N802
        u = urlparse(self.path)
        body = self._body()
        try:
            if u.path == "/api/connect":
                source = str(body.get("source") or "mock")
                kw = {k: body.get(k) for k in ("port", "baud", "host", "interval")}
                self._json(APP.connect(source, **{k: v for k, v in kw.items() if v is not None}))
            elif u.path == "/api/disconnect":
                self._json(APP.disconnect())
            elif u.path == "/api/cmd":
                cmd = str(body.get("cmd") or "")
                if not cmd.strip():
                    self._json({"ok": False, "error": "缺少 cmd"}, 400)
                    return
                timeout = float(body.get("timeout") or 5.0)
                self._json(APP.command(cmd, timeout=timeout))
            else:
                self._json({"ok": False, "error": "not found"}, 404)
        except Exception as e:                                  # noqa: BLE001
            self._json({"ok": False, "error": str(e)}, 500)

    # ---------- SSE ----------
    def _sse(self) -> None:
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream; charset=utf-8")
        self.send_header("Cache-Control", "no-cache")
        self.send_header("Connection", "keep-alive")
        self.send_header("X-Accel-Buffering", "no")
        self._cors()
        self.end_headers()
        self.wfile.write(b"retry: 2000\n\n")
        self.wfile.flush()
        last_sig = None
        try:
            while True:
                snap = APP.status()
                # 只在内容变化时推，省流量；但至少 2 s 一次心跳
                sig = (
                    snap["source"],
                    snap["connected"],
                    len(snap["series"]),
                    len(snap["raw"]),
                    snap["stats"]["lines"],
                )
                if sig != last_sig:
                    last_sig = sig
                    payload = json.dumps(snap, ensure_ascii=False)
                    self.wfile.write(f"event: snapshot\ndata: {payload}\n\n".encode("utf-8"))
                else:
                    self.wfile.write(b": keepalive\n\n")
                self.wfile.flush()
                time.sleep(1.0)
        except (BrokenPipeError, ConnectionResetError, OSError):
            self.close_connection = True
            return

    # ---------- 静态文件 ----------
    def _static(self, path: str) -> None:
        if path in ("/", ""):
            path = "/index.html"
        rel = os.path.normpath(path.lstrip("/")).replace("\\", "/")
        if rel.startswith("..") or os.path.isabs(rel):
            self._json({"ok": False, "error": "bad path"}, 400)
            return
        full = os.path.join(WEB_DIR, rel)
        if not os.path.isfile(full):
            self._json({"ok": False, "error": f"not found: {path}"}, 404)
            return
        ctype, _ = mimetypes.guess_type(full)
        ctype = ctype or "application/octet-stream"
        if ctype.startswith("text/") or ctype in ("application/javascript", "application/json"):
            ctype += "; charset=utf-8"
        with open(full, "rb") as f:
            data = f.read()
        self.send_response(200)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Cache-Control", "no-cache")
        self._cors()
        self.end_headers()
        self.wfile.write(data)


def auto_source() -> Tuple[str, Dict[str, Any]]:
    ports = list_serial_ports()
    for p in ports:
        if p["likely_esp32"]:
            return "serial", {"port": p["device"]}
    return "mock", {}


def main() -> int:
    ap = argparse.ArgumentParser(description="ESP32 信息节点上位机网关")
    ap.add_argument("--host", default="0.0.0.0", help="监听地址，默认 0.0.0.0（手机/模拟器可访问）")
    ap.add_argument("--port", "--http-port", dest="port", type=int, default=8080,
                    help="HTTP 监听端口，默认 8080（想指定串口请用 --serial COM3，别写成 --port）")
    ap.add_argument("--source", default="auto", choices=["auto", "serial", "wifi", "mock"])
    ap.add_argument("--serial", dest="serial_port", default="", help="串口号，如 COM3")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--wifi", dest="wifi_host", default="", help="ESP32 的 IP 或 host:port")
    ap.add_argument("--no-open", action="store_true", help="不自动打开浏览器")
    args = ap.parse_args()

    if not os.path.isdir(WEB_DIR):
        print(f"[!] 找不到前端目录：{WEB_DIR}", file=sys.stderr)

    source, kw = args.source, {}
    if args.source == "auto":
        source, kw = auto_source()
    elif args.source == "serial":
        kw = {"port": args.serial_port, "baud": args.baud}
    elif args.source == "wifi":
        kw = {"host": args.wifi_host or "192.168.1.50"}

    try:
        info = APP.connect(source, **kw)
        print(f"[+] 数据源：{source} {info}")
    except Exception as e:                                      # noqa: BLE001
        print(f"[!] 数据源启动失败（{e}），回退到模拟器", file=sys.stderr)
        APP.connect("mock")

    httpd = ThreadingHTTPServer((args.host, args.port), Handler)
    httpd.daemon_threads = True
    url = f"http://127.0.0.1:{args.port}/"
    print(f"[+] 上位机已启动：{url}")
    print(f"    Web  : {url}")
    print(f"    手机/模拟器: http://<本机局域网IP>:{args.port}/   （MuMu 用 http://10.0.2.2:{args.port}/）")
    print(f"    Web 目录: {WEB_DIR}")
    print("    Ctrl+C 退出")
    if not args.no_open:
        threading.Timer(0.8, lambda: webbrowser.open(url)).start()
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        print("\n[+] 退出中…")
    finally:
        httpd.server_close()
        APP.disconnect()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

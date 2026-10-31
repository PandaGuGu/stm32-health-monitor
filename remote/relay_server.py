#!/usr/bin/env python3
"""relay_server.py — 远程监测的中继服务：不落盘、不留历史、进程退出即清空。

    采集端手机 ──POST /api/vitals──▶ [ 内存一帧 ] ──GET /api/status──▶ 查看端手机

云端只解决一件事：**两端不在同一个网络**。它不存历史、不做回放、不认设备身份，
内存里永远只有最后一次 POST 上来的那一帧。7 天历史由 App 自己的 SQLite 保存，
因此换一台手机就看不到历史——这是"不存储"方案的硬边界。

鉴权：HTTP Basic Auth（账号密码）。上行与下行都校验，缺凭据或密码错误一律 401。
    ⚠️ Basic Auth 的凭据只是 base64，不是加密。明文 HTTP 下抓包即可拿到明文密码，
    所以公网部署必须套 HTTPS（见 remote/README.md 的 Caddy 反代，4 行配置自动证书）。

新鲜度（唯一需要小心的地方）：
    上报方会把自己算好的 age_ms（距最后一次有效帧）一起发上来，云端收到后再累加
    自己这一侧的滞留时间，两层相加才是真实帧龄。只算云端侧的话，STM32 断线但
    上报方仍在定时推送时，查看端会一直看到"刚刚更新"的旧读数。

接口：
    POST /api/vitals   {"vitals":{"ok":true,"bpm":88,...}}  或扁平 {"bpm":88,...}
                       需 Basic Auth；覆盖上一帧
                       跌倒报警帧另带 fall_ts（"yyyy-MM-dd HH:mm:ss"）与 fall_ms（epoch 毫秒），
                       中继原样透传，查看端据此显示报警时刻
    GET  /api/status   与 /api/latest 同义，返回 {"source":"relay","vitals":{...}}
                       需 Basic Auth（app-vitals 拼的是这个路径）
    GET  /api/health   收帧数、坏帧数、帧龄；需 Basic Auth

启动：
    python remote/relay_server.py --port 8000
    python remote/relay_server.py --user test1 --pass stm-esp-test1 --port 8000
    RELAY_USER=test1 RELAY_PASS=stm-esp-test1 python remote/relay_server.py

安全：内置凭据仅为测试账号，公网部署前必须用 --user/--pass 或环境变量换掉。

运行环境：Python 3.6+（CentOS 7 / Alibaba Cloud Linux 3 自带的 python3 是 3.6，
所以这里不用 PEP 604 的 `str | None` 写法，也不用 3.9+ 的内建泛型注解）。
"""

import argparse
import base64
import binascii
import hmac
import json
import os
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from typing import Any, Dict, Optional

LOCK = threading.Lock()
PAYLOAD: Dict[str, Any] = {}  # 只留一帧：最后一次 POST 解包后的 vitals
STATS: Dict[str, Any] = {"rx_frames": 0, "rx_bad": 0, "rx_denied": 0, "last_rx": 0.0}

# 测试账号。仅用于本地验证与演示，公网部署前必须换掉。
DEF_USER = "test1"
DEF_PASS = "stm-esp-test1"

ARGS = argparse.Namespace(
    host="0.0.0.0",
    port=8000,
    user=os.environ.get("RELAY_USER", DEF_USER),
    password=os.environ.get("RELAY_PASS", DEF_PASS),
    stale=10.0,
    verbose=False,
)

VITALS_KEYS = ("bpm", "spo2", "temp", "fall", "fall_ts", "fall_ms")
REALM = 'Basic realm="vitals-relay", charset="UTF-8"'


def unwrap(data: Dict[str, Any]) -> Dict[str, Any]:
    """App 发的是 {"vitals":{...}}，手测时可能只发扁平四字段，两种都收。"""
    inner = data.get("vitals")
    return inner if isinstance(inner, dict) else data


def check_credentials(header: Optional[str]) -> bool:
    """校验 Authorization: Basic base64(user:pass)。

    用 compare_digest 而不是 ==，避免逐字节短路带来的时序侧信道。
    """
    if not header or not header.startswith("Basic "):
        return False
    try:
        raw = base64.b64decode(header[6:].strip(), validate=False).decode("utf-8")
    except (binascii.Error, UnicodeDecodeError, ValueError):
        return False
    user, sep, pw = raw.partition(":")
    if not sep:
        return False
    return hmac.compare_digest(user, ARGS.user) and hmac.compare_digest(pw, ARGS.password)


def snapshot() -> Dict[str, Any]:
    with LOCK:
        if not PAYLOAD:
            return {
                "source": "relay",   # 顶层标记：App 据此区分"本地直连"与"经中继"
                "vitals": {
                    "ok": False,
                    "src": "cloud",
                    "age_ms": None,
                    "frames": STATS["rx_frames"],
                },
            }

        rx_age = int((time.time() - STATS["last_rx"]) * 1000)
        # 三种情况要分开：
        #   · 带数值 age_ms —— 上报方算好的帧龄，加上云端滞留才是真实帧龄
        #   · age_ms 为 null —— 上报方明确表示"没有有效读数"，上层 ok 必须为 false
        #   · 根本没有 age_ms —— 扁平手测报文，只能退回到云端侧滞留时间
        if "age_ms" in PAYLOAD:
            dev_age = PAYLOAD["age_ms"]
            age = int(dev_age) + rx_age if isinstance(dev_age, (int, float)) else None
        else:
            age = rx_age

        vitals: Dict[str, Any] = {k: PAYLOAD.get(k) for k in VITALS_KEYS}
        vitals["ok"] = (
            bool(PAYLOAD.get("ok", True))
            and age is not None
            and age < ARGS.stale * 1000
        )
        vitals["src"] = PAYLOAD.get("src") or "cloud"
        vitals["age_ms"] = age if age is not None else rx_age
        vitals["frames"] = PAYLOAD.get("frames", STATS["rx_frames"])
        vitals["bad"] = PAYLOAD.get("bad", 0)
        vitals["invalid"] = PAYLOAD.get("invalid", 0)
        return {"source": "relay", "vitals": vitals}


class Relay(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def _send(self, code: int, payload: Any, deny: bool = False) -> None:
        body = json.dumps(payload, ensure_ascii=False).encode("utf-8")
        self.send_response(code)
        if deny:
            self.send_header("WWW-Authenticate", REALM)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Access-Control-Allow-Origin", "*")
        self.end_headers()
        self.wfile.write(body)

    def _auth(self) -> bool:
        if check_credentials(self.headers.get("Authorization")):
            return True
        with LOCK:
            STATS["rx_denied"] += 1
        self._send(401, {"error": "unauthorized"}, deny=True)
        return False

    def do_OPTIONS(self) -> None:  # noqa: N802
        self.send_response(204)
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Access-Control-Allow-Headers", "Content-Type, Authorization")
        self.send_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS")
        self.send_header("Content-Length", "0")
        self.end_headers()

    def do_POST(self) -> None:  # noqa: N802
        if self.path.split("?")[0] != "/api/vitals":
            return self._send(404, {"error": "not_found"})
        if not self._auth():
            return

        try:
            length = int(self.headers.get("Content-Length") or 0)
            raw = self.rfile.read(length) if length else b"{}"
            data = json.loads(raw)
            if not isinstance(data, dict):
                raise ValueError("body must be a json object")
        except Exception as exc:  # noqa: BLE001
            with LOCK:
                STATS["rx_bad"] += 1
            return self._send(400, {"error": "bad_json", "detail": str(exc)[:80]})

        with LOCK:
            PAYLOAD.clear()
            PAYLOAD.update(unwrap(data))
            STATS["rx_frames"] += 1
            STATS["last_rx"] = time.time()

        self._send(200, {"ok": True})

    def do_GET(self) -> None:  # noqa: N802
        path = self.path.split("?")[0]
        # /api/status 与 /api/latest 同义：app-vitals 拼的是 baseUrl + "/api/status"，
        # 保留这个别名，App 才能只改地址、不动代码
        if path in ("/api/latest", "/api/status"):
            if not self._auth():
                return
            return self._send(200, snapshot())

        if path == "/api/health":
            if not self._auth():
                return
            with LOCK:
                age = (
                    int((time.time() - STATS["last_rx"]) * 1000)
                    if STATS["last_rx"]
                    else None
                )
                return self._send(
                    200,
                    {
                        "ok": True,
                        "rx_frames": STATS["rx_frames"],
                        "rx_bad": STATS["rx_bad"],
                        "rx_denied": STATS["rx_denied"],
                        "frame_age_ms": age,
                        "stale": ARGS.stale,
                        "buffer": "memory-only",
                    },
                )
        self._send(404, {"error": "not_found"})

    def log_message(self, fmt: str, *args: Any) -> None:
        if ARGS.verbose:
            super().log_message(fmt, *args)


def main() -> None:
    p = argparse.ArgumentParser(description="远程监测中继：不存储，只转发最新一帧")
    p.add_argument("--host", default="0.0.0.0", help="监听地址（手机要连就必须 0.0.0.0）")
    p.add_argument("--port", type=int, default=8000)
    p.add_argument("--user", default=os.environ.get("RELAY_USER", DEF_USER))
    p.add_argument("--pass", dest="password",
                   default=os.environ.get("RELAY_PASS", DEF_PASS))
    p.add_argument("--stale", type=float, default=10.0, help="超过多少秒视为掉线（ok=false）")
    p.add_argument("--verbose", action="store_true", help="打印每条 HTTP 请求")
    p.parse_args(namespace=ARGS)

    srv = ThreadingHTTPServer((ARGS.host, ARGS.port), Relay)
    print(f"[relay] 监听 http://{ARGS.host}:{ARGS.port}   存储=无（仅内存一帧）")
    print(f"[relay] Basic Auth 账号={ARGS.user} 密码={ARGS.password}   stale={ARGS.stale}s")
    if ARGS.user == DEF_USER and ARGS.password == DEF_PASS:
        print("[relay] ⚠ 正在使用内置测试凭据，公网部署前请用 --user/--pass 换掉")
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        print("\n[relay] 退出，内存中的那一帧随之丢弃")


if __name__ == "__main__":
    main()

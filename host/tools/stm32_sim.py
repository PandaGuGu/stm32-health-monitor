#!/usr/bin/env python3
"""STM32 生理数据模拟器

没有 STM32 时，用它替代上游：

  # 1) 走网关注入（最省事，ESP32 不用接任何线）
  python host/tools/stm32_sim.py

  # 2) 走真串口：USB-TTL 的 TX 接 ESP32 GPIO16(RX)、GND 共地
  python host/tools/stm32_sim.py --serial COM7

  # 3) 制造事件，看上位机高亮/告警是否跟随
  python host/tools/stm32_sim.py --pattern fall
  python host/tools/stm32_sim.py --pattern lowspo2

帧格式与固件一致（docs/esp32/protocol.md §8）：
  {"bpm":75,"spo2":98,"temp":36.5,"fall":0,"src":"stm32"}
"""

from __future__ import annotations

import argparse
import json
import random
import time
import urllib.request

try:
    import serial
except ImportError:
    serial = None


def make_frame(p: dict) -> bytes:
    line = json.dumps(
        {
            "bpm": p["bpm"],
            "spo2": p["spo2"],
            "temp": round(p["temp"], 1),
            "fall": p["fall"],
            "src": "stm32",
        },
        separators=(",", ":"),
        ensure_ascii=False,
    )
    return (line + "\r\n").encode("utf-8")


def step(p: dict, pattern: str) -> None:
    """一阶随机游走，保持在生理区间内。"""
    p["bpm"] = max(52, min(118, p["bpm"] + random.randint(-2, 2)))
    p["spo2"] = max(88, min(100, p["spo2"] + random.choice([-1, 0, 0, 1])))
    p["temp"] = max(35.0, min(38.5, p["temp"] + random.uniform(-0.06, 0.06)))

    if pattern == "fall":
        p["fall"] = 1
    elif pattern == "lowspo2":
        p["spo2"] = random.randint(88, 91)
        p["bpm"] = random.randint(100, 115)
    else:
        p["fall"] = 0


def main() -> int:
    ap = argparse.ArgumentParser(description="STM32 生理数据模拟器")
    ap.add_argument("--serial", help="串口，如 COM7（USB-TTL 接 ESP32 GPIO16）")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--gateway", default="http://127.0.0.1:8080", help="网关地址（注入模式）")
    ap.add_argument("--interval", type=float, default=1.0, help="发帧间隔（秒）")
    ap.add_argument("--pattern", default="normal", choices=["normal", "fall", "lowspo2"])
    ap.add_argument("--count", type=int, default=0, help="发多少帧后退出，0=一直发")
    args = ap.parse_args()

    p = {"bpm": 76, "spo2": 97, "temp": 36.5, "fall": 0}
    ser = None
    if args.serial:
        if serial is None:
            print("需要 pyserial：pip install pyserial")
            return 1
        ser = serial.Serial(args.serial, args.baud, timeout=1)
        print(f"[+] 串口模式 {args.serial} @ {args.baud}，Ctrl+C 退出")
    else:
        print(f"[+] 网关注入模式 {args.gateway}，Ctrl+C 退出")

    n = 0
    try:
        while True:
            step(p, args.pattern)
            frame = make_frame(p)
            if ser:
                ser.write(frame)
                ser.flush()
            else:
                body = json.dumps(
                    {"cmd": f"vitals.sim {p['bpm']} {p['spo2']} {p['temp']:.1f} {p['fall']}"}
                ).encode()
                req = urllib.request.Request(
                    args.gateway.rstrip("/") + "/api/cmd",
                    data=body,
                    headers={"Content-Type": "application/json"},
                )
                try:
                    with urllib.request.urlopen(req, timeout=4) as r:
                        r.read()
                except Exception as e:                       # noqa: BLE001
                    print(f"[!] 注入失败：{e}")
            n += 1
            if args.count and n >= args.count:
                break
            time.sleep(args.interval)
    except KeyboardInterrupt:
        print(f"\n[+] 已发 {n} 帧")
    finally:
        if ser:
            ser.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

# 上位机（host/）

PC 侧网关、两款 Android App、以及没有硬件时的数据注入工具。

```
host/
├─ gateway/       Python 网关：串口 / WiFi / 模拟器三种数据源，REST + SSE
├─ app/           Android 演示 App（Java，零第三方库），Gradle 构建
├─ app-vitals/    健康监测 App（本地工程，不入库；三档链路，成品 APK 见 Releases）
└─ tools/         stm32_sim.py：没有 STM32 时喂生理数据
```

## 1. 为什么需要网关

```
              ┌──────────────┐  USB 串口 (JSON Lines)  ┌─────────┐
              │              │◀───────────────────────▶│ ESP32   │
 Android ────▶│   PC 网关     │  HTTP 轮询 /api/*        │ 信息节点 │
  (App 端)    │  :8080       │◀───────────────────────▶│         │
              │              │                          └─────────┘
              │  同一份       │
              │  snapshot    │  模拟器：无硬件时的完全等价替身
              └──────────────┘
```

- **一份数据源接入**：串口只有 PC 碰得到，手机在别的网段，靠网关把同一份快照暴露成 HTTP
- **没硬件也能开发**：`--source mock` 生成的字段与命令语义和真机一致，App 的渲染逻辑可以先验完
- **ESP32 只吐 JSON**：4 MB Flash 不塞前端资源，设备 IP 变了也不用改客户端

网关只输出 JSON，不托管任何静态页面。浏览器直接访问根路径会返回 404 并在 `hint` 里指出该用哪个接口。

## 2. 启动网关

只依赖 `pyserial`，其余全是标准库。

```bash
# 建 venv（一次性）
python -m venv .venv
.venv/Scripts/pip install pyserial -i https://pypi.tuna.tsinghua.edu.cn/simple

# 自动挑：有 ESP32 串口就串口，否则模拟器
.venv/Scripts/python host/gateway/server.py

# 指定数据源（串口是 --serial；--port 是 HTTP 端口，别写混）
.venv/Scripts/python host/gateway/server.py --source mock
.venv/Scripts/python host/gateway/server.py --source serial --serial COM3 --baud 115200
.venv/Scripts/python host/gateway/server.py --source wifi  --wifi 192.168.1.66
```

| 参数 | 说明 |
|---|---|
| `--host` / `--port` | 监听地址与端口，默认 `0.0.0.0:8080`（手机要连就必须是 0.0.0.0） |
| `--source` | `auto` / `serial` / `wifi` / `mock` |
| `--serial` `--baud` | 串口号与波特率 |
| `--wifi` | 设备 IP（或 `host:port`） |

## 3. 接口

| 方法 | 路径 | 说明 |
|---|---|---|
| GET | `/api/snapshot`、`/api/status` | 合并后的完整快照 |
| GET | `/api/ports` | 串口枚举，带 `likely_esp32` 标记 |
| GET | `/api/log` | 最近日志与原始行 |
| GET | `/api/events` | **SSE** 推送，1 Hz 推整份 snapshot |
| GET | `/api/health` | 存活探测，返回版本号 |
| POST | `/api/connect` | `{"source":"serial","port":"COM3"}` / `{"source":"wifi","host":"..."}` / `{"source":"mock"}` |
| POST | `/api/disconnect` | 断开当前数据源 |
| POST | `/api/cmd` | `{"cmd":"gpio.set 2 1"}`，返回配对的 `ack` |

快照包含四类信息（芯片与固件、网络、运行时资源、板载 IO）以及 STM32 的生理数据段 `vitals`。

为什么用 SSE 而不是 WebSocket：`EventSource` 原生支持、自动重连，服务端一行行写 `text/event-stream` 即可，
标准库就能实现，不必引第三方库或手写帧协议。下行命令走普通 POST，本来就不需要长连接。

```bash
curl http://127.0.0.1:8080/api/snapshot | python -m json.tool
curl "http://127.0.0.1:8080/api/cmd?cmd=gpio.get" 
```

## 4. Android 演示 App

`host/app` 是纯 Java + 原生 View，不依赖任何第三方库（连 AndroidX 都没引），不需要联网拉依赖也能构建。

```bash
cd host/app
# local.properties 里写好 sdk.dir
JAVA_HOME=/path/to/jdk17 gradle assembleDebug
adb install -r app/build/outputs/apk/debug/app-debug.apk
```

宽屏（`sw600dp` 以上，例如 MuMu 横屏 1600×900）自动双列，窄屏单列；引脚可点，扫描与命令台与网关同源。

四个数据源档位：

| 档 | 地址 | 场景 |
|---|---|---|
| 串口 / WiFi / 模拟器 | `http://10.0.2.2:8080`（MuMu 访问宿主机） | 走 PC 网关 |
| **直连** | `http://192.168.4.1` | 手机连设备热点 `esp32-info-XXXX` / `12345678`，不需要电脑 |

- 直连模式下设备 `/api/status` 与网关快照同构（`source=device`），App 代码零改动
- 支持启动参数直达：`adb shell am start -n com.sit.esp32info/.MainActivity -e url http://192.168.4.1`
- MuMu 连设备热点时经宿主 NAT（宿主连上 AP，模拟器即可 ping 通 `192.168.4.1`）
- 明文 HTTP 已在 `AndroidManifest.xml` 里放开（`usesCleartextTraffic="true"`），仅用于内网调试

截图：[docs/esp32/app-mumu-1.png](../docs/esp32/app-mumu-1.png)、
[docs/esp32/app-mumu-2.png](../docs/esp32/app-mumu-2.png)

## 5. 健康监测 App

`host/app-vitals` 只做生理数据展示，源码为本地工程、不入库，成品 APK 从
[Releases](https://github.com/PandaGuGu/stm32-health-monitor/releases) 下载。

| 项 | 值 |
|---|---|
| 包名 | `com.sit.vitals` |
| 最低系统 | Android 7.0（API 24） |
| 链路 | 本地直连 / 远程中继 / 蓝牙，设置页切换 |
| 本地存储 | SQLite 保留 7 天，5 s 降采样，只存有效帧 |

界面、设置项与档位的对应关系、跌倒报警规则见 [docs/app/README.md](../docs/app/README.md)。

## 6. 远程监测（另一条链路）

`host/gateway` 这一套的前提是手机与设备同网：同一路由器，或连设备自带热点。异地场景走独立方案
[`remote/`](../remote/README.md)：

```
ESP32 ──BLE──▶ 采集端 App ──POST /api/vitals──▶ 公网中继（内存一帧）──GET /api/status──▶ 异地 App
                    └── 本地 SQLite 7 天
```

上行执行者是采集端 App，不是 ESP32——设备不需要 WiFi 凭据，也不必在 BLE 与 WiFi 之间争抢射频。
中继不存储，进程退出即清空。定位差异、部署步骤与帧龄算法见 [remote/README.md](../remote/README.md)。

## 7. 没有 STM32 时怎么验生理数据

`host/tools/stm32_sim.py` 是 STM32 的替身，两种喂法：

```bash
# A. 走网关注入（ESP32 不用接任何线）
python host/tools/stm32_sim.py
python host/tools/stm32_sim.py --pattern fall      # 制造跌倒
python host/tools/stm32_sim.py --pattern lowspo2   # 低血氧 + 心率偏快

# B. 走真串口：USB-TTL 的 TX 接 ESP32 GPIO16(RX)，GND 共地
python host/tools/stm32_sim.py --serial COM7
```

A 模式用的是 ESP32 固件的 `vitals.sim` 命令，与真串口共用同一个解析函数，
因此「注入能通」等于「解析这段逻辑能通」；接上 STM32 后只剩电平、波特率、共地三项要验。

## 8. 构建注意

- 仓库路径含中文会导致 AGP 报 "project path contains non-ASCII characters" 并退出，
  `host/app/gradle.properties` 已加 `android.overridePathCheck=true`，8.7.3 + Gradle 8.14.3 实测正常
- `compileSdk 35` / `minSdk 24`；release 暂时复用 debug 签名，方便直接 `adb install`
- MuMu 截图（1080×593）与物理分辨率（1600×900）不是 1:1，`adb shell input tap` 要用物理坐标；
  用 `uiautomator dump` 拿 `bounds` 最稳

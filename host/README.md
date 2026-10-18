# 上位机（host/）

ESP32 信息节点的调试上位机，**Web 端 + Android 端**共用一份 PC 网关。

```
host/
├─ gateway/       Python 网关：串口 / WiFi / 模拟器 三种数据源，REST + SSE
├─ web/           单页面板（无构建、无依赖），由网关直接托管
├─ app/           Android 原生 App（Java，零第三方库），Gradle 构建
└─ tools/         stm32_sim.py：没有 STM32 时的生理数据模拟器
```

## 1. 为什么是"网关 + 双端"

```
              ┌──────────────┐  USB 串口 (JSON Lines)  ┌─────────┐
              │              │◀───────────────────────▶│ ESP32   │
 浏览器 ─────▶│   PC 网关     │  HTTP 轮询 /api/*        │ 信息节点 │
 (Web 端)     │  :8080       │◀───────────────────────▶│         │
              │              │                          └─────────┘
 Android ────▶│  同一份       │
  (App 端)    │  snapshot    │  模拟器：无硬件时的完全等价替身
              └──────────────┘
```

- **一份数据源接入，两端复用**：串口只有 PC 能碰，App 在模拟器/手机上，靠网关暴露 HTTP 才能拿到同一份数据
- **没硬件也能开发**：`--source mock` 生成的字段、命令语义与真机一致，Web/App 的渲染逻辑可以全部先验完
- **ESP32 只吐 JSON**：4 MB Flash 不塞前端资源，设备 IP 变了也不用改前端

## 2. 启动网关

只依赖 `pyserial`，其余全是标准库（国内网络装不了 FastAPI 也能跑）。

```bash
# 建 venv（一次性）
python -m venv .venv
.venv/Scripts/pip install pyserial -i https://pypi.tuna.tsinghua.edu.cn/simple

# 自动挑：有 ESP32 串口就串口，否则模拟器
.venv/Scripts/python host/gateway/server.py

# 指定数据源（串口是 --serial；--port/--http-port 是 HTTP 端口，别写混）
.venv/Scripts/python host/gateway/server.py --source mock
.venv/Scripts/python host/gateway/server.py --source serial --serial COM3 --baud 115200
.venv/Scripts/python host/gateway/server.py --source wifi  --wifi 192.168.1.66
```

启动后浏览器自动打开 `http://127.0.0.1:8080/`。

| 参数 | 说明 |
|---|---|
| `--host` / `--port` | 监听地址与端口，默认 `0.0.0.0:8080`（手机要连就必须是 0.0.0.0） |
| `--source` | `auto` / `serial` / `wifi` / `mock` |
| `--serial` `--baud` | 串口号与波特率 |
| `--wifi` | 设备 IP（或 `host:port`） |
| `--no-open` | 不自动开浏览器 |

## 3. 接口

| 方法 | 路径 | 说明 |
|---|---|---|
| GET | `/` | Web 面板（`host/web` 静态托管） |
| GET | `/api/status`、`/api/snapshot` | 合并后的完整快照 |
| GET | `/api/ports` | 串口枚举，带 `likely_esp32` 标记 |
| POST | `/api/connect` | `{"source":"serial","port":"COM3"}` / `{"source":"wifi","host":"..."}` / `{"source":"mock"}` |
| POST | `/api/disconnect` | 断开当前数据源 |
| POST | `/api/cmd` | `{"cmd":"gpio.set 2 1"}`，返回配对的 `ack` |
| GET | `/api/log` | 最近日志与原始行 |
| GET | `/api/events` | **SSE** 推送场景，前端断线自动降级为轮询 |

> 为什么用 SSE 而不是 WebSocket：浏览器 `EventSource` 原生支持、自动重连，服务端只要
> `text/event-stream` 一行行写，标准库就能实现，不引 `websockets` 也不用手写帧协议。
> 下行命令走普通 POST，本来就不需要长连接。

## 4. Web 端

`host/web` 是**无构建**的单页应用（`index.html` + `style.css` + `app.js`），改完刷新即可。

- 数据源切换（串口 / WiFi / 模拟器）、串口枚举与一键连接
- 四类信息卡片：芯片与固件、网络、运行时资源、板载 IO
- **生理数据卡片**：STM32 的心率 / 血氧 / 体温 / 跌倒 + 帧龄 + 来源（`stm32` 真串口 / `sim` 注入），带心率曲线
- 三条 Canvas 迷你曲线（空闲堆 / 芯片温度 / RSSI）
- WiFi 扫描表（信号条 + 信道 + 加密）
- 命令台（带 ack 回执）与原始 JSON 流
- 「导出快照」按钮：把当前 snapshot 存成 JSON，贴进调试报告正好

截图：[docs/esp32/web-dashboard.png](../docs/esp32/web-dashboard.png)

## 5. Android 端

`host/app` 是**纯 Java + 原生 View** 的 App，**不依赖任何第三方库**（连 AndroidX 都没引），
所以不需要联网拉依赖也能构建，APK 只有 30 KB。

```bash
cd host/app
# local.properties 里写好 sdk.dir
JAVA_HOME=E:/android-env/jdk17 gradle assembleDebug
adb install -r app/build/outputs/apk/debug/app-debug.apk
```

界面：宽屏（`sw600dp` 以上，例如 MuMu 横屏 1600×900）自动双列，窄屏单列；1 Hz 轮询网关
`/api/status`，引脚可点、扫描/命令台与 Web 端完全同源。

- 默认网关地址：`http://10.0.2.2:8080`（Android 模拟器访问宿主机的固定地址）
- MuMu 若不通，可在 App 里改成电脑局域网 IP（如 `http://192.168.1.100:8080`），
  或 `adb reverse tcp:8080 tcp:8080` 后用 `http://127.0.0.1:8080`
- 明文 HTTP 已在 `AndroidManifest.xml` 里放开（`usesCleartextTraffic="true"`），仅用于内网调试

截图：[docs/esp32/app-mumu-1.png](../docs/esp32/app-mumu-1.png)、
[docs/esp32/app-mumu-2.png](../docs/esp32/app-mumu-2.png)

## 6. 没有 STM32 时怎么验生理数据

`host/tools/stm32_sim.py` 是 STM32 的替身，两种喂法：

```bash
# A. 走网关注入（ESP32 不用接任何线，最省事）
python host/tools/stm32_sim.py
python host/tools/stm32_sim.py --pattern fall      # 制造跌倒，看红色高亮
python host/tools/stm32_sim.py --pattern lowspo2   # 低血氧 + 心率偏快

# B. 走真串口：USB-TTL 的 TX 接 ESP32 GPIO16(RX)，GND 共地
python host/tools/stm32_sim.py --serial COM7
```

A 模式用的是 ESP32 固件的 `vitals.sim` 命令，与真串口**共用同一个解析函数**，
所以"注入能通"等于"解析这段逻辑能通"；接上 STM32 后只多验电平/波特率/共地。

## 7. 构建注意（踩过的坑）

- 仓库路径含中文（`小组嵌入式`），AGP 会直接报 "project path contains non-ASCII characters" 并退出。
  `host/app/gradle.properties` 里已加 `android.overridePathCheck=true`，实测 8.7.3 + Gradle 8.14.3 构建正常
- `compileSdk 35` / `minSdk 24`；release 暂时复用 debug 签名，方便直接 `adb install`
- MuMu 的截图（1080×593）与物理分辨率（1600×900）不是 1:1，`adb shell input tap` 要用物理坐标；
  用 `uiautomator dump` 拿 `bounds` 最稳

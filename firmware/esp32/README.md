# ESP32 信息节点（firmware/esp32）

ESP32-WROOM-32 上的**信息采集固件**，只干三件事：采集、上报、听命令。不掺业务判定，
方便上位机（`host/gateway` + `host/app`）单独调试。

- 协议：**JSON Lines 上行 / 文本命令下行**，详见 [docs/esp32/protocol.md](../../docs/esp32/protocol.md)
- 数据源可切：串口（USB 转串口）或 WiFi（设备自带 HTTP `/api/*`）
- 零第三方库：只用 esp32 core 自带的 `WiFi` / `WebServer` / `Preferences` / FreeRTOS

## 1. 目录

```
firmware/esp32/
├─ esp32_info_node/
│  ├─ esp32_info_node.ino   入口：setup/loop，遥测节拍 + LED 心跳
│  ├─ config.h              周期、AP 兜底、NVS 命名空间
│  ├─ json_out.*            JSON 行输出（含捕获态，供 HTTP 复用）
│  ├─ device_info.*         芯片型号/rev/Flash/SDK/MAC/复位原因
│  ├─ net_info.*            WiFi 状态、AP 扫描、凭据存 NVS
│  ├─ runtime_info.*        堆/碎片度/任务数/CPU/芯片温度/RSSI
│  ├─ io_probe.*            GPIO 白名单、电平、ADC1、芯片温度
│  ├─ uart_link.*           UART2 ↔ STM32：收 vitals 帧、解析、并入 telemetry
│  ├─ cmd_router.*          下行命令解析与派发
│  ├─ http_api.*            WiFi 模式下的 /api/* 接口
│  ├─ ble_link.*            蓝牙链路（可选，ENABLE_BLE=1）：BLE NUS 外设，notify vitals
└─ build/                   编译产物（不入库）
```

## 2. 与 STM32 的接线

```
STM32 PA9  (USART1_TX) ──▶ ESP32 GPIO16 (UART2_RX)
STM32 PA10 (USART1_RX) ◀── ESP32 GPIO17 (UART2_TX)
STM32 GND              ──── ESP32 GND
```

STM32 每周期发一行 `{"bpm":75,"spo2":98,"temp":36.5,"fall":0}`（见
[protocol.md §8](../../docs/esp32/protocol.md)），ESP32 解析后并入 `telemetry.vitals`。
**没有 STM32 也能验**：`vitals.sim 78 97 36.5 0` 走的是同一个解析入口。

## 3. 手机直连热点（不用电脑）

ESP32 没有配网时默认开热点，手机连上就能直接看数据 —— **不经过 PC 网关**：

| 项 | 值 |
|---|---|
| SSID | `esp32-info-DAD8`（`esp32-info-` + MAC 后 4 位） |
| 密码 | **`12345678`** |
| 信道 | 6 |
| 设备地址 | `http://192.168.4.1` |

步骤：

1. 手机 WiFi 连上 `esp32-info-DAD8`（会提示"无互联网"，选**保持连接**）
2. 打开 App → 数据源选 **直连** → 网关地址填 `http://192.168.4.1` → 连接
   （或用 adb 直接指定：`adb shell am start -n com.sit.esp32info/.MainActivity -e url http://192.168.4.1`）
3. 浏览器也能看：`http://192.168.4.1/` 是纯文本状态页；`/api/status` 是完整 JSON
4. 想加密码/改 SSID：`wifi.ap <ssid> <密码>`（密码 ≥ 8 位，空则开放热点）

> 设备侧 `/api/status` 与 PC 网关的 snapshot **同构**（`source=device`），所以同一套 App 既能连电脑网关，
> 也能直连设备。设备不缓存原始日志，App 的"原始数据流"区在直连模式下会提示改用 PC 网关。

### 蓝牙（BLE）

WiFi 没配、也不想到手开热点时，用 BLE 就近收数据。固件需按 `ENABLE_BLE=1` 编译。

| 项 | 值 |
|---|---|
| 广播名 | `stm32-vitals-XXXX`（`BLE_NAME` + MAC 后 4 位，如 `stm32-vitals-DAD8`） |
| 服务 | Nordic UART Service `6E400001-B5A3-F393-E0A9-E50E24DCCA9E` |
| 通知特征 | `6E400003-…`（设备 → 手机，`{"vitals":{...}}` 原样，约 132 字节） |
| 写特征 | `6E400002-…`（手机 → 设备，当前固件不解析，只为 UUID 完整） |
| MTU | 设备侧申请 517，手机端需 `requestMtu` 到 40 以上，否则一帧装不下会直接跳过不发 |

- 每 2 s 推一次（`BLE_NOTIFY_MS`）；有新有效帧时立刻推，没有也会按周期兜底推，
  这样手机能区分「设备在但没读数」和「设备掉了」
- 与 WiFi 共存可用，但射频分时，两边吞吐都会下降
- 断开后固件自动重启广播；手机侧不自动重连，需手动再选一次「蓝牙」档

## 4. 编译

需要 `arduino-cli` + **esp32 core 2.0.17**（3.x 会把所有芯片的预编译库都当依赖，下载 3–4 GB，
只跑 ESP32 没必要）。

```bash
# 一次性的环境准备
arduino-cli core update-index --additional-urls https://espressif.github.io/arduino-esp32/package_esp32_index.json
arduino-cli core install esp32:esp32@2.0.17 --additional-urls https://espressif.github.io/arduino-esp32/package_esp32_index.json

# 编译（仓库自带脚本，默认 huge_app 分区）
powershell -ExecutionPolicy Bypass -File scripts\build-esp32.ps1

# 带蓝牙
powershell -ExecutionPolicy Bypass -File scripts\build-esp32.ps1 -Define ENABLE_BLE=1
```

`ENABLE_BLE` 在 `config.h` 里，默认 `0`。关闭时模块内函数是空实现，sketch 无条件调用也能链接。

**BLE 必须用 `huge_app` 分区**（脚本默认已是）：BLE 栈让固件从 986 KB 涨到 1.73 MB，
默认分区只给应用 1.3 MB，会直接报 `text section exceeds available space in board`。

⚠️ **仓库父目录含中文（`心率血`），`xtensa-g++` 在中文路径下会静默崩溃**
（只报 `Error during build: exit status 1`，没有任何错误行，编译器进程卡在 0.1 s CPU）。
`build-esp32.ps1` 因此先把 sketch 复制到 `%TEMP%\esp32build` 再编译；手工调用 `arduino-cli` 时
也要自己复制到英文路径，否则会以为是代码写错了。

实测结果（2026-10-30，esp32 core 3.3.12）：

| 配置 | Flash | RAM |
|---|---|---|
| 默认（无 BLE） | 986 916 B / 75% | 49 104 B |
| `ENABLE_BLE=1`（huge_app） | 1 732 567 B / 55% | 64 596 B |

## 5. 烧录

```bash
powershell -ExecutionPolicy Bypass -File scripts\build-esp32.ps1 -Upload -Port COM3
```

USB 转串口驱动（CH340 / CP210x）装好后再插板；若提示占用，先关掉串口监视器与网关。

## 6. 上电行为

1. 读 NVS 里的 WiFi 凭据 → 有就 STA 连网，没有就开热点：`esp32-info-XXXX` / **`12345678`**（信道 6）
2. 串口 115200 立刻吐三行：`hello`（身份）、`net`（联网状态）、`telemetry`（首帧遥测）
3. 之后每 `interval`（默认 1000 ms）一行 `telemetry`；GPIO2 每 500 ms 翻转，作为"我还活着"的硬指标
4. WiFi 起来后，`http://<设备IP>/` 是纯文本状态页，`/api/*` 供网关轮询或 curl 手测

## 7. 命令速查

| 命令 | 作用 |
|---|---|
| `info` | 重发 hello + net |
| `telemetry` / `net` / `scan` / `temp` | 单项立即上报 |
| `gpio.get all` / `gpio.set 2 1` / `gpio.mode 4 in_pullup` | GPIO 读 / 写 / 改模式 |
| `adc.get 34` | 读 ADC1（32/33/34/35/36/39） |
| `interval 500` | 改上报周期（100–60000 ms） |
| `wifi.sta <ssid> <pass>` / `wifi.ap <ssid> [pass]` / `wifi.off` | 切换射频模式（STA 凭据存 NVS） |
| `vitals.status` | 看 STM32 链路状态（同 telemetry 的 `vitals`） |
| `vitals.sim <bpm> <spo2> <temp> [fall]` | 注入一帧模拟数据，走与真串口相同的解析 |
| `vitals.uart on\|off` | 开关 UART2（关掉可释放 GPIO16/17） |
| `vitals.echo` | 往 UART2 TX 发一帧样例，短接 GPIO17→16 可回环自检 |
| `ping` / `echo <text>` / `help` / `reboot` | 探活 / 回显 / 帮助 / 重启 |

上位机会给每条命令加自增 id（`12 scan`），固件在 `ack.id` 里带回，用于请求-响应配对。

## 8. 引脚策略

| 类别 | 引脚 | 说明 |
|---|---|---|
| 可输出 | 2,4,5,12–19,21–23,25–27,32,33 | `gpio.set` 仅接受这些，且会自动切 OUTPUT |
| 仅输入 / ADC1 | 34,35,36,39 | `gpio.set` 直接返回 `pin_not_output` |
| 禁用 | 0,1,3,6–11 | strap、UART0、SPI Flash |

**ADC2 不开放**：GPIO 0/2/4/12–15/25–27 属于 ADC2，WiFi 一开就读不了，避免"时好时坏"的坑。

## 9. 已知边界

- 芯片温度（`temperatureRead()`）比环境高 5–15 ℃，只能看趋势，不能当环境温度
- `scan` 为**同步阻塞**（1.5–3 s），期间遥测暂停；WIFI_AP 模式下会临时切 `AP_STA` 再切回
- 遥测只有 1 Hz；要更高频请改 `interval`，但串口 115200 下建议不低于 100 ms
- 固件本身不做任何健康判定，跌倒/心率仍由 STM32 侧负责（见 [docs/architecture.md](../../docs/architecture.md)）
- STM32 主循环 **5–6 s 才发一帧**（500 点采样 + DS18B20 转换），所以 `vitals.ok` 的陈旧阈值是 **8 s**
  而不是 5 s，否则接上线会周期性显示"已过期"
- GPIO16/17 被 UART2 占用，`gpio.set` / `gpio.mode` 对它们返回 `pin_uart`
- 热点用 **AP_STA** 起（不是纯 AP）：扫描只能在 station 接口做，纯 AP 下切模式会把已连的手机踢下线
- 无 STA 连接时 `net.mode` 仍报 `ap`、`state=ap_listening`，避免上位机把"热点模式"误显示成"连接中"

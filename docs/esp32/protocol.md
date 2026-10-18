# ESP32 信息节点 · 通信协议 v1

> 适用固件：`firmware/esp32/esp32_info_node`（`fw 0.1.0`）
> 适用上位机：`host/gateway`（串口 / WiFi / 模拟三种数据源）
> 涉及代码：固件 `json_out.cpp`、`cmd_router.cpp`；网关 `protocol.py`

## 1. 设计取向

| 方向 | 格式 | 理由 |
|---|---|---|
| ESP32 → 上位机 | **JSON Lines**（一行一个 JSON，`\n` 结尾） | 上位机/浏览器/App 都能直接 `JSON.parse`，加字段不破坏旧解析 |
| 上位机 → ESP32 | **极简文本命令**（`<id> <verb> [args...]`） | MCU 侧用 `strtok` 即可，不引入 ArduinoJson；命令可读、可手敲 |

波特率 **115200 8N1**。串口只跑这一路协议，调试日志也走 `log` 行（不污染 stdout 文本）。

## 2. 通用信封

所有上行行共享三个字段：

```json
{"v":1,"t":"<type>","ts":<毫秒>}
```

- `v` 协议版本，当前 `1`；上位机发现 `v != 1` 只告警、继续解析（前向兼容）
- `t` 行类型，见下表
- `ts` 设备上电以来的毫秒数（`millis()`），**不是**墙钟时间；上位机自行换算 `uptime`

| `t` | 触发时机 | 载荷 |
|---|---|---|
| `hello` | 上电、`info` 命令 | 芯片 / Flash / SDK / MAC / 复位原因 |
| `telemetry` | 周期上报（默认 1000 ms）、`telemetry` 命令 | 堆、CPU、任务数、芯片温度、RSSI、GPIO 电平、ADC |
| `net` | 联网状态变化、`net` 命令 | 模式 / 状态 / IP / 网关 / 掩码 / 信道 / 主机名 |
| `scan` | `scan` 命令 | 周边 AP 列表（单行返回，最多 24 个） |
| `ack` | 命令执行成功 | 回带命令 `id` 与 `data` |
| `err` | 命令执行失败 | 回带 `id`、错误码、说明 |
| `log` | 固件内部事件 | 级别 + 文本（上位机可折叠显示） |

## 3. 各上行行字段

### 3.1 `hello`

```json
{"v":1,"t":"hello","ts":312,"fw":"0.1.0","build":"Oct 18 2026 18:40:12",
 "model":"ESP32-D0WD-V3","rev":3,"cores":2,"freq":240,
 "flash_kb":4096,"psram_kb":0,"sdk":"v3.3.0","mac":"AA:BB:CC:DD:EE:FF",
 "chip_id":"aabbccddeeff","reset":"poweron",
 "features":["wifi","bt","ble"]}
```

`reset` 取值：`poweron` / `sw` / `panic` / `int_wdt` / `task_wdt` / `wdt` / `deepsleep` / `brownout` / `sdio` / `unknown`。

### 3.2 `telemetry`

```json
{"v":1,"t":"telemetry","ts":5120,"up":5,
 "heap":218340,"heap_min":201556,"heap_max":110580,"frag":4,
 "cpu":240,"tasks":9,"temp":43.25,"rssi":-57,
 "gpio":{"2":0,"4":1,"5":0,"12":0,"13":0},
 "adc":{"34":{"raw":2048,"mv":1560},"35":{"raw":1023,"mv":780}},
 "vitals":{"ok":true,"bpm":76,"spo2":98,"temp":36.5,"fall":0,
           "src":"stm32","age_ms":820,"frames":42,"bad":0,"uart":true}}
```

- `heap*` 单位字节；`frag` 为最大可分配块占空闲堆的百分比（碎片度，越小越好）。
  **实测基线**：ESP32 + WiFi 开启时 `heap ≈ 250 KB`、`heap_max ≈ 110 KB`，即 `frag ≈ 57%` 属正常，
  上位机的绿色阈值因此定为 ≤65%（早期按 ≤8% 判绿会把正常板子标成红色）
- `temp` 为**芯片内部温度**（`temperatureRead()`），不是环境温度，典型比环境高 5–15 ℃
- `rssi` 未联网时为 `0`
- `gpio` 只包含**监视集合**内的引脚（见 §5），值为 `0`/`1`
- `adc` 每个引脚是 `{"raw":0-4095,"mv":毫伏}`（`mv` 由 `analogReadMilliVolts()` 校准得出）；
  只包含默认采样引脚与调用过 `adc.get` 的引脚
- `vitals` 来自 STM32（见 §8），字段含义：
  | 键 | 说明 |
  |---|---|
  | `ok` | 最近 8 s 内有有效帧（`VITALS_STALE_MS = 8000`；STM32 主循环 ~5–6 s 一帧） |
  | `bpm` / `spo2` | 心率 / 血氧，无效时为 0 |
  | `temp` | 体温 ℃，0.1 分辨率 |
  | `fall` | 1 = 跌倒 |
  | `src` | `stm32`（真串口）/ `sim`（`vitals.sim` 注入）/ `off`（UART 关闭） |
  | `age_ms` | 距最后一帧的毫秒数；从没收到过则为 `null` |
  | `frames` / `bad` | 累计有效帧 / 被丢弃的坏帧 |
  | `uart` | UART2 是否开启 |

### 3.3 `net`

```json
{"v":1,"t":"net","ts":5200,"mode":"sta","state":"connected",
 "ssid":"MyWiFi","ip":"192.168.1.50","gw":"192.168.1.1",
 "mask":"255.255.255.0","dns":"192.168.1.1","mac":"AA:BB:CC:DD:EE:FF",
 "bssid":"11:22:33:44:55:66","ch":6,"rssi":-57,"host":"esp32-info-node","clients":0}
```

`mode`：`sta` / `ap` / `apsta` / `off`；`state`：`connected` / `connecting` / `disconnected` / `ap_listening` / `off`。

### 3.4 `scan`

```json
{"v":1,"t":"scan","ts":9000,"n":2,"dur_ms":1840,
 "aps":[{"ssid":"MyWiFi","bssid":"11:22:33:44:55:66","rssi":-57,"ch":6,"enc":"WPA2_PSK"},
        {"ssid":"Guest","bssid":"AA:BB:CC:00:11:22","rssi":-78,"ch":11,"enc":"OPEN"}]}
```

隐藏 SSID 的 `ssid` 为空串，上位机显示 `<hidden>`。

### 3.5 `ack` / `err`

```json
{"v":1,"t":"ack","ts":6100,"id":"7","cmd":"gpio.set","ok":true,"data":{"pin":2,"value":1,"mode":"out"}}
{"v":1,"t":"err","ts":6200,"id":"8","cmd":"gpio.set","code":"pin_reserved","msg":"pin 6 is flash"}
```

错误码：`unknown_cmd` / `bad_args` / `pin_reserved` / `pin_not_output` / `wifi_fail` / `timeout`。

### 3.6 `log`

```json
{"v":1,"t":"log","ts":100,"lvl":"I","msg":"wifi: sta connect start"}
```

`lvl`：`I` / `W` / `E`。

## 4. 下行命令

格式：`[<id>] <verb> [args...]`，一行一条，`\n` 结尾。

- `<id>` 可省略（省略则 `ack.id` 为空串，仅用于人工调试）；上位机始终带自增 id，用于请求-响应配对
- 参数以**空格**分隔；含空格的参数（SSID/密码/文本）必须放行尾，固件取该位置之后的所有剩余字符

| 命令 | 参数 | 作用 | 返回 |
|---|---|---|---|
| `help` | — | 命令表 | `ack.data.cmds` |
| `info` | — | 拉一次完整身份 | `hello` + `net` |
| `telemetry` | — | 立刻上报一行遥测 | `telemetry` |
| `net` | — | 立刻上报联网状态 | `net` |
| `scan` | — | 扫描周边 AP（阻塞约 1.5–3 s） | `scan` |
| `temp` | — | 单独读芯片温度 | `ack.data.temp` |
| `gpio.get` | `<pin|all>` | 读电平 / 读全部监视引脚 | `ack.data.gpio` |
| `gpio.set` | `<pin> <0|1>` | 置输出电平（自动切 OUT） | `ack.data` |
| `gpio.mode` | `<pin> in|out|in_pullup|in_pulldown` | 改引脚模式 | `ack.data.mode` |
| `adc.get` | `<pin>` | 读 ADC1（12 bit） | `ack.data.raw` + `mv` |
| `interval` | `<ms>` | 遥测周期，100–60000 | `ack.data.interval_ms` |
| `wifi.sta` | `<ssid> <pass>` | 切 STA 并连网（凭据存 NVS） | `ack` 后 `net` |
| `wifi.ap` | `<ssid> <pass>` | 切 AP 模式（空密码 = 开放） | `ack` 后 `net` |
| `wifi.off` | — | 关射频 | `ack` |
| `vitals.status` | — | 看 STM32 链路状态（同 telemetry 里的 `vitals`） | `ack.data.vitals` |
| `vitals.sim` | `<bpm> <spo2> <temp> [fall]` | **注入一帧**模拟数据，走与真串口完全相同的解析路径 | `ack.data.vitals` |
| `vitals.uart` | `on` / `off` | 开关 UART2（关掉可释放 GPIO16/17） | `ack.data.uart` |
| `vitals.echo` | — | 往 UART2 TX 发一帧样例；**短接 GPIO17→16 可回环自检** | `ack.data.sent` |
| `ping` | — | 链路探活 | `ack.data.pong` |
| `echo` | `<text...>` | 回显 | `ack.data.text` |
| `reboot` | — | 软复位（先回 ack，再重启） | `ack` |

示例会话（上位机视角）：

```
→ 12 scan
← {"v":1,"t":"scan","ts":9000,"n":2,...}
→ 13 gpio.set 2 1
← {"v":1,"t":"ack","ts":6100,"id":"13","cmd":"gpio.set","ok":true,"data":{"pin":2,"value":1,"mode":"out"}}
```

## 5. 引脚策略（硬编码在 `config.h`）

| 类别 | 引脚 | 说明 |
|---|---|---|
| 可输出 | 2,4,5,12–19,21–23,25–27,32,33 | `gpio.set` 只接受这些 |
| 仅输入 / ADC1 | 34,35,36,39 | `gpio.set` 返回 `pin_not_output` |
| ADC1 可采 | 32,33,34,35,36,39 | **ADC2（GPIO 0/2/4/12–15/25–27）在 WiFi 开启时不可用**，故不开放 |
| 监视集合 | 上述全部 | 每次 `telemetry` 上报其电平 |
| 硬禁用 | 0,1,3,6–11 | 0=启动 strap、1/3=UART0、6–11=SPI Flash |

## 6. WiFi 模式下的 HTTP 接口（可选，供直连调试）

ESP32 成功入网或起 AP 后，在 **80** 端口提供：

| 方法 | 路径 | 说明 |
|---|---|---|
| GET | `/api/snapshot` | `{"device":{...},"net":{...},"runtime":{...},"io":{...}}`（字段同 §3，无信封） |
| GET | `/api/status` | **直连模式**：与 PC 网关同构的完整快照（见 §6.1），App 直连时打这个 |
| GET | `/api/info` | 同 `hello` 载荷 |
| GET | `/api/telemetry` | 同 `telemetry` 载荷 |
| GET | `/api/net` | 同 `net` 载荷 |
| GET | `/api/scan` | 同 `scan` 载荷（阻塞 1–3 s） |
| GET | `/api/cmd?cmd=<命令>&id=<id>` | 返回 `{"lines":[{ack 或 err 载荷}, ...]}`；`info` 这类命令会带多行 |
| POST | `/api/cmd` | 请求 `{"cmd":"gpio.set 2 1"}`，返回网关同构的 `{"ok":true,"ack":{...}}` |
| POST | `/api/connect` | 直连占位：返回 `{"ok":true,"source":"device"}`（App 点"连接"时不会报错） |
| GET | `/api/ports` | 直连占位：返回 `{"ports":[]}`（设备没有串口可枚举） |
| GET | `/` | 纯文本状态页（IP / MAC / 堆 / 直连地址 / 接口清单） |

`/api/cmd` 示例：

```bash
$ curl "http://192.168.1.66/api/cmd?cmd=gpio.set%202%201&id=13"
{"lines":[{"v":1,"t":"ack","ts":6100,"id":"13","cmd":"gpio.set","ok":true,"data":{"pin":2,"value":1,"mode":"out"}}]}
```

### 6.1 直连模式：设备直接当"小网关"

手机连上 ESP32 的热点（默认 `esp32-info-XXXX` / `12345678`，设备 IP `192.168.4.1`）后，
**不需要 PC**，App 把地址填 `http://192.168.4.1` 即可。设备侧 `/api/status` 返回与 PC 网关
`/api/status` **同构**的 snapshot：

```json
{"source":"device","connected":true,"port":"","host":"192.168.4.1",
 "device":{...},"net":{...},"runtime":{...},"io":{"gpio":{},"adc":{}},
 "scan":{"ts":31639,"n":6,"dur_ms":3018,"aps":[...]},
 "vitals":{...},
 "series":[{"t":27,"heap":246776,"temp":62.22,"rssi":0,"bpm":0}, ...],
 "last_seen_age":0,"stats":{"lines":28,"errors":0,"reconnects":0,"acks":0},
 "proto_mismatch":false,"ts":27}
```

与走网关的差别：

- `source` 固定为 `device`（App 用它切到"直连"档）
- `series` 是设备上的环形缓冲（60 点，约 1 分钟），不是网关的 120 点
- 没有 `raw` 原始流（设备不为每条日志留内存）
- `stats.acks` 恒为 0（设备不记录 ack 次数），`stats.errors` 是 vitals 坏帧数

> 设备侧**不跑 Web 前端**：Web/App 由 PC 网关统一提供，避免在 4 MB Flash 里塞前端资源，也避免设备 IP 变化导致前端要跟着改地址。

## 7. 上位机侧合并规则（`host/gateway`）

网关把上述行合并成一份 **snapshot**：

```json
{
  "source": "serial|wifi|mock",
  "connected": true,
  "device":  { ...hello 字段 },
  "net":     { ...net 字段 },
  "runtime": { ...telemetry 字段（去掉信封） },
  "io":      { "gpio": {...}, "adc": {...} },
  "scan":    { ...scan 字段 },
  "vitals":  { ...telemetry.vitals },
  "series":  [ {"ts":..., "heap":..., "temp":..., "rssi":..., "bpm":..., "spo2":...} ],
  "last_seen_ms": 1234,
  "stats": {"lines": 320, "errors": 0, "reconnects": 1}
}
```

- `series` 保留最近 **120** 个遥测点，供前端画趋势；`vitals.ok` 为真时同时记录 `bpm`/`spo2`
- `vitals` 从 `telemetry` 里**提到顶层**，Web/App 不必再往下钻
- 断线时 `connected=false`，但保留最后一次 snapshot（前端展示「最后已知值」并置灰）
- `source=mock` 时全部字段由内置模拟器生成，接口与真机**完全一致**，用于无硬件开发

## 8. STM32 → ESP32 生理数据帧

STM32 的 `USART1(PA9/PA10)` 每周期发一行（与 §4 的命令**共用同一条 115200 链路**，
但方向相反；ESP32 在 `UART2` 上收）：

```json
{"bpm":75,"spo2":98,"temp":36.5,"fall":0,"src":"stm32"}
```

| 键 | 类型 | 说明 |
|---|---|---|
| `bpm` | int | 心率，**必需**（缺这个键的帧直接丢弃，计入 `bad`） |
| `spo2` | int | 血氧；越界（>100）整帧丢弃 |
| `temp` | float | 体温 ℃，允许 `36` 或 `36.5` |
| `fall` | 0/1 | 跌倒标志 |
| `src` | string | 可省略；ESP32 以发送端标识（`uart`/`sim`）为准 |

固件侧容错：

- 帧必须以 `\n`（`\r\n` 亦可）结束，最长 190 字节，超长截断
- `bpm` 0–250、`spo2` 0–100 之外视为坏帧，**不覆盖上一帧有效值**
- 8 s 没新帧 → `vitals.ok=false`，上位机显示「已过期」而不是继续用旧值冒充实时
  （STM32 一帧要 5–6 s，阈值定 8 s 才不会周期性误报）

帧的生成位置：`firmware/MAX30102/USER/main.c` 与 `firmware/MAX30102/gcc/app/app_main.c`
（`vitals_uart_init()` / `vitals_emit()`，自包含实现，不依赖 printf 重定向）。

没有 STM32 时用 `vitals.sim` 命令或 `host/tools/stm32_sim.py` 注入同格式帧，
解析路径与真串口**完全相同**。

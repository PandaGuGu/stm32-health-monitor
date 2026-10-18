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
 "adc":{"34":{"raw":2048,"mv":1560},"35":{"raw":1023,"mv":780}}}
```

- `heap*` 单位字节；`frag` 为最大可分配块占空闲堆的百分比（碎片度，越小越好）。
  **实测基线**：ESP32 + WiFi 开启时 `heap ≈ 250 KB`、`heap_max ≈ 110 KB`，即 `frag ≈ 57%` 属正常，
  上位机的绿色阈值因此定为 ≤65%（早期按 ≤8% 判绿会把正常板子标成红色）
- `temp` 为**芯片内部温度**（`temperatureRead()`），不是环境温度，典型比环境高 5–15 ℃
- `rssi` 未联网时为 `0`
- `gpio` 只包含**监视集合**内的引脚（见 §5），值为 `0`/`1`
- `adc` 每个引脚是 `{"raw":0-4095,"mv":毫伏}`（`mv` 由 `analogReadMilliVolts()` 校准得出）；
  只包含默认采样引脚与调用过 `adc.get` 的引脚

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
| GET | `/api/info` | 同 `hello` 载荷 |
| GET | `/api/telemetry` | 同 `telemetry` 载荷 |
| GET | `/api/scan` | 同 `scan` 载荷（阻塞 1.5–3 s） |
| GET | `/api/cmd?cmd=<命令>&id=<id>` | 返回 `{"lines":[{ack 或 err 载荷}, ...]}`；`info` 这类命令会带多行 |
| GET | `/` | 纯文本状态页（无 Web 资源，便于 `curl` 快速确认） |

`/api/cmd` 示例：

```bash
$ curl "http://192.168.1.66/api/cmd?cmd=gpio.set%202%201&id=13"
{"lines":[{"v":1,"t":"ack","ts":6100,"id":"13","cmd":"gpio.set","ok":true,"data":{"pin":2,"value":1,"mode":"out"}}]}
```

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
  "series":  [ {"ts":..., "heap":..., "temp":..., "rssi":...} ],
  "last_seen_ms": 1234,
  "stats": {"lines": 320, "errors": 0, "reconnects": 1}
}
```

- `series` 保留最近 **120** 个遥测点，供前端画趋势
- 断线时 `connected=false`，但保留最后一次 snapshot（前端展示「最后已知值」并置灰）
- `source=mock` 时全部字段由内置模拟器生成，接口与真机**完全一致**，用于无硬件开发

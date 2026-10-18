# ESP32 信息节点 · 搭建与调试记录

> 记录日期：2026-10-18　环境：Windows 10 · 真机 **ESP32-D0WD-V3**（COM3 / CH340，4 MB Flash）
> 结论先行：**固件零告警编译通过；真机串口链路与全部 21 条命令实测通过；Web + Android 双端都读到真机数据；
> 手机直连热点（AP，密码 12345678）全通；STM32 生理数据链路的协议/解析/展示已打通，只差一根线。**
> 下表是本次踩到的坑，正文给证据。

## 故障速查表

| # | 现象 | 根因 | 修复 |
|---|---|---|---|
| 1 | `pip install` 报 "from versions: none"，但 `curl` 访问同一镜像 200 | pip 的网络出口被环境拦截，并非镜像问题 | curl 手动下 wheel → `pip install --no-index` |
| 2 | `curl github.com:443` 21 s 超时，`api.github.com` 却 200 | GitHub 直连被限，Release 下载尤其慢（27 KB/s） | 走 `ghfast.top` 镜像 |
| 3 | `arduino-cli core install esp32:esp32` 1 分钟必失败 | 单文件 394 MB，60 s 内下不完 | 换 2.0.17（243 MB）+ 本地 HTTP 提供安装包 |
| 4 | 本地索引生效后仍去 GitHub 取 `...-win64.zip` | 官方索引缓存在 `%LOCALAPPDATA%\Arduino15` 里仍被优先读取 | 删缓存 + 裁剪索引 + 最后手工解压 + 写 `installed.json` |
| 5 | Gradle 构建直接 FAILED | 仓库路径含中文，AGP 硬性拒绝 | `android.overridePathCheck=true` |
| 6 | `adb devices` 里 `emulator-5554 offline` | MuMu 的 adb 端口不是 5554 | `adb connect 127.0.0.1:16384` |
| 7 | `input tap` 点了没反应 | 截图 1080×593、物理 1600×900，坐标不是 1:1 | 按物理坐标点，或 `uiautomator dump` 取 `bounds` |
| 8 | Web 点「扫描」后表格一直"尚未扫描" | 网关合并 snapshot 时把 `scan` 的 `ts` 过滤掉了，前端靠 `ts` 判断新扫描 | 保留 `ts` |
| 9 | App 里迷你曲线不显示 | `sparks` 容器只创建没 `addView` | 补 `body.addView(sparks)` |
| 10 | 固件编译报 `modeName defined but not used` | 重构后遗留的死函数 | 删除，复验零告警 |
| 11 | 真机上「碎片度 57%」标红 | 阈值按 ≤8% 判绿拍的，而 ESP32+WiFi 的常态就是 ~57% | 阈值改 ≤65% 绿 / ≤80% 黄，协议里写清基线 |
| 12 | `--port COM3` 报 `invalid int value` | `--port` 是 HTTP 端口，串口要用 `--serial` | 帮助文案点明 + 加 `--http-port` 别名 |
| 13 | App 连的是串口，高亮却在「模拟器」 | 首次没跟网关实际数据源对齐 | 首次拿到 snapshot 时同步选中态 |

## 1. 工具链：从"装不上"到"装上了"

### 1.1 pip 装不了包

```
$ python -m pip install -i https://pypi.tuna.tsinghua.edu.cn/simple pyserial -v
Looking in indexes: https://pypi.tuna.tsinghua.edu.cn/simple
ERROR: Could not find a version that satisfies the requirement pyserial (from versions: none)

$ curl -sS -o /dev/null -w "%{http_code} %{time_total}\n" https://pypi.tuna.tsinghua.edu.cn/simple/fastapi/
200 0.921045
```

同一台机器、同一个镜像，`curl` 通、`pip` 不通 —— 说明不是镜像的问题，是 pip 这个进程的出网被挡了。

**处置**：`curl` 把 wheel 拉下来，再让 pip 从本地装（零网络）：

```
$ curl -sSL -o pyserial-3.1.1-py2.py3-none-any.whl \
    https://pypi.tuna.tsinghua.edu.cn/packages/49/db/.../pyserial-3.1.1-py2.py3-none-any.whl
$ python -m pip install --no-index --no-deps pyserial-3.1.1-py2.py3-none-any.whl
Successfully installed pyserial-3.1.1
```

> 踩坑细节：wheel 文件必须叫 `pyserial-3.1.1-py2.py3-none-any.whl`，改名成 `pyserial.whl` 后
> pip 直接报 `Invalid wheel filename (wrong number of parts)` —— 它靠文件名解析包名和版本。

**连带的架构决定**：既然装包这么费劲，网关就**不引 FastAPI/uvicorn**，改用标准库
`http.server` + SSE；只保留 `pyserial` 这一个纯 Python 小依赖（200 KB）。见 `host/gateway/server.py`。

### 1.2 ESP32 core：1 GB 的下载怎么落地

GitHub 直连 27 KB/s，`core install esp32:esp32`（3.3.x）光工具链就 394 MB，而且 arduino-cli
下载器 60 s 就 `context deadline exceeded`。三条路依次试：

1. **镜像直连**：`ghfast.top` 对 6 MB 分片测速 566 KB/s，可用 → 但 1 GB 仍要半小时以上
2. **降版本**：3.3.x 把 esp32/esp32s3/c3/c6/h2/p4/s2 的预编译库**全部**列为依赖（合计 3–4 GB）；
   2.0.17 只要 243 MB。ESP32-WROOM-32 用 2.0.17 完全够 → 选定
3. **本地 HTTP 喂安装包**：curl 按清单把 11 个包下到 `cache/`，改写索引指向
   `http://127.0.0.1:9011/<file>`，arduino-cli 从本机取包，校验和/解压/写 `installed.json` 仍由它做

第 3 步仍失败：arduino-cli 坚持去 GitHub 取 `xtensa-esp32-elf-gcc...-win64.zip`（我本地只有 win32 变体），
即使删掉 `%LOCALAPPDATA%\Arduino15` 里所有 `package_*.json` 也一样。

**最终方案：手工落地 + 自己写 `installed.json`**

```
Arduino15/packages/esp32/
├─ hardware/esp32/2.0.17/           ← esp32-2.0.17.zip 解压（剥顶层目录）
├─ tools/<tool>/<version>/          ← 10 个工具包各自解压
└─ installed.json                   ← 由索引裁剪出 platforms + tools 列表
```

```
$ arduino-cli core list
ID          Installed Latest Name
esp32:esp32 2.0.17    3.3.12 esp32

$ arduino-cli board listall | grep "ESP32 Dev Module"
ESP32 Dev Module    esp32:esp32:esp32
```

### 1.3 编译结果

```
$ arduino-cli compile --fqbn esp32:esp32:esp32 --build-path firmware/esp32/build \
    --warnings all firmware/esp32/esp32_info_node
Sketch uses 807941 bytes (61%) of program storage space. Maximum is 1310720 bytes.
Global variables use 45568 bytes (13%) of dynamic memory, leaving 282112 bytes for local variables.
```

首次编译有一条 `io_probe.cpp:25:13: warning: 'modeName' defined but not used`，删掉这个重构遗留的死函数后
**0 error / 0 warning**（与仓库既有标准一致）。

> 顺带一个环境噪音：`gen_esp32part.py:517: UnicodeWarning` —— 中文路径下 Python 2 脚本的字符串比较告警，
> 不影响产物（`esp32_info_node.ino.bin` 814 512 B 正常生成）。

## 2. 上位机联调中修掉的问题

### 2.1 Web 端点了「扫描」没反应

现象：点「扫描」→ 网关返回 `ack {"n":6}`，原始流里也有 `{"t":"scan",...}`，但表格一直是"尚未扫描"。

根因在 `protocol.py` 的合并逻辑：

```python
elif t == T_SCAN:
    self.scan = {k: v for k, v in obj.items() if k not in ("v", "t", "ts")}   # ← ts 被丢了
```

前端靠 `scan.ts` 变化判断"这是新的一次扫描"（`if (scan.ts !== state.lastScanTs)`），`ts` 没了就永远不渲染。
改成保留 `ts` 后 6 个 AP 正常出现。

> 这类"端到端能通、界面不动"的问题，光看某一层都发现不了；所以网关保留 `raw` 环形缓冲，
> Web/App 的「原始数据流」区直接把收到的每一行打出来，一眼就能定位是设备没发、还是前端没渲染。

### 2.2 点击位置对不上（MuMu）

- `adb devices` 里只有 `emulator-5554 offline`；MuMu 的实际 adb 端口是 **16384**（7555/5555 也接受）
- 截图是 `1080×593`，而 `wm size` 是 `1600×900`、`wm density 240`、系统处于 `ROTATION_90`（横屏平板形态）
  → `input tap` 用的是**物理坐标**，比例约 x×1.481 / y×1.518

实测：`input tap 1098 553`（GPIO 4 格中心）→ 网关口 `gpio.4=1`、`acks 3→4`、App 里该格变绿 `HIGH`。
先用截图坐标点（`740 342`）时没有任何反应，就是因为差了这 1.48 倍。

定位手段优先用 `uiautomator dump`：

```
$ adb -s 127.0.0.1:16384 shell uiautomator dump /sdcard/ui.xml
$ adb -s 127.0.0.1:16384 exec-out cat /sdcard/ui.xml
text="GPIO 4" ... bounds="[1022,530][1174,550]"
```

### 2.3 App 端几个显示问题

| 现象 | 原因 | 修复 |
|---|---|---|
| 迷你曲线整块空白 | `buildRuntimeCard()` 里建了 `sparks` 容器但漏了 `body.addView(sparks)` | 补一行 |
| 标题被状态栏压住 | 内容区没吃 window insets | `scroll.setFitsSystemWindows(true)` |
| 选「模拟器」时仍显示"设备地址（WiFi 源）"标签 | 只隐藏了 `EditText`，标签是独立节点 | 包成 `wifiGroup` 整体显隐 |
| 引脚最后一行被拉宽 | 23 个格子在 4 列布局里末行只有 3 个，权重平摊 | 末行补不可见占位，保持等宽 |
| 进页面就有个"水滴"手柄 | 首个 `EditText` 自动获焦 | 让 `ScrollView` 拿焦点，`clearFocus()` |

### 2.4 Android 构建

```
> Failed to apply plugin 'com.android.internal.application'.
  > Your project path contains non-ASCII characters. This will most likely cause the build to fail on Windows.
```

仓库在 `C:\Users\Administrator\Desktop\小组嵌入式`，AGP 8.x 见到非 ASCII 路径直接拒绝。加一行放行：

```properties
android.overridePathCheck=true
```

之后 Gradle 8.14.3 + AGP 8.7.3 + JDK 17 构建 `BUILD SUCCESSFUL`，APK 30 KB（无第三方依赖，含 dex 就这点）。

## 3. 真机接入与命令回归（2026-10-18 19:30）

换了一根数据线后 `COM3` 上线（`USB-SERIAL CH340`，`VID:PID=1A86:7523`）。

### 3.1 板子不是空的

上电先抓串口，发现里面跑的是**别人的飞控固件**：

```
ets Jul 29 2019 12:21:46
rst:0x1 (POWERON_RESET),boot:0x13 (SPI_FAST_FLASH_BOOT)
程序初始化！
Setup Motors
  motor0 pin=12 ledcAttach=OK
  motor1 pin=13 ledcAttach=OK ...
Setup Wi-Fi
✓ Web RC 已启动: http://192.168.4.1
Setup IMU
⚠ IMU初始化失败！请检查 IMU 硬件连接！
输入命令，然后回车:   (help / p / mfr / ca ...)
```

结论：这是一块**回收/复用**的 ESP32，里面是四轴飞控（电机 12/13/15/14、Web RC、参数控制台）。
烧我们的固件会覆盖它，所以先做全片备份：

```
$ esptool -p COM3 -b 460800 read_flash 0 0x400000 dist/esp32-backup/original-fw-20500d33dad8.bin
Read 4194304 bytes at 0x00000000 in 99.4 seconds (337.7 kbit/s)...
```

> 备份文件在 `dist/esp32-backup/`（`dist/` 已 gitignore）。要恢复飞控：
> `esptool -p COM3 -b 460800 write_flash 0x0 original-fw-20500d33dad8.bin`

### 3.2 芯片实测

```
Chip is ESP32-D0WD-V3 (revision v3.1)
Features: WiFi, BT, Dual Core, 240MHz, VRef calibration in efuse
Crystal is 40MHz
MAC: 20:50:0d:33:da:d8
Detected flash size: 4MB
```

与固件里 `Device::begin()` 的自报完全一致（`model/rev/cores/freq/flash_kb/mac` 全对），
说明 `esp_chip_info` + `esp_read_mac` 这条路没问题。

### 3.3 烧录与首帧

```
$ arduino-cli upload -p COM3 --fqbn esp32:esp32:esp32 --input-dir firmware/esp32/build firmware/esp32/esp32_info_node
Wrote 814512 bytes (519560 compressed) at 0x00010000 in 8.1 seconds (effective 805.8 kbit/s)...
Hash of data verified.
```

上电后 500 ms 内出现首帧（`ts:500`），顺序与设计一致：

```json
{"v":1,"t":"net","ts":500,"mode":"ap","state":"ap_listening","ssid":"esp32-info-DAD8","ip":"192.168.4.1","mac":"20:50:0D:33:DA:D8"}
{"v":1,"t":"log","ts":513,"lvl":"I","msg":"no credentials, fallback AP: esp32-info-DAD8"}
{"v":1,"t":"log","ts":525,"lvl":"I","msg":"http api on :80"}
{"v":1,"t":"hello","ts":535,"fw":"0.1.0","build":"Oct 18 2026 19:02:25","model":"ESP32-D0WD-V3","rev":3,"cores":2,"freq":240,"flash_kb":4096,"sdk":"v4.4.7-dirty","mac":"20:50:0D:33:DA:D8","reset":"poweron"}
{"v":1,"t":"telemetry","ts":578,"up":0,"heap":252600,"heap_min":252132,"heap_max":110580,"frag":57,"cpu":240,"tasks":10,"temp":56.67,"rssi":0,"gpio":{...},"adc":{"34":{"raw":0,"mv":142},"35":{"raw":0,"mv":142}}}
```

### 3.4 命令回归（全部真机）

| 命令 | 返回值 | 结论 |
|---|---|---|
| `help` | 21 条命令 + `fw` | ✅ |
| `ping` | `{"pong":57701}` | ✅ 下行 → 上行往返正常 |
| `temp` | `{"temp":59.44}` | ✅ |
| `gpio.set 2 1` → `gpio.get 2` | `{"pin":2,"value":1,"mode":"out"}` → `{"pin":2,"value":1}` | ✅ 板载 LED 亮，状态可回读 |
| `adc.get 32` | `{"raw":442,"mv":456}` | ✅ 悬空脚读数（非 0，正常） |
| `adc.get 34` | `{"raw":0,"mv":142}` | ✅ 噪声底 |
| `interval 500` | 3 秒内新增 6 行 | ✅ 周期可调（≈2 Hz） |
| `scan` | `{"n":6,"dur_ms":6835}`，扫到 `FENG(-33,WPA3)` `TP-LINK_81F7(OPEN)` 等 | ✅ 真 AP 列表 |
| `reboot` | `{"restarting":true}` → 6 s 后 `reset:"sw"`、`up:4`、`reconnects:0` | ✅ 串口未掉，网关无感 |

`scan` 在 **AP 模式**下耗时 6.8 s（固件要先切 `AP_STA` 再扫全信道），期间遥测停更约 7 秒——
STA 模式下通常 1.5–2 s。真机数据见 Web 面板截图。

### 3.5 真机才暴露的三个问题

| 现象 | 根因 | 修复 |
|---|---|---|
| 「碎片度 57%」被标成红色 | 阈值按"越小越好、≤8% 绿"拍脑袋定的；而 ESP32 开 WiFi 后 `heap_max` 只有 `heap` 的 ~44%（110 KB / 250 KB），57% 是**常态** | 阈值改为 ≤65% 绿 / ≤80% 黄；`protocol.md` 写清基线 |
| `--port COM3` 被当成 HTTP 端口 | `--port` 是监听端口，串口是 `--serial`，名字撞了 | 帮助文案点明，并加 `--http-port` 别名 |
| App 明明连的是串口，高亮却在「模拟器」 | App 的源选择来自本地偏好，首次没有跟网关实际源对齐 | 首次拿到 snapshot 时同步选中态 |

另外两个属于"真机就是这样、不是 bug"：

- **GPIO 5 / 14 / 15 上电读高**：这几个是 strapping 脚，复位后默认上拉；固件不主动拉低它们是对的（拉低会改启动模式）
- **芯片温度 60–62 ℃**：AP 常开 + CPU 240 MHz 的常态，比环境高 15–20 ℃；`wifi.off` 后会掉几度

## 4. STM32 数据链路的预打通（ESP32 单板阶段）

板子只有 ESP32 一块，但 STM32→ESP32 的格式、解析、上位机展示可以先全部做完 ——
因为这条链路里只有"电平/波特率"必须见到真板子，其余都能在注入帧上验。

### 4.1 STM32 侧：第一次编译就撞上 `L6218E`

直接按 `docs/architecture.md §5` 的思路 `#include "usart.h"` + `uart_init(115200)` + `printf`，结果：

```
linking...
..\OBJ\IIC.axf: Error: L6218E: Undefined symbol uart_init (referred from main.o).
"..\OBJ\IIC.axf" - 1 Error(s), 0 Warning(s).
```

`grep` 才发现 Keil 工程里根本没有 `SYSTEM/usart/usart.c`（工程里那个是 SPL 的 `stm32f10x_usart.c`），
所以既没有 `uart_init()`，也没有 `fputc` 重定向 —— 就算链接过了，`printf` 在 microlib 下也会走半主机调用挂死。

**改成自包含实现**（`main.c` / `gcc/app/app_main.c` 各一份，都是静态函数）：

```c
static void vitals_uart_init(void);                       /* PA9=TX, PA10=RX, 115200 */
static void vitals_send(const char *s);                   /* 轮询 TXE/TC 发字节 */
static char *vitals_u2s(char *p, unsigned v);             /* 无 printf 的整数转字符串 */
static void vitals_emit(unsigned bpm, unsigned spo2, unsigned temp_raw, unsigned fall);
```

改完 Keil：

```
compiling stm32f10x_tim.c...
linking...
Program Size: Code=10492 RO-data=3428 RW-data=80 ZI-data=12640
"..\OBJ\IIC.axf" - 0 Error(s), 0 Warning(s).
```

### 4.2 ESP32 侧：一个解析器，两条来源

`uart_link.cpp` 里 `Vitals::feedLine()` 是唯一的解析入口：

- 真串口：`Serial2`（GPIO16 RX / GPIO17 TX）按 `\n` 切行后调它
- 注入：`vitals.sim` 命令拼出同样格式的字符串后也调它

所以"注入能通"就等于"解析这部分逻辑能通"，接上 STM32 后只剩电平和波特率的问题。

容错做了三层：必需字段 `bpm` 缺失 → 丢帧计 `bad`；`bpm` 0–250 / `spo2` 0–100 越界 → 丢帧；
8 s 无新帧 → `ok=false`（上位机显示"已过期"，不会拿旧值冒充实时）。阈值取 8 s 而不是 5 s，
因为 STM32 主循环一帧就要 5–6 s（500 点采样 + DS18B20 转换），5 s 会周期性误报。

### 4.3 验证（真机 ESP32 + 注入帧）

```
→ vitals.sim 78 97 36.5 0
← {"ok":true,"id":101,"ack":{"t":"ack","cmd":"vitals.sim","data":{"vitals":
   {"ok":true,"bpm":78,"spo2":97,"temp":36.5,"fall":0,"src":"sim",
    "age_ms":0,"frames":1,"bad":0,"uart":true}}}}
```

连上 `host/tools/stm32_sim.py`（1 Hz 随机游走）后，Web/App 的「生理数据 · STM32」卡片实时更新：

| 场景 | 卡片表现 |
|---|---|
| 正常（75–80 BPM / 97–99%） | 心率、血氧绿色；跌倒"否"；标签「模拟帧」黄色；心率曲线平滑 |
| 注入 `118 88 37.9 1` | 心率 118 → 黄、血氧 88 → 红、跌倒"是" → 红；标签仍标「模拟帧」 |
| 停掉模拟器 5 s 后 | 标签变「已过期」，数值置灰（不沿用旧值） |

`vitals.sim` 与 `vitals.status`、`vitals.echo`（短接 GPIO17→16 可回环自检）都进 `help` 列表。

### 4.4 还没验的

- **真线**：STM32 侧 `USART1` 到 ESP32 `GPIO16` 的实际电平、共地、长线干扰
- **时序**：STM32 每 ~5 s 一帧（受 MAX30102 采样节流），ESP32 侧 5 s 陈阈值刚好卡在边缘，
  接线后要确认 `age_ms` 是否长期贴着 5000 —— 如果经常"过期闪一下"，把 `VITALS_STALE_MS` 调到 8000

## 5. 手机直连热点（不经电脑）

需求：手机不接电脑也要看到板子的实时数据。做法是让**设备自己当小网关**：

- 设备侧新增 `/api/status`（与 PC 网关同构的 snapshot，`source=device`）与 `POST /api/cmd`
- App 增加「直连」档，地址填 `http://192.168.4.1`
- 热点 `esp32-info-DAD8` / **`12345678`** / 信道 6

### 5.1 验收（PC 连热点 → MuMu 当手机）

```
$ netsh wlan connect name="esp32-info-DAD8"        # PC 拿到 192.168.4.2
$ curl http://192.168.4.1/api/status
source: device | connected: True | host: 192.168.4.1
device: {'model': 'ESP32-D0WD-V3', 'mac': '20:50:0D:33:DA:D8', 'reset': 'poweron'}
io.gpio 个数: 23 | series 点数: 28 | stats: {'lines': 28, ...}

$ curl -X POST http://192.168.4.1/api/cmd -d '{"cmd":"vitals.sim 82 96 36.6 0"}'
{"ok":true,"id":9,"ack":{"t":"ack","data":{"vitals":{"ok":true,"bpm":82,"spo2":96,...}}}}
```

MuMu 里 `adb shell ping 192.168.4.1` 3.95 ms 通（NAT 走宿主），App 直连后：

- 卡片读到真身：`ESP32-D0WD-V3 / 4 MB / 20:50:0D:33:DA:D8`，`直连设备` 标签
- 注入帧后生理数据卡实显 `81 BPM / 95% / 36.4 ℃ / 0 s 前`
- 点 GPIO 4 → 设备侧 `gpio.4=1`（反向控制走 WiFi）
- 点「扫描」→ 8 个 AP（`FENG -35 WPA3`、`TP-LINK_81F7 OPEN`、两个隐藏 …）

### 5.2 踩坑：一扫描就把手机踢下线

第一版能在直连模式取数，但点「扫描」后 PC 立刻掉线：

```
$ curl -X POST http://192.168.4.1/api/cmd -d '{"cmd":"scan"}'
curl: (56) Recv failure: Connection was reset
$ curl http://192.168.4.1/api/status
curl: (28) Connection timed out after 8002 milliseconds
```

根因在固件的扫描实现：**扫描只能在 station 接口上做**，而第一版热点是用 `WiFi.mode(WIFI_AP)` 起的，
扫描前要临时切 `WIFI_AP_STA`、扫完再切回 `WIFI_AP` —— 这两次模式切换会把已连的客户端全部踢掉。

修法：**开机就用 `AP_STA`**（STA 不连路由器也没关系），扫描不动模式：

```c
WiFi.mode(WIFI_AP_STA);                                  // 不是 WIFI_AP
WiFi.softAP(ssid, pass, AP_CHANNEL, 0, 4);
```

改完两个收益：

| | 改前（纯 AP） | 改后（AP_STA） |
|---|---|---|
| 扫描耗时 | 6.8 s（含两次模式切换） | **3.0 s** |
| 扫描期间客户端 | 被踢下线 | 保持连接（`clients:1`） |
| 直连模式扫描 | 不可用 | 可用，结果进 `/api/status` 的 `scan` |

顺带补了两处：`scan` 结果缓存在设备上（否则 `/api/status` 里 `scan` 永远是空，App 直连看不到列表），
以及 AP_STA 但 STA 未连时仍对外报 `mode=ap / state=ap_listening`，避免上位机把"热点模式"显示成"连接中"。

### 5.3 手机侧的注意点

- Android 连上热点会提示"无互联网"，选**保持连接**；部分机型开了"智能网络切换"会把请求切回移动数据，
  表现为 App 一直转圈 —— 关掉该选项或临时关移动数据
- 直连模式没有原始流（设备不为日志留内存），App 的"原始数据流"区会提示改用 PC 网关
- 设备最多 4 个客户端（`softAP(..., 4)`）

## 6. 当前状态与待办

**已验证（真机）**

- 串口链路：`hello` 首帧 500 ms、1 Hz 遥测连续、21 条命令全通、`reboot` 后 `reset=sw` 且网关无感
- Web/App 双端：都从真机读到型号/MAC/堆/温度/10 个任务、真 AP 列表、真 ack
- 固件编译：0 error / 0 warning，Flash 62%（825 457 B，含直连模式的 60 点序列缓冲）
- **WiFi 直连（AP）**：PC/MuMu 连热点 → `/api/status` 同构快照、`POST /api/cmd` 生效、
  点引脚设备侧 `gpio.4=1`、扫描 8 个 AP（3.0 s 且不掉线）

**未验证**

- **WiFi STA**：`wifi.sta <ssid> <pass>` 配网到路由器、网关切 `--source wifi` 这条路径还没试
- 热插拔恢复：串口拔掉 → 网关 3 s 重连这一段没做真机拔插验证
- 长时间稳定性：目前只跑了十几分钟，`heap_min` 还在 242 KB，未做 24 h 观察

下一步建议（按性价比排）：

1. 接 STM32 `USART1`（PA9→GPIO16、共地），确认 `vitals.src` 变成 `stm32`、`age_ms` 稳定在 5–6 s
2. `wifi.sta <你的WiFi> <密码>` 配网 → 网关切 `--source wifi --wifi <设备IP>`
3. 拔插 USB 一次，确认网关 `reconnects` 涨 1 且自动恢复


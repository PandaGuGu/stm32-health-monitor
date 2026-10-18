# 更新日志

格式参考 [Keep a Changelog](https://keepachangelog.com/zh-CN/1.1.0/)，版本号遵循
[语义化版本](https://semver.org/lang/zh-CN/)。

## [v0.2.0] - 2026-10-18

新增 **ESP32 信息节点** 与配套 **双端上位机**。ESP32 先不接 STM32，单独采自己的芯片/网络/资源/IO，
把"协议 + 上位机"这条链路先跑通；随后把 **STM32 → ESP32 的生理数据链路**（协议、解析、双端展示）也提前打通，
接上真板子即用。

### 新增

- **ESP32 固件** `firmware/esp32/esp32_info_node/`（Arduino 框架，ESP32-WROOM-32，零第三方库）
  - 四类信息：芯片与固件（型号/rev/Flash/SDK/MAC/复位原因）、网络（模式/IP/RSSI/AP 扫描）、
    运行时（堆/历史最低/碎片度/任务数/CPU/芯片温度）、板载 IO（23 路 GPIO + ADC1）
  - 上行 **JSON Lines**（`hello`/`telemetry`/`net`/`scan`/`ack`/`err`/`log`），下行 **文本命令**（21 条）
  - 串口 115200 与 WiFi 自带 HTTP `/api/*` 两条通路；STA 凭据存 NVS，无凭据自动起 AP 兜底
  - 编译：`820005 B`（62% Flash）/ `45632 B` RAM，**0 error 0 warning**
- **STM32 → ESP32 生理数据链路** `uart_link.*` + 两侧固件
  - STM32 `USART1(PA9/PA10)` 每周期发 `{"bpm":75,"spo2":98,"temp":36.5,"fall":0}`，
    ESP32 `UART2(GPIO16/17)` 收行、极简 JSON 解析、范围校验，并入 `telemetry.vitals`
  - `vitals.status` / `vitals.sim` / `vitals.uart` / `vitals.echo` 四条命令；坏帧不覆盖旧值，8 s 无帧置 `ok=false`
  - STM32 侧自包含实现（`vitals_uart_init` / `vitals_send` / `vitals_u2s`），不依赖 printf 重定向与 `SYSTEM/usart`
- **STM32 替身** `host/tools/stm32_sim.py`：无 STM32 时喂帧，支持网关注入与真串口两种模式，含跌倒/低血氧场景
- **设备自包含服务器（直连模式）**：ESP32 自己提供与 PC 网关**同构**的 `/api/status`（含 60 点序列、
  扫描缓存 + `ts`）与 `POST /api/cmd`，手机连热点后**不需要电脑**也能看数据、点引脚、跑扫描；
  代价仅 ~4 KB Flash（825 457 B，62%）
- **App「直连」档**：数据源多一档，地址填 `http://192.168.4.1`；支持
  `adb shell am start ... -e url http://192.168.4.1` 直接拉起
- **上位机网关** `host/gateway/`（Python，标准库 + pyserial）
  - 三种数据源可切：USB 串口 / WiFi 轮询 / **内置模拟器**（无硬件时与真机同构）
  - REST + **SSE** 推送；串口 3 s 自动重连；120 点遥测序列；原始流环形缓冲
- **Web 面板** `host/web/`（无构建单页）
  - 数据源切换、四类信息卡片 + **生理数据卡片**、四条 Canvas 曲线、WiFi 扫描表、命令台（带 ack 回执）、原始 JSON 流、导出快照
- **Android App** `host/app/`（Java 原生，零第三方依赖，APK 30 KB）
  - 1 Hz 轮询网关；`sw600dp` 以上自动双列；引脚可点、命令台、扫描、生理数据卡片；已在 MuMu 实测
- **文档**：`docs/esp32/`（架构取舍、协议 v1、搭建与调试记录 + 三张实测截图）
- **脚本**：`scripts/build-esp32.ps1`（编译 / `-Upload -Port COMx` 烧录）

### 修复

- 网关合并 snapshot 时丢掉了 `scan.ts`，导致前端无法判断"新的一次扫描"→ 保留该字段
- Android：迷你曲线容器漏 `addView`、状态栏遮挡标题、WiFi 输入框标签未随输入框隐藏、
  引脚末行宽度不齐、输入框自动获焦留下手柄 —— 均已修正
- **STM32 侧链接失败** `L6218E: Undefined symbol uart_init`：Keil 工程未包含 `SYSTEM/usart/usart.c`，
  改用自包含的 USART1 初始化；同时避开 microlib `printf`（无 `fputc` 重定向会走半主机挂死）
- **扫描把客户端踢下线**：每次扫描临时切 `AP_STA` 再切回，已连的手机/PC 会被断网一次
  → 改成**开机即 AP_STA**（不来回切模式），实测扫描 3.0 s 且客户端不掉线
- **App 直连时看不到 AP 列表**：设备侧不缓存扫描结果 → 扫描结果进 `/api/status` 并补 `ts` 字段
  （App 靠它判断"这是新的一次扫描"）
- **vitals 陈旧阈值 5 s → 8 s**：STM32 一帧要 5–6 s（500 点采样 + DS18B20 转换），5 s 会周期性误报"已过期"
- **真机复验后**：`frag`（碎片度）阈值按 ≤8% 判绿是错的（ESP32+WiFi 常态 ~57%）→ 改 65/80；
  `--port` 与串口号混淆 → 加 `--http-port` 别名并在帮助里点明；App 数据源高亮不与网关同步 → 首次对齐

### 真机验证（ESP32-D0WD-V3，COM3，4 MB Flash）

- 烧录前整片备份 4 194 304 B → `dist/esp32-backup/`（该板原为四轴飞控固件）
- 烧录 814 512 B / 8.1 s，Hash verified；上电 500 ms 内出 `net`+`hello`+`telemetry`
- 21 条命令真机全通：`gpio.set/get` 回读、`adc.get 32`=442/456 mV、`interval 500` 实测 2 Hz、
  `scan` 扫到 6 个真 AP（含 WPA3/OPEN/隐藏）、`reboot` 后 `reset=sw` 且网关 `reconnects=0`
- Web 与 App 均读到同一份真机数据（MAC `20:50:0D:33:DA:D8`）
- **vitals 链路**：`vitals.sim` 注入 → telemetry → 网关顶层 `vitals` → Web/App 卡片（心率/血氧/体温/跌倒/帧龄/来源）；
  注入 `118 88 37.9 1` 时三处告警色正确；STM32 两侧固件编译通过（Keil `0 Error 0 Warning`，`Code=10492`）
- **WiFi 直连（AP）**：MuMu（经宿主 NAT）与 PC 先后连 `esp32-info-DAD8` → App 直连 `192.168.4.1`，
  读到真 MAC/SDK、`vitals` 实时；App 点引脚 → 设备 `gpio.4=1`（WiFi 反向控制通）；扫描 8 个 AP / 3.0 s / 客户端不掉线
- **STM32 侧改动**：`USER/main.c` 与 `gcc/app/app_main.c` 各加一段自包含 USART1 上行，实物 hex 重新生成

### 已知限制

- **WiFi STA 未验**：AP 直连已实测；`wifi.sta` 连路由器的通路代码就绪但没配过网
- **STM32 真线未接**：`vitals` 的协议/解析/展示已用注入帧验证，PA9→GPIO16 的实际电平与时序待接线复验
- **WiFi 链路未实测**：设备侧 `/api/*` 已实现、AP 模式日志 `http api on :80`，但还没从 PC 连 AP 打过接口
- 热插拔恢复（串口拔掉 → 3 s 重连）未做真机拔插验证
- AP 模式下 `scan` 需临时切 `AP_STA`，耗时 6.8 s（STA 模式 1.5–2 s），期间遥测停更
- 芯片温度取自 `temperatureRead()`，比环境高 15–20 ℃，只能看趋势
- ADC2 与 WiFi 冲突，固件只开放 ADC1（32/33/34/35/36/39）
- 网关为明文 HTTP 且无鉴权，仅限内网调试

## [v0.0.1] - 2026-10-17

首个可交付版本：**实物四路监测跑通 + Proteus 仿真可完整演示**。

### 新增

- **实物固件** `firmware/MAX30102/`（正点原子标准库 Keil 工程 `IIC.uvprojx`）
  - MAX30102 心率 / 血氧（软件 I2C，Maxim 官方算法，500 点缓冲）
  - DS18B20 体温（单总线，PA8，带外部 4.7 kΩ 上拉）
  - MPU6050 跌倒判定（合矢量 |a| 去重力偏差，阈值 3072 LSB ≈ 2.5 g，连续 2 拍确认）
  - 0.96" OLED 显示（SSD1306，软 I2C，中文字库）+ 有源蜂鸣器报警（PA12 低电平）
- **GCC 构建系统** `firmware/MAX30102/gcc/`：IIC / APP / LIVE / DIAG 四种变体，可脱离 Keil 授权编译
- **仿真工程** `sim/proteus/SIT_sim.pdsprj` + **仿真专用固件** `firmware/MAX30102_sim/`
  - 温度走高精度电位器 RV2 → PA0(ADC1_IN0)，0~3.3 V 映射 15.0~45.0 ℃，一位小数显示
  - 心率内置节律 66~90 BPM；跌倒由 SW3(PB1) 触发；蜂鸣器跟随报警
  - 上电自动扫描 48 组单总线时序，能读到真传感器就自动切换数据源
  - 屏幕标记数据来源：`POT` / `SIM` / `DS18`
- **Wokwi 网页方案** `sim/wokwi/`（diagram.json + sketch.ino）
- **文档**：架构说明、系统原理图（SVG + 引脚表）、`docs/debug/` 10 篇调试报告与故障速查表

### 修复

- `ds18b20.c:68` 无超时死等导致 OLED 全黑、程序卡在初始化 → 改用带超时的实现
- 诊断固件的 `pin_survey_port()` 把 PA5/PA6 重配为输入，导致 OLED 永久空写 → 每帧恢复开漏输出
- 跌倒判定频率 0.52 Hz（漏报）→ 87.7 Hz；判据改为倾斜免疫的 |a| 偏差
- 手指移开后仍显示旧心率/血氧 → 算法层 30–220 bpm 门控 + 5 项信号质量门控
- 数字位数变化残留、图标叠字 → 绘制前 `OLED_ClearArea`

### 已知限制

- 仿真工程无 MAX30102 / MPU6050 模型；DS18B20 实际加载 `DS1822.MDF`，单总线命令写不进
  （48 组时序全部失败），因此温度改用 RV2 电位器模拟
- Proteus 的 MCU 模型 `CLOCK_SCALE = 8`（≈64 MHz），与固件假设的 72 MHz 相差 12.5%，
  任何"靠时间算"的量都会偏；`Clock Scale` 改 `9 Times` 可消除
- 本项目为课程/学习性质，**不是医疗器械**，心率/血氧与跌倒阈值未经临床验证

[v0.2.0]: https://github.com/PandaGuGu/stm32-health-monitor/compare/v0.0.1...main
[v0.0.1]: https://github.com/PandaGuGu/stm32-health-monitor/releases/tag/v0.0.1

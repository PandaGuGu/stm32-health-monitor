# 更新日志

格式参考 [Keep a Changelog](https://keepachangelog.com/zh-CN/1.1.0/)，版本号遵循
[语义化版本](https://semver.org/lang/zh-CN/)。

## [v0.7.2] - 2026-11-01

跌倒记录改为**前台常驻**（App 0.0.6）。弹窗只负责打断一次，关掉就没了；
「什么时候跌的」这件事必须一直摆在眼前，直到用户主动确认。

### 新增

- **跌倒记录横幅**（`MainActivity`）：首页标题下方常驻一条，显示报警时刻与状态
  （`进行中` / `已恢复`），右侧「关闭」按钮是唯一的清除入口
  - 存 `SharedPreferences`（`fall_rec_ts` / `fall_rec_ms` / `fall_rec_off`），
    **重启 App 依然在**；`fall` 归 0 只把状态改成「已恢复」，记录不消失
  - 新一场跌到会覆盖成最新时刻并重新显示，哪怕上一条已被关闭
  - 与弹窗解耦：弹窗里的「知道了」只关窗，删不掉这条记录
- **网页端同款横幅**（`local_web`，不入库）：`store.js` 新增 `fallRecord` + `closeFallRecord()`，
  落 `localStorage`（键 `vitals-fall`），刷新、重开浏览器都还在

## [v0.7.1] - 2026-11-01

跌倒改为**前台弹窗**提示（App 0.0.5）。上一版只是把卡片上的数字变红，可跌倒后手指
离开传感器，读数区先一步变成「无读数」，最要紧的那条信息反而最容易被忽略。

### 新增

- **App 跌倒弹窗**（`MainActivity`，本地工程）：首次检出跌倒立即弹 `AlertDialog`，
  标题「检测到跌倒」，正文给出报警时间与最近一帧读数；无有效读数时写明
  「当前无有效读数，可能已离开传感器」，不拿残留值充数
  - 「知道了」只关窗，云端报警继续每 5 s 重发
  - 「停止报警」置 `alarmMuted`：本轮不再弹、不再重发，卡片注脚转为「已确认」
  - `fallNotified` 保证一次跌倒只弹一次，否则用户关窗后每来一帧都会重新弹
  - 掉线路径（`onBleError` / `onError`）只刷新卡片、不弹窗，避免重连失败反复打扰
  - `onPause` 关闭弹窗；`fall` 归 0 时复位弹窗状态、`alarmMuted`、`fallNotified`
- **网页端跌倒弹窗**（`local_web`，不入库）：`AlarmDialog.vue` 配合 `store.js` 的
  `syncAlarm()`，同样按边沿触发、只弹一次，提供「知道了 / 停止提醒」

### 变更

- 设置页诊断补一档「报警已关闭（用户确认）」

## [v0.7.0] - 2026-10-31

跌倒报警改为**主动持续上报**：采集端一旦检出跌倒，不再依赖"是否还有有效帧"，
按周期反复推送到中继，并带上报警时刻；中继透传两个时间字段（App 0.0.4）。

### 新增

- **跌倒报警帧**（`CloudClient.pushAlarm`，本地工程）：`fall` 恒为 1，附
  `fall_ts`（`yyyy-MM-dd HH:mm:ss`）与 `fall_ms`（epoch 毫秒）
  - 不受 `ok` 限制：老人跌倒后手指离开传感器，`ok` 立刻变 false，按老规矩"只推有效帧"
    最要紧的这一帧反而推不上去
  - 读数不伪造：`s` 无效时 `ok=false`、`bpm/spo2/temp` 照实为 0
  - 5 s 节流；`MainActivity` 用定时器在跌倒期间反复调用，蓝牙断开也照推
  - 最长重发窗口 5 分钟，避免设备侧 `fall` 卡在 1 时无限推送
- **中继白名单扩充**（`remote/relay_server.py`）：`VITALS_KEYS` 增加 `fall_ts` / `fall_ms`，
  `/api/status` 原样透传，查看端与网页端据此显示报警时刻

### 变更

- 跌倒状态改为**锁存**：首次检出即记下时刻，卡片注脚显示「报警 MM-dd HH:mm:ss」；
  链路断开时卡片不再退回 `—`，报警信息保持可见
- 设置页诊断增加「跌倒报警」一行（未触发 / 上行中 · 已持续 N s / 已停止重发）

## [v0.6.0] - 2026-10-30

远程监测方案改为**「App 上行 + 账号鉴权 + App 本地 7 天」**：设备不再自己往外推，
云端仍然不落盘，历史保存在采集手机本地。

### 新增

- **中继换成 HTTP Basic Auth**（`remote/relay_server.py`）：`X-Token` 移除，上行与下行都校验
  `Authorization: Basic`，缺凭据或密码错误一律 401 并带 `WWW-Authenticate`；密码比较用
  `hmac.compare_digest`。内置测试账号 `test1` / `stm-esp-test1`，可用 `--user` / `--pass`
  或 `RELAY_USER` / `RELAY_PASS` 覆盖
- **App 云端上行**（`CloudClient`，本地工程）：蓝牙档每帧 `POST /api/vitals`
  - 单线程池执行，同一时刻只发一个请求，在途时丢弃新帧，不排队堆风暴
  - 只推 `ok==true` 的帧，并把设备侧 `age_ms` 原样带上，供中继做两段帧龄相加
  - 失败节流：首次失败、恢复、之后每 30 次各一行，绝不打扰本地显示
- **App 本地历史**（`Store`，本地工程）：SQLite 一张表 `hist(ts,bpm,spo2,temp,fall)`
  - 保留 7 天，5 s 降采样，只存有效帧；每小时最多清理一次过期行
  - 首页趋势图启动时从库里 seed，关掉 App 再开曲线还在
- **设置页新增两个分组**：`云端（远程中继）`（地址 / 账号 / 密码 / 自动上传开关）、
  `本地历史`（条数、最早时间、清空按钮）

### 移除

- **固件侧上报模块** `firmware/esp32/esp32_info_node/cloud_link.{h,cpp}` 及其配置
  （`ENABLE_CLOUD` / `CLOUD_URL` / `CLOUD_TOKEN` / `CLOUD_INTERVAL_MS` / `CLOUD_TIMEOUT_MS`）
  - 上行改由 App 完成，设备只负责「收 STM32 帧 + BLE 通知 + 本地 HTTP」
  - 原实现仍发 `X-Token`，在新中继上必然 401；且设备常连 WiFi 会与 BLE 分时争抢射频
  - 默认构建本就未编译该模块，移除后固件体积、行为、功耗均无变化
    （`ENABLE_BLE=1` + `huge_app` 实测 1 732 567 B，移除前 1 732 599 B，差 32 字节来自链接期空实现）

### 变更

- `remote/README.md` 重写：上行执行者从设备改为 App，部署补 HTTPS 反代，新增「本地 7 天」一节
- 单个中继只服务一个采集端：两台上报会互相覆盖同一帧
- **App 源码与 APK 不再入库**：`host/app-vitals/` 加入 `.gitignore` 并从索引移除（本地文件保留），
  成品 APK 改由 GitHub Releases 分发，仓库里只留固件、网关、Web 与中继服务

## [v0.5.0] - 2026-10-30

新增**蓝牙链路**：手机不用连 WiFi、也不经过任何服务器，直接用 BLE 收 vitals。
固件侧新增 BLE NUS 外设，App 新增第三档「蓝牙」，三条链路共用一套显示逻辑。

### 新增

- **BLE 外设** `firmware/esp32/esp32_info_node/ble_link.{h,cpp}`
  - Nordic UART Service `6E400001-…`：通知特征 `6E400003-…`（设备→手机），
    写特征 `6E400002-…`（预留，当前固件不解析）
  - `config.h` 新增 `ENABLE_BLE`（默认 `0`）、`BLE_NAME`、`BLE_MTU`、`BLE_NOTIFY_MS`
  - 有新的有效帧立刻 notify，没有也按 2 s 兜底推一次——手机才能区分「设备在但没读数」
    和「设备已经掉了」
  - 断开后自动重启广播；广播名 `stm32-vitals-` + MAC 后四位
- **App 蓝牙档** `host/app-vitals/.../BleClient.java`
  - 扫 `stm32-vitals*`：名字匹配失败时用 NUS 服务 UUID 兜底（部分机型拿不到 scan response 里的名字）
  - `requestMtu(517)`；协商不到 40 字节以上直接报错，不静默截断（截断的 JSON 必然解析失败）
  - 收帧交给 `VitalsClient.parse()`，与 HTTP 链路共用同一套字段映射，不为蓝牙另写解析
  - 蓝牙源打 `viaBle` 标记，进设置页诊断区的「当前链路」；该链路没有的 `runtime` / `device` 显示 `—`
- 三档链路切换（本地直连 / 远程中继 / 蓝牙），蓝牙档把地址行与连接按钮置灰

### 变更

- **首页精简，设置项收进右上角齿轮**（`host/app-vitals`）
  - 首页只留状态、四张指标卡、心率趋势：地址栏、连接按钮、链路切换、帧计数页脚全部移除
  - 新增 `SettingsActivity`：连接方式（三档 chip）、服务器地址、诊断区（当前链路 / 数据新鲜度 /
    有效帧 / 坏帧 / 无读数帧 / 运行时长 / 芯片型号 / 固件版本）
  - 新增 `Cfg.java` 承载两个 Activity 共用的配色、偏好键与几何工具，避免两边各写一份后跑偏
  - 首页状态栏不再暴露链路与数据源，一律只写「已连接 / 未连接」；链路名挪到设置页诊断区
  - 升级兼容：旧版只有 `addr` 一个键，首次启动把它当作本地档地址迁移到 `addr_local`
- **构建默认分区改为 `huge_app`**：BLE 栈让固件从 986 KB 涨到 1.73 MB，
  默认分区只给应用 1.3 MB，会报 `text section exceeds available space in board`
- `scripts/build-esp32.ps1` 重写
  - 先把 sketch 复制到 `%TEMP%\esp32build` 再编译：`xtensa-g++` 在中文路径下会**静默崩溃**，
    只报 `Error during build: exit status 1` 且没有任何错误行，编译器进程 CPU 停在 0.1 s
  - 新增 `-Define ENABLE_BLE=1,ENABLE_CLOUD=1` 传编译宏
  - 自动在常见安装位置查找 `arduino-cli`
  - 输出统一 UTF-8，避免中文提示在 GBK 控制台变成乱码

### 修复

- **Android 12+ 扫描结果被系统整体过滤**（`AndroidManifest.xml`）：`BLUETOOTH_SCAN` 缺少
  `android:usesPermissionFlags="neverForLocation"`，即使权限已授予，扫描回调也一条都不来。
  表现为 App 提示「没找到 stm32-vitals* 设备」，而系统蓝牙设置页能看到该设备
- 重复 `startScan` 报 `SCAN_FAILED_ALREADY_STARTED`（code 1）：启动前先 `stopScan` 清场

## [v0.4.0] - 2026-10-30

新增**远程监测方案**，与现有局域网方案并列：设备主动上报到公网中继，异地 App 轮询取数。
中继不存数据，只保留最新一帧。

### 新增

- **远程监测方案** `remote/`
  - `remote/relay_server.py`：纯标准库中继。`POST /api/vitals` 覆盖内存里唯一一帧，
    `GET /api/status` 返回与设备同构的快照，`GET /api/health` 给收帧数与帧龄；
    无数据库、无落盘、进程退出即清空；可选 `--token` 鉴权与 `--stale` 掉线阈值
  - `remote/README.md`：定位对照（与局域网方案八项差异）、部署步骤（含 systemd 单元与
    内网穿透）、帧龄算法、五条边界、故障速查、实测记录
- **固件上行模块** `firmware/esp32/esp32_info_node/cloud_link.{h,cpp}`
  - `config.h` 新增 `ENABLE_CLOUD`（默认 `0`）、`CLOUD_URL`、`CLOUD_TOKEN`、`CLOUD_INTERVAL_MS`
  - `ENABLE_CLOUD=0` 时 `Cloud::begin()/loop()` 为 .cpp 内的空实现，现有固件行为与体积不变
  - 只在 WiFi 已连上时发送；AP 兜底模式直接返回，不计数、不报错
  - 失败日志节流：状态码异常只在首次失败、恢复、或每 30 次时打一行
- `vitals` 帧整段（含设备侧 `ok` / `age_ms`）原样上行，而非只发四个数字

### 变更

- `host/app-vitals` 无需改动即可接入远程方案：中继在 `/api/status` 路径应答同构快照，
  App 只把地址栏改成中继地址，不用重新编译

### 说明

- 中继不存储：无历史、无回放、无离线补传，重启即清空；趋势由 App 的 90 点滚动窗口自己攒
- 明文 HTTP，`X-Token` 随报文裸传，只能防误连、防不了抓包；需要时用 Nginx/Caddy 反代上 HTTPS
- 一个中继实例对应一台设备，多设备多起实例

## [v0.3.0] - 2026-10-30

联调中发现并修复了 ESP32 侧两个缺陷，同时补一个**只做健康数据展示**的安卓上位机，
把成品 APK 直接放进仓库，装机即用。

### 新增

- **健康监测 App** `host/app-vitals/`（`com.sit.vitals`，Java 原生、零第三方依赖，APK 25 KB）
  - 直连 ESP32 `GET /api/status`：四大指标卡片 + 90 点心率趋势 + 数据新鲜度页脚
  - **只信 `ok == true` 的读数**：门控失败时显示「无读数」，不把 0 或残留旧值画出来
  - 心率 <60 或 >100、血氧 <95、跌倒 =1 时标红；跌倒首次触发震动一次
  - 成品 `host/app-vitals/app-vitals.apk` 随仓库分发（debug 签名，`adb install` 直接用）
  - 无 gradle 时的命令行构建脚本 `build-apk.sh`（aapt2 + d8 + apksigner）
- `vitals` 新增 `invalid` 字段：格式合法但读数无效的帧数

### 修复

- **`Net::ip()` 漏判 `WIFI_AP_STA`**（`net_info.cpp`）：设备实际运行在 AP_STA（STA 连不上路由器），
  旧逻辑落到 `localIP()` 返回 `0.0.0.0`，导致 `/` 首页显示 `ip : 0.0.0.0` 并提示
  「App 里把地址填 http://0.0.0.0」，照做必然连不上。改为 AP / AP_STA 都取 `softAPIP()`。
- **`bpm = 0` 被当成有效读数**（`uart_link.cpp`）：STM32 门控失败时 `dis_hr`/`dis_spo2` 置 0 后
  照发，ESP32 旧逻辑认为 0–250 都合法，`ok` 仍为 `true`，上位机把「本轮没测到」显示成
  「心率 0」，8 s 过期机制形同虚设。现在这类帧计入 `invalid`、不更新数值、**不刷新 `age_ms`
  计时起点**，8 s 后 `ok` 自然转 `false`；体温（DS18B20）与跌倒（MPU6050）来自独立传感器，
  不受 PPG 门控影响，照常更新。

### 变更

- `docs/esp32/protocol.md` §3.2 / §8：修正帧周期描述（实测 ~2 s 一帧，原文写 5–6 s）、
  补充 `invalid` 字段与无读数帧的处理规则

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

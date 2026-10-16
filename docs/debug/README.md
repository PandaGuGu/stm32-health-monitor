# 调试与故障排查记录

本目录收录本项目的硬件调试报告，全部基于**实机测量**（ST-Link + OpenOCD 直读寄存器/内存，非推断）。
每份报告都包含：现象 → 证据链 → 根因 → 修复动作 → 复现命令。

> 说明：报告中的本机绝对路径已脱敏为 `$HOME`（Git Bash）/ `%USERPROFILE%`（PowerShell）。

## 一、报告清单（按时间顺序）

| # | 报告 | 日期 | 核心结论 |
|---|---|---|---|
| 01 | [硬件调试报告](01-硬件调试报告.md) | 2026-10-13 | 固件烧录成功但卡死在 `ds18b20_init()`；定位到 `ds18b20.c:68` 无超时死等 |
| 02 | [引脚排查报告](02-引脚排查报告.md) | 2026-10-16 | SWD 打通（`adapter speed 20` 是唯一稳定档位）；建立 RC 电容探针法 |
| 03 | [元件复测报告](03-元件复测报告.md) | 2026-10-16 | 4 个元件中 3 个实测通过，仅 MAX30102 无任何电气痕迹 |
| 04 | [MAX30102 专项诊断](04-MAX30102专项诊断.md) | 2026-10-16 | 四项独立测量交叉印证：PB7/PB8 上无连接，且模块不在任何其它引脚上 |
| 05 | [业务固件跑通报告](05-业务固件跑通报告.md) | 2026-10-16 | 黑屏根因是固件死等而非屏坏；除 MAX30102 外全部功能实测跑通 |

> 更早的同一支线迭代（`v1.0`–`v1.2` 引脚排查）已被 `02` 取代，未重复收录。

## 二、故障速查表

| 现象 | 根因 | 修复 |
|---|---|---|
| 烧录/校验成功，但 OLED 全黑、所有传感器读数恒为 0 | `ds18b20.c:68` 处 `while(READ_DQ)` 无超时死等，DS18B20 未应答即永久阻塞在 `ds18b20_init()` | ① DS18B20 的 DQ 补 4.7 kΩ 上拉；② 用带超时的 DS18B20 实现（见 `gcc/app/app_main.c`） |
| PC/GDB 调用栈停在 `myiic.c:51` | `while(READ_SDA)` 等 MAX30102 的 I2C ACK，无超时 | MAX30102 整段 `#ifndef NO_MAX30102` 屏蔽（`make APP=1`） |
| 卡在 `main.c:104 / :141` | `while(MAX30102_INT==1);` 轮询中断脚，INT 悬空即死等 | 同上屏蔽；或正确接好 INT 线 |
| OpenOCD 报 `init mode failed`，100 kHz 以上连不上 | 杜邦线过长/无屏蔽，高频握手失败（信号完整性问题，非软件） | **`-c "adapter speed 20"`**（20 kHz 是实测唯一稳定档位） |
| ST-Link 报 `LIBUSB_ERROR_TIMEOUT` | 设备句柄卡死 | `Disable-PnpDevice` + `Enable-PnpDevice` 软件重枚举（无需拔插） |
| RC 探针读到 `PB3 = 3392` 异常大 | PB3 = JTDO，复位后被 JTAG 控制器接管，写 `CRH` 不生效 → 读数是 JTAG 时序伪影 | 忽略 PB3/PB4；任何 DIY 引脚复用都不要用这两脚 |
| `PA12` 检出"外部上拉"，误判蜂鸣器已接通 | PA12 兼作 USB_DP，板载 1.5 kΩ 上拉是**单片机自带**，与蜂鸣器无关 | 不能据此判定；以"开机短鸣两声"为准 |
| MAX30102 全 256 个 I2C 地址 `ACK = 0`、`PART_ID = 0x00` | 「看起来插上了」≠「电气上接通了」：面包板弹片失效／杜邦线内部断芯／插在相邻一列／排针悬空 | 万用表通断档量线两端；把 SCL/SDA 临时挪到 PB10/PB11 做隔离测试 |
| `adapter speed` 用 `hla_swd` 无效果 | OpenOCD 0.12 已删除 `hla_swd`，静默忽略该参数 | 用 0.11 或放弃 `hla_swd`，直接调 `adapter speed` |
| Keil 授权不可用 / 无法编译 | — | 改用 arm-none-eabi-gcc 独立构建（见 [`gcc/README.md`](../../firmware/MAX30102/gcc/README.md)） |

### 三处已知的代码/文档不一致（调试中发现）

| 项 | 文档/宏 | 实际代码 | 影响 |
|---|---|---|---|
| MAX30102 中断脚 | `board/schematic.md` 写 **PB9**；`max30102.h:6` `#define MAX30102_INT PBin(9)` | `max30102.c:247` 初始化的是 **PB14** | 代码内部自相矛盾：**读**的是 PB9（未初始化为输入），**初始化**的是 PB14（从未被读）。按 PB9 接线才是当前有效行为 |
| 软件 I2C 读 ACK 时的引脚模式 | — | `myiic.h:19` `SDA_IN()` 配的是**输入浮空**（CNF=01），不是上拉 | 总线无外部上拉时读 ACK 电平悬空；建议改为 `IPU` |
| DS18B20 上拉 | 文档要求 4.7 kΩ 外部上拉 | `ds18b20.c:45` 仅用内部 IPU（约 40 kΩ）兜底 | 长线/劣质模块下不足以拉高，建议补外部电阻 |

## 三、复现命令速查

```bash
export PATH="$HOME/tools/xpack-arm-none-eabi-gcc-14.2.1-1.1/bin:$PATH"
cd firmware/MAX30102/gcc

# 编译（变体见 gcc/README.md）：缺省 IIC / APP=1 / LIVE=1 / DIAG=1
SHELL=/usr/bin/bash /c/mingw/mingw64/bin/mingw32-make.exe -j4 APP=1

# 烧录（adapter speed 20 是关键）
openocd -f interface/stlink.cfg -f target/stm32f1x.cfg \
        -c "adapter speed 20" -c "init" -c "reset halt" \
        -c "program build/APP.elf verify" -c "reset run" -c "exit"
```

## 四、当前状态

| 元件 | 引脚 | 状态 |
|---|---|---|
| OLED（0.96" SSD1306） | PA5 / PA6 | ✅ 实测通过 |
| MPU6050（姿态/跌倒） | PB10 / PB11 | ✅ `WHO_AM_I = 0x68` |
| DS18B20（体温） | PA8 | ✅ 应答 110 µs，读到 27 ℃ |
| 蜂鸣器 | PA12 | ✅ 已初始化，开机短鸣两声 |
| **MAX30102（心率/血氧）** | PB7 / PB8 | ❌ 零电气痕迹，待接线复核 |

除 MAX30102 外全部功能已实测跑通（`APP=1` 目标）。MAX30102 接回后改回缺省目标 `make` 即可。

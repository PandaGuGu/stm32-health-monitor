# GCC 构建说明（arm-none-eabi-gcc 替代 Keil MDK）

Keil 授权不可用，本目录提供一套独立于 Keil 的 GCC 构建系统，产出 `.elf / .hex / .bin`。

## 目录内容

| 文件 | 说明 |
|---|---|
| `Makefile` | 构建脚本 |
| `STM32F103C8.ld` | 链接脚本（64KB Flash / 20KB RAM），替代 Keil 的 `.sct` |
| `startup_stm32f10x_md_gcc.s` | GNU as 版启动文件，向量表与 Keil 版逐项对应 |
| `.gitignore` | 忽略 `build/` 产物，避免污染仓库 |
| `diag/` | 诊断固件入口（`diag_main.c` 一次性 / `diag_live.c` 实时） |
| `app/` | 业务固件入口（`app_main.c`，由 `make_app.py` 从 `USER/main.c` 字节级派生 + `make_app.py` 生成器本体） |
| `build/` | 编译输出（不入库） |

## 环境依赖

| 组件 | 版本 | 位置（示例） |
|---|---|---|
| arm-none-eabi-gcc | 14.2.1 (xPack) | `%USERPROFILE%\tools\xpack-arm-none-eabi-gcc-14.2.1-1.1\bin` |
| OpenOCD | 0.12.0 (xPack) | `%USERPROFILE%\tools\xpack-openocd-0.12.0-7\bin` |
| make | GNU Make (MinGW) | `C:\mingw\mingw64\bin\mingw32-make.exe` |

> 建议把工具链装在**无中文、无空格的路径**下（如 `%USERPROFILE%\tools\`），避免中文路径在 make/gcc 传递时出现编码问题。
> 若工具链已在 `PATH` 中，可跳过下面的 `-BIN` 变量；否则用 `TOOLCHAIN_BIN` / `OPENOCD_BIN` 显式指定其 `bin` 目录。

## 构建变体

四个目标切换靠 Makefile 变量，入口文件不同、互不干扰；**仓库中原有文件一律不改**。

| 变量 | 目标 | 入口 | 说明 |
|---|---|---|---|
| （缺省） | `IIC` | `USER/main.c` | 原业务固件，含 MAX30102 |
| `APP=1` | `APP` | `gcc/app/app_main.c` | 业务固件 **MAX30102 关闭版**，自动加 `-DNO_MAX30102` |
| `LIVE=1` | `LIVE` | `gcc/diag/diag_live.c` | 实时诊断，每轮刷新全部引脚/器件状态 |
| `DIAG=1` | `DIAG` | `gcc/diag/diag_main.c` | 一次性诊断，跑一遍即停 |

### `APP=1` 为什么需要单独一版

`USER/main.c` 有两处**无超时死等**，任一条件不满足就永久卡死，导致 OLED 不显示、其余传感器全部不执行：

| 位置 | 代码 | 触发条件 |
|---|---|---|
| `HARDWARE/IIC/myiic.c:51` | `while(READ_SDA)` | MAX30102 不应答 ACK 时死循环（`max30102_init()` 内） |
| `USER/main.c:104 / :141` | `while(MAX30102_INT==1);` | MAX30102 中断脚悬空时死循环（FIFO 采集） |
| `HARDWARE/DS18B20/ds18b20.c:68-69` | `while(...)` ×2 | DS18B20 不在线时死循环（`ds18b20_init()` 内） |

`APP=1` 的处理：

1. **MAX30102 整段编译屏蔽**（`#ifndef NO_MAX30102`）：初始化 + 两处 FIFO 采集 + 两处 INT 死等全部不参与编译；心率/血氧栏显示 `--`。
2. **DS18B20 换成带超时的本地实现**（`app_main.c` 内 `app_dq_reset()`，应答等待上限 300µs）：器件不在线直接返回 0，绝不阻塞。PA8 接线不变。
3. **蜂鸣器开机自检**：短鸣两声，便于确认已接通。

`app_main.c` 由 `app/make_app.py` 从 `USER/main.c` **按字节派生**（原文件是 GBK 编码，中文串直接透传，避免编码破坏字模匹配）。改动源码后重新生成：

```bash
python gcc/app/make_app.py     # 覆盖生成 gcc/app/app_main.c
SHELL=/usr/bin/bash $MAKE -j4 APP=1
```

## 构建 / 烧录

在 `gcc/` 目录下执行（Git Bash）：

```bash
export PATH="$HOME/tools/xpack-arm-none-eabi-gcc-14.2.1-1.1/bin:$PATH"
MAKE=/c/mingw/mingw64/bin/mingw32-make.exe
OCD=$HOME/tools/xpack-openocd-0.12.0-7/bin

SHELL=/usr/bin/bash $MAKE -j4        # 编译（必须显式指定 SHELL，否则 make 用 cmd.exe 跑不通 POSIX 命令）
SHELL=/usr/bin/bash $MAKE clean      # 清理
$MAKE OPENOCD_BIN=$OCD probe         # 探测 ST-Link 连接（不写 Flash）
$MAKE OPENOCD_BIN=$OCD flash         # 烧录
$MAKE OPENOCD_BIN=$OCD erase         # 全片擦除
```

> 烧录若报 `init mode failed`，在 OpenOCD 命令里加 `-c "adapter speed 20"`（见下文「调试链路」）。

## 当前编译结果

| 目标 | FLASH | RAM | 说明 |
|---|---|---|---|
| `IIC`（缺省） | 11124 B / 64 KB = **17.0%** | 12792 B / 20 KB = 62.5% | 原业务固件，含 MAX30102 |
| **`APP=1`** | **8112 B / 64 KB = 12.4%** | **6800 B / 20 KB = 33.2%** | MAX30102 关闭版（已实测跑通） |
| `LIVE=1` | 22.9% | — | 实时诊断 |
| `DIAG=1` | 23% | — | 一次性诊断 |

`APP` 版因屏蔽 MAX30102 而比 `IIC` 版小 3 KB Flash、省 6 KB RAM，余量充足。

## 与 Keil 工程的两处必要差异（原因已核实）

1. **不编译 `CORE/core_cm3.c`**
   CMSIS 内联汇编 `strexb/strexh` 的输出约束漏写 early-clobber，GCC 14 报
   `registers may not be the same -- strexb r0,r0,[r1]`。
   已核查 USER/HARDWARE/SYSTEM/FWLib 全工程**无任何文件引用**该文件中的符号，排除后无功能损失（Keil/AC5 下可正常编译）。

2. **`SYSTEM/usart/usart.c` 单独用 `-std=gnu89` 编译**
   该文件是正点原子为 ARMCC 写的 printf 重定向：`#pragma import(__use_no_semihosting)`（GCC 不识别）、
   `_sys_exit(int x)` 省略返回类型（GCC 14 在 C99+ 下视为错误）、`FILE __stdout;`（已确认 newlib 未定义同名符号，无害）。
   用 gnu89 单独编译该文件即可通过，**不改动仓库源码**。

3. **`STM32F10X_MD` 由命令行 `-D` 传入**
   Keil 工程的 Define 只写了 `USE_STDPERIPH_DRIVER`，未定义器件宏（在 Keil 下同样会报
   "Please select first the target STM32F10x device"）。Makefile 里补了 `-DSTM32F10X_MD`。

## 首次烧录前

1. 插入 ST-Link，接到板上 **SWDIO / SWCLK / GND / 3V3**（四根即可，勿接 SWO）；
2. 跑 `make probe` 验证连接；
3. 若报 USB `open failed`：ST-Link 需 WinUSB 驱动（DriverStore 中通常已有 `stlink_winusb.inf`，
   正常插上会自动匹配；若不匹配，用 Zadig 或 STM32CubeProgrammer 安装 WinUSB 驱动）。
   注意：换成 WinUSB 后 Keil 的 ST-Link 下载会失效（本项目已不用 Keil）。

## 调试链路（ST-Link + OpenOCD）

**关键参数：`adapter speed 20`。** 杜邦线较长/无屏蔽时，OpenOCD 默认速率会导致
`init mode failed`；20 kHz 是实测唯一稳定档位。

```bash
# 烧录（在 gcc/ 下，build/APP.elf 换成对应目标产物）
openocd -f interface/stlink.cfg -f target/stm32f1x.cfg \
        -c "adapter speed 20" -c "init" -c "reset halt" \
        -c "program build/APP.elf verify" -c "reset run" -c "exit"

# 只探测（不写 Flash）
openocd -f interface/stlink.cfg -f target/stm32f1x.cfg \
        -c "adapter speed 20" -c "init; targets; reset halt; reg pc; exit"
```

若 ST-Link 报 `LIBUSB_ERROR_TIMEOUT`（设备句柄卡死），用 USB 软件重枚举复位设备，无需拔插：

```powershell
Get-PnpDevice -Class USB | Where-Object { $_.InstanceId -like '*VID_0483*' } |
  Disable-PnpDevice -Confirm:$false
Get-PnpDevice -Class USB | Where-Object { $_.InstanceId -like '*VID_0483*' } |
  Enable-PnpDevice  -Confirm:$false
```

## 相关文档

- 硬件调试与故障排查报告（根因分析、证据链、复现命令）：[`docs/debug/`](../../../docs/debug/README.md)
- 系统架构与数据流：[`docs/architecture.md`](../../../docs/architecture.md)
- 引脚接线表：[`board/schematic.md`](../../../board/schematic.md)

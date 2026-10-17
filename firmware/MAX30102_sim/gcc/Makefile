# =============================================================================
# STM32F103C8 健康监测固件 · arm-none-eabi-gcc 构建脚本
# 替代 Keil MDK（授权不可用），产出 .elf / .hex / .bin
#
# 用法（在 gcc/ 目录下）：
#   make              编译
#   make clean        清理
#   make flash        用 OpenOCD + ST-Link 烧录
#   make size         查看代码大小
#
# 依赖：arm-none-eabi-gcc 与 openocd 已在 PATH 中
#       若不在 PATH，可显式指定：
#   make TOOLCHAIN_BIN=$HOME/tools/arm-none-eabi/bin \
#        OPENOCD_BIN=$HOME/tools/openocd/bin
# =============================================================================

TARGET     = IIC
BUILD_DIR  = build
FIRMWARE   = ..

# ---- 工具链 ----------------------------------------------------------------
ifdef TOOLCHAIN_BIN
PREFIX = $(TOOLCHAIN_BIN)/arm-none-eabi-
else
PREFIX = arm-none-eabi-
endif

ifdef OPENOCD_BIN
OPENOCD = $(OPENOCD_BIN)/openocd.exe
else
OPENOCD = openocd
endif

CC = $(PREFIX)gcc
AS = $(PREFIX)gcc -x assembler-with-cpp
CP = $(PREFIX)objcopy
SZ = $(PREFIX)size

# ---- 目标芯片 --------------------------------------------------------------
CPU   = -mcpu=cortex-m3
MCU   = $(CPU) -mthumb -mfloat-abi=soft

# ---- 源文件 ----------------------------------------------------------------
# 诊断固件开关：
#   make DIAG=1  -> 一次性诊断（跑一遍就冻结），入口 gcc/diag/diag_main.c
#   make LIVE=1  -> 实时诊断（持续刷新，边接线边看），入口 gcc/diag/diag_live.c
#   make APP=1   -> 业务固件（MAX30102 关闭版），入口 gcc/app/app_main.c
#                   自动加 -DNO_MAX30102：OLED/MPU6050/DS18B20/蜂鸣器全部跑通，
#                   MAX30102 相关初始化与 FIFO 采集整段编译屏蔽。
#                   出处：USER/main.c 经 gcc/app/make_app.py 字节级派生，原文件不动。
#   缺省         -> 原业务固件 IIC（USER/main.c，含 MAX30102）
ifeq ($(APP),1)
TARGET       = APP
# 业务固件默认【包含 MAX30102】（心率/血氧全功能）。
# 如需旧的无 30102 关闭版（例如 30102 模块拔掉时只想跑其它功能）：make APP=1 NOMAX=1
ifeq ($(NOMAX),1)
EXTRA_DEFS   = -DNO_MAX30102
endif
USER_SOURCES = $(FIRMWARE)/USER/stm32f10x_it.c \
               $(FIRMWARE)/USER/system_stm32f10x.c \
               $(FIRMWARE)/gcc/app/app_main.c
else ifeq ($(LIVE),1)
TARGET       = LIVE
USER_SOURCES = $(FIRMWARE)/USER/stm32f10x_it.c \
               $(FIRMWARE)/USER/system_stm32f10x.c \
               $(FIRMWARE)/gcc/diag/diag_live.c
else ifeq ($(DIAG),1)
TARGET       = DIAG
USER_SOURCES = $(FIRMWARE)/USER/stm32f10x_it.c \
               $(FIRMWARE)/USER/system_stm32f10x.c \
               $(FIRMWARE)/gcc/diag/diag_main.c
else
TARGET       = IIC
USER_SOURCES = $(wildcard $(FIRMWARE)/USER/*.c)
endif

# 注意：CORE/core_cm3.c 不参与编译。
#   原因：其中实现 __STREXB/__STREXH 等 CMSIS 内联函数的 ARMCC 风格内联汇编
#         （输出约束漏写 early-clobber &），在 GCC 14 下报
#         "Error: registers may not be the same -- strexb r0,r0,[r1]"；
#         且已核查全工程（USER/HARDWARE/SYSTEM/FWLib）无任何文件引用这些符号，
#         排除后无功能损失。Keil(AC5) 下该文件可正常编译。
C_SOURCES  = $(USER_SOURCES) \
             $(wildcard $(FIRMWARE)/SYSTEM/*/*.c) \
             $(wildcard $(FIRMWARE)/HARDWARE/*/*.c) \
             $(wildcard $(FIRMWARE)/STM32F10x_FWLib/src/*.c)

ASM_SOURCES = startup_stm32f10x_md_gcc.s

# ---- 编译选项 --------------------------------------------------------------
# STM32F10X_MD / USE_STDPERIPH_DRIVER 由命令行传入（Keil 工程里只定义了后者）
C_DEFS = -DUSE_STDPERIPH_DRIVER -DSTM32F10X_MD $(EXTRA_DEFS)

C_INCLUDES = -I$(FIRMWARE)/USER \
             -I$(FIRMWARE)/CORE \
             -I$(FIRMWARE)/STM32F10x_FWLib/inc \
             -I$(FIRMWARE)/SYSTEM/delay \
             -I$(FIRMWARE)/SYSTEM/sys \
             -I$(FIRMWARE)/SYSTEM/timer \
             -I$(FIRMWARE)/SYSTEM/usart \
             -I$(FIRMWARE)/HARDWARE/BUZZER \
             -I$(FIRMWARE)/HARDWARE/DS18B20 \
             -I$(FIRMWARE)/HARDWARE/IIC \
             -I$(FIRMWARE)/HARDWARE/LED \
             -I$(FIRMWARE)/HARDWARE/MPU6050 \
             -I$(FIRMWARE)/HARDWARE/OLED

# -Wall 但屏蔽 SPL 库自身的常见告警噪音
OPT = -O1 -g3 -ffunction-sections -fdata-sections -Wall \
      -Wno-unused-parameter -fno-strict-aliasing

CFLAGS   = $(MCU) $(C_DEFS) $(C_INCLUDES) $(OPT) -std=gnu11
ASFLAGS  = $(MCU) $(OPT)

# ---- 链接选项 --------------------------------------------------------------
LDSCRIPT = STM32F103C8.ld
LIBS     = -lc -lm -lnosys
LDFLAGS  = $(MCU) -specs=nano.specs -specs=nosys.specs \
           -T$(LDSCRIPT) -nostartfiles \
           -Wl,-Map=$(BUILD_DIR)/$(TARGET).map,--cref \
           -Wl,--gc-sections \
           -Wl,--print-memory-usage

# ---- 目标文件（按 basename 展平，已确认无重名） ----------------------------
OBJECTS  = $(addprefix $(BUILD_DIR)/,$(notdir $(C_SOURCES:.c=.o)))
vpath %.c $(sort $(dir $(C_SOURCES)))
OBJECTS += $(addprefix $(BUILD_DIR)/,$(notdir $(ASM_SOURCES:.s=.o)))
vpath %.s $(sort $(dir $(ASM_SOURCES)))

# ---- 构建规则 --------------------------------------------------------------
all: $(BUILD_DIR)/$(TARGET).elf $(BUILD_DIR)/$(TARGET).hex $(BUILD_DIR)/$(TARGET).bin size

$(BUILD_DIR)/%.o: %.c Makefile | $(BUILD_DIR)
	$(CC) -c $(CFLAGS) -Wa,-a,-ad,-alms=$(BUILD_DIR)/$(notdir $(<:.c=.lst)) $< -o $@

$(BUILD_DIR)/%.o: %.s Makefile | $(BUILD_DIR)
	$(AS) -c $(ASFLAGS) $< -o $@

# ---- usart.c 单独处理 -------------------------------------------------------
# 该文件含正点原子为 ARMCC 编写的 printf 重定向代码：
#   #pragma import(__use_no_semihosting)  -> GCC 不认识（仅告警，用 -Wno-unknown-pragmas 静音）
#   _sys_exit(int x)                      -> 省略返回类型，GCC 14 在 C99+ 下视为错误
#   FILE __stdout;                        -> 已核查 newlib 未定义同名符号，无害
# 因此对该文件单独用 gnu89 标准编译，无需改动仓库源码。
$(BUILD_DIR)/usart.o: $(FIRMWARE)/SYSTEM/usart/usart.c Makefile | $(BUILD_DIR)
	$(CC) -c $(CFLAGS) -std=gnu89 -Wno-unknown-pragmas -Wa,-a,-ad,-alms=$(BUILD_DIR)/usart.lst $< -o $@

$(BUILD_DIR)/$(TARGET).elf: $(OBJECTS) Makefile $(LDSCRIPT)
	$(CC) $(OBJECTS) $(LDFLAGS) $(LIBS) -o $@
	$(SZ) $@

$(BUILD_DIR)/%.hex: $(BUILD_DIR)/%.elf | $(BUILD_DIR)
	$(CP) -O ihex $< $@

$(BUILD_DIR)/%.bin: $(BUILD_DIR)/%.elf | $(BUILD_DIR)
	$(CP) -O binary -S $< $@

$(BUILD_DIR):
	mkdir -p $@

size: $(BUILD_DIR)/$(TARGET).elf
	@$(SZ) -A -x $< | tail -20

# ---- 烧录 / 调试（ST-Link + OpenOCD） --------------------------------------
OPENOCD_CFG = -f interface/stlink.cfg -f target/stm32f1x.cfg

flash: $(BUILD_DIR)/$(TARGET).elf
	$(OPENOCD) $(OPENOCD_CFG) \
	  -c "init; reset halt; program $(BUILD_DIR)/$(TARGET).elf verify reset exit"

erase:
	$(OPENOCD) $(OPENOCD_CFG) -c "init; reset halt; stm32f1x mass_erase 0; reset; exit"

# 仅探测连接（不写 Flash）
probe:
	$(OPENOCD) $(OPENOCD_CFG) -c "init; targets; reset halt; reg pc; exit"

# 启动 GDB server（阻塞；另开终端跑 arm-none-eabi-gdb）
#   arm-none-eabi-gdb build/IIC.elf -ex "target extended-remote 127.0.0.1:3333"
debug-server:
	$(OPENOCD) $(OPENOCD_CFG)

clean:
	-rm -rf $(BUILD_DIR)

.PHONY: all clean size flash erase probe debug-server

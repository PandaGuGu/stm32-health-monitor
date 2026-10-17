# =============================================================================
# STM32F103C8 固件调试用 GDB 脚本
#
# 用法（先在一个终端跑 `make debug-server` 起 OpenOCD，再执行）：
#   arm-none-eabi-gdb build/IIC.elf -x debug.gdb
#
# 说明：OpenOCD 的 gdb server 监听 127.0.0.1:3333。
#       用 127.0.0.1 而非 localhost，避免 Windows 下解析成 IPv6 连接失败。
# =============================================================================

set confirm off
set pagination off

# 载入符号（若未作为命令行参数传入，取消下一行注释）
# file IIC.elf

target extended-remote 127.0.0.1:3333

# 复位并停在复位向量
monitor reset halt

# 常用观察点：main 里各传感器初始化
break main.c:88       # ds18b20_init
break main.c:84       # max30102_init
break main.c:86       # MPU6050_Init
break main.c:183      # get_tempetature（主循环内）

echo \n===== 已连接，输入 c 运行 / bt 看栈 / info locals 看局部变量 =====\n

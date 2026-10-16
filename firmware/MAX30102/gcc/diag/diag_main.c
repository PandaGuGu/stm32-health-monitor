/* =============================================================================
 * STM32F103C8 硬件引脚逐个排查 · 诊断固件（独立入口，不修改仓库原码）
 *
 * 构建：  make DIAG=1          -> build/DIAG.elf / .hex / .bin
 * 烧录：  make DIAG=1 flash
 *
 * 输出通道（任选）：
 *   1) 串口 USART1  PA9(TX) / PA10(RX)  @115200  —— 用 CH340 接上，Xshell 看
 *   2) OLED（若 OLED 正常，屏上直接显示结论）
 *   3) 内存结构 diag（GDB 直接读：p diag 或 p/x diag）
 *
 * 测试项：
 *   [1] 空闲电平快照（PA/PB 的 IDR）
 *   [2] 引脚推挽驱动/回读自检（8 个用到的引脚）
 *   [3] 软件 I2C 总线扫描  SCL=PB7  SDA=PB8   （MAX30102 期望 0x57）
 *   [4] 硬件 I2C2  总线扫描 SCL=PB10 SDA=PB11 （MPU6050 期望 0x68）
 *   [5] DS18B20 单总线应答（PA8，带超时，不会卡死）
 *
 * 设计要点：所有等待都带超时 —— 单个器件缺失不会拖死整机，
 *           这样可以一次性看到全部引脚的排查结果。
 * ========================================================================== */
#include "stm32f10x.h"
#include "sys.h"
#include "delay.h"
#include "usart.h"
#include "myiic.h"
#include "MPU6050_Reg.h"
#include "MPU6050.h"
#include "OLED.h"
#include <stdio.h>
#include <stdint.h>

/* --------------------------------------------------------------------------
 * 诊断结果结构（volatile，防止被优化掉；GDB 可直接读）
 * -------------------------------------------------------------------------- */
struct diag_s {
    uint32_t magic;             /* 0x47414944 = "DIAG" */
    uint32_t step;              /* 执行进度，卡住时看它 */
    uint32_t heartbeat;         /* 主循环心跳，增长即代表未死机 */
    uint8_t  pin_hi[8];         /* 各引脚驱动高时的回读值 */
    uint8_t  pin_lo[8];         /* 各引脚驱动低时的回读值 */
    uint8_t  idr_pa, idr_pb;    /* 空闲 IDR 快照 */
    uint8_t  bus_pd[5];         /* 输入下拉读数(PB7,PB8,PB10,PB11,PA8)：1=有外部上拉 */
    uint8_t  bus_pu[5];         /* 输入上拉读数(同序)：0=被外部拉低/短路到地 */
    uint8_t  sw_cnt;            /* 软件 I2C 扫到几个器件 */
    uint8_t  sw_addr[8];        /* 扫到的 7bit 地址 */
    uint8_t  max_partid;        /* 期望 0x15 */
    uint8_t  hw_cnt;            /* I2C2 扫到几个器件 */
    uint8_t  hw_addr[8];
    uint8_t  bb_cnt;            /* 位翻转 I2C2 扫到几个器件（独立验证） */
    uint8_t  bb_addr[8];
    uint8_t  mpu_whoami;        /* 期望 0x68 */
    uint32_t ds_low_us;         /* DS18B20 应答低电平时长(us)，0=无应答 */
};
volatile struct diag_s diag;

/* 引脚清单（与 pin_hi/pin_lo 下标一一对应） */
static const char *PIN_NAME[8] = {
    "PA5  OLED_SCL", "PA6  OLED_SDA", "PA8  DS18B20_DQ", "PA12 BUZZER",
    "PB7  MAX_SCL ", "PB8  MAX_SDA ", "PB10 I2C2_SCL", "PB11 I2C2_SDA"
};

/* 总线引脚名（与 bus_pd/bus_pu 下标对应） */
static const char *BUS_NAME[5] = {
    "PB7  MAX_SCL ", "PB8  MAX_SDA ", "PB10 I2C2_SCL",
    "PB11 I2C2_SDA", "PA8  DS18B20 "
};

/* ==========================================================================
 * 串口重定向：GCC + newlib 下 printf 走 _write，覆盖它才有效（fputc 不够）
 * ========================================================================== */
int _write(int fd, char *ptr, int len)
{
    int i;
    (void)fd;
    for (i = 0; i < len; i++) {
        while ((USART1->SR & 0x40) == 0) { }   /* 等 TXE */
        USART1->DR = (uint8_t)ptr[i];
    }
    while ((USART1->SR & 0x40) == 0) { }
    return len;
}

/* --------------------------------------------------------------------------
 * [2] 单引脚推挽驱动 + 回读自检
 *     期望：驱动 1 回读 1，驱动 0 回读 0。
 *     若不满足 -> 该引脚被外部强拉 / 短路到地或电源 / 焊接问题。
 * -------------------------------------------------------------------------- */
static void pin_test(GPIO_TypeDef *port, uint16_t pin, int idx)
{
    GPIO_InitTypeDef g;
    g.GPIO_Pin   = pin;
    g.GPIO_Speed = GPIO_Speed_50MHz;
    g.GPIO_Mode  = GPIO_Mode_Out_PP;
    GPIO_Init(port, &g);

    GPIO_SetBits(port, pin);   delay_us(20);
    diag.pin_hi[idx] = (uint8_t)GPIO_ReadInputDataBit(port, pin);
    GPIO_ResetBits(port, pin); delay_us(20);
    diag.pin_lo[idx] = (uint8_t)GPIO_ReadInputDataBit(port, pin);

    GPIO_SetBits(port, pin);   /* 释放为高，避免影响后续测试 */
}

/* --------------------------------------------------------------------------
 * [2b] 外部上拉检测（决定性）
 *     把引脚设为“输入下拉”(IPD) 再读：
 *       读到 1 -> 该线有外部上拉（4.7k 之类打赢 40k 内部下拉）=> 总线正常
 *       读到 0 -> 无外部上拉 => 软件 I2C 必然误判 ACK（驱动用浮空输入）
 * -------------------------------------------------------------------------- */
static uint8_t pin_pulldown_read(GPIO_TypeDef *port, uint16_t pin)
{
    GPIO_InitTypeDef g;
    g.GPIO_Pin   = pin;
    g.GPIO_Speed = GPIO_Speed_50MHz;
    g.GPIO_Mode  = GPIO_Mode_IPD;
    GPIO_Init(port, &g);
    delay_us(50);
    return (uint8_t)GPIO_ReadInputDataBit(port, pin);
}

/* 输入上拉读取：读到 0 说明该线被外部拉低（器件占用/短路到地/接错） */
static uint8_t pin_pullup_read(GPIO_TypeDef *port, uint16_t pin)
{
    GPIO_InitTypeDef g;
    g.GPIO_Pin   = pin;
    g.GPIO_Speed = GPIO_Speed_50MHz;
    g.GPIO_Mode  = GPIO_Mode_IPU;
    GPIO_Init(port, &g);
    delay_us(50);
    return (uint8_t)GPIO_ReadInputDataBit(port, pin);
}

/* --------------------------------------------------------------------------
 * [3] 软件 I2C（PB7/PB8）总线扫描，7bit 地址 0x08~0x77
 *     IIC_Wait_Ack 自带超时，不会卡死。
 * -------------------------------------------------------------------------- */
static void sw_i2c_scan(void)
{
    uint8_t a;
    IIC_Init();
    diag.sw_cnt = 0;
    for (a = 0x08; a <= 0x77; a++) {
        IIC_Start();
        IIC_Send_Byte((uint8_t)(a << 1));
        if (IIC_Wait_Ack() == 0) {
            if (diag.sw_cnt < 8) diag.sw_addr[diag.sw_cnt] = a;
            diag.sw_cnt++;
        }
        IIC_Stop();
        delay_us(60);
    }
}

/* --------------------------------------------------------------------------
 * [4] 硬件 I2C2（PB10/PB11）扫描 + 读寄存器
 *     全部带超时。返回 1=ACK，0=NACK/超时。
 * -------------------------------------------------------------------------- */
static void I2C_InitStructure_Load(I2C_InitTypeDef *i);   /* 前向声明 */

static void hw_i2c2_init(void)
{
    GPIO_InitTypeDef g;
    I2C_InitTypeDef  i;
    RCC_APB1PeriphClockCmd(RCC_APB1Periph_I2C2, ENABLE);
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOB, ENABLE);

    g.GPIO_Pin   = GPIO_Pin_10 | GPIO_Pin_11;
    g.GPIO_Speed = GPIO_Speed_50MHz;
    g.GPIO_Mode  = GPIO_Mode_AF_OD;
    GPIO_Init(GPIOB, &g);

    I2C_InitStructure_Load(&i);
    I2C_Init(I2C2, &i);
    I2C_Cmd(I2C2, ENABLE);
}

/* 小工具：把 I2C2 初始化结构体填好 */
static void I2C_InitStructure_Load(I2C_InitTypeDef *i)
{
    i->I2C_Mode                = I2C_Mode_I2C;
    i->I2C_DutyCycle           = I2C_DutyCycle_2;
    i->I2C_OwnAddress1         = 0x00;
    i->I2C_Ack                 = I2C_Ack_Enable;
    i->I2C_AcknowledgedAddress = I2C_AcknowledgedAddress_7bit;
    i->I2C_ClockSpeed          = 50000;
}

static uint8_t hw_i2c2_probe(uint8_t a7)
{
    volatile uint32_t t;
    uint8_t ok;

    /* 等总线空闲 */
    t = 50000; while (I2C_GetFlagStatus(I2C2, I2C_FLAG_BUSY) && --t) { }

    I2C_GenerateSTART(I2C2, ENABLE);
    t = 30000; while (I2C_GetFlagStatus(I2C2, I2C_FLAG_SB) == RESET && --t) { }
    if (!t) { I2C_GenerateSTOP(I2C2, ENABLE); return 0; }

    I2C_Send7bitAddress(I2C2, (uint8_t)(a7 << 1), I2C_Direction_Transmitter);
    t = 30000; while (I2C_GetFlagStatus(I2C2, I2C_FLAG_ADDR) == RESET && --t) { }
    ok = (t != 0);
    if (ok) { (void)I2C2->SR1; (void)I2C2->SR2; }        /* 清 ADDR */
    else    { I2C_ClearFlag(I2C2, I2C_FLAG_AF); }        /* 清 NACK */

    I2C_GenerateSTOP(I2C2, ENABLE);
    t = 50000; while (I2C_GetFlagStatus(I2C2, I2C_FLAG_BUSY) && --t) { }
    return ok;
}

/* WHO_AM_I 读取改用工程自带驱动 MPU6050_GetID()（下面 main 中调用） */

/* --------------------------------------------------------------------------
 * [4b] 位翻转(bit-bang) I2C 扫描 PB10/PB11 —— 独立于硬件 I2C2 外设，
 *      用开漏 GPIO + 外部上拉，是"该总线上到底有无器件"的最权威判据。
 * -------------------------------------------------------------------------- */
#define BB_SCL_H()  (GPIOB->BSRR = GPIO_Pin_10)
#define BB_SCL_L()  (GPIOB->BRR  = GPIO_Pin_10)
#define BB_SDA_H()  (GPIOB->BSRR = GPIO_Pin_11)
#define BB_SDA_L()  (GPIOB->BRR  = GPIO_Pin_11)
#define BB_SDA_R()  ((GPIOB->IDR & GPIO_Pin_11) ? 1 : 0)

static void bb_dly(void) { delay_us(5); }

static void bb_init(void)
{
    GPIO_InitTypeDef g;
    g.GPIO_Pin   = GPIO_Pin_10 | GPIO_Pin_11;
    g.GPIO_Speed = GPIO_Speed_50MHz;
    g.GPIO_Mode  = GPIO_Mode_Out_OD;      /* 开漏，靠外部上拉拉高 */
    GPIO_Init(GPIOB, &g);
    BB_SCL_H();
    BB_SDA_H();
    delay_us(20);
}

static void bb_start(void)
{
    BB_SDA_H(); BB_SCL_H(); bb_dly();
    BB_SDA_L(); bb_dly();
    BB_SCL_L(); bb_dly();
}

static void bb_stop(void)
{
    BB_SDA_L(); bb_dly();
    BB_SCL_H(); bb_dly();
    BB_SDA_H(); bb_dly();
}

/* 返回 1=收到 ACK，0=无应答 */
static uint8_t bb_send(uint8_t b)
{
    int i;
    uint8_t ack;
    for (i = 0; i < 8; i++) {
        if (b & 0x80) BB_SDA_H(); else BB_SDA_L();
        b <<= 1;
        bb_dly();
        BB_SCL_H(); bb_dly();
        BB_SCL_L(); bb_dly();
    }
    BB_SDA_H(); bb_dly();          /* 释放 SDA 等 SLAVE 应答 */
    BB_SCL_H(); bb_dly();
    ack = (BB_SDA_R() == 0);
    BB_SCL_L(); bb_dly();
    return ack;
}

static void bb_scan_i2c2(void)
{
    uint8_t a;
    bb_init();
    diag.bb_cnt = 0;
    for (a = 0x08; a <= 0x77; a++) {
        bb_start();
        if (bb_send((uint8_t)(a << 1))) {
            if (diag.bb_cnt < 8) diag.bb_addr[diag.bb_cnt] = a;
            diag.bb_cnt++;
        }
        bb_stop();
        delay_us(50);
    }
}

/* --------------------------------------------------------------------------
 * [5] DS18B20 应答检测（PA8），全程带超时 —— 不会像原固件那样卡死。
 *     返回低电平时长(us)；0 表示无器件应答。
 * -------------------------------------------------------------------------- */
static uint32_t ds18b20_presence(void)
{
    GPIO_InitTypeDef g;
    uint32_t t, low = 0;

    g.GPIO_Pin   = GPIO_Pin_8;
    g.GPIO_Speed = GPIO_Speed_50MHz;

    g.GPIO_Mode  = GPIO_Mode_Out_PP;
    GPIO_Init(GPIOA, &g);
    GPIO_ResetBits(GPIOA, GPIO_Pin_8);      /* 拉低 750us */
    delay_us(750);
    GPIO_SetBits(GPIOA, GPIO_Pin_8);        /* 释放 */

    g.GPIO_Mode  = GPIO_Mode_IPU;
    GPIO_Init(GPIOA, &g);

    /* 等器件拉低（应答脉冲应在 15~60us 后出现），超时 500us */
    for (t = 0; t < 500; t++) {
        if (GPIO_ReadInputDataBit(GPIOA, GPIO_Pin_8) == 0) break;
        delay_us(1);
    }
    if (t >= 500) return 0;                 /* 无应答 */

    /* 测量低电平持续时长（正常 60~240us） */
    while (GPIO_ReadInputDataBit(GPIOA, GPIO_Pin_8) == 0 && low < 1000) {
        delay_us(1);
        low++;
    }
    return (low == 0) ? 1 : low;
}

/* ==========================================================================
 * 主流程
 * ========================================================================== */
int main(void)
{
    int i;
    uint8_t *p;

    NVIC_Configuration();
    delay_init();
    uart_init(115200);

    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOA | RCC_APB2Periph_GPIOB, ENABLE);

    /* 结构清零（volatile 绕开写法） */
    p = (uint8_t *)(void *)&diag;
    for (i = 0; i < (int)sizeof(diag); i++) p[i] = 0;
    diag.magic = 0x47414944;   /* "DIAG" */

    printf("\r\n\r\n");
    printf("===== STM32F103C8 PIN DIAG =====\r\n");
    printf("uart PA9/PA10 @115200, mcu VDD ~3.2V\r\n");

    diag.idr_pa = (uint8_t)(GPIOA->IDR & 0xFF);
    diag.idr_pb = (uint8_t)(GPIOB->IDR & 0xFF);
    printf("idle IDR: PA=0x%02X  PB=0x%02X\r\n", diag.idr_pa, diag.idr_pb);
    printf("  PB9 (MAX30102_INT) = %d\r\n", (int)((GPIOB->IDR >> 9) & 1));

    /* ---------- [2] 引脚回读自检 ---------- */
    printf("\r\n[2] pin drive/readback (expect drive1->1 drive0->0)\r\n");
    pin_test(GPIOA, GPIO_Pin_5,  0);
    pin_test(GPIOA, GPIO_Pin_6,  1);
    pin_test(GPIOA, GPIO_Pin_8,  2);
    pin_test(GPIOA, GPIO_Pin_12, 3);
    pin_test(GPIOB, GPIO_Pin_7,  4);
    pin_test(GPIOB, GPIO_Pin_8,  5);
    pin_test(GPIOB, GPIO_Pin_10, 6);
    pin_test(GPIOB, GPIO_Pin_11, 7);
    for (i = 0; i < 8; i++) {
        int good = (diag.pin_hi[i] == 1 && diag.pin_lo[i] == 0);
        printf("  %s  d1=%d d0=%d  %s\r\n", PIN_NAME[i],
               diag.pin_hi[i], diag.pin_lo[i], good ? "OK" : "<-- CHECK");
    }

    /* ---------- [2b] 总线上下拉诊断（决定性） ---------- */
    printf("\r\n[2b] bus line test  (PD:1=external pull-up present, PU:0=held low)\r\n");
    diag.bus_pd[0] = pin_pulldown_read(GPIOB, GPIO_Pin_7);
    diag.bus_pd[1] = pin_pulldown_read(GPIOB, GPIO_Pin_8);
    diag.bus_pd[2] = pin_pulldown_read(GPIOB, GPIO_Pin_10);
    diag.bus_pd[3] = pin_pulldown_read(GPIOB, GPIO_Pin_11);
    diag.bus_pd[4] = pin_pulldown_read(GPIOA, GPIO_Pin_8);
    diag.bus_pu[0] = pin_pullup_read(GPIOB, GPIO_Pin_7);
    diag.bus_pu[1] = pin_pullup_read(GPIOB, GPIO_Pin_8);
    diag.bus_pu[2] = pin_pullup_read(GPIOB, GPIO_Pin_10);
    diag.bus_pu[3] = pin_pullup_read(GPIOB, GPIO_Pin_11);
    diag.bus_pu[4] = pin_pullup_read(GPIOA, GPIO_Pin_8);
    for (i = 0; i < 5; i++) {
        const char *verdict;
        if      (diag.bus_pu[i] == 1 && diag.bus_pd[i] == 1) verdict = "OK: pull-up present, idle high";
        else if (diag.bus_pu[i] == 1 && diag.bus_pd[i] == 0) verdict = "FLOATING: NO pull-up";
        else if (diag.bus_pu[i] == 0 && diag.bus_pd[i] == 0) verdict = "HELD LOW: short/wrong wiring";
        else                                                 verdict = "BAD";
        printf("  %s PU=%d PD=%d  %s\r\n", BUS_NAME[i],
               diag.bus_pu[i], diag.bus_pd[i], verdict);
    }
    diag.step = 2;

    /* ---------- [3] 软件 I2C 扫描 ---------- */
    printf("\r\n[3] soft I2C scan  SCL=PB7 SDA=PB8  (MAX30102 @0x57)\r\n");
    sw_i2c_scan();
    if (diag.sw_cnt == 0) {
        printf("  no device ACK  <-- 检查 PB7/PB8 接线、3.3V、GND\r\n");
    } else {
        printf("  found %u device(s):", (unsigned)diag.sw_cnt);
        for (i = 0; i < 8 && i < (int)diag.sw_cnt; i++)
            printf(" 0x%02X", diag.sw_addr[i]);
        printf("\r\n");
    }
    if (diag.sw_cnt) {
        uint8_t pid = 0;
        IIC_Read_One_Byte(0xAE, 0xFF, &pid);   /* MAX30102 写地址 0xAE，PART_ID 在 0xFF */
        diag.max_partid = pid;
        printf("  MAX30102 PART_ID(0xFF) = 0x%02X  %s\r\n",
               diag.max_partid, (diag.max_partid == 0x15) ? "OK" : "<-- CHECK");
    }
    diag.step = 3;

    /* ---------- [4] 硬件 I2C2 扫描 ---------- */
    printf("\r\n[4] hw I2C2 scan  SCL=PB10 SDA=PB11  (MPU6050 @0x68)\r\n");
    hw_i2c2_init();
    diag.hw_cnt = 0;
    for (i = 0x08; i <= 0x77; i++) {
        if (hw_i2c2_probe((uint8_t)i)) {
            if (diag.hw_cnt < 8) diag.hw_addr[diag.hw_cnt] = (uint8_t)i;
            diag.hw_cnt++;
        }
    }
    if (diag.hw_cnt == 0) {
        printf("  no device ACK  <-- 检查 PB10/PB11 接线、3.3V、GND\r\n");
    } else {
        printf("  found %u device(s):", (unsigned)diag.hw_cnt);
        for (i = 0; i < 8 && i < (int)diag.hw_cnt; i++)
            printf(" 0x%02X", diag.hw_addr[i]);
        printf("\r\n");
    }
    /* [4b] 位翻转独立扫描 PB10/PB11 */
    printf("\r\n[4b] bit-bang I2C scan  SCL=PB10 SDA=PB11 (independent)\r\n");
    bb_scan_i2c2();
    if (diag.bb_cnt == 0) {
        printf("  no device ACK (bit-bang)  <-- 该总线确实没有器件\r\n");
    } else {
        printf("  found %u device(s):", (unsigned)diag.bb_cnt);
        for (i = 0; i < 8 && i < (int)diag.bb_cnt; i++)
            printf(" 0x%02X", diag.bb_addr[i]);
        printf("\r\n");
    }

    /* 用工程自带驱动重新初始化并读 WHO_AM_I（比自写读时序更可靠） */
    MPU6050_Init();
    diag.mpu_whoami = MPU6050_GetID();
    printf("  MPU6050 WHO_AM_I(0x75) = 0x%02X  %s\r\n",
           diag.mpu_whoami, (diag.mpu_whoami == 0x68) ? "OK" : "<-- CHECK");
    diag.step = 4;

    /* ---------- [5] DS18B20 应答 ---------- */
    printf("\r\n[5] DS18B20 presence  DQ=PA8  (timeout protected)\r\n");
    diag.ds_low_us = ds18b20_presence();
    if (diag.ds_low_us == 0)
        printf("  NO presence pulse  <-- DS18B20 未接/接错/缺上拉\r\n");
    else
        printf("  presence pulse %u us  OK\r\n", (unsigned)diag.ds_low_us);
    diag.step = 5;

    /* ---------- OLED 显示结论 ---------- */
    OLED_Init();
    OLED_Clear();
    OLED_ShowString(0, 0,  "DIAG RUN", OLED_8X16);
    OLED_Printf(0, 16, OLED_8X16, "MAX %02X", diag.max_partid);
    OLED_Printf(0, 32, OLED_8X16, "MPU %02X", diag.mpu_whoami);
    OLED_Printf(0, 48, OLED_8X16, "DS  %s", diag.ds_low_us ? "OK" : "NO");
    OLED_Update();
    diag.step = 6;

    /* ---------- 汇总 + 心跳循环 ---------- */
    printf("\r\n===== SUMMARY =====\r\n");
    printf(" softI2C(PB7/PB8) : %u dev  MAX_PART_ID=0x%02X %s\r\n",
           (unsigned)diag.sw_cnt, diag.max_partid,
           (diag.max_partid == 0x15) ? "[OK]" : "[FAIL]");
    printf(" hwI2C2(PB10/PB11): %u dev  MPU_WHOAMI =0x%02X %s\r\n",
           (unsigned)diag.hw_cnt, diag.mpu_whoami,
           (diag.mpu_whoami == 0x68) ? "[OK]" : "[FAIL]");
    printf(" DS18B20(PA8)     : %u us %s\r\n", (unsigned)diag.ds_low_us,
           diag.ds_low_us ? "[OK]" : "[FAIL]");
    printf(" pin readback     : ");
    for (i = 0; i < 8; i++)
        printf("%s%d ", PIN_NAME[i], (diag.pin_hi[i] == 1 && diag.pin_lo[i] == 0));
    printf("\r\n===================\r\n");
    printf("(heartbeat below; re-open serial any time)\r\n");

    while (1) {
        diag.heartbeat++;
        printf("hb %lu  swI2C=%u MAX=0x%02X  hwI2C2=%u MPU=0x%02X  DS=%u\r\n",
               (unsigned long)diag.heartbeat, (unsigned)diag.sw_cnt,
               diag.max_partid, (unsigned)diag.hw_cnt, diag.mpu_whoami,
               (unsigned)diag.ds_low_us);
        delay_ms(2000);
    }
}

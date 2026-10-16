/* =============================================================================
 * STM32F103C8 硬件引脚 / 元件 · 【实时】诊断固件（独立入口，不修改仓库原码）
 *
 * 构建：  make LIVE=1          -> build/LIVE.elf / .hex / .bin
 * 烧录：  make LIVE=1 flash
 *
 * 与 diag_main.c（一次性）的区别：
 *   本固件【持续循环、实时刷新】—— 你可以边接线边看 OLED / 串口，
 *   检测到某个元件"从无到有"时会【蜂鸣提示】，三个元件全部就绪时连续三声。
 *
 * 实时监测对象：
 *   MAX30102  软 I2C  SCL=PB7  SDA=PB8   INT=PB9    期望地址 0x57, PART_ID=0x15
 *   MPU6050   硬 I2C2 SCL=PB10 SDA=PB11             期望地址 0x68, WHO_AM_I=0x68
 *   DS18B20   单总线  DQ=PA8                         期望有应答脉冲(60~240us)
 *   OLED      软 I2C  SCL=PA5  SDA=PA6               目视确认（写-only）
 *   蜂鸣器    PA12（低电平触发）
 *
 * 启动阶段额外做【全引脚上拉普查】：
 *   对 PA0~PA15 / PB0~PB15（跳过 SWD/JTAG/UART）逐个设为“输入下拉/上拉”读回，
 *   找出【哪个引脚上有外部上拉电阻】——用于定位 I2C 总线和单总线器件实际接在哪。
 *
 * 输出通道：
 *   1) OLED 实时状态（最直观）
 *   2) 串口 USART1 PA9(TX)/PA10(RX) @115200
 *   3) 内存结构 live（GDB 直接读：p live）
 *
 * 设计要点：所有等待都带超时，任一元件缺失都不会拖死整机。
 * ========================================================================== */
#include "stm32f10x.h"
#include "sys.h"
#include "delay.h"
#include "usart.h"
#include "myiic.h"
#include "MPU6050_Reg.h"
#include "MPU6050.h"
#include "OLED.h"
#include "Buzzer.h"
#include <stdio.h>
#include <stdint.h>

/* --------------------------------------------------------------------------
 * 实时状态结构（volatile，GDB 可直接读）
 * -------------------------------------------------------------------------- */
struct live_s {
    uint32_t magic;             /* 0x4C495645 = "LIVE" */
    uint32_t cycle;             /* 循环计数，增长即代表活着 */

    uint8_t  pin_hi[8];         /* 启动时引脚回读自检（1=驱动高读回高） */
    uint8_t  pin_lo[8];         /* 同上（0=驱动低读回低） */
    uint8_t  bus_pd[5];         /* 启动时：输入下拉读数 1=有外部上拉 */
    uint8_t  bus_pu[5];         /* 启动时：输入上拉读数 0=被外部拉低 */

    uint16_t surv_pu_a;         /* 普查：PA 各引脚有外部上拉的位掩码 */
    uint16_t surv_pu_b;         /* 普查：PB 各引脚有外部上拉的位掩码 */
    uint16_t surv_lo_a;         /* 普查：PA 各引脚被外部拉低的位掩码 */
    uint16_t surv_lo_b;         /* 普查：PB 各引脚被外部拉低的位掩码 */

    uint8_t  max_found;         /* MAX30102 就绪 */
    uint8_t  max_weak;          /* 1 = 靠内部上拉才通（模块缺外部上拉） */
    uint8_t  max_addr;          /* 实际扫到的地址(7bit) */
    uint8_t  max_partid;        /* 期望 0x15 */

    uint8_t  mpu_found;         /* MPU6050 就绪 */
    uint8_t  mpu_id;            /* WHO_AM_I，期望 0x68 */

    uint8_t  ds_found;          /* DS18B20 有应答 */
    uint8_t  ds_us;             /* 应答低电平时长(us)，截断到 0~255 */
    uint8_t  ds_idle;           /* PA8 空闲电平(IPU 下)：1=浮空/有上拉, 0=被拉低 */
    uint8_t  load_ref;          /* 参考空脚(PB5) 负载等级 0=空 1/2=接线 */
    uint8_t  load_pa8;          /* PA8  DS18B20_DQ 负载等级 */
    uint8_t  load_pb7;          /* PB7  MAX_SCL    负载等级 */
    uint8_t  load_pb8;          /* PB8  MAX_SDA    负载等级 */
    uint8_t  load_pb9;          /* PB9  MAX_INT    负载等级 */
    uint16_t ds_scan_pa;        /* 全引脚扫描：PA 上发现 DS18B20 应答的位掩码 */
    uint16_t ds_scan_pb;        /* 全引脚扫描：PB 上发现 DS18B20 应答的位掩码 */

    uint16_t rc[10];            /* RC 上升时间探针：紧循环计数（越大=引脚上挂的线越长/负载越大） */
    uint16_t rc_att_a;          /* 全引脚 RC 扫描：PA 上“挂了线”的引脚位掩码 */
    uint16_t rc_att_b;          /* 全引脚 RC 扫描：PB 上“挂了线”的引脚位掩码 */
    uint16_t rc_all_a[16];      /* 全引脚 RC 原始计数（PA0..PA15） */
    uint16_t rc_all_b[16];      /* 全引脚 RC 原始计数（PB0..PB15） */
    uint16_t rc_wire[4];        /* 实时：PB7 / PB8 / PB9 / PA8 的 RC 计数（边插边看） */

    uint8_t  wk_cnt;            /* PB7/PB8 全地址扫描：应答的 I2C 地址个数 */
    uint8_t  wk_addr[4];        /* 前 4 个应答地址 */

    uint8_t  bus2_cnt;          /* PB10/PB11(MPU 总线)全地址扫描：应答个数 */
    uint8_t  bus2_addr[8];      /* 应答地址列表 */
    uint8_t  bus2_max_ack;      /* 该总线上 MAX30102(0x57) 是否应答 */

    uint8_t  all_ok;            /* 三个全就绪 */
};
volatile struct live_s live;

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
 * 蜂鸣器：PA12 低电平触发
 * -------------------------------------------------------------------------- */
static void beep(uint16_t ms)
{
    Buzzer_on();
    delay_ms(ms);
    Buzzer_off();
}

/* --------------------------------------------------------------------------
 * 引脚推挽驱动 + 回读自检（启动时跑一次）
 * -------------------------------------------------------------------------- */
static void pin_test(GPIO_TypeDef *port, uint16_t pin, int idx)
{
    GPIO_InitTypeDef g;
    g.GPIO_Pin   = pin;
    g.GPIO_Speed = GPIO_Speed_50MHz;
    g.GPIO_Mode  = GPIO_Mode_Out_PP;
    GPIO_Init(port, &g);

    GPIO_SetBits(port, pin);   delay_us(20);
    live.pin_hi[idx] = (uint8_t)GPIO_ReadInputDataBit(port, pin);
    GPIO_ResetBits(port, pin); delay_us(20);
    live.pin_lo[idx] = (uint8_t)GPIO_ReadInputDataBit(port, pin);

    GPIO_SetBits(port, pin);
}

/* 输入下拉读取：读到 1 说明该线有外部上拉 */
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
 * 全引脚上拉普查：逐引脚判断“有无外部上拉/被拉低”
 *   跳过 PA9/PA10（串口）与 PA13/PA14/PA15、PB3/PB4（SWD/JTAG）
 * -------------------------------------------------------------------------- */
static void pin_survey_port(GPIO_TypeDef *port, const char *name,
                            uint16_t skipmask, uint16_t *pu_mask, uint16_t *lo_mask)
{
    int i;
    uint16_t pu = 0, lo = 0;
    for (i = 0; i < 16; i++) {
        uint16_t bit = (uint16_t)(1u << i);
        uint8_t d, u;
        if (skipmask & bit) continue;
        d = pin_pulldown_read(port, bit);   /* 1 = 外部上拉存在 */
        u = pin_pullup_read(port, bit);     /* 0 = 被外部拉低   */
        if (d) {
            pu |= bit;
            printf("  %s%-2d : external PULL-UP detected\r\n", name, i);
        }
        if (!u) {
            lo |= bit;
            printf("  %s%-2d : held LOW (device/short)\r\n", name, i);
        }
    }
    *pu_mask = pu;
    *lo_mask = lo;
}

/* --------------------------------------------------------------------------
 * “内部上拉弱 I2C”（PB7=SCL / PB8=SDA）—— 用于模块【没有外部上拉】的场合
 *   STM32 输出开漏模式下内部上拉被禁用，无法既驱动又上拉；这里在“释放为高”
 *   时把引脚切成 IPU(输入上拉, ~40k)，靠内部弱上拉把线拉高，低速(相位 4us)
 *   下通常足以完成一次通信。若模块本就有外部上拉，则用不到本路径。
 * -------------------------------------------------------------------------- */
static void wk_mode(uint16_t pin, GPIOMode_TypeDef m)
{
    GPIO_InitTypeDef g;
    g.GPIO_Pin   = pin;
    g.GPIO_Speed = GPIO_Speed_50MHz;
    g.GPIO_Mode  = m;
    GPIO_Init(GPIOB, &g);
}
#define WK_SCL_H()  (wk_mode(GPIO_Pin_7, GPIO_Mode_IPU))
#define WK_SCL_L()  (wk_mode(GPIO_Pin_7, GPIO_Mode_Out_OD), GPIO_ResetBits(GPIOB, GPIO_Pin_7))
#define WK_SDA_H()  (wk_mode(GPIO_Pin_8, GPIO_Mode_IPU))
#define WK_SDA_L()  (wk_mode(GPIO_Pin_8, GPIO_Mode_Out_OD), GPIO_ResetBits(GPIOB, GPIO_Pin_8))
#define WK_SDA_R()  ((GPIOB->IDR & GPIO_Pin_8) ? 1 : 0)

static void wk_dly(void) { delay_us(4); }

static void wk_start(void)
{
    WK_SDA_H(); WK_SCL_H(); wk_dly();
    WK_SDA_L(); wk_dly();
    WK_SCL_L(); wk_dly();
}

static void wk_stop(void)
{
    WK_SDA_L(); wk_dly();
    WK_SCL_H(); wk_dly();
    WK_SDA_H(); wk_dly();
}

static uint8_t wk_send(uint8_t b)
{
    int i; uint8_t ack;
    for (i = 0; i < 8; i++) {
        if (b & 0x80) WK_SDA_H(); else WK_SDA_L();
        b <<= 1; wk_dly();
        WK_SCL_H(); wk_dly();
        WK_SCL_L(); wk_dly();
    }
    WK_SDA_H(); wk_dly();       /* 释放 SDA 等应答 */
    WK_SCL_H(); wk_dly();
    ack = (WK_SDA_R() == 0);
    WK_SCL_L(); wk_dly();
    return ack;
}

static uint8_t wk_read(uint8_t ack)
{
    int i; uint8_t d = 0;
    WK_SDA_H(); wk_dly();       /* 释放 SDA 由从机驱动 */
    for (i = 0; i < 8; i++) {
        d <<= 1;
        WK_SCL_H(); wk_dly();
        if (WK_SDA_R()) d |= 1;
        WK_SCL_L(); wk_dly();
    }
    if (ack) WK_SDA_L(); else WK_SDA_H();
    wk_dly(); WK_SCL_H(); wk_dly(); WK_SCL_L(); wk_dly();
    return d;
}

/* 读一个寄存器：有 ACK 成功返回 1，*val 为读到的值 */
static uint8_t wk_probe_reg(uint8_t a7, uint8_t reg, uint8_t *val)
{
    wk_start();
    if (!wk_send((uint8_t)(a7 << 1)))       { wk_stop(); return 0; }
    if (!wk_send(reg))                      { wk_stop(); return 0; }
    wk_start();
    if (!wk_send((uint8_t)((a7 << 1) | 1))) { wk_stop(); return 0; }
    *val = wk_read(0);
    wk_stop();
    return 1;
}

/* 在 PB7/PB8 上做全地址 I2C 扫描（用内部上拉，模块没外部上拉也能测）
 * 返回 ACK 的地址个数；把前 4 个地址存入 out */
static uint8_t wk_scan(uint8_t *out, uint8_t out_max)
{
    uint8_t a, cnt = 0, v;
    for (a = 0x08; a <= 0x77; a++) {
        if (wk_probe_reg(a, 0x00, &v)) {
            if (cnt < out_max) out[cnt] = a;
            cnt++;
            printf("  >>> I2C ACK at 0x%02X (reg0=0x%02X)\r\n", a, v);
        }
    }
    return cnt;
}

/* --------------------------------------------------------------------------
 * MAX30102：软 I2C(PB7/PB8) 探测 + 读 PART_ID(0xFF)
 *   先试工程原生软 I2C（依赖模块外部上拉）；
 *   失败再试“内部上拉弱 I2C”（模块没上拉也能通，40k 内部上拉，低速可用）。
 *   仅当 PART_ID == 0x15 才认定“就绪”（避免无上拉时浮空误判 ACK）。
 * -------------------------------------------------------------------------- */
static uint8_t max_probe(void)
{
    uint8_t pid = 0x00;

    live.max_addr = 0x57;
    live.max_weak = 0;

    /* --- 方式1：工程原生软 I2C --- */
    IIC_Init();                 /* 恢复 PB7/PB8 为输出 */
    IIC_Start();
    IIC_Send_Byte(0xAE);        /* MAX30102 写地址 0x57<<1 */
    if (IIC_Wait_Ack() == 0) {
        IIC_Stop();
        IIC_Read_One_Byte(0xAE, 0xFF, &pid);
        if (pid == 0x15) { live.max_partid = pid; return 1; }
    } else {
        IIC_Stop();
    }

    /* --- 方式2：内部上拉弱 I2C --- */
    pid = 0x00;
    if (wk_probe_reg(0x57, 0xFF, &pid)) {
        live.max_partid = pid;
        if (pid == 0x15) { live.max_weak = 1; return 1; }
    }
    live.max_partid = pid;
    return 0;
}

/* --------------------------------------------------------------------------
 * MPU6050：位翻转(bit-bang) 开漏扫描 PB10/PB11 —— 不依赖 I2C2 外设，最可靠
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
    BB_SDA_H(); bb_dly();
    BB_SCL_H(); bb_dly();
    ack = (BB_SDA_R() == 0);
    BB_SCL_L(); bb_dly();
    return ack;
}

static uint8_t bb_probe(uint8_t a7)
{
    uint8_t ack;
    bb_start();
    ack = bb_send((uint8_t)(a7 << 1));
    bb_stop();
    return ack;
}

/* --------------------------------------------------------------------------
 * 【隔离测试】在 PB10/PB11 总线（MPU6050 已证明可用）上扫全部 I2C 地址。
 *   目的：把「MAX30102 模块本身坏 / 没供电」与「PB7/PB8 引脚或走线坏」分开。
 *   做法：把 MAX30102 的 SCL/SDA 临时改插到 PB10/PB11（可与 MPU 并联，
 *         地址不同互不冲突：MAX30102=0x57，MPU6050=0x68）。
 *   若此总线能扫到 0x57 -> 模块是好的，问题在 PB7/PB8 侧。
 *   若只能扫到 0x68      -> 模块坏或没供电。
 * -------------------------------------------------------------------------- */
static uint8_t bb_scan_bus2(uint8_t *out, uint8_t out_max)
{
    uint8_t cnt = 0, a;
    bb_init();
    for (a = 0x08; a <= 0x77; a++) {
        if (bb_probe(a)) {
            if (cnt < out_max) out[cnt] = a;
            cnt++;
            printf("  BUS2 (PB10/PB11) ACK @ 0x%02X\r\n", a);
        }
    }
    return cnt;
}

/* --------------------------------------------------------------------------
 * DS18B20 应答检测（PA8），全程带超时 —— 不会像原固件那样卡死。
 * -------------------------------------------------------------------------- */
/* 更稳健的 DS18B20 应答检测（不依赖外部上拉，也兼容裸 TO-92 / 防水探头）：
 *   1) 先测空闲电平（IPU）—— 用于区分“器件拉低”与“浮空/上拉”
 *   2) 开漏拉低 low_us 构成复位脉冲，再释放（高阻，靠上拉拉高）
 *   3) 以 10us 步长采样 0~800us：找低电平(应答)并测其持续时长
 * 返回：0=无应答；>0=低电平持续(us 粗估)。 *idle 回传空闲电平。
 * 说明：采样步长用 10us 而非 1us，可规避 delay_us 微小误差导致的“窗口过短”漏检。 */
static uint32_t ds_presence_ex(GPIO_TypeDef *port, uint16_t pin,
                               uint16_t low_us, uint8_t *idle)
{
    GPIO_InitTypeDef g;
    uint32_t k, low;

    g.GPIO_Pin   = pin;
    g.GPIO_Speed = GPIO_Speed_50MHz;

    /* 1) 空闲电平 */
    g.GPIO_Mode = GPIO_Mode_IPU;
    GPIO_Init(port, &g);
    delay_us(60);
    if (idle) *idle = (uint8_t)GPIO_ReadInputDataBit(port, pin);

    /* 2) 复位脉冲：推挽拉低 low_us */
    g.GPIO_Mode = GPIO_Mode_Out_PP;
    GPIO_Init(port, &g);
    GPIO_ResetBits(port, pin);
    delay_us(low_us);

    /* 3) 推挽主动驱高 2us —— 关键：避免"刚释放还低着"的 RC 拖尾被误判成应答 */
    GPIO_SetBits(port, pin);
    delay_us(2);

    /* 4) 转输入上拉，采样 0~800us 找低电平脉冲(应答) */
    g.GPIO_Mode = GPIO_Mode_IPU;
    GPIO_Init(port, &g);

    for (k = 0; k < 80; k++) {
        if (GPIO_ReadInputDataBit(port, pin) == 0) break;
        delay_us(10);
    }
    if (k >= 80) return 0;                  /* 无应答 */

    low = 0;
    while (GPIO_ReadInputDataBit(port, pin) == 0 && low < 100) {
        delay_us(10);
        low++;
    }
    return (low == 0) ? 10 : (low * 10);
}

/* --------------------------------------------------------------------------
 * 引脚“负载”探针：判断某根针上“到底插没插东西”。
 *   原理：推挽拉低 50us(充分放电) -> 转 IPU(仅内部~40k上拉) ->
 *         在 ~2us 与 ~10us 两个时刻采样。空脚(几 pF)瞬间回高；
 *         接了线/模块的脚(几十~几百 pF)回高明显更慢。
 *   返回 0=空脚(快) / 1=中等 / 2=重负载(基本确定接了线)。
 *   同一轮内必须与参考空脚(load_ref)横向对比才有意义。
 * -------------------------------------------------------------------------- */
static uint8_t pin_load_class(GPIO_TypeDef *port, uint16_t pin)
{
    GPIO_InitTypeDef g;
    uint8_t r1, r2, lv = 0;

    g.GPIO_Pin   = pin;
    g.GPIO_Speed = GPIO_Speed_50MHz;

    g.GPIO_Mode = GPIO_Mode_Out_PP;
    GPIO_Init(port, &g);
    GPIO_ResetBits(port, pin);
    delay_us(50);                            /* 充分放电 */

    g.GPIO_Mode = GPIO_Mode_IPU;             /* 松手，靠内部弱上拉回高 */
    GPIO_Init(port, &g);

    delay_us(2);  r1 = (uint8_t)GPIO_ReadInputDataBit(port, pin);
    delay_us(8);  r2 = (uint8_t)GPIO_ReadInputDataBit(port, pin);

    if (!r1) lv++;
    if (!r2) lv++;
    return lv;                               /* 0=空 1=中 2=重负载 */
}

static uint32_t ds_presence(void)
{
    return ds_presence_ex(GPIOA, GPIO_Pin_8, 500, &live.ds_idle);
}

/* --------------------------------------------------------------------------
 * 【RC 上升时间探针 v2】—— 直接操作 CRL/CRH 寄存器，避免 GPIO_Init 的耗时
 *   遮盖被测电容的充电过程。
 *   步骤(每引脚重复 8 次)：开漏输出拉低 300us 充分放电 -> 一次性把该引脚的
 *   配置位切回“输入上拉”(仅内部 ~40k) -> 立刻用紧循环数“多少圈才读到高”。
 *   空脚(几 pF) 只需几圈；插了杜邦线/模块(几十~几百 pF) 需几十~几百圈。
 *   取 8 次中【最小值】(无干扰的那次)。与已知空脚对比即可判定“插没插线”。
 *   注：调用前需已 RCC 使能 GPIOA/GPIOB 时钟。
 * -------------------------------------------------------------------------- */
static uint32_t rc_fast(GPIO_TypeDef *port, uint16_t pinidx)
{
    volatile uint32_t *cr;
    uint32_t shift, tmp, n, best = 0xFFFFFFFFu;
    uint16_t bit = (uint16_t)(1u << pinidx);
    int rep;

    if (pinidx < 8) { cr = &port->CRL; shift = (uint32_t)pinidx * 4u; }
    else            { cr = &port->CRH; shift = (uint32_t)(pinidx - 8) * 4u; }

    for (rep = 0; rep < 8; rep++) {
        /* 输入模式 + CNF=10(上下拉) + ODR=0 -> 内部下拉(40k)，把线拉低放电 */
        tmp  = *cr;
        tmp &= ~(0xFu << shift);
        tmp |=  (0x8u << shift);
        *cr  = tmp;
        port->BRR = bit;              /* ODR=0 : 在输入上下拉模式下 = 下拉 */
        delay_us(300);

        n = 0;
        port->BSRR = bit;             /* ODR=1 : 切换为内部上拉(40k)，开始充电 */
        while ((port->IDR & bit) == 0 && n < 200000u) n++;
        if (n < best) best = n;
    }
    return best;
}

/* 被测引脚清单：前两个与最后一个是【已知空脚】作基线 */
static GPIO_TypeDef *const RC_PORT[10] = {
    GPIOA, GPIOA, GPIOB, GPIOB, GPIOB, GPIOB, GPIOB, GPIOA, GPIOA, GPIOB
};
static const uint8_t RC_IDX[10] = {
    0, 5, 5, 7, 8, 9, 10, 6, 8, 11
};
static const char *RC_NM[10] = {
    "PA0  (bare ref)", "PA5  OLED_SCL ", "PB5  (bare ref)", "PB7  MAX_SCL  ",
    "PB8  MAX_SDA  ", "PB9  MAX_INT  ", "PB10 I2C2_SCL ", "PA6  OLED_SDA ",
    "PA8  DS18B20  ", "PB11 I2C2_SDA "
};

static void rc_test(void)
{
    int i;
    printf("\r\n[RC] wire-presence probe v2 (tight-loop counts; higher = wire/module attached)\r\n");
    for (i = 0; i < 10; i++) {
        live.rc[i] = (uint16_t)rc_fast(RC_PORT[i], RC_IDX[i]);
        printf("  RC %-15s = %u\r\n", RC_NM[i], (unsigned)live.rc[i]);
    }
    printf("  hint: bare refs PA0/PB5 should be tiny; wired pins much larger\r\n");
}

/* 全引脚 RC 扫描：列出所有“挂了线/模块”的引脚（裸脚基线≈2） */
static void rc_scan_all(void)
{
    int i;
    uint32_t c;
    printf("\r\n[RC SCAN ALL] any pin with a wire attached? (bare ref ~2)\r\n");
    live.rc_att_a = 0;
    live.rc_att_b = 0;
    for (i = 0; i < 16; i++) {
        if (i == 13 || i == 14) { live.rc_all_a[i] = 0; continue; }  /* SWD 不动 */
        c = rc_fast(GPIOA, (uint16_t)i);
        live.rc_all_a[i] = (uint16_t)c;
        if (c >= 5u) { live.rc_att_a |= (uint16_t)(1u << i);
                       printf("  PA%-2d = %-6u <-- ATTACHED\r\n", i, (unsigned)c); }
    }
    for (i = 0; i < 16; i++) {
        c = rc_fast(GPIOB, (uint16_t)i);
        live.rc_all_b[i] = (uint16_t)c;
        if (c >= 5u) { live.rc_att_b |= (uint16_t)(1u << i);
                       printf("  PB%-2d = %-6u <-- ATTACHED\r\n", i, (unsigned)c); }
    }
    printf("  (only pins above threshold listed; none listed = no stray wires)\r\n");
}

/* 全引脚扫描：找出 DS18B20 到底接在哪一根针（用内部上拉，无需外部上拉） */
static void ds_scan_all(void)
{
    int i;
    for (i = 0; i < 16; i++) {
        uint16_t bit = (uint16_t)(1u << i);
        uint8_t  idle = 1;
        uint32_t us;
        if (0x6000 & bit) continue;         /* 仅跳过 PA13/PA14(SWD)，避免掉调试连接 */
        us = ds_presence_ex(GPIOA, bit, 500, &idle);
        if (idle == 0) {
            printf("  PA%-2d idle=LOW (held down / short) - not a device\r\n", i);
            continue;
        }
        if (us) {
            printf("  >>> DS18B20 presence on PA%d (%u us)\r\n", i, (unsigned)us);
            live.ds_scan_pa |= bit;
        }
    }
    for (i = 0; i < 16; i++) {
        uint16_t bit = (uint16_t)(1u << i);
        uint8_t  idle = 1;
        uint32_t us;
        /* PB 全扫（含 PB3/PB4 JTAG 脚，无 JTAG 设备时安全） */
        us = ds_presence_ex(GPIOB, bit, 500, &idle);
        if (idle == 0) {
            printf("  PB%-2d idle=LOW (held down / short) - not a device\r\n", i);
            continue;
        }
        if (us) {
            printf("  >>> DS18B20 presence on PB%d (%u us)\r\n", i, (unsigned)us);
            live.ds_scan_pb |= bit;
        }
    }
    if (live.ds_scan_pa == 0 && live.ds_scan_pb == 0)
        printf("  >>> DS18B20 NOT found on any pin (check VCC/GND & DQ)\r\n");
}

/* --------------------------------------------------------------------------
 * OLED 实时刷新
 *   第 1 行：DIAGLIVE + 循环计数
 *   第 2 行：MAX 状态 + PART_ID / MPU 状态 + WHO_AM_I
 *   第 3 行：DS 状态 + 应答时长
 *   第 4 行(6x8)：四根关键引脚的【实时 RC 计数】= 插线指示
 *                 裸脚≈2；插上线/模块会明显变大。边接线边看这行即可。
 * -------------------------------------------------------------------------- */
static void oled_show(void)
{
    char buf[40];

    OLED_Clear();

    OLED_ShowString(0, 0, "DIAGLIVE", OLED_8X16);
    OLED_ShowNum(80, 0, live.cycle % 1000u, 3, OLED_8X16);

    /* MAX30102：两路总线同时判（M1=PB7/PB8 原总线，M2=PB10/PB11 MPU总线） */
    sprintf(buf, "M1%s M2%s ack%u", live.max_found ? "OK" : "--",
            live.bus2_max_ack ? "OK" : "--", (unsigned)live.wk_cnt);
    OLED_ShowString(0, 16, buf, OLED_8X16);

    sprintf(buf, "MPU%s %02X DS%s %3u", live.mpu_found ? "." : "-", live.mpu_id,
            live.ds_found ? "." : "-", (unsigned)live.ds_us);
    OLED_ShowString(0, 32, buf, OLED_8X16);

    /* 第 4/5 行：关键引脚 RC 计数（裸脚≈2；>10 = 这根针上真挂了线）。
       A5/A6/B10/B11 有器件时为 0（外部上拉把线瞬间拉高）。 */
    {
        unsigned a = live.rc_wire[0] > 999 ? 999u : live.rc_wire[0];
        unsigned b = live.rc_wire[1] > 999 ? 999u : live.rc_wire[1];
        unsigned c = live.rc_wire[2] > 999 ? 999u : live.rc_wire[2];
        unsigned d = live.rc_wire[3] > 999 ? 999u : live.rc_wire[3];
        sprintf(buf, "P7=%-3u P8=%-3u P9=%-3u A8=%-3u", a, b, c, d);
    }
    OLED_ShowString(0, 48, buf, OLED_6X8);

    {
        unsigned a5  = live.rc_all_a[5]  > 999 ? 999u : live.rc_all_a[5];
        unsigned a6  = live.rc_all_a[6]  > 999 ? 999u : live.rc_all_a[6];
        unsigned b10 = live.rc_all_b[10] > 999 ? 999u : live.rc_all_b[10];
        unsigned b11 = live.rc_all_b[11] > 999 ? 999u : live.rc_all_b[11];
        sprintf(buf, "A5=%-3u A6=%-3u B10=%-3u B11=%-3u", a5, a6, b10, b11);
    }
    OLED_ShowString(0, 56, buf, OLED_6X8);

    OLED_Update();
}

/* --------------------------------------------------------------------------
 * 【周期全量复扫】每 ~3 秒跑一次，让用户边插线边看结果，无需重新烧录。
 *   - 外部上拉普查：哪根针上真的挂了器件
 *   - 全引脚 RC 电容扫描：哪根针上有线
 *   - PB7/PB8 总线（MAX30102 原定总线）全地址 I2C 扫描
 *   - PB10/PB11 总线（MPU6050 已验证可用）全地址 I2C 扫描
 *     若把 MAX30102 的 SCL/SDA 挪到 PB10/PB11，这里会立刻出现 0x57
 * -------------------------------------------------------------------------- */
static void periodic_scan(void)
{
    int i;

    pin_survey_port(GPIOA, "PA", 0xE600, &live.surv_pu_a, &live.surv_lo_a);
    pin_survey_port(GPIOB, "PB", 0x0018, &live.surv_pu_b, &live.surv_lo_b);

    rc_scan_all();

    live.wk_cnt = wk_scan(live.wk_addr, 4);

    live.bus2_cnt = bb_scan_bus2(live.bus2_addr, 8);
    live.bus2_max_ack = 0;
    for (i = 0; i < 8 && i < (int)live.bus2_cnt; i++)
        if (live.bus2_addr[i] == 0x57) live.bus2_max_ack = 1;

    printf("[RESCAN] PApu=0x%04X PBpu=0x%04X | M1(PB7/PB8)=%ud  M2(PB10/PB11)=%ud  MAX@M2=%s\r\n",
           (unsigned)live.surv_pu_a, (unsigned)live.surv_pu_b,
           (unsigned)live.wk_cnt, (unsigned)live.bus2_cnt,
           live.bus2_max_ack ? "YES" : "no");
}

/* ==========================================================================
 * 主流程
 * ========================================================================== */
int main(void)
{
    int i;
    uint8_t *p;
    uint8_t prev_max = 0, prev_mpu = 0, prev_ds = 0, prev_all = 0;

    NVIC_Configuration();
    delay_init();
    uart_init(115200);
    Buzzer_init();
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOA | RCC_APB2Periph_GPIOB, ENABLE);

    /* 结构清零 */
    p = (uint8_t *)(void *)&live;
    for (i = 0; i < (int)sizeof(live); i++) p[i] = 0;
    live.magic = 0x4C495645;   /* "LIVE" */

    printf("\r\n\r\n");
    printf("===== STM32F103C8 LIVE PIN DIAG =====\r\n");
    printf("uart PA9/PA10 @115200\r\n");

    /* --- 蜂鸣器自检：两声短鸣 --- */
    printf("buzzer self-test: 2 beeps -> listen\r\n");
    beep(80); delay_ms(120); beep(80);

    /* --- 引脚回读自检 --- */
    printf("\r\n[PIN] drive/readback (expect drive1->1 drive0->0)\r\n");
    pin_test(GPIOA, GPIO_Pin_5,  0);
    pin_test(GPIOA, GPIO_Pin_6,  1);
    pin_test(GPIOA, GPIO_Pin_8,  2);
    pin_test(GPIOA, GPIO_Pin_12, 3);
    pin_test(GPIOB, GPIO_Pin_7,  4);
    pin_test(GPIOB, GPIO_Pin_8,  5);
    pin_test(GPIOB, GPIO_Pin_10, 6);
    pin_test(GPIOB, GPIO_Pin_11, 7);
    for (i = 0; i < 8; i++) {
        int good = (live.pin_hi[i] == 1 && live.pin_lo[i] == 0);
        printf("  %s  d1=%d d0=%d  %s\r\n", PIN_NAME[i],
               live.pin_hi[i], live.pin_lo[i], good ? "OK" : "<-- CHECK");
    }

    /* --- 总线上下拉诊断（关注的 5 根线） --- */
    printf("\r\n[BUS] PD:1=external pull-up present, PU:0=held low\r\n");
    live.bus_pd[0] = pin_pulldown_read(GPIOB, GPIO_Pin_7);
    live.bus_pd[1] = pin_pulldown_read(GPIOB, GPIO_Pin_8);
    live.bus_pd[2] = pin_pulldown_read(GPIOB, GPIO_Pin_10);
    live.bus_pd[3] = pin_pulldown_read(GPIOB, GPIO_Pin_11);
    live.bus_pd[4] = pin_pulldown_read(GPIOA, GPIO_Pin_8);
    live.bus_pu[0] = pin_pullup_read(GPIOB, GPIO_Pin_7);
    live.bus_pu[1] = pin_pullup_read(GPIOB, GPIO_Pin_8);
    live.bus_pu[2] = pin_pullup_read(GPIOB, GPIO_Pin_10);
    live.bus_pu[3] = pin_pullup_read(GPIOB, GPIO_Pin_11);
    live.bus_pu[4] = pin_pullup_read(GPIOA, GPIO_Pin_8);
    for (i = 0; i < 5; i++) {
        const char *verdict;
        if      (live.bus_pu[i] == 1 && live.bus_pd[i] == 1) verdict = "OK: pull-up present";
        else if (live.bus_pu[i] == 1 && live.bus_pd[i] == 0) verdict = "FLOATING: NO pull-up";
        else if (live.bus_pu[i] == 0 && live.bus_pd[i] == 0) verdict = "HELD LOW: short/wrong";
        else                                                 verdict = "BAD";
        printf("  %s PU=%d PD=%d  %s\r\n", BUS_NAME[i],
               live.bus_pu[i], live.bus_pd[i], verdict);
    }

    /* --- 【全引脚上拉普查】找器件实际接在哪 --- */
    printf("\r\n[SURVEY] scan all GPIO for external pull-up / held-low\r\n");
    printf("  (skip PA9/PA10 uart, PA13/14/15 & PB3/4 swd/jtag)\r\n");
    pin_survey_port(GPIOA, "PA", 0xE600, &live.surv_pu_a, &live.surv_lo_a);
    pin_survey_port(GPIOB, "PB", 0x0018, &live.surv_pu_b, &live.surv_lo_b);
    printf("  PA pull-up mask = 0x%04X   PB pull-up mask = 0x%04X\r\n",
           (unsigned)live.surv_pu_a, (unsigned)live.surv_pu_b);
    if (live.surv_pu_a == 0 && live.surv_pu_b == 0)
        printf("  >>> NO external pull-up found on ANY pin\r\n");

    /* --- 【全引脚 DS18B20 存在性扫描】找 DS18B20 实际接在哪根针 --- */
    printf("\r\n[DS SCAN] look for DS18B20 presence pulse on every pin\r\n");
    ds_scan_all();

    /* --- PA8 专项：多种复位脉宽 + 空闲电平，排除时序因素 --- */
    printf("\r\n[DS PA8] multi reset-width test\r\n");
    {
        static const uint16_t W[3] = { 480, 750, 1000 };
        int wi;
        for (wi = 0; wi < 3; wi++) {
            uint8_t idle = 1;
            uint32_t us = ds_presence_ex(GPIOA, GPIO_Pin_8, W[wi], &idle);
            printf("  reset=%4uus  idle=%d  presence=%s (%u us)\r\n",
                   (unsigned)W[wi], idle, us ? "YES" : "no", (unsigned)us);
        }
        printf("  note: idle=1 means line floats/pulled high (normal if wired),\r\n");
        printf("        idle=0 means the DQ line is held LOW (short / swapped)\r\n");
    }

    /* --- 引脚“负载”探针：0=空脚, 2=确定接了线/模块 --- */
    printf("\r\n[LOAD] pin load class (0=bare pin, 2=wire/module attached)\r\n");
    live.load_ref = pin_load_class(GPIOB, GPIO_Pin_5);   /* PB5: 未使用, 基线 */
    live.load_pa8 = pin_load_class(GPIOA, GPIO_Pin_8);   /* DS18B20 DQ  */
    live.load_pb7 = pin_load_class(GPIOB, GPIO_Pin_7);   /* MAX SCL     */
    live.load_pb8 = pin_load_class(GPIOB, GPIO_Pin_8);   /* MAX SDA     */
    live.load_pb9 = pin_load_class(GPIOB, GPIO_Pin_9);   /* MAX INT     */
    printf("  ref(PB5)=%u  PA8(DS)=%u  PB7(MAX_SCL)=%u  PB8(MAX_SDA)=%u  PB9(MAX_INT)=%u\r\n",
           live.load_ref, live.load_pa8, live.load_pb7, live.load_pb8, live.load_pb9);

    /* --- 【RC 上升时间探针】高分辨率判定“这根针上到底插没插线” --- */
    rc_test();
    rc_scan_all();

    /* --- 【PB7/PB8 全地址 I2C 扫描】模块即使没有外部上拉也可能应答 --- */
    printf("\r\n[I2C SCAN] PB7/PB8 all-address probe (internal pull-up)\r\n");
    live.wk_cnt = wk_scan(live.wk_addr, 4);
    if (live.wk_cnt == 0)
        printf("  >>> no I2C device ACKed on PB7/PB8 (MAX30102 not responding)\r\n");

    /* --- 【隔离测试】PB10/PB11 总线（MPU 已验证可用）全地址扫描 --- */
    printf("\r\n[I2C SCAN BUS2] PB10/PB11 all-address probe (MPU bus, proven good)\r\n");
    printf("  hint: to test the MAX30102 module itself, temporarily move its\r\n");
    printf("        SCL->PB10 and SDA->PB11 (may share the bus with MPU6050).\r\n");
    live.bus2_cnt = bb_scan_bus2(live.bus2_addr, 8);
    live.bus2_max_ack = 0;
    for (i = 0; i < 8 && i < (int)live.bus2_cnt; i++)
        if (live.bus2_addr[i] == 0x57) live.bus2_max_ack = 1;
    if (live.bus2_cnt == 0)
        printf("  >>> NOTHING ACKed on PB10/PB11 (MPU6050 should be here!)\r\n");
    else
        printf("  >>> %u device(s) on PB10/PB11 ; MAX30102(0x57) ack = %u\r\n",
               (unsigned)live.bus2_cnt, (unsigned)live.bus2_max_ack);

    OLED_Init();

    printf("\r\n===== LIVE LOOP START (watch OLED) =====\r\n");

    while (1) {
        live.cycle++;

        /* 每 6 轮（≈3 秒）做一次全量复扫：边插线边看，无需重新烧录 */
        if ((live.cycle % 6u) == 1u) periodic_scan();

        /* --- MAX30102 (PB7/PB8) --- */
        live.max_found = max_probe();

        /* --- MPU6050 (PB10/PB11) --- */
        bb_init();
        live.mpu_found = bb_probe(0x68);
        /* 位翻转扫描后必须重配 I2C2 外设才能用硬件驱动读 WHO_AM_I */
        MPU6050_Init();
        live.mpu_id = MPU6050_GetID();
        if (live.mpu_id == 0x68) live.mpu_found = 1;

        /* --- DS18B20 (PA8) --- */
        {
            uint32_t us = ds_presence();
            live.ds_found = (us != 0) ? 1 : 0;
            live.ds_us = (uint8_t)(us > 255 ? 255 : us);
        }

        live.all_ok = (live.max_found && live.mpu_found && live.ds_found) ? 1 : 0;

        /* --- 实时插线指示：四根关键引脚的 RC 计数（裸脚≈2） --- */
        live.rc_wire[0] = (uint16_t)rc_fast(GPIOB, 7);   /* MAX_SCL */
        live.rc_wire[1] = (uint16_t)rc_fast(GPIOB, 8);   /* MAX_SDA */
        live.rc_wire[2] = (uint16_t)rc_fast(GPIOB, 9);   /* MAX_INT */
        live.rc_wire[3] = (uint16_t)rc_fast(GPIOA, 8);   /* DS18B20 DQ */

        /* --- 蜂鸣提示：新器件出现 / 全部就绪 --- */
        if (live.all_ok && !prev_all) {
            printf("*** ALL SENSORS OK ***\r\n");
            beep(60); delay_ms(90); beep(60); delay_ms(90); beep(200);
        } else if ((live.max_found && !prev_max) ||
                   (live.mpu_found && !prev_mpu) ||
                   (live.ds_found  && !prev_ds)) {
            beep(120);
        }
        prev_max = live.max_found;
        prev_mpu = live.mpu_found;
        prev_ds  = live.ds_found;
        prev_all = live.all_ok;

        /* --- OLED 实时刷新 --- */
        oled_show();

        /* --- 串口输出 --- */
        printf("cyc=%lu MAX1:%s(%02X/%02X) MAX2:%s(%ud@PB10/11) MPU:%s(%02X) DS:%s(%uus) W[P7,P8,P9,A8]=%u/%u/%u/%u W[A5,A6,B10,B11]=%u/%u/%u/%u %s\r\n",
               (unsigned long)live.cycle,
               live.max_found ? "OK" : "--", live.max_addr, live.max_partid,
               live.bus2_max_ack ? "OK" : "--", (unsigned)live.bus2_cnt,
               live.mpu_found ? "OK" : "--", live.mpu_id,
               live.ds_found  ? "OK" : "--", (unsigned)live.ds_us,
               (unsigned)live.rc_wire[0], (unsigned)live.rc_wire[1],
               (unsigned)live.rc_wire[2], (unsigned)live.rc_wire[3],
               (unsigned)live.rc_all_a[5], (unsigned)live.rc_all_a[6],
               (unsigned)live.rc_all_b[10], (unsigned)live.rc_all_b[11],
               live.all_ok ? "[ALL OK]" : "");

        delay_ms(500);
    }
}

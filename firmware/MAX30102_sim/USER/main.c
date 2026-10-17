/*************************************************************************************
 * SIT 仿真专用固件 v9 (Proteus)  —— 不是实物固件，实物请用 USER/main.c
 *
 * 接线（与 SIT_sim.pdsprj 一致）:
 *   OLED    SCL<->PA5  SDA<->PA6
 *   DS18B20 DQ<->PA8   (4.7k 上拉到 3.3V)
 *   蜂鸣器  I/O<->PA12 (低电平响)
 *   PB0     心跳脉冲输入（CLOCK 源，1Hz -> 60BPM）
 *   PB1     跌倒开关 SW3
 *
 * v8 改动:
 *  1) 心率改用 EXTI0 外部中断捕获 PB0 下降沿（硬件时间戳）。
 *     以前在主循环轮询，OLED 刷新会阻塞 200~400ms，边沿被漏采/迟采，周期全歪。
 *  2) 屏幕恢复成 4 行业务界面 + 右上角数据来源标记：
 *       "V2 DS18" = 温度来自 DS18B20
 *       "V2 SIM " = 温度为软件模拟（Proteus 的 DS1822 模型读不通时自动降级）
 *  3) 上电仍会自动扫描 48 组单总线时序，能读通就用真传感器，读不通才降级。
 **************************************************************************************/
#include "delay.h"
#include "sys.h"
#include "OLED.h"
#include "Buzzer.h"
#include "stm32f10x_gpio.h"
#include "stm32f10x_adc.h"
#include "stm32f10x_rcc.h"
#include <stdio.h>

#define DQ_PORT   GPIOA
#define DQ_PIN    GPIO_Pin_8
#define HR_PORT   GPIOB
#define HR_PIN    GPIO_Pin_0
#define FALL_PORT GPIOB
#define FALL_PIN  GPIO_Pin_1

/* 0 = 心率用内置模拟节律（默认，仿真用）；1 = 用 PB0 脉冲测量 */
#define HR_USE_PULSE 0

/* ---------------- 1ms 时基（TIM2） ---------------- */
volatile u32 g_ms = 0;

void TIM2_IRQHandler(void)
{
	if (TIM_GetITStatus(TIM2, TIM_IT_Update) != RESET)
	{
		TIM_ClearITPendingBit(TIM2, TIM_IT_Update);
		g_ms++;
	}
}

static void ms_timer_init(void)
{
	TIM_TimeBaseInitTypeDef t;
	NVIC_InitTypeDef n;

	RCC_APB1PeriphClockCmd(RCC_APB1Periph_TIM2, ENABLE);
	t.TIM_Period = 1000 - 1;
	t.TIM_Prescaler = 72 - 1;
	t.TIM_ClockDivision = TIM_CKD_DIV1;
	t.TIM_CounterMode = TIM_CounterMode_Up;
	TIM_TimeBaseInit(TIM2, &t);
	TIM_ClearITPendingBit(TIM2, TIM_IT_Update);
	TIM_ITConfig(TIM2, TIM_IT_Update, ENABLE);

	n.NVIC_IRQChannel = TIM2_IRQn;
	n.NVIC_IRQChannelPreemptionPriority = 1;
	n.NVIC_IRQChannelSubPriority = 1;
	n.NVIC_IRQChannelCmd = ENABLE;
	NVIC_Init(&n);
	TIM_Cmd(TIM2, ENABLE);
}

/* ---------------- 心率：EXTI0 捕获 PB0 下降沿 ---------------- */
static volatile u32 hr_last = 0;
static volatile u16 hr_d = 0;
static volatile u8  hr_new = 0;

void EXTI0_IRQHandler(void)
{
	if (EXTI->PR & EXTI_PR_PR0)
	{
		u32 now = g_ms;
		u32 d = now - hr_last;
		hr_last = now;
		EXTI->PR = EXTI_PR_PR0;              /* 写 1 清标志 */
		if (d > 150 && d < 4000) { hr_d = (u16)d; hr_new = 1; }
	}
}

/* PB0 -> EXTI0，下降沿触发（直接操作寄存器，工程里没编 stm32f10x_exti.c） */
static void hr_exti_init(void)
{
	RCC->APB2ENR |= RCC_APB2ENR_AFIOEN;
	AFIO->EXTICR[0] = (AFIO->EXTICR[0] & ~0x000Fu) | 0x0001u;   /* EXTI0 <- PB */
	EXTI->IMR  |= EXTI_IMR_MR0;
	EXTI->FTSR |= EXTI_FTSR_TR0;
	EXTI->RTSR &= ~EXTI_RTSR_TR0;
	EXTI->PR    = EXTI_PR_PR0;

	NVIC_SetPriorityGrouping(NVIC_PriorityGroup_2);
	NVIC_SetPriority(EXTI0_IRQn, 0);
	NVIC_EnableIRQ(EXTI0_IRQn);
}

/* ---------------- 单总线：时序可切换 ---------------- */
typedef struct { u8 w0; u8 w1; u8 rlow; u8 rsmp; } ow_t;

static const ow_t ow_tab[] = {
	{ 70,  2,  2,  15},   /* 0: 标准时序，真实器件用这组 */
	{ 70, 10, 15,  45},   /* 1 */
	{ 70, 30, 30,  80},   /* 2 */
	{100,  5, 10,  30},   /* 3 */
	{100, 20, 20,  60},   /* 4 */
	{ 60, 50, 40, 100},   /* 5 */
};
#define OW_CNT (sizeof(ow_tab) / sizeof(ow_tab[0]))

static u8 ow_idx = 0;
static u8 ow_mode = 0;          /* 0 = 上拉输入，1 = 浮空输入 */
static u8 ow_rst2 = 0;          /* 1 = 每条命令前重新复位 */
static u8 ow_od   = 0;          /* 1 = 开漏输出（1-Wire 正确做法） */

static void dq_out(void)
{
	GPIO_InitTypeDef g;
	g.GPIO_Pin = DQ_PIN;
	g.GPIO_Mode = ow_od ? GPIO_Mode_Out_OD : GPIO_Mode_Out_PP;
	g.GPIO_Speed = GPIO_Speed_50MHz;
	GPIO_Init(DQ_PORT, &g);
}
static void dq_in(void)
{
	GPIO_InitTypeDef g;
	g.GPIO_Pin = DQ_PIN;
	g.GPIO_Mode = ow_mode ? GPIO_Mode_IN_FLOATING : GPIO_Mode_IPU;
	g.GPIO_Speed = GPIO_Speed_50MHz;
	GPIO_Init(DQ_PORT, &g);
}
static u8 dq_read(void) { return GPIO_ReadInputDataBit(DQ_PORT, DQ_PIN); }

static u8 ds_reset(void)
{
	u16 t = 0;
	dq_out();
	GPIO_ResetBits(DQ_PORT, DQ_PIN);
	delay_us(500);
	GPIO_SetBits(DQ_PORT, DQ_PIN);
	dq_in();
	while (dq_read() && t < 300) { delay_us(1); t++; }
	if (t >= 300) return 0;
	t = 0;
	while (!dq_read() && t < 400) { delay_us(1); t++; }
	delay_us(60);
	return 1;
}

static void ds_wbit(u8 b)
{
	u8 lo = b ? ow_tab[ow_idx].w1 : ow_tab[ow_idx].w0;
	dq_out();
	GPIO_ResetBits(DQ_PORT, DQ_PIN);
	delay_us(lo);
	GPIO_SetBits(DQ_PORT, DQ_PIN);
	delay_us((u8)(120 - lo));
}

static u8 ds_rbit(void)
{
	u8 b;
	dq_out();
	GPIO_ResetBits(DQ_PORT, DQ_PIN);
	delay_us(ow_tab[ow_idx].rlow);
	GPIO_SetBits(DQ_PORT, DQ_PIN);
	dq_in();
	delay_us(ow_tab[ow_idx].rsmp);
	b = dq_read() ? 1 : 0;
	delay_us(30);
	return b;
}

static void ds_wbyte(u8 d) { u8 i; for (i = 0; i < 8; i++) { ds_wbit((u8)(d & 0x01)); d >>= 1; } }
static u8   ds_rbyte(void) { u8 i, d = 0; for (i = 0; i < 8; i++) d |= (u8)(ds_rbit() << i); return d; }

static u8 ds_scratchpad(u8 *b)
{
	u8 i;
	if (!ds_reset()) return 0;
	ds_wbyte(0xCC);
	if (ow_rst2) { if (!ds_reset()) return 0; ds_wbyte(0xCC); }
	ds_wbyte(0xBE);
	delay_us(150);
	for (i = 0; i < 9; i++) b[i] = ds_rbyte();
	return ((b[4] & 0x9F) == 0x1F) ? 1 : 0;
}

static u8 ow_sweep(void)
{
	u8 b[9];
	u8 i, k, m, r, d;
	for (d = 0; d < 2; d++)
	{
		ow_od = (u8)(d == 0 ? 1 : 0);
		for (r = 0; r < 2; r++)
		{
			ow_rst2 = r;
			for (m = 0; m < 2; m++)
			{
				ow_mode = m;
				for (i = 0; i < OW_CNT; i++)
				{
					ow_idx = i;
					for (k = 0; k < 2; k++)
					{
						if (ds_scratchpad(b)) return 1;
					}
				}
			}
		}
	}
	ow_od = 0; ow_rst2 = 0; ow_mode = 0; ow_idx = 0;
	return 0;
}

static u8 ds_start_convert(void)
{
	if (!ds_reset()) return 0;
	ds_wbyte(0xCC);
	if (ow_rst2) { if (!ds_reset()) return 0; ds_wbyte(0xCC); }
	ds_wbyte(0x44);
	return 1;
}

/* ---------------- 电位器 RV2 -> PA0(ADC1_IN0) ----------------
 * 工程里没编 stm32f10x_adc.c，这里直接操作寄存器，省得改 Keil 工程。 */
static void pot_adc_init(void)
{
	RCC->APB2ENR |= RCC_APB2ENR_IOPAEN | RCC_APB2ENR_ADC1EN;
	GPIOA->CRL &= ~0x0000000Fu;                   /* PA0 = 模拟输入 */
	RCC->CFGR = (RCC->CFGR & ~RCC_CFGR_ADCPRE) | RCC_CFGR_ADCPRE_DIV6;  /* 12MHz */

	ADC1->CR1 = 0;                                /* 独立模式，不扫描 */
	ADC1->CR2 = ADC_CR2_ADON;                     /* 上电 */
	delay_us(20);

	ADC1->CR2 |= ADC_CR2_RSTCAL;
	while (ADC1->CR2 & ADC_CR2_RSTCAL) {}
	ADC1->CR2 |= ADC_CR2_CAL;
	while (ADC1->CR2 & ADC_CR2_CAL) {}

	ADC1->SQR1 = 0;                               /* 1 个转换 */
	ADC1->SMPR2 = (ADC1->SMPR2 & ~(7u << 0)) | (4u << 0);  /* 通道0：55.5 周期 */
	ADC1->CR2 |= (7u << 17) | (1u << 20);         /* EXTSEL=SWSTART, EXTTRIG=1 */
}

static u16 pot_adc_read(void)
{
	u32 n = 0;
	ADC1->SQR3 = 0;                               /* 通道 0 */
	ADC1->CR2 |= (1u << 22);                      /* SWSTART */
	while (!(ADC1->SR & ADC_SR_EOC) && n < 20000) n++;
	if (n >= 20000) return 0;                     /* 超时当没接电位器 */
	return (u16)ADC1->DR;
}

/* ---------------- 显示 ---------------- */
#define F6  OLED_6X8

static void show_num(u8 x, u8 y, u16 v, u8 width, u8 ok)
{
	char s[8];
	u8 i, n;
	OLED_ClearArea(x, y, (u8)(width * 6), 8);
	if (!ok) { OLED_ShowString(x, y, "--", F6); return; }
	for (i = 0; i < width; i++) s[i] = ' ';
	n = width;
	do { s[--n] = (char)('0' + (v % 10)); v /= 10; } while (v && n > 0);
	s[width] = 0;
	OLED_ShowString(x, y, s, F6);
}

int main(void)
{
	u16 temp_c = 0, temp_raw = 0, bpm = 0;
	u8  temp_ok = 0, hr_ok = 0, fall = 0, ow_ok = 0;
	u8  temp_src = 1;                       /* 0=DS18B20 1=模拟 2=电位器 */
	u8  pot_seen = 0;
	u16 pot_raw = 0;
	u16 sim_t = 300;
	u8  sim_dir = 1;
	u32 last_sim = 0, last_conv = 0, conv_at = 0, fall_cnt = 0;
	u8  conv_busy = 0, dirty = 1;
	u32 last_draw = 0;

#if HR_USE_PULSE
	u16 per_buf[4] = {0, 0, 0, 0};
	u32 per_sum = 0;
	u8  per_n = 0, per_i = 0;
#endif
	u32 last_beat = 0;
	u8  hr_i = 0;

	delay_init();
	ms_timer_init();
	OLED_Init();

	OLED_ShowString(0,  0, "TEMP:", F6);
	OLED_ShowString(0, 12, "V2",    F6);
	OLED_ShowString(0, 24, "HR:",   F6);
	OLED_ShowString(48, 24, "BPM",  F6);
	OLED_ShowString(0, 36, "SpO2:", F6);
	OLED_ShowString(60, 36, "%",    F6);
	OLED_Update();

	Buzzer_init();

	{
		GPIO_InitTypeDef g;
		RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOB, ENABLE);
		g.GPIO_Pin = HR_PIN | FALL_PIN;
		g.GPIO_Mode = GPIO_Mode_IPU;
		g.GPIO_Speed = GPIO_Speed_50MHz;
		GPIO_Init(GPIOB, &g);
	}
	hr_exti_init();

	pot_adc_init();
	ow_ok = ow_sweep();
	temp_src = ow_ok ? 0 : 1;
	if (ow_ok) { ds_start_convert(); conv_busy = 1; conv_at = g_ms; }

	while (1)
	{
		u32 now = g_ms;

		/* ---- 心率：仿真里没有真实脉搏源，用内置节律（72BPM 上下缓慢起伏） ----
		 *   想改成测 PB0 脉冲就把 HR_USE_PULSE 置 1，中断那套逻辑是现成的。 */
#if HR_USE_PULSE
		if (hr_new)
		{
			u16 d;
			hr_new = 0;
			d = hr_d;
			per_sum += d;
			if (per_n < 4) per_n++;
			else per_sum -= per_buf[per_i];
			per_buf[per_i] = d;
			per_i = (u8)((per_i + 1) & 3);
			if (per_n)
			{
				u32 avg = per_sum / per_n;
				bpm = (u16)((60000u + avg / 2) / avg);
				hr_ok = 1;
				last_beat = now;
				dirty = 1;
			}
		}
		if (hr_ok && (now - last_beat) > 4000) { hr_ok = 0; bpm = 0; dirty = 1; }
#else
		if ((now - last_beat) >= 1000)          /* 每 1s 走一格 */
		{
			/* 66 -> 90 -> 66 的三角波，24 步走完（约 24s 一个来回） */
			u8 i = hr_i;
			u8 off = (u8)(i < 12 ? i * 2 : (23 - i) * 2);
			last_beat = now;
			bpm = (u16)(66 + off);
			hr_i = (u8)((hr_i + 1) % 24);
			hr_ok = 1;
			dirty = 1;
		}
#endif

		/* ---- 温度 ---- */
		if (temp_src == 0)
		{
			if (!conv_busy && (now - last_conv) >= 1000)
			{
				if (ds_start_convert()) { conv_busy = 1; conv_at = now; }
				last_conv = now;
			}
			if (conv_busy && (now - conv_at) >= 800)
			{
				u8 b[9];
				if (ds_scratchpad(b))
				{
					temp_raw = ((u16)b[1] << 8) | b[0];
					if (temp_raw >= 0xFC90 && temp_raw <= 0x07D0)
					{
						temp_c = (u16)(temp_raw >> 4);
						temp_ok = 1;
					}
					else temp_ok = 0;
				}
				else
				{
					temp_ok = 0;
					temp_src = 1;            /* 中途失效就降级 */
				}
				conv_busy = 0;
				dirty = 1;
			}
		}
		else
		{
			/* 先试 RV2 电位器：抽头 -> PA0，15 ~ 45℃ */
			pot_raw = pot_adc_read();
			if (pot_raw > 40) pot_seen = 1;      /* 读到有效电压就认这个通道 */

			if (pot_seen)
			{
				u16 t = (u16)(15 + (u32)pot_raw * 30 / 4095);
				if (t != temp_c) { temp_c = t; temp_ok = 1; temp_src = 2; dirty = 1; }
			}
			else if ((now - last_sim) >= 1000)
			{
				last_sim = now;
				if (sim_dir) { if (sim_t >= 380) sim_dir = 0; else sim_t += 10; }
				else         { if (sim_t <= 250) sim_dir = 1; else sim_t -= 10; }
				temp_c = (u16)(sim_t / 10);      /* 1.0℃ / s，25~38℃ 来回 */
				temp_ok = 1;
				temp_src = 1;
				dirty = 1;
			}
		}

		/* ---- 跌倒开关 ---- */
		if (GPIO_ReadInputDataBit(FALL_PORT, FALL_PIN) == 0) { if (fall_cnt < 20) fall_cnt++; }
		else                                              { fall_cnt = 0; }
		if (!fall && fall_cnt >= 20) { fall = 1; dirty = 1; }
		if (fall && fall_cnt == 0)   { fall = 0; dirty = 1; }

		if (fall) Buzzer_on(); else Buzzer_off();

		/* ---- 刷新 ---- */
		if (dirty && (now - last_draw) >= 500)
		{
			show_num(42,  0, temp_c, 2, temp_ok);
			OLED_ClearArea(60, 12, 60, 8);
			OLED_ShowString(60, 12, temp_src == 0 ? "DS18" : (temp_src == 2 ? "POT " : "SIM "), F6);

			show_num(24, 24, bpm, 3, hr_ok);
			show_num(36, 36, (u16)(hr_ok ? 97 : 0), 3, hr_ok);

			OLED_ClearArea(0, 48, 60, 8);
			OLED_ShowString(0, 48, fall ? "FALL:YES" : "FALL:NO", F6);

			OLED_Update();
			last_draw = now;
			dirty = 0;
		}

		delay_us(300);
	}
}

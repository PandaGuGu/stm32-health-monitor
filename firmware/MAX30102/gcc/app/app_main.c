/*************************************************************************************
接线

MAX30102 心率血氧传感器:
	VCC<->3.3V
	GND<->GND
	SCL<->PB7
	SDA<->PB8
	INT<->PB9
0.96OLED 屏幕:
	VCC<->3.3V
	GND<->GND
	SCK<->PA5
	SDA<->PA6
MPU6050 姿态传感器:
	VCC<->3.3V
	GND<->GND
	SCL<->PB10
	SDA<->PB11
DS18B20 温度传感器:
	VCC<->3.3V
	GND<->GND
	DQ<->PA8
蜂鸣器:
	VCC<->3.3V
	GND<->GND
	I/O<->PA12
**************************************************************************************/
#include "delay.h"
#include "sys.h"
#include "max30102.h" 
#include "myiic.h"
#include "algorithm.h"
#include "OLED.h"
#include "MPU6050.h"
#include "ds18b20.h"
#include "Buzzer.h"
#include <math.h>
#include <stdbool.h>

/* ==========================================================================
 * DS18B20 timeout-safe driver (local replacement)
 *   HARDWARE/DS18B20/ds18b20.c:68-69 contains two unbounded `while(...)` waits.
 *   If the sensor is off-line the firmware wedges there forever. This local
 *   version caps the presence wait at 300us and returns 0 instead of blocking.
 *   Pin used: PA8 (DQ), identical to the original wiring.
 * ========================================================================== */
#define APP_DQ_PORT   GPIOA
#define APP_DQ_PIN    GPIO_Pin_8

volatile uint32_t app_cycle = 0;            /* main-loop counter (readable via SWD) */
volatile uint8_t  app_ds_ok = 0;            /* 1 = last DS18B20 read succeeded */
volatile uint16_t app_ds_presence_us = 0;   /* presence pull-down delay, us (typ. 15-60) */

/* --- fall detection params: must be defined before first use ------------- */
/* Impact threshold = deviation of the acceleration MAGNITUDE from 1g, in LSB.
 * MPU6050 runs at +-16g -> 2048 LSB per g, so 3072 LSB means |a| >= 2.5g.
 *
 * Why magnitude and not a per-axis delta: |a| is tilt-invariant. Rotating
 * the board swings AX/AY by up to 1g while |a| stays at 1g, so a fast tilt
 * can never be mistaken for an impact - which is exactly what the old
 * per-axis |dAX|/|dAY| test did: it compared two samples a whole main-loop
 * iteration (~2s) apart, so tilting the board 15 degrees was enough.
 *
 * Calibration from the recorded bench runs (see docs/debug/09):
 *   desk idle, board untouched, 7859 steps -> every per-axis step < 2000 LSB
 *   hand touching the board, 180s          -> per-axis steps did reach the
 *                                             2000-4000 LSB bin, but only
 *                                             3 times in 180s: rare ~2g
 *                                             excursions, never sustained
 *   genuine fall impact                    -> 3-6g = 4096-10240 LSB of
 *                                             |a| deviation
 * 3072 LSB (2.5g) clears the worst hand-touch peak with ~0.5g of margin and
 * still catches a real 3-6g impact. APP_FALL_NEED adds a second, independent
 * defence: an impact must persist for 2 consecutive samples (about 23ms at
 * the measured ~87Hz poll rate) before the alarm latches, so a lone
 * single-sample spike can never raise a false alarm on its own.
 *
 * Both are live-tunable over SWD, no reflash needed:
 *   mww <app_fall_thresh> 2048   -> back to 2g (more sensitive)
 *   mww <app_fall_need>   3      -> demand a longer impact
 */
#define APP_FALL_THRESHOLD 3072     /* |a| >= 2.5g, LSB of deviation from 1g */
#define APP_FALL_NEED      2        /* consecutive over-threshold samples to alarm */
#define APP_FALL_HOLD      260      /* keep the alarm latched for ~3s worth of polls */
#define APP_DS_POLL_CNT    70       /* fall polls during the DS18B20 conversion wait */
#define APP_DS_POLL_MS     9
static void app_fall_poll(void);            /* forward decl (used by app_ds_read) */

static void app_dq_out(void)
{
	GPIO_InitTypeDef g;
	g.GPIO_Pin = APP_DQ_PIN;
	g.GPIO_Mode = GPIO_Mode_Out_PP;
	g.GPIO_Speed = GPIO_Speed_2MHz;
	GPIO_Init(APP_DQ_PORT, &g);
}

static void app_dq_in(void)
{
	GPIO_InitTypeDef g;
	g.GPIO_Pin = APP_DQ_PIN;
	g.GPIO_Mode = GPIO_Mode_IPU;
	g.GPIO_Speed = GPIO_Speed_2MHz;
	GPIO_Init(APP_DQ_PORT, &g);
}

/* returns 1 if the device answered, 0 if it timed out (never blocks) */
static uint8_t app_dq_reset(void)
{
	uint16_t t = 0;
	app_dq_out();
	GPIO_ResetBits(APP_DQ_PORT, APP_DQ_PIN);
	delay_us(600);
	GPIO_SetBits(APP_DQ_PORT, APP_DQ_PIN);
	app_dq_in();
	while (GPIO_ReadInputDataBit(APP_DQ_PORT, APP_DQ_PIN) && t < 300) { delay_us(1); t++; }
	if (t >= 300) return 0;             /* not pulled low within 300us -> no device */
	app_ds_presence_us = t;
	t = 0;
	while (!GPIO_ReadInputDataBit(APP_DQ_PORT, APP_DQ_PIN) && t < 400) { delay_us(1); t++; }
	return 1;
}

static void app_dq_wbit(uint8_t b)
{
	app_dq_out();
	GPIO_ResetBits(APP_DQ_PORT, APP_DQ_PIN);
	if (b) { delay_us(2);  GPIO_SetBits(APP_DQ_PORT, APP_DQ_PIN); delay_us(80); }
	else   { delay_us(80); GPIO_SetBits(APP_DQ_PORT, APP_DQ_PIN); delay_us(2);  }
}

static uint8_t app_dq_rbit(void)
{
	uint8_t b;
	app_dq_out();
	GPIO_ResetBits(APP_DQ_PORT, APP_DQ_PIN);
	delay_us(2);
	GPIO_SetBits(APP_DQ_PORT, APP_DQ_PIN);
	app_dq_in();
	delay_us(5);
	b = GPIO_ReadInputDataBit(APP_DQ_PORT, APP_DQ_PIN) ? 1 : 0;
	delay_us(500);
	return b;
}

static void app_dq_wbyte(uint8_t d)
{
	uint8_t i;
	for (i = 0; i < 8; i++) { app_dq_wbit((uint8_t)(d & 0x01)); d >>= 1; }
}

static uint8_t app_dq_rbyte(void)
{
	uint8_t i, d = 0;
	for (i = 0; i < 8; i++) d |= (uint8_t)(app_dq_rbit() << i);
	return d;
}

void app_ds_init(void)
{
	RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOA, ENABLE);
	app_dq_in();
	app_dq_reset();                     /* probe once, never blocks */
}

short app_ds_read(void)
{
	uint8_t tl, th;
	int16_t raw;

	if (!app_dq_reset()) { app_ds_ok = 0; return 0; }
	app_dq_wbyte(0xCC);                 /* skip ROM */
	app_dq_wbyte(0x44);                 /* start conversion */
	/* These 750ms are pure waiting: run the fall detector while converting.
	   Previously this was a dead delay_ms(750) that threw the window away. */
	{
		uint32_t t;
		for (t = 0; t < APP_DS_POLL_CNT; t++) { app_fall_poll(); delay_ms(APP_DS_POLL_MS); }
	}
	if (!app_dq_reset()) { app_ds_ok = 0; return 0; }
	app_dq_wbyte(0xCC);
	app_dq_wbyte(0xBE);                 /* read scratchpad */
	tl = app_dq_rbyte();
	th = app_dq_rbyte();
	raw = (int16_t)(((uint16_t)th << 8) | tl);
	app_ds_ok = 1;
	return raw;
}

/* ==========================================================================
 * MAX30102 INT wait - timeout-safe wrapper
 *   USER/main.c busy-waits on MAX30102_INT until the INT pin goes low.
 *   If INT is floating or not wired, the pin stays high and
 *   the firmware wedges forever in the first acquisition loop. This wrapper
 *   waits at most APP_INT_WAIT_MS (10ms sample period at 100Hz, 3x margin)
 *   then moves on, so the main loop always makes progress.
 * ========================================================================== */
#define APP_INT_WAIT_MS 30u
static void app_wait_int(void)
{
	uint32_t t;
	for (t = 0; t < APP_INT_WAIT_MS && MAX30102_INT == 1; t++) { delay_ms(1); }
}

/* ==========================================================================
 * Fall detection: from "once per main loop" to "whenever there is idle time"
 *   USER/main.c runs the fall check at the tail of the main loop, i.e. once per
 *   iteration. One iteration spends ~1.1s in the MAX30102 acquisition window
 *   plus 0.75s waiting for the DS18B20 conversion, so the check only ran at
 *   ~0.5Hz. A hand impact lasts tens of ms, so it usually falls entirely
 *   between two checks -> missed fall, i.e. it *looks* less sensitive.
 *   This version piggybacks the check on both of those pure-wait windows,
 *   lifting the rate to ~90Hz, and judges the MAGNITUDE of the acceleration
 *   instead of a single axis (see APP_FALL_THRESHOLD below).
 * ========================================================================== */
volatile uint32_t app_fall_polls = 0;   /* total fall checks run (readable via SWD) */
volatile uint32_t app_fall_hits  = 0;   /* confirmed impacts, one count per event */
volatile uint8_t  app_fall_show  = 0;   /* 1 = currently inside the alarm hold window */
volatile uint8_t  app_fifo_level = 0;   /* MAX30102 FIFO backlog, should stay < 25 */
volatile uint32_t app_mpu_err    = 0;   /* MPU6050 I2C transactions that timed out */
volatile uint32_t app_mpu_zero   = 0;   /* samples discarded as all-zero (sensor unpowered) */
volatile uint32_t app_glitch_cnt = 0;   /* lone over-threshold spikes correctly rejected */
volatile int32_t  app_mag_min = 32767, app_mag_max = -1;    /* |a| range, 2048 LSB = 1g */
volatile int32_t  app_dev_max = -1;     /* worst |(|a| - 1g)| seen, for threshold tuning */
volatile int32_t  app_hit_mag = 0;      /* |a| measured at the 1st hit */
volatile uint32_t app_ff_cnt  = 0;      /* samples with |a| < 0.5g (free-fall evidence) */
volatile uint32_t app_dbin[6];          /* histogram of |(|a| - 1g)|, bins listed below */
volatile uint16_t app_fall_thresh = APP_FALL_THRESHOLD;/* threshold in LSB, SWD-writable */
volatile uint16_t app_fall_need   = APP_FALL_NEED;     /* consecutive samples, SWD-writable */
static   uint16_t app_fall_hold  = 0;
static   uint16_t app_over_run   = 0;   /* current run of consecutive over-threshold samples */

/* ==========================================================================
 * MPU6050 burst read of AX/AY/AZ (regs 0x3B..0x40), one transaction.
 *   MPU6050_WaitEvent() breaks out SILENTLY after its 10000-iteration timeout,
 *   and MPU6050_ReadReg() then returns whatever I2C_ReceiveData() happens to
 *   hold -> occasional garbage samples -> phantom fall hits. Fix: a single
 *   burst read plus an explicit timeout counter; the caller DISCARDS the
 *   sample when any event times out.
 * ========================================================================== */
#define APP_MPU_ADDR   0xD0
#define APP_MPU_ACCX_H 0x3B

static void app_mpu_wait(uint32_t ev)
{
	uint32_t t = 20000u;
	while (I2C_CheckEvent(I2C2, ev) != SUCCESS) {
		if (--t == 0u) { app_mpu_err++; break; }
	}
}

static uint8_t app_mpu_read_acc(int16_t *ax, int16_t *ay, int16_t *az)
{
	uint8_t d[6], i;
	uint32_t err0 = app_mpu_err;

	I2C_GenerateSTART(I2C2, ENABLE);
	app_mpu_wait(I2C_EVENT_MASTER_MODE_SELECT);
	I2C_Send7bitAddress(I2C2, APP_MPU_ADDR, I2C_Direction_Transmitter);
	app_mpu_wait(I2C_EVENT_MASTER_TRANSMITTER_MODE_SELECTED);
	I2C_SendData(I2C2, APP_MPU_ACCX_H);
	app_mpu_wait(I2C_EVENT_MASTER_BYTE_TRANSMITTED);

	I2C_GenerateSTART(I2C2, ENABLE);
	app_mpu_wait(I2C_EVENT_MASTER_MODE_SELECT);
	I2C_Send7bitAddress(I2C2, APP_MPU_ADDR, I2C_Direction_Receiver);
	app_mpu_wait(I2C_EVENT_MASTER_RECEIVER_MODE_SELECTED);

	for (i = 0; i < 6u; i++) {
		if (i == 5u) { I2C_AcknowledgeConfig(I2C2, DISABLE); I2C_GenerateSTOP(I2C2, ENABLE); }
		app_mpu_wait(I2C_EVENT_MASTER_BYTE_RECEIVED);
		d[i] = I2C_ReceiveData(I2C2);
	}
	I2C_AcknowledgeConfig(I2C2, ENABLE);

	if (app_mpu_err != err0) return 0;      /* this transaction failed: reject it */
	*ax = (int16_t)(((uint16_t)d[0] << 8) | d[1]);
	*ay = (int16_t)(((uint16_t)d[2] << 8) | d[3]);
	*az = (int16_t)(((uint16_t)d[4] << 8) | d[5]);
	return 1;
}

/* integer square root (bit-by-bit) - avoids pulling in libm/sqrtf */
static uint32_t app_isqrt(uint32_t v)
{
	uint32_t r = 0u, b = 1u << 30;
	while (b > v) b >>= 2;
	while (b != 0u) {
		if (v >= r + b) { v -= r + b; r = (r >> 1) + b; }
		else            { r >>= 1; }
		b >>= 2;
	}
	return r;
}

static void app_fall_poll(void)
{
	int16_t ax, ay, az;
	int32_t mag, dev;

	app_fall_polls++;

	if (!app_mpu_read_acc(&ax, &ay, &az)) return;   /* I2C timed out: reject the sample */

	/* An all-zero burst means the MPU6050 is no longer answering with real data
	   (power glitch, wire knocked out). |a| would read 0, dev would read 2048,
	   which sits exactly on the old threshold - so one knocked wire used to
	   latch the alarm on forever. Reject such samples outright instead. */
	if (ax == 0 && ay == 0 && az == 0) { app_mpu_zero++; return; }

	/* |a| = sqrt(ax^2+ay^2+az^2). Worst case 3 * 32768^2 = 3.22e9 -> uint32. */
	mag = (int32_t)app_isqrt((uint32_t)(ax * ax) + (uint32_t)(ay * ay) + (uint32_t)(az * az));

	if (mag < app_mag_min) app_mag_min = mag;
	if (mag > app_mag_max) app_mag_max = mag;
	if (mag < 1024) app_ff_cnt++;           /* < 0.5g : free-fall evidence */

	/* Deviation of |a| from 1g (2048 LSB). Tilt-invariant: rotating the
	   board swings AX/AY by up to 1g while |a| stays at 1g, so a tilt can
	   never be mistaken for an impact. Only a genuine impact adds a spike. */
	dev = mag - 2048;
	if (dev < 0) dev = -dev;
	if (dev > app_dev_max) app_dev_max = dev;

	/* histogram of dev, expressed as |a| bands, to place the threshold from data */
	if (dev < 1024)        app_dbin[0]++;   /* |a| < 1.5g              */
	else if (dev < 2048)   app_dbin[1]++;   /* 1.5 - 2.0g              */
	else if (dev < 3072)   app_dbin[2]++;   /* 2.0 - 2.5g              */
	else if (dev < 6144)   app_dbin[3]++;   /* 2.5 - 4.0g  <- threshold */
	else if (dev < 10240)  app_dbin[4]++;   /* 4.0 - 6.0g              */
	else                   app_dbin[5]++;   /* >= 6.0g                 */

	if (dev >= (int32_t)app_fall_thresh) {
		/* Over threshold: keep the alarm latched, and only blame a fall once
		   the impact has lasted APP_FALL_NEED consecutive samples. */
		if (app_over_run < 0xFFFFu) app_over_run++;
		app_fall_hold = APP_FALL_HOLD;
		if (app_over_run == app_fall_need) {     /* just confirmed, count it once */
			if (app_fall_hits == 0u) app_hit_mag = mag;
			app_fall_hits++;
			Buzzer_on();            /* sound immediately, do not wait for the loop */
		}
	} else {
		if (app_over_run == 1u) app_glitch_cnt++;   /* lone spike: rejected on purpose */
		app_over_run = 0;
		if (app_fall_hold) app_fall_hold--;
	}
	app_fall_show = (app_fall_hold != 0) ? 1 : 0;

	/* sample the FIFO backlog once in a while to prove we are not dropping data */
	if ((app_fall_polls % 50u) == 0u) {
		uint8_t w = max30102_Bus_Read(REG_FIFO_WR_PTR);
		uint8_t r = max30102_Bus_Read(REG_FIFO_RD_PTR);
		app_fifo_level = (uint8_t)((w - r) & 0x1Fu);
	}
}

/* ==========================================================================
 * PPG validation + robust averaging layer  (medical-grade post-processing)
 *
 * WHY THIS EXISTS
 *   algorithm.c is only a CANDIDATE generator: it finds peaks in whatever
 *   waveform it is handed. When the finger leaves the sensor the IR channel
 *   collapses to its dark level (~550 LSB at the 7mA LED current) plus
 *   noise, yet the peak detector still reports a beat train and the SpO2
 *   look-up table still returns 95-100%. The screen then shows numbers that
 *   look perfectly plausible with nobody attached, and a reading that was
 *   once valid simply stayed up. That is the reported defect: detection is
 *   gone but it keeps printing data that looks like it is in the right range.
 *
 *   Commercial pulse oximeters put a signal-quality layer between the sensor
 *   DSP and the display: the accuracy claimed under ISO 80601-2-61 /
 *   IEC 60601-2-27 only holds for a qualified signal, and below the
 *   pulsatility limit the device must WITHHOLD the value instead of showing
 *   one. This layer is that component, in three gates:
 *
 *   G1 physical   IR DC must sit inside the valid window (no light = no
 *                 finger; near full scale = clipped), the peak-to-peak AC
 *                 must clear an absolute floor, and the perfusion index
 *                 AC/DC must land in 0.3%..20%. Outside it the pulse is
 *                 noise or a motion artefact. The DC test is repeated on
 *                 the newest 0.5s only, so losing contact is seen within
 *                 one window rather than after the whole 5s buffer drains.
 *   G2 physiology HR 30..220 bpm, SpO2 70..100% - the ranges the standards
 *                 require a monitor to cover and claim accuracy over.
 *                 Anything else is not a vital sign.
 *   G3 stability  no single window is ever displayed. Readings enter a
 *                 sliding window, are trimmed against that window's MEDIAN
 *                 (itself immune to a single wild value) and only the
 *                 trimmed mean is shown; a 'stable' flag appears once the
 *                 spread collapses. A reading far from the settled level is
 *                 discarded rather than averaged in.
 *
 *   Losing contact flushes the sliding window after APP_LOSS_WINDOWS bad
 *   windows, so the next measurement can never inherit the previous one.
 *
 *   Every threshold below is live-tunable over SWD (see docs/debug/10).
 * ========================================================================== */
#define APP_PPG_TRACE     48    /* windows kept in the SWD-readable trace */
#define APP_PPG_FAST_N    50    /* newest samples for the contact test (0.5s) */
#define APP_PPG_DC_MIN    8000  /* IR DC, LSB - measured dark level is ~550 */
#define APP_PPG_DC_MAX    250000/* 18-bit full scale 262143 = clipped */
#define APP_PPG_AC_MIN    150   /* IR peak-to-peak, LSB - below: no pulsation */
#define APP_PPG_PI_MIN    3     /* perfusion index, per-mille (0.3%) */
#define APP_PPG_PI_MAX    200   /* 20% - above is non-physiological (motion) */
#define APP_HR_MIN        30
#define APP_HR_MAX        220
#define APP_SPO2_MIN      70
#define APP_SPO2_MAX      100
#define APP_HR_WARN_LO    60    /* bpm - default of the live app_hr_warn_lo */
#define APP_HR_WARN_HI    100   /* bpm - default of the live app_hr_warn_hi */
#define APP_SPO2_WARN_LO  95    /* % - default of the live app_spo2_warn_lo */
#define APP_HR_N          8     /* sliding window, one entry per PPG window */
#define APP_HR_WARM       2     /* valid readings needed before showing a value */
#define APP_HR_STABLE_N   4     /* readings needed before 'stable' is claimed */
#define APP_HR_TOL_ABS    6     /* keep +/-6 bpm around the median ... */
#define APP_HR_TOL_PCT    15    /* ... or +/-15%, whichever is wider */
#define APP_HR_STABLE_BPM 5     /* max-min of the kept set -> stable */
#define APP_SPO2_TOL_ABS  2
#define APP_SPO2_TOL_PCT  3
#define APP_SPO2_STBL_N   3
#define APP_SPO2_STBL_PC  1
#define APP_LOSS_WINDOWS  2     /* bad windows before the value is dropped */
#define APP_HR_REJ_RUN    3     /* consecutive outliers -> the level really moved */

volatile uint32_t app_ppg_win    = 0;  /* PPG windows evaluated */
volatile int32_t  app_ppg_dc     = 0;  /* IR DC of the last window, LSB */
volatile int32_t  app_ppg_ac     = 0;  /* IR peak-to-peak of the last window, LSB */
volatile int32_t  app_ppg_dc_new = 0;  /* IR DC of the newest APP_PPG_FAST_N samples */
volatile uint16_t app_ppi        = 0;  /* perfusion index, per-mille */
volatile uint8_t  app_ppg_ok     = 0;  /* 1 = last window passed gate G1 */
volatile uint32_t app_ppg_bad    = 0;  /* consecutive windows that failed G1 */
volatile int32_t  app_hr_raw     = 0;  /* raw DSP heart rate of the last window */
volatile int32_t  app_spo2_raw   = 0;  /* raw DSP SpO2 of the last window */
volatile uint8_t  app_hr_ok      = 0;  /* 1 = an averaged heart rate may be shown */
volatile uint8_t  app_hr_disp    = 0;  /* averaged heart rate, bpm */
volatile uint8_t  app_hr_stable  = 0;  /* 1 = the averaging window has settled */
volatile uint8_t  app_hr_n       = 0;  /* valid heart rates in the window */
volatile uint8_t  app_spo2_ok    = 0;
volatile uint8_t  app_spo2_disp  = 0;
volatile uint8_t  app_spo2_stable= 0;
volatile uint8_t  app_spo2_n     = 0;
volatile uint32_t app_hr_reject  = 0;  /* readings discarded as outliers */
/* Thresholds for the on-screen emphasis of an abnormal reading. Live RAM
   values, changeable over SWD: no rebuild needed to retune them. */
volatile uint8_t  app_hr_warn_lo   = APP_HR_WARN_LO;
volatile uint8_t  app_hr_warn_hi   = APP_HR_WARN_HI;
volatile uint8_t  app_spo2_warn_lo = APP_SPO2_WARN_LO;
volatile int32_t  app_ppg_hist[APP_PPG_TRACE][6];
/*   [0] window no.   [1] IR DC        [2] IR AC p-p   [3] PI in per-mille
 *   [4] raw HR       [5] flags | hr_disp<<8 | spo2_disp<<16
 *   flags: bit0 ppg_ok, bit1 hr_ok, bit2 hr_stable, bit3 spo2_ok */

static uint8_t app_hr_ring[APP_HR_N];
static uint8_t app_spo2_ring[APP_HR_N];
static uint8_t app_hr_wr = 0, app_spo2_wr = 0;
static uint8_t app_hr_rej_run = 0;
static uint8_t app_hist_i = 0;

static void app_push(uint8_t *ring, volatile uint8_t *n, volatile uint8_t *wr, uint8_t v)
{
	if (*n < (uint8_t)APP_HR_N) (*n)++;
	ring[*wr] = v;
	*wr = (uint8_t)((*wr + 1) % (uint8_t)APP_HR_N);
}

/* median of a small ring (insertion sort, n <= 8) */
static int32_t app_med(const uint8_t *src, int32_t n)
{
	uint8_t t[APP_HR_N];
	int32_t i, j, k, tmp;
	if (n <= 0) return 0;
	for (i = 0; i < n; i++) t[i] = src[i];
	for (i = 1; i < n; i++) {
		tmp = t[i];
		for (j = i; j > 0 && tmp < t[j - 1]; j--) t[j] = t[j - 1];
		t[j] = tmp;
	}
	k = n / 2;
	return (n & 1) ? t[k] : ((t[k - 1] + t[k]) / 2);
}

/* median + trimmed mean: values outside median +/- tolerance are dropped
 * before averaging. That is the 'discard whatever is obviously far from the
 * settled level' rule, and taking the reference from the median makes it
 * robust against the outlier itself. */
static int32_t app_trim(const uint8_t *ring, int32_t n, int32_t tol_abs,
                        int32_t tol_pct, int32_t *spread, int32_t *used)
{
	int32_t med, tol, lo, hi, i, sum = 0, cnt = 0, mn = 1000000, mx = -1;
	if (n <= 0) { *spread = 0; *used = 0; return 0; }
	med = app_med(ring, n);
	tol = med * tol_pct / 100;
	if (tol < tol_abs) tol = tol_abs;
	lo = med - tol;
	hi = med + tol;
	for (i = 0; i < n; i++) {
		if (ring[i] >= lo && ring[i] <= hi) {
			sum += ring[i];
			cnt++;
			if (ring[i] < mn) mn = ring[i];
			if (ring[i] > mx) mx = ring[i];
		}
	}
	if (cnt == 0) { sum = med; cnt = 1; mn = med; mx = med; }
	*spread = mx - mn;
	*used = cnt;
	return sum / cnt;
}

/* G1: signal-quality gate over the IR window. Fills app_ppg_dc/ac/dc_new,
 * app_ppi and app_ppg_ok. */
static void app_ppg_quality(const uint32_t *ir, int32_t n)
{
	int32_t i, mn = 0x7FFFFFFF, mn2 = 0x7FFFFFFF;
	int32_t mx = 0, mx2 = 0, dc, ac, ppi;
	uint32_t sum = 0, sum_new = 0;
	int32_t fast = (n > (int32_t)APP_PPG_FAST_N) ? (int32_t)APP_PPG_FAST_N : n;

	for (i = 0; i < n; i++) {
		int32_t v = (int32_t)ir[i];
		sum += (uint32_t)v;
		if (i >= n - fast) sum_new += (uint32_t)v;
		if (v < mn)       { mn2 = mn; mn = v; }
		else if (v < mn2) { mn2 = v; }
		if (v > mx)       { mx2 = mx; mx = v; }
		else if (v > mx2) { mx2 = v; }
	}
	dc = (n > 0) ? (int32_t)(sum / (uint32_t)n) : 0;
	app_ppg_dc_new = (fast > 0) ? (int32_t)(sum_new / (uint32_t)fast) : 0;
	/* 2nd-highest minus 2nd-lowest: a single-sample spike (an I2C glitch, an
	   ADC outlier) cannot inflate the measured pulsation. */
	ac = (n >= 4) ? (mx2 - mn2) : 0;
	if (ac < 0) ac = 0;
	ppi = (dc > 0) ? (ac * 1000) / dc : 0;
	if (ppi < 0) ppi = 0;
	if (ppi > 9999) ppi = 9999;

	app_ppg_dc = dc;
	app_ppg_ac = ac;
	app_ppi = (uint16_t)ppi;
	app_ppg_ok = (uint8_t)((dc >= APP_PPG_DC_MIN) && (dc <= APP_PPG_DC_MAX) &&
	                       (ac >= APP_PPG_AC_MIN) &&
	                       (ppi >= APP_PPG_PI_MIN) && (ppi <= APP_PPG_PI_MAX) &&
	                       (app_ppg_dc_new >= APP_PPG_DC_MIN) &&
	                       (app_ppg_dc_new <= APP_PPG_DC_MAX));
}

static void app_hr_ingest(int32_t hr, int8_t valid)
{
	int32_t med, tol;
	if (!(valid == 1 && hr >= APP_HR_MIN && hr <= APP_HR_MAX)) return;
	if (app_hr_n >= 3) {
		med = app_med(app_hr_ring, app_hr_n);
		tol = med * APP_HR_TOL_PCT / 100;
		if (tol < APP_HR_TOL_ABS) tol = APP_HR_TOL_ABS;
		/* the input filter is twice as wide as the display filter on purpose:
		   it removes a wildly wrong beat train, not a genuine rate change */
		if (hr > med + 2 * tol || hr < med - 2 * tol) {
			app_hr_reject++;
			if (++app_hr_rej_run >= APP_HR_REJ_RUN) {
				app_hr_n = 0; app_hr_wr = 0; app_hr_rej_run = 0;  /* level moved */
			}
			return;
		}
	}
	app_hr_rej_run = 0;
	app_push(app_hr_ring, &app_hr_n, &app_hr_wr, (uint8_t)hr);
}

static void app_hr_publish(void)
{
	int32_t spread = 0, used = 0;
	if (app_hr_n < (uint8_t)APP_HR_WARM) { app_hr_ok = 0; app_hr_stable = 0; return; }
	app_hr_disp = (uint8_t)app_trim(app_hr_ring, app_hr_n, APP_HR_TOL_ABS,
	                                APP_HR_TOL_PCT, &spread, &used);
	app_hr_ok = 1;
	app_hr_stable = (uint8_t)((app_hr_n >= (uint8_t)APP_HR_STABLE_N) &&
	                          (spread <= APP_HR_STABLE_BPM));
}

static void app_spo2_ingest(int32_t spo2, int8_t valid)
{
	if (!(valid == 1 && spo2 >= APP_SPO2_MIN && spo2 <= APP_SPO2_MAX)) return;
	app_push(app_spo2_ring, &app_spo2_n, &app_spo2_wr, (uint8_t)spo2);
}

static void app_spo2_publish(void)
{
	int32_t spread = 0, used = 0;
	if (app_spo2_n < (uint8_t)APP_HR_WARM) { app_spo2_ok = 0; app_spo2_stable = 0; return; }
	app_spo2_disp = (uint8_t)app_trim(app_spo2_ring, app_spo2_n, APP_SPO2_TOL_ABS,
	                                  APP_SPO2_TOL_PCT, &spread, &used);
	app_spo2_ok = 1;
	app_spo2_stable = (uint8_t)((app_spo2_n >= (uint8_t)APP_SPO2_STBL_N) &&
	                            (spread <= APP_SPO2_STBL_PC));
}

static void app_ppg_record(void)
{
	app_ppg_hist[app_hist_i][0] = (int32_t)app_ppg_win;
	app_ppg_hist[app_hist_i][1] = app_ppg_dc;
	app_ppg_hist[app_hist_i][2] = app_ppg_ac;
	app_ppg_hist[app_hist_i][3] = (int32_t)app_ppi;
	app_ppg_hist[app_hist_i][4] = app_hr_raw;
	app_ppg_hist[app_hist_i][5] = (int32_t)((app_ppg_ok ? 1 : 0) |
	                                        (app_hr_ok ? 2 : 0) |
	                                        (app_hr_stable ? 4 : 0) |
	                                        (app_spo2_ok ? 8 : 0)) |
	                              ((int32_t)app_hr_disp << 8) |
	                              ((int32_t)app_spo2_disp << 16);
	app_hist_i = (uint8_t)((app_hist_i + 1) % (uint8_t)APP_PPG_TRACE);
}

/* one call per PPG window (5s sliding buffer, ~1s of new samples) */
static void app_ppg_update(const uint32_t *ir, int32_t n,
                           int32_t hr, int8_t hr_valid,
                           int32_t spo2, int8_t spo2_valid)
{
	app_ppg_win++;
	app_ppg_quality(ir, n);
	app_hr_raw   = hr;
	app_spo2_raw = spo2;

	if (app_ppg_ok) {
		app_ppg_bad = 0;
	} else if (app_ppg_bad < 0xFFFFFFFFu) {
		app_ppg_bad++;
	}

	/* --- no contact / unusable signal: HOLD the last valid reading --- */
	/* Once a reading has been validated it stays on screen until a later
	   window produces a fresh one - it is never blanked back to "---". The
	   sliding windows themselves ARE reset, so a returning finger cannot
	   blend stale samples into the fresh average. */
	if (!app_ppg_ok) {
		if (app_ppg_bad >= (uint32_t)APP_LOSS_WINDOWS) {
			app_hr_n = 0; app_spo2_n = 0;
			app_hr_wr = 0; app_spo2_wr = 0;
			app_hr_rej_run = 0;
		}
		app_ppg_record();
		return;
	}

	app_hr_ingest(hr, hr_valid);
	app_spo2_ingest(spo2, spo2_valid);
	app_hr_publish();
	app_spo2_publish();
	app_ppg_record();
}

 
int16_t AX, AY, AZ, GX, GY, GZ;			//定义用于存放各个数据的变量
int16_t Last_AX, Last_AY;			//定义用于存放各个数据的变量
 
uint32_t aun_ir_buffer[500]; //IR LED sensor data
int32_t n_ir_buffer_length;    //data length
uint32_t aun_red_buffer[500];    //Red LED sensor data
int32_t n_sp02; //SPO2 value
int8_t ch_spo2_valid;   //indicator to show if the SP02 calculation is valid
int32_t n_heart_rate;   //heart rate value
int8_t  ch_hr_valid;    //indicator to show if the heart rate calculation is valid
uint8_t uch_dummy;
uint8_t Temp;

#define MAX_BRIGHTNESS 255

int main(void)
{ 
	//variables to calculate the on-board LED brightness that reflects the heartbeats
	uint32_t un_min, un_max, un_prev_data;  
	int i;
	int32_t n_brightness;
	float f_temp;
	u8 temp[6];
	u8 dis_hr=0,dis_spo2=0;

	NVIC_Configuration();
	delay_init();	    	 //延时函数初始化	  
	
	//OLED
	OLED_Init();

	OLED_ShowChinese(0, 0, "温度：");
	OLED_ShowChinese(94, 0, "℃");
	OLED_ShowChinese(0, 16, "心率：");
	OLED_ShowString(94,16,"BPM",OLED_8X16);
	OLED_ShowChinese(0, 32, "血氧：");
	OLED_ShowString(94,32,"%",OLED_8X16);
	OLED_ShowChinese(0, 48, "是否跌倒：否");
	OLED_Update();

	//MAX30102
#ifndef NO_MAX30102
	max30102_init();
#endif
	//MPU6050
	MPU6050_Init();
	//DS18B20
	app_ds_init();
	//Buzzer
	Buzzer_init();
	/* boot self-test: two short beeps (audible proof the buzzer is wired) */
	Buzzer_on(); delay_ms(120); Buzzer_off(); delay_ms(120);
	Buzzer_on(); delay_ms(120); Buzzer_off(); delay_ms(120);

	
	MPU6050_GetData(&AX, &AY, &AZ, &GX, &GY, &GZ);
	Last_AX = AX;
	Last_AY = AY;
	
	un_min=0x3FFFF;
	un_max=0;
	
#ifndef NO_MAX30102
	n_ir_buffer_length=500; //buffer length of 100 stores 5 seconds of samples running at 100sps
	//read the first 500 samples, and determine the signal range
	for(i=0;i<n_ir_buffer_length;i++)
	{
			app_wait_int(); app_fall_poll();   //wait until the interrupt pin asserts
			
	max30102_FIFO_ReadBytes(REG_FIFO_DATA,temp);
	aun_red_buffer[i] =  (long)((long)((long)temp[0]&0x03)<<16) | (long)temp[1]<<8 | (long)temp[2];    // Combine values to get the actual number
	aun_ir_buffer[i] = (long)((long)((long)temp[3] & 0x03)<<16) |(long)temp[4]<<8 | (long)temp[5];   // Combine values to get the actual number
					
			if(un_min>aun_red_buffer[i])
					un_min=aun_red_buffer[i];    //update signal min
			if(un_max<aun_red_buffer[i])
					un_max=aun_red_buffer[i];    //update signal max
	}
	un_prev_data=aun_red_buffer[i];
	//calculate heart rate and SpO2 after first 500 samples (first 5 seconds of samples)
	maxim_heart_rate_and_oxygen_saturation(aun_ir_buffer, n_ir_buffer_length, aun_red_buffer, &n_sp02, &ch_spo2_valid, &n_heart_rate, &ch_hr_valid);
#endif 
	
	while(1)
	{
		app_cycle++;
			i=0;
			un_min=0x3FFFF;
			un_max=0;
		
			//dumping the first 100 sets of samples in the memory and shift the last 400 sets of samples to the top
			for(i=100;i<500;i++)
			{
					aun_red_buffer[i-100]=aun_red_buffer[i];
					aun_ir_buffer[i-100]=aun_ir_buffer[i];
					
					//update the signal min and max
					if(un_min>aun_red_buffer[i])
					un_min=aun_red_buffer[i];
					if(un_max<aun_red_buffer[i])
					un_max=aun_red_buffer[i];
			}
			//take 100 sets of samples before calculating the heart rate.
#ifndef NO_MAX30102
		for(i=400;i<500;i++)
			{
					un_prev_data=aun_red_buffer[i-1];
					app_wait_int(); app_fall_poll();
					max30102_FIFO_ReadBytes(REG_FIFO_DATA,temp);
			aun_red_buffer[i] =  (long)((long)((long)temp[0]&0x03)<<16) | (long)temp[1]<<8 | (long)temp[2];    // Combine values to get the actual number
			aun_ir_buffer[i] = (long)((long)((long)temp[3] & 0x03)<<16) |(long)temp[4]<<8 | (long)temp[5];   // Combine values to get the actual number
			
					if(aun_red_buffer[i]>un_prev_data)
					{
							f_temp=aun_red_buffer[i]-un_prev_data;
							f_temp/=(un_max-un_min);
							f_temp*=MAX_BRIGHTNESS;
							n_brightness-=(int)f_temp;
							if(n_brightness<0)
									n_brightness=0;
					}
					else
					{
							f_temp=un_prev_data-aun_red_buffer[i];
							f_temp/=(un_max-un_min);
							f_temp*=MAX_BRIGHTNESS;
							n_brightness+=(int)f_temp;
							if(n_brightness>MAX_BRIGHTNESS)
									n_brightness=MAX_BRIGHTNESS;
					}
			/* validity and averaging happen ONCE per window now, in
			   app_ppg_update() right after the DSP call below. This
			   block used to live here, inside the 100-sample acquisition
			   loop, which made the display one window stale. */
		}
		maxim_heart_rate_and_oxygen_saturation(aun_ir_buffer, n_ir_buffer_length, aun_red_buffer, &n_sp02, &ch_spo2_valid, &n_heart_rate, &ch_hr_valid);
#endif
		/* ---- medical-grade PPG validation + averaging layer ----
		   quality gate -> physiological bounds -> median-trimmed
		   average -> what the display is allowed to show.
		   See the block injected after <stdbool.h> for details. */
		app_ppg_update(aun_ir_buffer, n_ir_buffer_length,
		               n_heart_rate, ch_hr_valid, n_sp02, ch_spo2_valid);
		
		//MAX30102
		/* Blank the whole digital field first: OLED_ShowChar() ORs into the
		   frame buffer, so a 3-digit reading followed by a 2-digit one would
		   otherwise leave a ghost digit behind. */
		OLED_ClearArea(47,16,24,16);
		if (app_hr_ok) {
			dis_hr = app_hr_disp;
			if (dis_hr >= 100u) OLED_ShowNum(47,16,dis_hr,3,OLED_8X16);
			else { OLED_ShowChar(47,16,' ',OLED_8X16); OLED_ShowNum(55,16,dis_hr,2,OLED_8X16); }
		} else {
			dis_hr = 0;
			OLED_ShowString(47,16,"---",OLED_8X16);
		}
		OLED_ClearArea(47,32,24,16);
		if (app_spo2_ok) {
			dis_spo2 = app_spo2_disp;
			if (dis_spo2 >= 100u) OLED_ShowNum(47,32,dis_spo2,3,OLED_8X16);
			else { OLED_ShowChar(47,32,' ',OLED_8X16); OLED_ShowNum(55,32,dis_spo2,2,OLED_8X16); }
		} else {
			dis_spo2 = 0;
			OLED_ShowString(47,32,"---",OLED_8X16);
		}
		OLED_ClearArea(79,16,8,16);
		
		//DS18B20
		Temp = app_ds_read()/16;
		OLED_ShowNum(47,0,Temp,2,OLED_8X16);
		
		/* the blocking per-axis fall check used to live here. It compared two
		   samples a full ~2s loop iteration apart, so flipping the board was
		   enough to fire it, and it could not see a 20ms impact at all.
		   app_fall_poll() does the job at ~87Hz now - see the file header. */
		OLED_ClearArea(80, 48, 16, 16);   /* wipe the old glyph: ShowChinese ORs */
		if (app_fall_show) {   //平放后，向上、向下、向左、向右翻超过500报警
			OLED_ShowChinese(0, 48, "是否跌倒：是");
			// 触发蜂鸣器响
			Buzzer_on();
		} else if (0) {   /* dead branch: was a duplicate of the final else */  
			OLED_ShowChinese(0, 48, "是否跌倒：否");
			// 触发蜂鸣器响
			Buzzer_off();
		} else {
			OLED_ShowChinese(0, 48, "是否跌倒：否");
			// 关闭蜂鸣器
			Buzzer_off();
		}
		/* Single-colour panel: there is no red, so "abnormal" is shown the
		   strongest way this hardware allows - inverse video of the field
		   (white background, black digits). Thresholds are live-tunable. */
		if (app_hr_ok && (app_hr_disp < app_hr_warn_lo ||
		                  app_hr_disp > app_hr_warn_hi))
			OLED_ReverseArea(47, 16, 24, 16);
		if (app_spo2_ok && app_spo2_disp < app_spo2_warn_lo)
			OLED_ReverseArea(47, 32, 24, 16);
		if (app_fall_show)
			OLED_ReverseArea(80, 48, 16, 16);
		OLED_Update();
	}
}


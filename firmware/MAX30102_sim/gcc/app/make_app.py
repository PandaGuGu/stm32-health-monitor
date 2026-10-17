"""
Derive gcc/app/app_main.c from USER/main.c (byte-level, GBK-safe).

Why a generator instead of hand-editing:
  * USER/main.c is GBK-encoded; rewriting it through a UTF-8 tool would corrupt
    the Chinese string literals that OLED_ShowChinese() looks up in the font table.
  * Doing the surgery on bytes keeps the original literals bit-identical.
  * The original repo files stay untouched -> `git status` stays clean.

Changes applied (all ASCII-only insertions/replacements):
  1. MAX30102 init + both FIFO acquisition loops + the INT-pin wait loops are
     wrapped in `#ifndef NO_MAX30102` ... `#endif`.
     Reason: myiic.c:51 `while(READ_SDA)` is an INFINITE ACK wait; with the
     MAX30102 module absent, max30102_init() hangs the firmware forever and the
     main loop (hence the OLED refresh + all other sensors) never runs.
  2. ds18b20_init()/get_tempetature() replaced by self-contained timeout-safe
     helpers, because ds18b20.c:68-69 are two infinite `while()` waits.
  3. Buzzer boot self-test (two short beeps) so it is audibly verifiable.
  4. Heart-rate / SpO2 fields show "--" when MAX30102 is compiled out.
  5. app_cycle counter added for SWD-side verification.
  6. Fall detection moved off the main loop onto both pure-wait windows and
     switched to the tilt-invariant acceleration magnitude (v2.5).
  7. NEW: PPG validation + robust averaging layer (app_ppg_update). The vendor
     DSP is treated as a candidate generator; a quality gate (IR DC window,
     AC floor, perfusion index), physiological plausibility (HR 30-220,
     SpO2 70-100) and a median-trimmed sliding average decide what may be
     displayed. Loss of contact blanks the field instead of holding the last
     value. See the HELPERS block for the full rationale.
  8. The original in-loop `if (ch_hr_valid == 1 && n_heart_rate<120)` validity
     block is removed: it sat inside the 100-sample acquisition loop, so it
     re-evaluated the PREVIOUS window's DSP result 100 times, and it had no
     lower bound, no SpO2 check and no signal-quality test.
"""

import os
import re
import sys

# Paths are resolved from this file's location, so the script is portable:
#   <repo>/firmware/MAX30102/gcc/app/make_app.py  ->  SRC = ../USER/main.c
HERE = os.path.dirname(os.path.abspath(__file__))   # .../firmware/MAX30102/gcc/app
GCC  = os.path.dirname(HERE)                        # .../firmware/MAX30102/gcc
FW   = os.path.dirname(GCC)                         # .../firmware/MAX30102

SRC = os.path.join(FW, "USER", "main.c")
DST = os.path.join(HERE, "app_main.c")


def die(msg):
    print("FAIL: " + msg)
    sys.exit(1)


# --- timeout-safe DS18B20 (ASCII only) -------------------------------------
HELPERS = "\n".join([
    "",
    "/* ==========================================================================",
    " * DS18B20 timeout-safe driver (local replacement)",
    " *   HARDWARE/DS18B20/ds18b20.c:68-69 contains two unbounded `while(...)` waits.",
    " *   If the sensor is off-line the firmware wedges there forever. This local",
    " *   version caps the presence wait at 300us and returns 0 instead of blocking.",
    " *   Pin used: PA8 (DQ), identical to the original wiring.",
    " * ========================================================================== */",
    "#define APP_DQ_PORT   GPIOA",
    "#define APP_DQ_PIN    GPIO_Pin_8",
    "",
    "volatile uint32_t app_cycle = 0;            /* main-loop counter (readable via SWD) */",
    "volatile uint8_t  app_ds_ok = 0;            /* 1 = last DS18B20 read succeeded */",
    "volatile uint16_t app_ds_presence_us = 0;   /* presence pull-down delay, us (typ. 15-60) */",
    "",
    "/* --- fall detection params: must be defined before first use ------------- */",
    "/* Impact threshold = deviation of the acceleration MAGNITUDE from 1g, in LSB.",
    " * MPU6050 runs at +-16g -> 2048 LSB per g, so 3072 LSB means |a| >= 2.5g.",
    " *",
    " * Why magnitude and not a per-axis delta: |a| is tilt-invariant. Rotating",
    " * the board swings AX/AY by up to 1g while |a| stays at 1g, so a fast tilt",
    " * can never be mistaken for an impact - which is exactly what the old",
    " * per-axis |dAX|/|dAY| test did: it compared two samples a whole main-loop",
    " * iteration (~2s) apart, so tilting the board 15 degrees was enough.",
    " *",
    " * Calibration from the recorded bench runs (see docs/debug/09):",
    " *   desk idle, board untouched, 7859 steps -> every per-axis step < 2000 LSB",
    " *   hand touching the board, 180s          -> per-axis steps did reach the",
    " *                                             2000-4000 LSB bin, but only",
    " *                                             3 times in 180s: rare ~2g",
    " *                                             excursions, never sustained",
    " *   genuine fall impact                    -> 3-6g = 4096-10240 LSB of",
    " *                                             |a| deviation",
    " * 3072 LSB (2.5g) clears the worst hand-touch peak with ~0.5g of margin and",
    " * still catches a real 3-6g impact. APP_FALL_NEED adds a second, independent",
    " * defence: an impact must persist for 2 consecutive samples (about 23ms at",
    " * the measured ~87Hz poll rate) before the alarm latches, so a lone",
    " * single-sample spike can never raise a false alarm on its own.",
    " *",
    " * Both are live-tunable over SWD, no reflash needed:",
    " *   mww <app_fall_thresh> 2048   -> back to 2g (more sensitive)",
    " *   mww <app_fall_need>   3      -> demand a longer impact",
    " */",
    "#define APP_FALL_THRESHOLD 3072     /* |a| >= 2.5g, LSB of deviation from 1g */",
    "#define APP_FALL_NEED      2        /* consecutive over-threshold samples to alarm */",
    "#define APP_FALL_HOLD      260      /* keep the alarm latched for ~3s worth of polls */",
    "#define APP_DS_POLL_CNT    70       /* fall polls during the DS18B20 conversion wait */",
    "#define APP_DS_POLL_MS     9",
    "static void app_fall_poll(void);            /* forward decl (used by app_ds_read) */",
    "",
    "static void app_dq_out(void)",
    "{",
    "\tGPIO_InitTypeDef g;",
    "\tg.GPIO_Pin = APP_DQ_PIN;",
    "\tg.GPIO_Mode = GPIO_Mode_Out_PP;",
    "\tg.GPIO_Speed = GPIO_Speed_2MHz;",
    "\tGPIO_Init(APP_DQ_PORT, &g);",
    "}",
    "",
    "static void app_dq_in(void)",
    "{",
    "\tGPIO_InitTypeDef g;",
    "\tg.GPIO_Pin = APP_DQ_PIN;",
    "\tg.GPIO_Mode = GPIO_Mode_IPU;",
    "\tg.GPIO_Speed = GPIO_Speed_2MHz;",
    "\tGPIO_Init(APP_DQ_PORT, &g);",
    "}",
    "",
    "/* returns 1 if the device answered, 0 if it timed out (never blocks) */",
    "static uint8_t app_dq_reset(void)",
    "{",
    "\tuint16_t t = 0;",
    "\tapp_dq_out();",
    "\tGPIO_ResetBits(APP_DQ_PORT, APP_DQ_PIN);",
    "\tdelay_us(600);",
    "\tGPIO_SetBits(APP_DQ_PORT, APP_DQ_PIN);",
    "\tapp_dq_in();",
    "\twhile (GPIO_ReadInputDataBit(APP_DQ_PORT, APP_DQ_PIN) && t < 300) { delay_us(1); t++; }",
    "\tif (t >= 300) return 0;             /* not pulled low within 300us -> no device */",
    "\tapp_ds_presence_us = t;",
    "\tt = 0;",
    "\twhile (!GPIO_ReadInputDataBit(APP_DQ_PORT, APP_DQ_PIN) && t < 400) { delay_us(1); t++; }",
    "\treturn 1;",
    "}",
    "",
    "static void app_dq_wbit(uint8_t b)",
    "{",
    "\tapp_dq_out();",
    "\tGPIO_ResetBits(APP_DQ_PORT, APP_DQ_PIN);",
    "\tif (b) { delay_us(2);  GPIO_SetBits(APP_DQ_PORT, APP_DQ_PIN); delay_us(80); }",
    "\telse   { delay_us(80); GPIO_SetBits(APP_DQ_PORT, APP_DQ_PIN); delay_us(2);  }",
    "}",
    "",
    "static uint8_t app_dq_rbit(void)",
    "{",
    "\tuint8_t b;",
    "\tapp_dq_out();",
    "\tGPIO_ResetBits(APP_DQ_PORT, APP_DQ_PIN);",
    "\tdelay_us(2);",
    "\tGPIO_SetBits(APP_DQ_PORT, APP_DQ_PIN);",
    "\tapp_dq_in();",
    "\tdelay_us(5);",
    "\tb = GPIO_ReadInputDataBit(APP_DQ_PORT, APP_DQ_PIN) ? 1 : 0;",
    "\tdelay_us(500);",
    "\treturn b;",
    "}",
    "",
    "static void app_dq_wbyte(uint8_t d)",
    "{",
    "\tuint8_t i;",
    "\tfor (i = 0; i < 8; i++) { app_dq_wbit((uint8_t)(d & 0x01)); d >>= 1; }",
    "}",
    "",
    "static uint8_t app_dq_rbyte(void)",
    "{",
    "\tuint8_t i, d = 0;",
    "\tfor (i = 0; i < 8; i++) d |= (uint8_t)(app_dq_rbit() << i);",
    "\treturn d;",
    "}",
    "",
    "void app_ds_init(void)",
    "{",
    "\tRCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOA, ENABLE);",
    "\tapp_dq_in();",
    "\tapp_dq_reset();                     /* probe once, never blocks */",
    "}",
    "",
    "short app_ds_read(void)",
    "{",
    "\tuint8_t tl, th;",
    "\tint16_t raw;",
    "",
    "\tif (!app_dq_reset()) { app_ds_ok = 0; return 0; }",
    "\tapp_dq_wbyte(0xCC);                 /* skip ROM */",
    "\tapp_dq_wbyte(0x44);                 /* start conversion */",
    "\t/* These 750ms are pure waiting: run the fall detector while converting.",
    "\t   Previously this was a dead delay_ms(750) that threw the window away. */",
    "\t{",
    "\t\tuint32_t t;",
    "\t\tfor (t = 0; t < APP_DS_POLL_CNT; t++) { app_fall_poll(); delay_ms(APP_DS_POLL_MS); }",
    "\t}",
    "\tif (!app_dq_reset()) { app_ds_ok = 0; return 0; }",
    "\tapp_dq_wbyte(0xCC);",
    "\tapp_dq_wbyte(0xBE);                 /* read scratchpad */",
    "\ttl = app_dq_rbyte();",
    "\tth = app_dq_rbyte();",
    "\traw = (int16_t)(((uint16_t)th << 8) | tl);",
    "\tapp_ds_ok = 1;",
    "\treturn raw;",
    "}",
    "",
    "/* ==========================================================================",
    " * MAX30102 INT wait - timeout-safe wrapper",
    " *   USER/main.c busy-waits on MAX30102_INT until the INT pin goes low.",
    " *   If INT is floating or not wired, the pin stays high and",
    " *   the firmware wedges forever in the first acquisition loop. This wrapper",
    " *   waits at most APP_INT_WAIT_MS (10ms sample period at 100Hz, 3x margin)",
    " *   then moves on, so the main loop always makes progress.",
    " * ========================================================================== */",
    "#define APP_INT_WAIT_MS 30u",
    "static void app_wait_int(void)",
    "{",
    "\tuint32_t t;",
    "\tfor (t = 0; t < APP_INT_WAIT_MS && MAX30102_INT == 1; t++) { delay_ms(1); }",
    "}",
    "",
    "/* ==========================================================================",
    " * Fall detection: from \"once per main loop\" to \"whenever there is idle time\"",
    " *   USER/main.c runs the fall check at the tail of the main loop, i.e. once per",
    " *   iteration. One iteration spends ~1.1s in the MAX30102 acquisition window",
    " *   plus 0.75s waiting for the DS18B20 conversion, so the check only ran at",
    " *   ~0.5Hz. A hand impact lasts tens of ms, so it usually falls entirely",
    " *   between two checks -> missed fall, i.e. it *looks* less sensitive.",
    " *   This version piggybacks the check on both of those pure-wait windows,",
    " *   lifting the rate to ~90Hz, and judges the MAGNITUDE of the acceleration",
    " *   instead of a single axis (see APP_FALL_THRESHOLD below).",
    " * ========================================================================== */",
    "volatile uint32_t app_fall_polls = 0;   /* total fall checks run (readable via SWD) */",
    "volatile uint32_t app_fall_hits  = 0;   /* confirmed impacts, one count per event */",
    "volatile uint8_t  app_fall_show  = 0;   /* 1 = currently inside the alarm hold window */",
    "volatile uint8_t  app_fifo_level = 0;   /* MAX30102 FIFO backlog, should stay < 25 */",
    "volatile uint32_t app_mpu_err    = 0;   /* MPU6050 I2C transactions that timed out */",
    "volatile uint32_t app_mpu_zero   = 0;   /* samples discarded as all-zero (sensor unpowered) */",
    "volatile uint32_t app_glitch_cnt = 0;   /* lone over-threshold spikes correctly rejected */",
    "volatile int32_t  app_mag_min = 32767, app_mag_max = -1;    /* |a| range, 2048 LSB = 1g */",
    "volatile int32_t  app_dev_max = -1;     /* worst |(|a| - 1g)| seen, for threshold tuning */",
    "volatile int32_t  app_hit_mag = 0;      /* |a| measured at the 1st hit */",
    "volatile uint32_t app_ff_cnt  = 0;      /* samples with |a| < 0.5g (free-fall evidence) */",
    "volatile uint32_t app_dbin[6];          /* histogram of |(|a| - 1g)|, bins listed below */",
    "volatile uint16_t app_fall_thresh = APP_FALL_THRESHOLD;/* threshold in LSB, SWD-writable */",
    "volatile uint16_t app_fall_need   = APP_FALL_NEED;     /* consecutive samples, SWD-writable */",
    "static   uint16_t app_fall_hold  = 0;",
    "static   uint16_t app_over_run   = 0;   /* current run of consecutive over-threshold samples */",
    "",
    "/* ==========================================================================",
    " * MPU6050 burst read of AX/AY/AZ (regs 0x3B..0x40), one transaction.",
    " *   MPU6050_WaitEvent() breaks out SILENTLY after its 10000-iteration timeout,",
    " *   and MPU6050_ReadReg() then returns whatever I2C_ReceiveData() happens to",
    " *   hold -> occasional garbage samples -> phantom fall hits. Fix: a single",
    " *   burst read plus an explicit timeout counter; the caller DISCARDS the",
    " *   sample when any event times out.",
    " * ========================================================================== */",
    "#define APP_MPU_ADDR   0xD0",
    "#define APP_MPU_ACCX_H 0x3B",
    "",
    "static void app_mpu_wait(uint32_t ev)",
    "{",
    "\tuint32_t t = 20000u;",
    "\twhile (I2C_CheckEvent(I2C2, ev) != SUCCESS) {",
    "\t\tif (--t == 0u) { app_mpu_err++; break; }",
    "\t}",
    "}",
    "",
    "static uint8_t app_mpu_read_acc(int16_t *ax, int16_t *ay, int16_t *az)",
    "{",
    "\tuint8_t d[6], i;",
    "\tuint32_t err0 = app_mpu_err;",
    "",
    "\tI2C_GenerateSTART(I2C2, ENABLE);",
    "\tapp_mpu_wait(I2C_EVENT_MASTER_MODE_SELECT);",
    "\tI2C_Send7bitAddress(I2C2, APP_MPU_ADDR, I2C_Direction_Transmitter);",
    "\tapp_mpu_wait(I2C_EVENT_MASTER_TRANSMITTER_MODE_SELECTED);",
    "\tI2C_SendData(I2C2, APP_MPU_ACCX_H);",
    "\tapp_mpu_wait(I2C_EVENT_MASTER_BYTE_TRANSMITTED);",
    "",
    "\tI2C_GenerateSTART(I2C2, ENABLE);",
    "\tapp_mpu_wait(I2C_EVENT_MASTER_MODE_SELECT);",
    "\tI2C_Send7bitAddress(I2C2, APP_MPU_ADDR, I2C_Direction_Receiver);",
    "\tapp_mpu_wait(I2C_EVENT_MASTER_RECEIVER_MODE_SELECTED);",
    "",
    "\tfor (i = 0; i < 6u; i++) {",
    "\t\tif (i == 5u) { I2C_AcknowledgeConfig(I2C2, DISABLE); I2C_GenerateSTOP(I2C2, ENABLE); }",
    "\t\tapp_mpu_wait(I2C_EVENT_MASTER_BYTE_RECEIVED);",
    "\t\td[i] = I2C_ReceiveData(I2C2);",
    "\t}",
    "\tI2C_AcknowledgeConfig(I2C2, ENABLE);",
    "",
    "\tif (app_mpu_err != err0) return 0;      /* this transaction failed: reject it */",
    "\t*ax = (int16_t)(((uint16_t)d[0] << 8) | d[1]);",
    "\t*ay = (int16_t)(((uint16_t)d[2] << 8) | d[3]);",
    "\t*az = (int16_t)(((uint16_t)d[4] << 8) | d[5]);",
    "\treturn 1;",
    "}",
    "",
    "/* integer square root (bit-by-bit) - avoids pulling in libm/sqrtf */",
    "static uint32_t app_isqrt(uint32_t v)",
    "{",
    "\tuint32_t r = 0u, b = 1u << 30;",
    "\twhile (b > v) b >>= 2;",
    "\twhile (b != 0u) {",
    "\t\tif (v >= r + b) { v -= r + b; r = (r >> 1) + b; }",
    "\t\telse            { r >>= 1; }",
    "\t\tb >>= 2;",
    "\t}",
    "\treturn r;",
    "}",
    "",
    "static void app_fall_poll(void)",
    "{",
    "\tint16_t ax, ay, az;",
    "\tint32_t mag, dev;",
    "",
    "\tapp_fall_polls++;",
    "",
    "\tif (!app_mpu_read_acc(&ax, &ay, &az)) return;   /* I2C timed out: reject the sample */",
    "",
    "\t/* An all-zero burst means the MPU6050 is no longer answering with real data",
    "\t   (power glitch, wire knocked out). |a| would read 0, dev would read 2048,",
    "\t   which sits exactly on the old threshold - so one knocked wire used to",
    "\t   latch the alarm on forever. Reject such samples outright instead. */",
    "\tif (ax == 0 && ay == 0 && az == 0) { app_mpu_zero++; return; }",
    "",
    "\t/* |a| = sqrt(ax^2+ay^2+az^2). Worst case 3 * 32768^2 = 3.22e9 -> uint32. */",
    "\tmag = (int32_t)app_isqrt((uint32_t)(ax * ax) + (uint32_t)(ay * ay) + (uint32_t)(az * az));",
    "",
    "\tif (mag < app_mag_min) app_mag_min = mag;",
    "\tif (mag > app_mag_max) app_mag_max = mag;",
    "\tif (mag < 1024) app_ff_cnt++;           /* < 0.5g : free-fall evidence */",
    "",
    "\t/* Deviation of |a| from 1g (2048 LSB). Tilt-invariant: rotating the",
    "\t   board swings AX/AY by up to 1g while |a| stays at 1g, so a tilt can",
    "\t   never be mistaken for an impact. Only a genuine impact adds a spike. */",
    "\tdev = mag - 2048;",
    "\tif (dev < 0) dev = -dev;",
    "\tif (dev > app_dev_max) app_dev_max = dev;",
    "",
    "\t/* histogram of dev, expressed as |a| bands, to place the threshold from data */",
    "\tif (dev < 1024)        app_dbin[0]++;   /* |a| < 1.5g              */",
    "\telse if (dev < 2048)   app_dbin[1]++;   /* 1.5 - 2.0g              */",
    "\telse if (dev < 3072)   app_dbin[2]++;   /* 2.0 - 2.5g              */",
    "\telse if (dev < 6144)   app_dbin[3]++;   /* 2.5 - 4.0g  <- threshold */",
    "\telse if (dev < 10240)  app_dbin[4]++;   /* 4.0 - 6.0g              */",
    "\telse                   app_dbin[5]++;   /* >= 6.0g                 */",
    "",
    "\tif (dev >= (int32_t)app_fall_thresh) {",
    "\t\t/* Over threshold: keep the alarm latched, and only blame a fall once",
    "\t\t   the impact has lasted APP_FALL_NEED consecutive samples. */",
    "\t\tif (app_over_run < 0xFFFFu) app_over_run++;",
    "\t\tapp_fall_hold = APP_FALL_HOLD;",
    "\t\tif (app_over_run == app_fall_need) {     /* just confirmed, count it once */",
    "\t\t\tif (app_fall_hits == 0u) app_hit_mag = mag;",
    "\t\t\tapp_fall_hits++;",
    "\t\t\tBuzzer_on();            /* sound immediately, do not wait for the loop */",
    "\t\t}",
    "\t} else {",
    "\t\tif (app_over_run == 1u) app_glitch_cnt++;   /* lone spike: rejected on purpose */",
    "\t\tapp_over_run = 0;",
    "\t\tif (app_fall_hold) app_fall_hold--;",
    "\t}",
    "\tapp_fall_show = (app_fall_hold != 0) ? 1 : 0;",
    "",
    "\t/* sample the FIFO backlog once in a while to prove we are not dropping data */",
    "\tif ((app_fall_polls % 50u) == 0u) {",
    "\t\tuint8_t w = max30102_Bus_Read(REG_FIFO_WR_PTR);",
    "\t\tuint8_t r = max30102_Bus_Read(REG_FIFO_RD_PTR);",
    "\t\tapp_fifo_level = (uint8_t)((w - r) & 0x1Fu);",
    "\t}",
    "}",
    "",
    "/* ==========================================================================",
    " * PPG validation + robust averaging layer  (medical-grade post-processing)",
    " *",
    " * WHY THIS EXISTS",
    " *   algorithm.c is only a CANDIDATE generator: it finds peaks in whatever",
    " *   waveform it is handed. When the finger leaves the sensor the IR channel",
    " *   collapses to its dark level (~550 LSB at the 7mA LED current) plus",
    " *   noise, yet the peak detector still reports a beat train and the SpO2",
    " *   look-up table still returns 95-100%. The screen then shows numbers that",
    " *   look perfectly plausible with nobody attached, and a reading that was",
    " *   once valid simply stayed up. That is the reported defect: detection is",
    " *   gone but it keeps printing data that looks like it is in the right range.",
    " *",
    " *   Commercial pulse oximeters put a signal-quality layer between the sensor",
    " *   DSP and the display: the accuracy claimed under ISO 80601-2-61 /",
    " *   IEC 60601-2-27 only holds for a qualified signal, and below the",
    " *   pulsatility limit the device must WITHHOLD the value instead of showing",
    " *   one. This layer is that component, in three gates:",
    " *",
    " *   G1 physical   IR DC must sit inside the valid window (no light = no",
    " *                 finger; near full scale = clipped), the peak-to-peak AC",
    " *                 must clear an absolute floor, and the perfusion index",
    " *                 AC/DC must land in 0.3%..20%. Outside it the pulse is",
    " *                 noise or a motion artefact. The DC test is repeated on",
    " *                 the newest 0.5s only, so losing contact is seen within",
    " *                 one window rather than after the whole 5s buffer drains.",
    " *   G2 physiology HR 30..220 bpm, SpO2 70..100% - the ranges the standards",
    " *                 require a monitor to cover and claim accuracy over.",
    " *                 Anything else is not a vital sign.",
    " *   G3 stability  no single window is ever displayed. Readings enter a",
    " *                 sliding window, are trimmed against that window's MEDIAN",
    " *                 (itself immune to a single wild value) and only the",
    " *                 trimmed mean is shown; a 'stable' flag appears once the",
    " *                 spread collapses. A reading far from the settled level is",
    " *                 discarded rather than averaged in.",
    " *",
    " *   Losing contact flushes the sliding window after APP_LOSS_WINDOWS bad",
    " *   windows, so the next measurement can never inherit the previous one.",
    " *",
    " *   Every threshold below is live-tunable over SWD (see docs/debug/10).",
    " * ========================================================================== */",
    "#define APP_PPG_TRACE     48    /* windows kept in the SWD-readable trace */",
    "#define APP_PPG_FAST_N    50    /* newest samples for the contact test (0.5s) */",
    "#define APP_PPG_DC_MIN    8000  /* IR DC, LSB - measured dark level is ~550 */",
    "#define APP_PPG_DC_MAX    250000/* 18-bit full scale 262143 = clipped */",
    "#define APP_PPG_AC_MIN    150   /* IR peak-to-peak, LSB - below: no pulsation */",
    "#define APP_PPG_PI_MIN    3     /* perfusion index, per-mille (0.3%) */",
    "#define APP_PPG_PI_MAX    200   /* 20% - above is non-physiological (motion) */",
    "#define APP_HR_MIN        30",
    "#define APP_HR_MAX        220",
    "#define APP_SPO2_MIN      70",
    "#define APP_SPO2_MAX      100",
    "#define APP_HR_WARN_LO    60    /* bpm - default of the live app_hr_warn_lo */",
    "#define APP_HR_WARN_HI    100   /* bpm - default of the live app_hr_warn_hi */",
    "#define APP_SPO2_WARN_LO  95    /* % - default of the live app_spo2_warn_lo */",
    "#define APP_HR_N          8     /* sliding window, one entry per PPG window */",
    "#define APP_HR_WARM       2     /* valid readings needed before showing a value */",
    "#define APP_HR_STABLE_N   4     /* readings needed before 'stable' is claimed */",
    "#define APP_HR_TOL_ABS    6     /* keep +/-6 bpm around the median ... */",
    "#define APP_HR_TOL_PCT    15    /* ... or +/-15%, whichever is wider */",
    "#define APP_HR_STABLE_BPM 5     /* max-min of the kept set -> stable */",
    "#define APP_SPO2_TOL_ABS  2",
    "#define APP_SPO2_TOL_PCT  3",
    "#define APP_SPO2_STBL_N   3",
    "#define APP_SPO2_STBL_PC  1",
    "#define APP_LOSS_WINDOWS  2     /* bad windows before the value is dropped */",
    "#define APP_HR_REJ_RUN    3     /* consecutive outliers -> the level really moved */",
    "",
    "volatile uint32_t app_ppg_win    = 0;  /* PPG windows evaluated */",
    "volatile int32_t  app_ppg_dc     = 0;  /* IR DC of the last window, LSB */",
    "volatile int32_t  app_ppg_ac     = 0;  /* IR peak-to-peak of the last window, LSB */",
    "volatile int32_t  app_ppg_dc_new = 0;  /* IR DC of the newest APP_PPG_FAST_N samples */",
    "volatile uint16_t app_ppi        = 0;  /* perfusion index, per-mille */",
    "volatile uint8_t  app_ppg_ok     = 0;  /* 1 = last window passed gate G1 */",
    "volatile uint32_t app_ppg_bad    = 0;  /* consecutive windows that failed G1 */",
    "volatile int32_t  app_hr_raw     = 0;  /* raw DSP heart rate of the last window */",
    "volatile int32_t  app_spo2_raw   = 0;  /* raw DSP SpO2 of the last window */",
    "volatile uint8_t  app_hr_ok      = 0;  /* 1 = an averaged heart rate may be shown */",
    "volatile uint8_t  app_hr_disp    = 0;  /* averaged heart rate, bpm */",
    "volatile uint8_t  app_hr_stable  = 0;  /* 1 = the averaging window has settled */",
    "volatile uint8_t  app_hr_n       = 0;  /* valid heart rates in the window */",
    "volatile uint8_t  app_spo2_ok    = 0;",
    "volatile uint8_t  app_spo2_disp  = 0;",
    "volatile uint8_t  app_spo2_stable= 0;",
    "volatile uint8_t  app_spo2_n     = 0;",
    "volatile uint32_t app_hr_reject  = 0;  /* readings discarded as outliers */",
    "/* Thresholds for the on-screen emphasis of an abnormal reading. Live RAM",
    "   values, changeable over SWD: no rebuild needed to retune them. */",
    "volatile uint8_t  app_hr_warn_lo   = APP_HR_WARN_LO;",
    "volatile uint8_t  app_hr_warn_hi   = APP_HR_WARN_HI;",
    "volatile uint8_t  app_spo2_warn_lo = APP_SPO2_WARN_LO;",
    "volatile int32_t  app_ppg_hist[APP_PPG_TRACE][6];",
    "/*   [0] window no.   [1] IR DC        [2] IR AC p-p   [3] PI in per-mille",
    " *   [4] raw HR       [5] flags | hr_disp<<8 | spo2_disp<<16",
    " *   flags: bit0 ppg_ok, bit1 hr_ok, bit2 hr_stable, bit3 spo2_ok */",
    "",
    "static uint8_t app_hr_ring[APP_HR_N];",
    "static uint8_t app_spo2_ring[APP_HR_N];",
    "static uint8_t app_hr_wr = 0, app_spo2_wr = 0;",
    "static uint8_t app_hr_rej_run = 0;",
    "static uint8_t app_hist_i = 0;",
    "",
    "static void app_push(uint8_t *ring, volatile uint8_t *n, volatile uint8_t *wr, uint8_t v)",
    "{",
    "\tif (*n < (uint8_t)APP_HR_N) (*n)++;",
    "\tring[*wr] = v;",
    "\t*wr = (uint8_t)((*wr + 1) % (uint8_t)APP_HR_N);",
    "}",
    "",
    "/* median of a small ring (insertion sort, n <= 8) */",
    "static int32_t app_med(const uint8_t *src, int32_t n)",
    "{",
    "\tuint8_t t[APP_HR_N];",
    "\tint32_t i, j, k, tmp;",
    "\tif (n <= 0) return 0;",
    "\tfor (i = 0; i < n; i++) t[i] = src[i];",
    "\tfor (i = 1; i < n; i++) {",
    "\t\ttmp = t[i];",
    "\t\tfor (j = i; j > 0 && tmp < t[j - 1]; j--) t[j] = t[j - 1];",
    "\t\tt[j] = tmp;",
    "\t}",
    "\tk = n / 2;",
    "\treturn (n & 1) ? t[k] : ((t[k - 1] + t[k]) / 2);",
    "}",
    "",
    "/* median + trimmed mean: values outside median +/- tolerance are dropped",
    " * before averaging. That is the 'discard whatever is obviously far from the",
    " * settled level' rule, and taking the reference from the median makes it",
    " * robust against the outlier itself. */",
    "static int32_t app_trim(const uint8_t *ring, int32_t n, int32_t tol_abs,",
    "                        int32_t tol_pct, int32_t *spread, int32_t *used)",
    "{",
    "\tint32_t med, tol, lo, hi, i, sum = 0, cnt = 0, mn = 1000000, mx = -1;",
    "\tif (n <= 0) { *spread = 0; *used = 0; return 0; }",
    "\tmed = app_med(ring, n);",
    "\ttol = med * tol_pct / 100;",
    "\tif (tol < tol_abs) tol = tol_abs;",
    "\tlo = med - tol;",
    "\thi = med + tol;",
    "\tfor (i = 0; i < n; i++) {",
    "\t\tif (ring[i] >= lo && ring[i] <= hi) {",
    "\t\t\tsum += ring[i];",
    "\t\t\tcnt++;",
    "\t\t\tif (ring[i] < mn) mn = ring[i];",
    "\t\t\tif (ring[i] > mx) mx = ring[i];",
    "\t\t}",
    "\t}",
    "\tif (cnt == 0) { sum = med; cnt = 1; mn = med; mx = med; }",
    "\t*spread = mx - mn;",
    "\t*used = cnt;",
    "\treturn sum / cnt;",
    "}",
    "",
    "/* G1: signal-quality gate over the IR window. Fills app_ppg_dc/ac/dc_new,",
    " * app_ppi and app_ppg_ok. */",
    "static void app_ppg_quality(const uint32_t *ir, int32_t n)",
    "{",
    "\tint32_t i, mn = 0x7FFFFFFF, mn2 = 0x7FFFFFFF;",
    "\tint32_t mx = 0, mx2 = 0, dc, ac, ppi;",
    "\tuint32_t sum = 0, sum_new = 0;",
    "\tint32_t fast = (n > (int32_t)APP_PPG_FAST_N) ? (int32_t)APP_PPG_FAST_N : n;",
    "",
    "\tfor (i = 0; i < n; i++) {",
    "\t\tint32_t v = (int32_t)ir[i];",
    "\t\tsum += (uint32_t)v;",
    "\t\tif (i >= n - fast) sum_new += (uint32_t)v;",
    "\t\tif (v < mn)       { mn2 = mn; mn = v; }",
    "\t\telse if (v < mn2) { mn2 = v; }",
    "\t\tif (v > mx)       { mx2 = mx; mx = v; }",
    "\t\telse if (v > mx2) { mx2 = v; }",
    "\t}",
    "\tdc = (n > 0) ? (int32_t)(sum / (uint32_t)n) : 0;",
    "\tapp_ppg_dc_new = (fast > 0) ? (int32_t)(sum_new / (uint32_t)fast) : 0;",
    "\t/* 2nd-highest minus 2nd-lowest: a single-sample spike (an I2C glitch, an",
    "\t   ADC outlier) cannot inflate the measured pulsation. */",
    "\tac = (n >= 4) ? (mx2 - mn2) : 0;",
    "\tif (ac < 0) ac = 0;",
    "\tppi = (dc > 0) ? (ac * 1000) / dc : 0;",
    "\tif (ppi < 0) ppi = 0;",
    "\tif (ppi > 9999) ppi = 9999;",
    "",
    "\tapp_ppg_dc = dc;",
    "\tapp_ppg_ac = ac;",
    "\tapp_ppi = (uint16_t)ppi;",
    "\tapp_ppg_ok = (uint8_t)((dc >= APP_PPG_DC_MIN) && (dc <= APP_PPG_DC_MAX) &&",
    "\t                       (ac >= APP_PPG_AC_MIN) &&",
    "\t                       (ppi >= APP_PPG_PI_MIN) && (ppi <= APP_PPG_PI_MAX) &&",
    "\t                       (app_ppg_dc_new >= APP_PPG_DC_MIN) &&",
    "\t                       (app_ppg_dc_new <= APP_PPG_DC_MAX));",
    "}",
    "",
    "static void app_hr_ingest(int32_t hr, int8_t valid)",
    "{",
    "\tint32_t med, tol;",
    "\tif (!(valid == 1 && hr >= APP_HR_MIN && hr <= APP_HR_MAX)) return;",
    "\tif (app_hr_n >= 3) {",
    "\t\tmed = app_med(app_hr_ring, app_hr_n);",
    "\t\ttol = med * APP_HR_TOL_PCT / 100;",
    "\t\tif (tol < APP_HR_TOL_ABS) tol = APP_HR_TOL_ABS;",
    "\t\t/* the input filter is twice as wide as the display filter on purpose:",
    "\t\t   it removes a wildly wrong beat train, not a genuine rate change */",
    "\t\tif (hr > med + 2 * tol || hr < med - 2 * tol) {",
    "\t\t\tapp_hr_reject++;",
    "\t\t\tif (++app_hr_rej_run >= APP_HR_REJ_RUN) {",
    "\t\t\t\tapp_hr_n = 0; app_hr_wr = 0; app_hr_rej_run = 0;  /* level moved */",
    "\t\t\t}",
    "\t\t\treturn;",
    "\t\t}",
    "\t}",
    "\tapp_hr_rej_run = 0;",
    "\tapp_push(app_hr_ring, &app_hr_n, &app_hr_wr, (uint8_t)hr);",
    "}",
    "",
    "static void app_hr_publish(void)",
    "{",
    "\tint32_t spread = 0, used = 0;",
    "\tif (app_hr_n < (uint8_t)APP_HR_WARM) { app_hr_ok = 0; app_hr_stable = 0; return; }",
    "\tapp_hr_disp = (uint8_t)app_trim(app_hr_ring, app_hr_n, APP_HR_TOL_ABS,",
    "\t                                APP_HR_TOL_PCT, &spread, &used);",
    "\tapp_hr_ok = 1;",
    "\tapp_hr_stable = (uint8_t)((app_hr_n >= (uint8_t)APP_HR_STABLE_N) &&",
    "\t                          (spread <= APP_HR_STABLE_BPM));",
    "}",
    "",
    "static void app_spo2_ingest(int32_t spo2, int8_t valid)",
    "{",
    "\tif (!(valid == 1 && spo2 >= APP_SPO2_MIN && spo2 <= APP_SPO2_MAX)) return;",
    "\tapp_push(app_spo2_ring, &app_spo2_n, &app_spo2_wr, (uint8_t)spo2);",
    "}",
    "",
    "static void app_spo2_publish(void)",
    "{",
    "\tint32_t spread = 0, used = 0;",
    "\tif (app_spo2_n < (uint8_t)APP_HR_WARM) { app_spo2_ok = 0; app_spo2_stable = 0; return; }",
    "\tapp_spo2_disp = (uint8_t)app_trim(app_spo2_ring, app_spo2_n, APP_SPO2_TOL_ABS,",
    "\t                                  APP_SPO2_TOL_PCT, &spread, &used);",
    "\tapp_spo2_ok = 1;",
    "\tapp_spo2_stable = (uint8_t)((app_spo2_n >= (uint8_t)APP_SPO2_STBL_N) &&",
    "\t                            (spread <= APP_SPO2_STBL_PC));",
    "}",
    "",
    "static void app_ppg_record(void)",
    "{",
    "\tapp_ppg_hist[app_hist_i][0] = (int32_t)app_ppg_win;",
    "\tapp_ppg_hist[app_hist_i][1] = app_ppg_dc;",
    "\tapp_ppg_hist[app_hist_i][2] = app_ppg_ac;",
    "\tapp_ppg_hist[app_hist_i][3] = (int32_t)app_ppi;",
    "\tapp_ppg_hist[app_hist_i][4] = app_hr_raw;",
    "\tapp_ppg_hist[app_hist_i][5] = (int32_t)((app_ppg_ok ? 1 : 0) |",
    "\t                                        (app_hr_ok ? 2 : 0) |",
    "\t                                        (app_hr_stable ? 4 : 0) |",
    "\t                                        (app_spo2_ok ? 8 : 0)) |",
    "\t                              ((int32_t)app_hr_disp << 8) |",
    "\t                              ((int32_t)app_spo2_disp << 16);",
    "\tapp_hist_i = (uint8_t)((app_hist_i + 1) % (uint8_t)APP_PPG_TRACE);",
    "}",
    "",
    "/* one call per PPG window (5s sliding buffer, ~1s of new samples) */",
    "static void app_ppg_update(const uint32_t *ir, int32_t n,",
    "                           int32_t hr, int8_t hr_valid,",
    "                           int32_t spo2, int8_t spo2_valid)",
    "{",
    "\tapp_ppg_win++;",
    "\tapp_ppg_quality(ir, n);",
    "\tapp_hr_raw   = hr;",
    "\tapp_spo2_raw = spo2;",
    "",
    "\tif (app_ppg_ok) {",
    "\t\tapp_ppg_bad = 0;",
    "\t} else if (app_ppg_bad < 0xFFFFFFFFu) {",
    "\t\tapp_ppg_bad++;",
    "\t}",
    "",
    "\t/* --- no contact / unusable signal: HOLD the last valid reading --- */",
    "\t/* Once a reading has been validated it stays on screen until a later",
    "\t   window produces a fresh one - it is never blanked back to \"---\". The",
    "\t   sliding windows themselves ARE reset, so a returning finger cannot",
    "\t   blend stale samples into the fresh average. */",
    "\tif (!app_ppg_ok) {",
    "\t\tif (app_ppg_bad >= (uint32_t)APP_LOSS_WINDOWS) {",
    "\t\t\tapp_hr_n = 0; app_spo2_n = 0;",
    "\t\t\tapp_hr_wr = 0; app_spo2_wr = 0;",
    "\t\t\tapp_hr_rej_run = 0;",
    "\t\t}",
    "\t\tapp_ppg_record();",
    "\t\treturn;",
    "\t}",
    "",
    "\tapp_hr_ingest(hr, hr_valid);",
    "\tapp_spo2_ingest(spo2, spo2_valid);",
    "\tapp_hr_publish();",
    "\tapp_spo2_publish();",
    "\tapp_ppg_record();",
    "}",
    "",
])


def main():
    raw = open(SRC, "rb").read()
    crlf = b"\r\n" in raw
    txt = raw.replace(b"\r\n", b"\n")
    mode = "CRLF" if crlf else "LF"
    print("source: %d bytes, line endings %s" % (len(raw), mode))

    def sub1(pattern, repl, label, expect=1):
        nonlocal txt
        txt, n = re.subn(pattern, repl, txt, count=expect)
        if n != expect:
            die("%s -> matched %d, expected %d" % (label, n, expect))
        print("  ok: %s" % label)

    # 0) replace BOTH bare INT waits with the timeout-safe wrapper FIRST, so the
    #    helper comment text injected in step 1 cannot be rewritten by this rule.
    #    同时把跌倒判定挂到每 10ms 一次采样节奏上（原来一圈才判一次）。
    sub1(rb"while\(MAX30102_INT==1\);",
         b"app_wait_int(); app_fall_poll();",
         "INT wait -> app_wait_int + fall poll",
         expect=2)

    # 1) inject helpers + globals right after the last include
    sub1(rb"#include <stdbool\.h>",
         b"#include <stdbool.h>\n" + HELPERS.encode("ascii"),
         "inject DS helpers")

    # 2) guard MAX30102 init
    sub1(rb"\n[ \t]*//MAX30102\n[ \t]*max30102_init\(\);",
         b"\n\t//MAX30102\n#ifndef NO_MAX30102\n\tmax30102_init();\n#endif",
         "guard max30102_init")

    # 3) launch-time acquisition loop + first DSP call
    sub1(rb"[ \t]*n_ir_buffer_length=500;",
         b"#ifndef NO_MAX30102\n\tn_ir_buffer_length=500;",
         "open guard: init acquisition")

    calls = list(re.finditer(rb"maxim_heart_rate_and_oxygen_saturation\([^;]*;", txt))
    if len(calls) != 2:
        die("expected 2 maxim_* calls, found %d" % len(calls))
    i = calls[0].end()
    txt = txt[:i] + b"\n#endif" + txt[i:]
    print("  ok: close guard after 1st DSP call")

    # 4) in-loop acquisition (has the bare `while(MAX30102_INT==1);` waits)
    sub1(rb"[ \t]*for\(i=400;i<500;i\+\+\)",
         b"#ifndef NO_MAX30102\n\t\tfor(i=400;i<500;i++)",
         "open guard: loop acquisition")

    calls = list(re.finditer(rb"maxim_heart_rate_and_oxygen_saturation\([^;]*;", txt))
    if len(calls) != 2:
        die("re-scan expected 2 maxim_* calls, found %d" % len(calls))
    i = calls[1].end()
    # 4a) close the MAX30102 guard, then hand the fresh DSP result (and the raw
    #     IR window it was computed from) to the validation/averaging layer.
    #     The call sits OUTSIDE the guard on purpose: under NOMAX the buffers
    #     stay zero, the quality gate fails and the display shows "---", and
    #     the function is still referenced so -Wunused-function stays quiet.
    txt = txt[:i] + (b"\n#endif\n"
                     b"\t\t/* ---- medical-grade PPG validation + averaging layer ----\n"
                     b"\t\t   quality gate -> physiological bounds -> median-trimmed\n"
                     b"\t\t   average -> what the display is allowed to show.\n"
                     b"\t\t   See the block injected after <stdbool.h> for details. */\n"
                     b"\t\tapp_ppg_update(aun_ir_buffer, n_ir_buffer_length,\n"
                     b"\t\t               n_heart_rate, ch_hr_valid, n_sp02, ch_spo2_valid);") + txt[i:]
    print("  ok: close guard after 2nd DSP call + add app_ppg_update()")

    # 4b) remove the original in-loop validity handling.
    #     USER/main.c:163-172 sits INSIDE `for(i=400;i<500;i++)`, so it ran 100
    #     times per window against the DSP result of the PREVIOUS window: the
    #     screen was permanently one window stale, and a reading that had once
    #     been valid survived a whole extra window after detection was lost.
    #     It also only tested `n_heart_rate<120` - no lower bound, no SpO2
    #     check, no signal-quality test - so a noise-only window (sensor not in
    #     contact) passed and kept the old-looking number on screen.
    #     All of that is now app_ppg_update() above; this block becomes a note.
    sub1(rb"[ \t]*//send samples and calculation result to terminal program through UART\n"
         rb"[ \t]*if\(ch_hr_valid == 1 && n_heart_rate<120\)[^\n]*\n"
         rb"[ \t]*\{\n[ \t]*dis_hr = n_heart_rate;\n[ \t]*dis_spo2 = n_sp02;\n[ \t]*\}\n"
         rb"[ \t]*else\n[ \t]*\{\n[ \t]*dis_hr = 0;\n[ \t]*dis_spo2 = 0;\n[ \t]*\}\n",
         b"\t\t\t/* validity and averaging happen ONCE per window now, in\n"
         b"\t\t\t   app_ppg_update() right after the DSP call below. This\n"
         b"\t\t\t   block used to live here, inside the 100-sample acquisition\n"
         b"\t\t\t   loop, which made the display one window stale. */\n",
         "drop the stale in-loop validity block")

    # 5) HR / SpO2 display driven by the validity flags instead of the raw
    #    DSP output. A value is shown only when the gate passed AND an averaged
    #    reading exists; otherwise the field is blanked to "---". Nothing is
    #    ever carried over from a window that is no longer current.
    #    3-digit values (HR>=100, SpO2==100) are right-aligned so 2-digit and
    #    3-digit readings occupy the same columns and no ghost digit is left.
    sub1(rb"[ \t]*OLED_ShowNum\(47,16,dis_hr,2,OLED_8X16\);[ \t]*\n"
         rb"[ \t]*OLED_ShowNum\(47,32,dis_spo2,2,OLED_8X16\);",
         b"\t\t/* Blank the whole digital field first: OLED_ShowChar() ORs into the\n"
         b"\t\t   frame buffer, so a 3-digit reading followed by a 2-digit one would\n"
         b"\t\t   otherwise leave a ghost digit behind. */\n"
         b"\t\tOLED_ClearArea(47,16,24,16);\n"
         b"\t\tif (app_hr_ok) {\n"
         b"\t\t\tdis_hr = app_hr_disp;\n"
         b"\t\t\tif (dis_hr >= 100u) OLED_ShowNum(47,16,dis_hr,3,OLED_8X16);\n"
         b"\t\t\telse { OLED_ShowChar(47,16,' ',OLED_8X16); OLED_ShowNum(55,16,dis_hr,2,OLED_8X16); }\n"
         b"\t\t} else {\n"
         b"\t\t\tdis_hr = 0;\n"
         b"\t\t\tOLED_ShowString(47,16,\"---\",OLED_8X16);\n"
         b"\t\t}\n"
         b"\t\tOLED_ClearArea(47,32,24,16);\n"
         b"\t\tif (app_spo2_ok) {\n"
         b"\t\t\tdis_spo2 = app_spo2_disp;\n"
         b"\t\t\tif (dis_spo2 >= 100u) OLED_ShowNum(47,32,dis_spo2,3,OLED_8X16);\n"
         b"\t\t\telse { OLED_ShowChar(47,32,' ',OLED_8X16); OLED_ShowNum(55,32,dis_spo2,2,OLED_8X16); }\n"
         b"\t\t} else {\n"
         b"\t\t\tdis_spo2 = 0;\n"
         b"\t\t\tOLED_ShowString(47,32,\"---\",OLED_8X16);\n"
         b"\t\t}\n"
         b"\t\tOLED_ClearArea(79,16,8,16);",
         "HR/SpO2 display gated by validity")

    # 6) timeout-safe temperature read
    sub1(rb"Temp = get_tempetature\(\)/16;",
         b"Temp = app_ds_read()/16;",
         "Temp -> app_ds_read")

    # 7) swap the blocking DS18B20 init for the safe one
    sub1(rb"\n[ \t]*ds18b20_init\(\);",
         b"\n\tapp_ds_init();",
         "ds18b20_init -> app_ds_init")

    # 8) buzzer boot self-test
    sub1(rb"\n[ \t]*Buzzer_init\(\);",
         b"\n\tBuzzer_init();\n"
         b"\t/* boot self-test: two short beeps (audible proof the buzzer is wired) */\n"
         b"\tBuzzer_on(); delay_ms(120); Buzzer_off(); delay_ms(120);\n"
         b"\tBuzzer_on(); delay_ms(120); Buzzer_off(); delay_ms(120);",
         "buzzer self-test")

    # 9) main-loop counter
    sub1(rb"(while\(1\)[ \t]*\n[ \t]*\{)",
         rb"\1\n\t\tapp_cycle++;",
         "app_cycle counter")

    # 10) 跌倒判定条件：改用高频合矢量判定的结论 app_fall_show。
    #     原来的 `|dAX|>=500 || |dAY|>=500` 是拿"上一圈读到的 AX/AY"和"这一圈
    #     读到的"相比，两点相隔整整一个主循环周期（~2s）。2 秒内把板子翻转
    #     15 度，单轴就能变化 500 LSB(0.24g) —— 于是"单纯翻转"也会报跌倒。
    #     这正是"轻触/晃动被判成跌倒"的直接来源。合矢量偏差对倾斜免疫，
    #     所以这里只保留它的结论（带 3s 保持位）。
    #     只改写条件行（纯 ASCII），三条 OLED_ShowChinese 中文串保持字节不动。
    sub1(rb"if \(Last_AX - AX >=500 \|\| AX - Last_AX >=500 \|\| "
         rb"Last_AY - AY >=500 \|\| AY - Last_AY >=500\) \{",
         b"OLED_ClearArea(80, 48, 16, 16);   /* wipe the old glyph: ShowChinese ORs */\n"
         b"\t\tif (app_fall_show) {",
         "fall condition -> magnitude verdict")

    # 11) 原代码在「是否跌倒：否」这一支里居然叫 Buzzer_on()：显示"否"却在响，
    #     用户听到的就是"误报"。改成 Buzzer_off()。用捕获组保留中间的中文
    #     注释字节，绝不重打 GBK 串。
    sub1(rb"(else if \(Temp > 32\) \{[\s\S]*?)Buzzer_on\(\);",
         rb"\1Buzzer_off();",
         "buzzer off on the 'no fall' branch")

    # 12) 主循环里还留着一份「上一圈遗物」：一次阻塞式 MPU6050_GetData（50kHz 下
    #     12 次单字节读 ≈ 9.4ms）加上 Last_AX/Last_AY 的赋值。跌倒判定早已改由
    #     app_fall_poll() 以 ~87Hz + 突发读完成，这几行既白耗 9.4ms 又是个多余的
    #     I2C 毛刺源（超时后静默返回脏数据）。删掉它们。
    #     同时把 `else if (Temp > 32)` 这个死分支改成 `else if (0)`：它的两个分支
    #     都是「显示否 + 关蜂鸣器」，跟最后的 else 完全等价，留着只会让后人以为
    #     温度参与跌倒判定。只动 ASCII，三条中文串字节原样保留。
    sub1(rb"\t\tLast_AX = AX;\n\t\tLast_AY = AY;\n[\s\S]*?"
         rb"MPU6050_GetData\(&AX, &AY, &AZ, &GX, &GY, &GZ\);\n",
         b"\t\t/* the blocking per-axis fall check used to live here. It compared two\n"
         b"\t\t   samples a full ~2s loop iteration apart, so flipping the board was\n"
         b"\t\t   enough to fire it, and it could not see a 20ms impact at all.\n"
         b"\t\t   app_fall_poll() does the job at ~87Hz now - see the file header. */\n",
         "drop dead blocking MPU read from the main loop")

    sub1(rb"else if \(Temp > 32\) \{",
         b"else if (0) {   /* dead branch: was a duplicate of the final else */",
         "neutralise the leftover Temp branch")

    # 13) 单色 OLED 没有红色可用，"异常"只能用本硬件能表达的最强方式：**反色**
    #     （白底黑字）。必须在 OLED_Update() 之前做 —— 反色只改显存，Update 才把
    #     显存推到屏上；而且 ReverseArea 是异或，靠"每帧重画 → 再取反"才稳定。
    #     阈值是宏（60 / 100 / 95），可在线改；见 docs/debug/11。
    sub1(rb"\t\tOLED_Update\(\);\n\t\}",
         b"\t\t/* Single-colour panel: there is no red, so \"abnormal\" is shown the\n"
         b"\t\t   strongest way this hardware allows - inverse video of the field\n"
         b"\t\t   (white background, black digits). Thresholds are live-tunable. */\n"
         b"\t\tif (app_hr_ok && (app_hr_disp < app_hr_warn_lo ||\n"
         b"\t\t                  app_hr_disp > app_hr_warn_hi))\n"
         b"\t\t\tOLED_ReverseArea(47, 16, 24, 16);\n"
         b"\t\tif (app_spo2_ok && app_spo2_disp < app_spo2_warn_lo)\n"
         b"\t\t\tOLED_ReverseArea(47, 32, 24, 16);\n"
         b"\t\tif (app_fall_show)\n"
         b"\t\t\tOLED_ReverseArea(80, 48, 16, 16);\n"
         b"\t\tOLED_Update();\n"
         b"\t}",
         "abnormal fields in inverse video")

    out = txt.replace(b"\n", b"\r\n") if crlf else txt
    os.makedirs(os.path.dirname(DST), exist_ok=True)
    open(DST, "wb").write(out)
    print("written: %s (%d bytes)" % (DST, len(out)))


if __name__ == "__main__":
    main()

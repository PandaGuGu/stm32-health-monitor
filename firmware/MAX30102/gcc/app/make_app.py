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
    "\tdelay_ms(750);",
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
    txt = txt[:i] + b"\n#endif" + txt[i:]
    print("  ok: close guard after 2nd DSP call")

    # 5) HR / SpO2 placeholders when MAX30102 is off
    sub1(rb"[ \t]*OLED_ShowNum\(47,16,dis_hr,2,OLED_8X16\);[ \t]*\n"
         rb"[ \t]*OLED_ShowNum\(47,32,dis_spo2,2,OLED_8X16\);",
         b"#ifndef NO_MAX30102\n"
         b"\t\tOLED_ShowNum(47,16,dis_hr,2,OLED_8X16);\n"
         b"\t\tOLED_ShowNum(47,32,dis_spo2,2,OLED_8X16);\n"
         b"#else\n"
         b'\t\tOLED_ShowString(47,16,"--",OLED_8X16);\n'
         b'\t\tOLED_ShowString(47,32,"--",OLED_8X16);\n'
         b"#endif",
         "HR/SpO2 placeholder")

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

    out = txt.replace(b"\n", b"\r\n") if crlf else txt
    os.makedirs(os.path.dirname(DST), exist_ok=True)
    open(DST, "wb").write(out)
    print("written: %s (%d bytes)" % (DST, len(out)))


if __name__ == "__main__":
    main()

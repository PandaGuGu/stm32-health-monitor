#!/usr/bin/env python3
"""
Read back the STM32 health monitor's PPG validation layer over SWD and print it
in human form.  Works on the live target: it halts, reads, then resumes.

  python _ppg_read.py                 # read + decode
  python _ppg_read.py --keep-halted   # leave the core halted afterwards

What it decodes
  * the OLED framebuffer (OLED_DisplayBuf) as actual characters, so the report
    can quote what the screen showed at that instant
  * app_ppg_*  : signal-quality metrics of the last PPG window
  * app_hr_* / app_spo2_* : what the layer decided may be displayed
  * app_ppg_hist : the per-window trace (48 windows, oldest -> newest)

Symbol addresses are taken from build/APP.elf, so a rebuild needs no edit here.
"""

import os
import re
import subprocess
import sys

HOME = os.path.expanduser("~")
GCC = os.path.join(HOME, r"tools\xpack-arm-none-eabi-gcc-14.2.1-1.1\bin")
OCD = os.path.join(HOME, r"tools\xpack-openocd-0.12.0-7\bin")
# this script lives in firmware/MAX30102/gcc/tools/
GCCDIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OLED_DATA = os.path.join(GCCDIR, "..", "HARDWARE", "OLED", "OELD_Data.c")

WANT = [
    "app_cycle", "app_ppg_win", "app_ppg_dc", "app_ppg_dc_new", "app_ppg_ac",
    "app_ppi", "app_ppg_ok", "app_ppg_bad", "app_hr_raw", "app_spo2_raw",
    "app_hr_ok", "app_hr_disp", "app_hr_stable", "app_hr_n", "app_hr_reject",
    "app_spo2_ok", "app_spo2_disp", "app_spo2_stable", "app_spo2_n",
    "app_ppg_hist", "OLED_DisplayBuf",
    "ch_hr_valid", "n_heart_rate", "ch_spo2_valid", "n_sp02",
    "aun_ir_buffer", "app_fall_hits", "app_fall_polls",
]


def symbols():
    nm = os.path.join(GCC, "arm-none-eabi-nm.exe")
    out = subprocess.run([nm, "-S", os.path.join(GCCDIR, "build", "APP.elf")],
                         capture_output=True, text=True).stdout
    sym = {}
    for line in out.splitlines():
        p = line.split()
        if len(p) == 4:
            sym[p[3]] = (int(p[0], 16), int(p[1], 16))
    return {k: sym[k] for k in WANT if k in sym}


def build_oem(fonts):
    """openocd -c argument list: halt, read everything we need.
    mdw must start on a 4-byte boundary, so round the start down and pad the
    length - an unaligned mdw aborts the rest of the script."""
    cmds = ["-c", "adapter speed 20", "-c", "init", "-c", "halt"]
    for name, (addr, size) in fonts.items():
        base = addr & ~3
        words = (addr - base + size + 3) // 4
        cmds += ["-c", "mdw 0x%08x %d" % (base, words)]
    return cmds


def run_openocd(zaps, keep_halted):
    exe = os.path.join(OCD, "openocd.exe")
    args = [exe, "-f", "interface/stlink.cfg", "-f", "target/stm32f1x.cfg"] + zaps
    if not keep_halted:
        args += ["-c", "resume"]
    args += ["-c", "exit"]
    r = subprocess.run(args, capture_output=True, text=True, cwd=GCCDIR)
    return r.stdout + r.stderr


def parse(text):
    mem = {}
    for line in text.splitlines():
        m = re.match(r"^0x([0-9a-f]{8}):((?:\s+[0-9a-f]{8})+)\s*$", line.strip())
        if not m:
            continue
        base = int(m.group(1), 16)
        for i, w in enumerate(m.group(2).split()):
            mem[base + 4 * i] = int(w, 16)
    return mem


def rd8(mem, a):
    return (mem.get(a & ~3, 0) >> (8 * (a & 3))) & 0xFF


def rd16(mem, a):
    return rd8(mem, a) | (rd8(mem, a + 1) << 8)


def rds32(mem, a):
    v = rd32(mem, a)
    return v - (1 << 32) if v & 0x80000000 else v


def rd32(mem, a):
    return (rd8(mem, a) | (rd8(mem, a + 1) << 8) |
            (rd8(mem, a + 2) << 16) | (rd8(mem, a + 3) << 24))


# ---------------------------------------------------------------- OLED decode
def load_fonts():
    raw = open(OLED_DATA, "rb").read()

    def table(start, n, per):
        i = raw.find(start)
        j = raw.find(b"};", i)
        vals = [int(x, 16) for x in re.findall(rb"0x([0-9A-Fa-f]{2})", raw[i:j])]
        return [bytes(vals[k * per:(k + 1) * per]) for k in range(min(n, len(vals) // per))]

    ascii_glyphs = table(b"OLED_F8x16", 95, 16)          # index = ord(ch)-0x20

    # OLED_CF16x16: ChineseCell_t { char Index[4]; uint8_t Data[32]; } written as
    #   "字",
    #   0x00, ... x32
    # (aggregate initialisation, so there is no inner brace pair to match)
    i = raw.find(b"OLED_CF16x16")
    j = raw.find(b"\n};", i)
    body = raw[i:j]
    cn = []
    for m in re.finditer(rb'"([^"]*)"\s*,\s*((?:0x[0-9A-Fa-f]{2}\s*,\s*){32})', body):
        idx = m.group(1)
        vals = [int(x, 16) for x in re.findall(rb"0x([0-9A-Fa-f]{2})", m.group(2))]
        if len(vals) == 32 and idx:
            cn.append((idx, bytes(vals)))
    return ascii_glyphs, cn


def glyph_at(mem, base, page, x, w):
    top = bytes(rd8(mem, base + page * 128 + x + i) for i in range(w))
    bot = bytes(rd8(mem, base + (page + 1) * 128 + x + i) for i in range(w))
    return top + bot


def inv(b):
    """Bit-invert a glyph: an emphasised field is drawn in inverse video."""
    return bytes((~x) & 0xFF for x in b)


def decode_row(mem, base, page, ascii_glyphs, cn):
    """Return the row as a 128-column laid-out string.

    Text is not guaranteed to sit on an 8-pixel grid (the original code draws
    the digits at x=47, one column into the Chinese label), so ASCII glyphs are
    searched at every column and then placed at the column they were found at.

    A field may be drawn in inverse video (white background, black glyph) to
    mark an abnormal reading, so every glyph is matched twice - as-is, then
    bit-inverted. An inverted match is reported as an x-range at the end of
    the row.
    """
    amap = {g: chr(0x20 + k) for k, g in enumerate(ascii_glyphs)}
    cmap = {}
    for idx, g in cn:
        cmap.setdefault(g, idx)
    canvas = [" "] * 128
    hl = []

    def look(g, table):
        if g in table:
            return table[g], False
        gi = inv(g)
        if gi in table:
            return table[gi], True
        return None, False

    # Chinese: normally a run of 16x16 cells from x=0
    ch_span = []
    x = 0
    while x + 16 <= 128:
        g = glyph_at(mem, base, page, x, 16)
        v, invd = look(g, cmap)
        if v is None:
            break
        if not (invd and g == bytes(32)):
            canvas[x] = v.decode("utf-8", "replace")
        if invd:
            hl.append((x, 16))
        ch_span.append((x, x + 16))
        x += 16

    # ASCII glyphs anywhere in the row (text is not always on an 8px grid)
    x = 0
    while x + 8 <= 128:
        inside = any(s <= x and x + 8 <= e for s, e in ch_span)
        g = glyph_at(mem, base, page, x, 8)
        v, invd = look(g, amap)
        if v is not None and not inside:
            if not (invd and g == bytes(16)):
                canvas[x] = v
            if invd:
                hl.append((x, 8))
            x += 8
        else:
            x += 1
    text = "".join(canvas).rstrip()
    if hl:
        text += "    <INVERSE x=%d..%d>" % (min(a for a, _ in hl),
                                           max(a + w for a, w in hl))
    return text


def show(mem, sym, ascii_glyphs, cn):
    print("=" * 78)
    print("STM32 health monitor - PPG validation layer, read back over SWD")
    print("=" * 78)

    cyc = rd32(mem, sym["app_cycle"][0])
    win = rd32(mem, sym["app_ppg_win"][0])
    print("app_cycle (main-loop rounds) = %d     app_ppg_win (5s PPG windows) = %d"
          % (cyc, win))
    print()

    b = sym["OLED_DisplayBuf"][0]
    print("OLED framebuffer, decoded:")
    for y in (0, 16, 32, 48):
        txt = decode_row(mem, b, y // 8, ascii_glyphs, cn)
        if y == 16:
            print("  y=16  %s      <- 心率 digits at x=47..70, S/~ flag at x=79" % txt)
        elif y == 32:
            print("  y=32  %s      <- 血氧 digits at x=47..70" % txt)
        else:
            print("  y=%d  %s" % (y, txt))
    print()

    print("signal quality of the last window (gate G1):")
    print("  IR DC          = %6d LSB      (valid window %d .. %d)"
          % (rds32(mem, sym["app_ppg_dc"][0]) , 8000, 250000))
    print("  IR DC newest   = %6d LSB      (newest 50 samples = 0.5s)"
          % rds32(mem, sym["app_ppg_dc_new"][0]))
    print("  IR AC p-p      = %6d LSB      (floor 150)" % rds32(mem, sym["app_ppg_ac"][0]))
    print("  perfusion idx  = %6d per-mille(valid 3 .. 200)"
          % rd16(mem, sym["app_ppi"][0]))
    print("  gate verdict   = %s   consecutive failures = %d"
          % ("PASS" if rd8(mem, sym["app_ppg_ok"][0]) else "FAIL",
             rd32(mem, sym["app_ppg_bad"][0])))
    print()

    print("what the display layer decided:")
    ok = rd8(mem, sym["app_hr_ok"][0])
    print("  HR     ok=%d disp=%-3s stable=%d n=%d outliers_rejected=%d"
          % (ok, rd8(mem, sym["app_hr_disp"][0]), rd8(mem, sym["app_hr_stable"][0]),
             rd8(mem, sym["app_hr_n"][0]), rd32(mem, sym["app_hr_reject"][0])))
    print("  SpO2   ok=%d disp=%-3s stable=%d n=%d"
          % (rd8(mem, sym["app_spo2_ok"][0]), rd8(mem, sym["app_spo2_disp"][0]),
             rd8(mem, sym["app_spo2_stable"][0]), rd8(mem, sym["app_spo2_n"][0])))
    print("  raw DSP output : ch_hr_valid=%d n_heart_rate=%d | ch_spo2_valid=%d n_sp02=%d"
          % (rd8(mem, sym["ch_hr_valid"][0]), rds32(mem, sym["n_heart_rate"][0]),
             rd8(mem, sym["ch_spo2_valid"][0]), rds32(mem, sym["n_sp02"][0])))
    print()

    h = sym["app_ppg_hist"][0]
    rows = []
    for k in range(48):
        a = h + k * 24
        rows.append([rds32(mem, a + 4 * i) for i in range(6)])
    rows = [r for r in rows if r[0] != 0]
    rows.sort(key=lambda r: r[0])
    print("per-window trace (oldest first):")
    print("  win |  IR DC | AC p-p |  PI | raw HR | flags(pok/hok/stable/sok) | shown")
    for r in rows:
        fl = r[5] & 0xFF
        hd = (r[5] >> 8) & 0xFF
        sd = (r[5] >> 16) & 0xFF
        shown = ("HR=%d SpO2=%d" % (hd, sd)) if (fl & 2) else "--- (withheld)"
        print("  %3d | %6d | %6d | %4d | %6d | %d%d%d%d                     | %s"
              % (r[0], r[1], r[2], r[3], r[4],
                 (fl >> 0) & 1, (fl >> 1) & 1, (fl >> 2) & 1, (fl >> 3) & 1, shown))
    print()
    print("  app_fall_polls=%d  app_fall_hits=%d"
          % (rd32(mem, sym["app_fall_polls"][0]), rd32(mem, sym["app_fall_hits"][0])))


def main():
    sym = symbols()
    ascii_glyphs, cn = load_fonts()
    zaps = build_oem(sym)
    text = run_openocd(zaps, "--keep-halted" in sys.argv)
    mem = parse(text)
    if not mem:
        print(text[-3000:])
        sys.exit(1)
    show(mem, sym, ascii_glyphs, cn)


if __name__ == "__main__":
    main()

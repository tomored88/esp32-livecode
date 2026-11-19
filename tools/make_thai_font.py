"""Turn a Thai TrueType font into a small bitmap font for the OLED (src/font_thai.h).

    python tools/make_thai_font.py                         # writes src/font_thai.h
    python tools/make_thai_font.py --preview out.png       # also renders sample text (6x zoom)
    python tools/make_thai_font.py --font tools/fonts/K2D-Bold.ttf --size 15

Every glyph sits in a 16-pixel-tall line cell. A glyph stores its bitmap, its offset from
the pen position (x may be negative: Thai marks hang back over the previous letter) and
its advance (0 for vowels/tone marks above or below a consonant). The renderer in
src/i2c_dev.cpp draws glyph after glyph exactly like that, so marks land on their base.

Needs Pillow (pip install pillow).
"""
import argparse
import os
from PIL import Image, ImageDraw, ImageFont

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_FONT = os.path.join(ROOT, "tools", "fonts", "K2D-Bold.ttf")      # K2D Bold, SIL Open Font License 1.1
CELL = int(os.environ.get("THAI_CELL", "20"))                                     # line height in pixels (text size 1)
CHARS = [chr(c) for c in range(0x20, 0x7F)] + ["°"] + \
        [chr(c) for c in range(0x0E01, 0x0E3B)] + [chr(c) for c in range(0x0E3F, 0x0E5C)]
ABOVE = set("\u0E31\u0E34\u0E35\u0E36\u0E37\u0E47\u0E48\u0E49\u0E4A\u0E4B\u0E4C\u0E4D\u0E4E")
BELOW = set("\u0E38\u0E39\u0E3A")
TONES = set("\u0E48\u0E49\u0E4A\u0E4B\u0E4C")
UPPER_VOWELS = set("\u0E31\u0E34\u0E35\u0E36\u0E37\u0E47\u0E4D")
TALL = set("\u0E1B\u0E1D\u0E1F\u0E2C")          # \u0E1B \u0E1D \u0E1F \u0E2C: marks above shift left
SARA_AM = "\u0E33"


def layout_params(glyphs):
    """tone drop: move a tone mark down so it sits just above a consonant when there is
    no upper vowel; tall shift: how far marks above move left on \u0E1B \u0E1D \u0E1F \u0E2C"""
    tone = glyphs["\u0E48"]
    base_top = glyphs["\u0E01"][1]                   # top row of \u0E01
    drop = max(0, base_top - 1 - (tone[1] + tone[3]))
    tall = glyphs["\u0E1B"]
    shift = max(1, round(tall[2] * 0.3))
    return drop, shift


def place(glyphs, text):
    """yield (glyph, pen_x, dx, dy) using the same rules as the firmware renderer"""
    drop, shift = layout_params(glyphs)
    pen, prev_upper, base_tall = 0, False, False
    for i, c in enumerate(text):
        g = glyphs.get(c, glyphs["?"])
        dx = dy = 0
        if c in TONES and not prev_upper and text[i + 1:i + 2] != SARA_AM:
            dy = drop
        if c in ABOVE and base_tall:
            dx = -shift
        yield g, pen, dx, dy
        pen += g[4]
        if c in UPPER_VOWELS:
            prev_upper = True
        elif g[4] > 0:
            prev_upper, base_tall = False, c in TALL


# glyph extents that must fit in the cell: tallest stack above, deepest part below
FIT_TEXT = "ปั้ ปี่ ฟื้ ญู ฎุ ฤ ฦ ำ้ Ag|"


SS = 4          # supersampling: render 4x larger with anti-aliasing, shrink, then threshold (1 = FreeType hinted 1-bit)
THRESH = 160    # 0-255; lower = bolder


class Scaled:
    """FreeType font rendered at SS x size; glyphs come back at 1x as clean 1-bit masks."""
    def __init__(self, path, size):
        self.f = ImageFont.truetype(path, size * SS, layout_engine=ImageFont.Layout.BASIC)

    def getlength(self, ch):
        return self.f.getlength(ch) / SS

    def getmask2(self, ch, mode="1", anchor="ls"):
        m, (ox, oy) = self.f.getmask2(ch, mode="L", anchor=anchor)
        w, h = m.size
        if not w or not h:
            return Image.new("1", (0, 0)), (round(ox / SS), round(oy / SS))
        # place on a grid aligned to the 1x pixel grid, then box-filter down
        x0, y0 = ox // SS * SS, oy // SS * SS
        W = -(-(ox - x0 + w) // SS) * SS
        H = -(-(oy - y0 + h) // SS) * SS
        big = Image.new("L", (W, H), 0)
        big.paste(Image.frombytes("L", (w, h), bytes(m)), (ox - x0, oy - y0))
        small = big.reduce(SS).point(lambda v: 255 if v >= THRESH else 0).convert("1")
        return small, (x0 // SS, y0 // SS)


def load(path, size):
    if SS == 1:     # FreeType's own hinted 1-bit rendering
        return ImageFont.truetype(path, size, layout_engine=ImageFont.Layout.BASIC)
    return Scaled(path, size)


def glyph(font, ch, baseline):
    """-> (xoff, yoff, w, h, advance, rows[list of int bit rows, MSB = leftmost])"""
    mask, (ox, oy) = font.getmask2(ch, mode="1", anchor="ls")
    w, h = mask.size
    adv = 0 if ch in ABOVE or ch in BELOW else int(round(font.getlength(ch)))
    rows = []
    for y in range(h):
        bits = 0
        for x in range(w):
            if mask.getpixel((x, y)):
                bits |= 1 << (w - 1 - x)
        rows.append(bits)
    # trim empty rows/columns so the table stays small
    while rows and rows[0] == 0:
        rows.pop(0); oy += 1; h -= 1
    while rows and rows[-1] == 0:
        rows.pop(); h -= 1
    if not rows:
        return (0, 0, 0, 0, adv, [])
    left = max(r.bit_length() for r in rows)          # leftmost used column from the right
    shift = min((r & -r).bit_length() - 1 for r in rows if r)
    rows = [r >> shift for r in rows]
    nw = left - shift
    ox += w - left
    return (ox, baseline + oy, nw, h, adv, rows)


def extents(font):
    """highest ink (most negative) and lowest ink relative to the baseline"""
    top, bot = 0, 0
    for ch in FIT_TEXT + "".join(CHARS):
        if ch == " ":
            continue
        _, (ox, oy) = font.getmask2(ch, mode="1", anchor="ls")
        m = font.getmask2(ch, mode="1", anchor="ls")[0]
        if m.size[1]:
            top, bot = min(top, oy), max(bot, oy + m.size[1])
    return top, bot


def pick(path, size):
    if size:
        font = load(path, size)
        top, bot = extents(font)
        return font, size, -top
    for size in range(24, 8, -1):
        font = load(path, size)
        top, bot = extents(font)
        if bot - top <= CELL:
            return font, size, -top
    raise SystemExit("font does not fit in %d px" % CELL)


def build(path, size):
    font, size, baseline = pick(path, size)
    glyphs = {ch: glyph(font, ch, baseline) for ch in CHARS}
    return glyphs, size, baseline


def render(glyphs, text, scale=1):
    """same algorithm as the firmware: draw at pen + offset, then advance"""
    w = sum(glyphs.get(c, glyphs["?"])[4] for c in text) + 8
    img = Image.new("1", (max(w, 1) * scale, CELL * scale), 0)
    px = img.load()
    for (xo, yo, gw, gh, adv, rows), pen, dx, dy in place(glyphs, text):
        pen += 2
        for y, bits in enumerate(rows):
            for x in range(gw):
                if bits >> (gw - 1 - x) & 1:
                    X, Y = pen + xo + dx + x, yo + dy + y
                    for sy in range(scale):
                        for sx in range(scale):
                            if 0 <= X * scale + sx < img.size[0] and 0 <= Y * scale + sy < img.size[1]:
                                px[X * scale + sx, Y * scale + sy] = 1
    return img


def write_header(glyphs, path, size, baseline, out):
    data, index = [], []
    for ch in CHARS:
        xo, yo, w, h, adv, rows = glyphs[ch]
        bpr = (w + 7) // 8
        off = len(data)
        for r in rows:
            r <<= bpr * 8 - w                 # left-align in whole bytes
            data += [(r >> (8 * (bpr - 1 - i))) & 0xFF for i in range(bpr)]
        index.append((ord(ch), off, xo, yo, w, h, adv))
    lines = ["// Generated by tools/make_thai_font.py from %s (%d px) -- do not edit." % (os.path.basename(path), size),
             "#pragma once", "#include <stdint.h>", "",
             "#define THAI_FONT_CELL %d" % CELL, "#define THAI_FONT_BASELINE %d" % baseline,
             "#define THAI_TONE_DROP %d" % layout_params(glyphs)[0], "#define THAI_TALL_SHIFT %d" % layout_params(glyphs)[1], "",
             "struct ThaiGlyph { uint16_t cp; uint16_t off; int8_t xo, yo; uint8_t w, h, adv; };", "",
             "static const uint8_t THAI_FONT_BITS[] PROGMEM = {"]
    for i in range(0, len(data), 24):
        lines.append("  " + ",".join(str(b) for b in data[i:i + 24]) + ",")
    lines += ["};", "", "static const ThaiGlyph THAI_FONT[] PROGMEM = {"]
    for cp, off, xo, yo, w, h, adv in index:
        lines.append("  {0x%04X, %d, %d, %d, %d, %d, %d}," % (cp, off, xo, yo, w, h, adv))
    lines += ["};", "static const uint16_t THAI_FONT_COUNT = %d;" % len(index), ""]
    with open(out, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(lines))
    print("%s: %d glyphs, %d bytes of bitmaps, %d px font, baseline %d" % (out, len(index), len(data), size, baseline))


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--font", default=DEFAULT_FONT)
    ap.add_argument("--size", type=int, default=15, help="pixel size (default 15 for K2D Bold; 0 = largest that fits)")
    ap.add_argument("--preview", help="write a zoomed PNG of sample lines instead of only the header")
    ap.add_argument("--out", default=os.path.join(ROOT, "src", "font_thai.h"))
    ap.add_argument("--no-header", action="store_true")
    ap.add_argument("--ss", type=int, default=SS, help="supersampling factor (1 = FreeType hinted 1-bit)")
    ap.add_argument("--thresh", type=int, default=THRESH)
    a = ap.parse_args()
    SS, THRESH = a.ss, a.thresh
    glyphs, size, baseline = build(a.font, a.size)
    if a.preview:
        lines = ["สวัสดีครับ ปี่ น้ำ ฟ้า", "อุณหภูมิ 25.3°C", "ความชื้น 61% ญู ฎุ", "ก่ ก้ ปั้ ฝึ่ ที่ เมื่อ ฬ้", "Hello ESP32 abc"]
        imgs = [render(glyphs, l, 6) for l in lines]
        W = max(i.size[0] for i in imgs)
        sheet = Image.new("1", (W, sum(i.size[1] for i in imgs) + 6 * len(imgs)), 0)
        y = 0
        for i in imgs:
            sheet.paste(i, (0, y)); y += i.size[1] + 6
        sheet.convert("L").save(a.preview)
        print("preview:", a.preview, "size", size, "baseline", baseline)
    if not a.no_header:
        write_header(glyphs, a.font, size, baseline, a.out)

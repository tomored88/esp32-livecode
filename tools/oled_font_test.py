"""Check the board's Thai text renderer against tools/make_thai_font.py, pixel for pixel.

    python -X utf8 tools/oled_font_test.py COM3

Uses the firmware's serial command "--oledtest <text>", which draws into the OLED frame
buffer (no display needed) and prints it. Also saves a zoomed PNG of what the board drew.
"""
import os
import sys
import time

import serial
from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import make_thai_font as m  # noqa: E402

TEXTS = ["สวัสดีครับ", "อุณหภูมิ 25.3°C", "ที่ ปั้ ก่ น้ำ", "เมื่อ ฝึ่ ญู ฎุ", "Hello ESP32"]


def board_render(s, text):
    s.reset_input_buffer()
    s.write(("--oledtest " + text + "\n").encode())
    end, buf = time.time() + 4, b""
    while time.time() < end and b"oledtest end" not in buf:
        buf += s.read(4096)
    lines = buf.decode("utf-8", "replace").splitlines()
    i = next(k for k, l in enumerate(lines) if l.startswith("oledtest width="))
    width = int(lines[i].split("=")[1])
    return width, lines[i + 1:i + 65]


def py_render(glyphs, text):
    img = Image.new("1", (128, 64), 0)
    px = img.load()
    width = 0
    for (xo, yo, gw, gh, adv, rows), pen, dx, dy in m.place(glyphs, text):
        for y, bits in enumerate(rows):
            for x in range(gw):
                X, Y = pen + xo + dx + x, yo + dy + y
                if bits >> (gw - 1 - x) & 1 and 0 <= X < 128 and 0 <= Y < 64:
                    px[X, Y] = 1
        width = pen + adv
    return width, ["".join("#" if px[x, y] else "." for x in range(128)) for y in range(64)]


def main():
    port = sys.argv[1] if len(sys.argv) > 1 else "COM3"
    glyphs, _, _ = m.build(m.DEFAULT_FONT, 15)
    s = serial.Serial()
    s.port, s.baudrate, s.timeout = port, 115200, 0.1
    s.dtr = s.rts = False
    s.open()
    time.sleep(0.5)
    ok_all = True
    sheet = Image.new("L", (128 * 4, 22 * 4 * len(TEXTS)), 0)
    for k, t in enumerate(TEXTS):
        bw, brows = board_render(s, t)
        pw, prows = py_render(glyphs, t)
        diff = sum(a != b for r1, r2 in zip(brows, prows) for a, b in zip(r1, r2))
        ok = diff == 0 and bw == pw
        ok_all &= ok
        print(("PASS " if ok else "FAIL ") + f"{t!r}: width board={bw} python={pw}, differing pixels={diff}")
        for y in range(20):
            for x in range(128):
                if brows[y][x] == "#":
                    for sy in range(4):
                        for sx in range(4):
                            sheet.putpixel((x * 4 + sx, (k * 22 + y) * 4 + sy), 255)
    # same renderer on the 128x32 display's buffer: rows 0-31 must match, nothing beyond
    t = "สวัสดี 0.91 นิ้ว"
    s.reset_input_buffer()
    s.write(("--oledtest32 " + t + "\n").encode())
    end, buf = time.time() + 4, b""
    while time.time() < end and b"oledtest end" not in buf:
        buf += s.read(4096)
    lines = buf.decode("utf-8", "replace").splitlines()
    i = next(k for k, l in enumerate(lines) if l.startswith("oledtest width="))
    rows32 = lines[i + 1:lines.index("oledtest end")]
    _, prows = py_render(glyphs, t)
    diff = sum(a != b for r1, r2 in zip(rows32, prows[:32]) for a, b in zip(r1, r2))
    ok = len(rows32) == 32 and diff == 0
    ok_all &= ok
    print(("PASS " if ok else "FAIL ") + f"128x32 {t!r}: rows={len(rows32)}, differing pixels={diff}")

    out = os.path.join(os.environ.get("TEMP", "."), "oled_board_render.png")
    sheet.save(out)
    print("board render saved:", out)
    print("ALL PASS" if ok_all else "SOME FAILED")


if __name__ == "__main__":
    main()

"""Autorun test over USB serial:  python -X utf8 tools/autorun_test.py COM3

Saves an autorun program through the serial console, resets the board through the
EN pin (same as unplugging power), and checks the program starts by itself.
"""
import serial
import sys
import time

s = serial.Serial()
s.port, s.baudrate, s.timeout = (sys.argv[1] if len(sys.argv) > 1 else "COM3"), 115200, 0.1
s.dtr = s.rts = False
s.open()


def read(sec):
    end, buf = time.time() + sec, b""
    while time.time() < end:
        buf += s.read(1024)
    return buf.decode("utf-8", "replace")


def hard_reset():
    s.rts = True          # pulls EN low
    time.sleep(0.2)
    s.rts = False


def check(name, ok, out):
    print(("PASS " if ok else "FAIL ") + name)
    if not ok:
        print("   output:", out.replace("\n", " | ")[-700:])
    return ok


results = []
read(1.0)
s.write(b'print("autorun alive", 1 + 1)\nfor i = 1, 3 do print("tick", i) delay(100) end\n--autorun\n')
out = read(1.5)
results.append(check("autorun saved", "autorun saved" in out, out))

hard_reset()
out = read(6.0)
results.append(check("after reset: autorun starts by itself",
                     "POWERON" in out and "autorun" in out and "autorun alive\t2" in out and "tick\t3" in out, out))

s.write(b"--reboot\n")
out = read(6.0)
results.append(check("after software reboot: autorun runs again", "SW_CPU_RESET" in out and "autorun alive\t2" in out, out))

s.write(b"--noautorun\n")
out = read(1.0)
results.append(check("autorun removed", "autorun removed" in out, out))
hard_reset()
out = read(5.0)
results.append(check("after reset: nothing runs", "POWERON" in out and "autorun alive" not in out, out))

print("ALL PASS" if all(results) else "SOME FAILED")

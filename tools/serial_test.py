"""Phase 1 runner tests over USB serial: python -X utf8 tools/serial_test.py COM3"""
import serial, time, sys

s = serial.Serial()
s.port = sys.argv[1] if len(sys.argv) > 1 else "COM3"
s.baudrate = 115200
s.timeout = 0.1
s.dtr = False
s.rts = False
s.open()
log = []


def read(sec):
    end = time.time() + sec
    buf = b""
    while time.time() < end:
        buf += s.read(1024)
    t = buf.decode("utf-8", "replace")
    log.append(t)
    return t


def run(code, wait):
    s.write((code.strip() + "\n--run\n").encode())
    return read(wait)


def stop(wait=1.0):
    s.write(b"--stop\n")
    return read(wait)


def check(name, ok, out):
    print(("PASS " if ok else "FAIL ") + name)
    if not ok:
        print("   output:", out.replace("\n", " | ")[:600])


read(1.5)

out = run("local x = 1\nprint(x)\nprint(nil + 1)", 1.5)
check("error shows line number (โปรแกรม:3:)", "โปรแกรม:3:" in out, out)

out = run("""
pinMode(LED_BUILTIN, OUTPUT)
for i = 1, 4 do
  digitalWrite(LED_BUILTIN, i % 2)
  print("led", digitalRead(LED_BUILTIN))
  delay(250)
end""", 2.0)
check("GPIO2 toggles (read back 1,0,1,0)", out.count("led\t1") == 2 and out.count("led\t0") == 2 and "จบการทำงาน" in out, out)

out = run("while true do end", 6.0)
alive = "ets Jun" not in out and "Guru" not in out and "wdt" not in out.lower()
out2 = stop(1.5)
check("tight loop runs 6s with no reset/WDT", alive, out)
check("Stop ends tight loop", "หยุดแล้ว" in out2, out2)

out = run("while true do pcall(function() while true do end end) end", 2.0)
out2 = stop(1.5)
check("Stop escapes pcall-wrapped loop", "หยุดแล้ว" in out2, out + out2)

out = run('print(pcall(function() error("boom") end)); print(xpcall(function() return 7 end, print))', 1.0)
check("pcall/xpcall still work normally", "false" in out and "boom" in out and "true\t7" in out, out)

out = run("while true do delay(1000) end", 1.0)
t0 = time.time(); out2 = stop(0.5)
check("Stop interrupts delay(1000) quickly", "หยุดแล้ว" in out2, out2)

run("while true do delay(10) end", 0.8)
out = run('print("second program")', 1.5)
check("Run while running replaces old program", "หยุดแล้ว" in out and "second program" in out, out)

out = run("digitalWrite(35, 1)", 1.0)
check("input-only pin gives Thai error", "อินพุตอย่างเดียว" in out, out)
out = run("pinMode(7, OUTPUT)", 1.0)
check("flash pin 6-11 rejected", "ใช้ไม่ได้" in out, out)

out = run("local t = {} for i = 1, 10000000 do t[i] = i * 1.5 end print('done', #t)", 12.0)
check("out-of-memory -> error, no reset", "หน่วยความจำไม่พอ" in out and "ets Jun" not in out, out)

out = run('print(map(512, 0, 1023, 0, 100), millis() > 0, HIGH, LOW, LED_BUILTIN); print(io, os, dofile, loadfile)', 1.0)
check("map/consts and sandbox (io/os/dofile nil)", "50.0" in out and "nil\tnil\tnil\tnil" in out, out)

out = run('print(analogRead(34), touchRead(4)); analogWrite(2, 128); delay(200); analogWrite(2, 0); print("ok-io")', 1.5)
check("analogRead/touchRead/analogWrite run", "ok-io" in out, out)

out = run("analogWrite(2, 128) delay(100)", 0.8)
out = run("analogWrite(2, 50) for i = 1, 4 do digitalWrite(2, i % 2) print(\"led\", digitalRead(2)) end", 1.0)
check("digitalWrite works after PWM on same pin", out.count("led	1") == 2 and out.count("led	0") == 2, out)
out = run("for i = 1, 4 do digitalWrite(2, i % 2) print(\"led\", digitalRead(2)) end", 1.0)
check("digitalWrite works in next run after PWM", out.count("led	1") == 2 and out.count("led	0") == 2, out)

print("\n==== raw serial ====")
print("".join(log))

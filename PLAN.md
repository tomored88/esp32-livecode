# ESP32 Live Code — แผนงาน

เฟิร์มแวร์ ESP32 ที่แฟลชครั้งเดียว จากนั้นผู้ใช้เปิดเว็บที่บอร์ดเสิร์ฟเอง ลากบล็อก (หรือพิมพ์โค้ด) แล้วกด **Run** โปรแกรมทำงานบนบอร์ดทันที ไม่ต้องคอมไพล์หรือแฟลชใหม่

## 1. เป้าหมายและขอบเขต

| หัวข้อ | การตัดสินใจ |
|---|---|
| บอร์ด (เฟสแรก) | **ESP32 Dev Module** (ESP32-WROOM-32, flash 4MB) — รุ่นอื่นทำทีหลัง |
| ผู้ใช้หลัก | ช่าง, ลูกค้า, ผู้เรียนรู้ → UI ภาษาไทย ใช้ง่าย บล็อกเป็นหลัก |
| วิธีเขียนโปรแกรม | **บล็อกลาก (Blockly)** เป็นหลัก + แท็บดู/แก้โค้ด Lua สำหรับคนที่เก่งขึ้น |
| ภาษาที่รันบนบอร์ด | **Lua 5.4** (MIT) ฝังในเฟิร์มแวร์ Arduino — Blockly มีตัวแปลงบล็อก→Lua ในตัว |
| แสดงผล | บนหน้าเว็บ: console + การ์ดค่า live (มีกราฟเส้นเล็ก ๆ อัตโนมัติถ้าเป็นตัวเลข) |
| ใช้งานออฟไลน์ | ต้องได้ — ไฟล์เว็บทั้งหมด (รวม Blockly) ฝังในแฟลชของบอร์ด ไม่โหลดจาก CDN |

นอกขอบเขตเฟสแรก: จอบนบอร์ด, เลือกรุ่นบอร์ด, Modbus/I2C/UART, OTA, ระบบ login

## 2. สถาปัตยกรรม

```
Browser ──HTTP──► WebServer (loop, core1)
  │                 ├─ GET  /, /blockly.js, /media/*   (gzip จาก PROGMEM)
  │                 ├─ POST /api/run  body=โค้ด Lua ──► pendingCode ──► luaTask
  │                 ├─ POST /api/stop ──► stopReq=true
  │                 └─ GET  /api/log?since=N&vs=M  (poll ทุก ~250ms)
  │
  └─ Blockly → luaGenerator → โค้ด Lua
                                     luaTask (FreeRTOS, core1, prio1, stack 16KB)
                                       ├─ lua_newstate ใหม่ทุกครั้งที่รัน (สะอาด)
                                       ├─ count hook ทุก 1000 คำสั่ง: เช็ค stopReq + vTaskDelay(1) ทุก 20ms
                                       └─ print/show → log buffer (mutex) → /api/log
```

### ประเด็นสำคัญที่ต้องทำให้ถูก
- **Stop ได้ทุกกรณี** แม้ `while true do end` ไม่มี delay → ใช้ `lua_sethook(LUA_MASKCOUNT)` แล้ว `luaL_error(L,"__STOP__")`
- **Web ต้องไม่ค้าง/WDT ไม่ดีด** ตอนสคริปต์วนแน่น → hook ทำ `vTaskDelay(1)` ทุก ~20ms
- `delay(ms)` ใน Lua ต้องแบ่งเป็นช่วง ≤20ms และเช็ค stop ทุกช่วง
- **กันหน่วยความจำหมด**: custom allocator คืน NULL ถ้า free heap < ~40KB → Lua ขึ้น error "หน่วยความจำไม่พอ" แทนบอร์ดรีเซ็ต
- กด Run ขณะกำลังรัน → หยุดตัวเก่าแล้วรันตัวใหม่อัตโนมัติ
- ปิด lib ที่เข้าถึงไฟล์/ระบบ: ไม่เปิด `io`, `os`, `package`; ลบ `dofile`, `loadfile`
- ตรวจขา GPIO: ห้าม 6–11 (flash), 34–39 เป็นอินพุตอย่างเดียว → error ภาษาไทยที่เข้าใจง่าย

## 3. โครงสร้างไฟล์

```
esp32-livecode/
├─ platformio.ini            env:esp32dev, partitions=min_spiffs.csv, filesystem=littlefs
├─ src/
│  ├─ config.h               WiFi default, ชื่อ AP, mDNS, ไฟล์ autorun
│  ├─ main.cpp               WiFi, web routes, Lua runner, bindings, ไฟล์
│  └─ web_assets.h           (สร้างอัตโนมัติ) gzip byte arrays
├─ lib/lua/                  Lua 5.4.6 core (ไม่รวม lua.c, luac.c, linit.c, liolib.c, loslib.c, loadlib.c, ldblib.c, ltests.c)
│  └─ library.json
├─ web/
│  ├─ index.html             หน้า IDE (HTML+CSS+JS ไฟล์เดียว)
│  └─ vendor/blockly/        จาก npm `blockly` (v13): blockly_compressed.js, blocks_compressed.js, lua_compressed.js, msg/th.js, media/*
└─ tools/
   └─ embed_web.py           รวม+gzip ไฟล์เว็บ → src/web_assets.h (ผูกเป็น extra_script pre-build)
```

`embed_web.py`:
- ต่อไฟล์ blockly_compressed + blocks_compressed + lua_compressed + msg/th.js เป็น `/blockly.js` ไฟล์เดียว แล้ว gzip
- gzip `index.html`
- gzip ไฟล์ใน `media/` ยกเว้น .mp3 (ปิดเสียงด้วย `sounds:false`)
- เขียน `INDEX_HTML_GZ`, `BLOCKLY_JS_GZ`, ตาราง `MEDIA[] {name,type,data,len}`
- ขนาดคาดการณ์ ~300–400KB หลัง gzip (พาร์ติชัน app ของ min_spiffs = 1.9MB พอ)

## 4. Web API

| Method | Path | รายละเอียด |
|---|---|---|
| GET | `/` | หน้า IDE (gzip, no-cache) |
| GET | `/blockly.js`, `/media/*` | ไฟล์ Blockly (gzip, cache 1 วัน) |
| POST | `/api/run` | body = โค้ด Lua (text/plain) |
| POST | `/api/stop` | หยุดโปรแกรม |
| GET | `/api/log?since=N&vs=M` | `{boot, seq, clear, running, lines:[{s,t,m}], vs, vals?:{k:v}}` — `boot` เปลี่ยน = บอร์ดรีสตาร์ต — t: 0=print 1=error 2=system; ส่ง `vals` เฉพาะเมื่อ vs เปลี่ยน; `clear`>since = ล้าง console ฝั่งเว็บ |
| GET | `/api/info` | `{ver, ip, mode(sta/ap), ssid, saved, heap, autorun, fs}` — `saved` = WiFi ที่ตั้งไว้ |
| GET | `/api/files` | รายชื่อโปรแกรมที่บันทึก |
| GET/POST/DELETE | `/api/file?name=x&kind=lua\|blk` | อ่าน/บันทึก/ลบ (`name=__autorun` = ไฟล์ autorun) ชื่อใช้ได้ `a-z0-9_-` ≤24 ตัว · POST ต้องเป็น multipart/form-data (เขียนลงแฟลชทีละส่วน) ≤64KB; 413 = ใหญ่เกิน, 507 = พื้นที่เต็ม |
| POST | `/api/wifi` | form `ssid`,`pass` → เก็บใน NVS (Preferences) แล้วรีบูต; `ssid` ว่าง = ลืม WiFi กลับเป็นโหมด AP |
| GET | `/api/scan` | `[{ssid, rssi, lock}]` WiFi รอบตัว (~4 วิ) |
| POST | `/api/reboot` | รีบูต |

ที่เก็บไฟล์ (LittleFS): `/p/<name>.lua`, `/p/<name>.blk` (JSON ของ workspace), `/autorun.lua`, `/autorun.blk`

## 5. ฟังก์ชันใน Lua (bindings)

| ฟังก์ชัน | หมายเหตุ |
|---|---|
| `print(...)` | ไป console เว็บ + Serial |
| `clear()` | ล้าง console และค่า live |
| `show(name, value)` | การ์ดค่า live (สูงสุด 24 ชื่อ) |
| `delay(ms)`, `millis()`, `micros()` | delay หยุดได้ |
| `pinMode(pin, mode)` | `OUTPUT, INPUT, INPUT_PULLUP, INPUT_PULLDOWN` |
| `digitalWrite(pin, v)` | v รับ 0/1/HIGH/LOW/true/false |
| `digitalRead(pin)`, `analogRead(pin)` (0–4095), `touchRead(pin)` | |
| `analogWrite(pin, 0–255)` | PWM |
| `map(x, inLo, inHi, outLo, outHi)` | |
| ค่าคงที่ | `HIGH, LOW, LED_BUILTIN(=2)` |

## 6. บล็อก (Toolbox ภาษาไทย)

หมวดของเรา (สีแยกชัด):
- **ขา I/O**: ตั้งขา [pin▾] เป็น [ส่งออก/รับเข้า/รับเข้า+pullup] · เขียนขา [pin▾] = [เปิด/ปิด] · อ่านขาดิจิทัล [pin▾] · อ่านอนาล็อก [pin▾] · PWM ขา [pin▾] ค่า (0–255) · อ่านค่าสัมผัส [touch pin▾]
- **บนบอร์ด**: LED บนบอร์ด [เปิด/ปิด] (GPIO2) · ปุ่ม BOOT ถูกกด? (GPIO0, active low)
- **เวลา**: รอ (ms) · รอ (วินาที) · เวลาตั้งแต่เริ่ม (ms)
- **แสดงผล**: พิมพ์ [ข้อความ] · แสดงค่า [ชื่อ] = [ค่า] · ล้างหน้าจอ
- **ควบคุม**: ทำซ้ำตลอดไป (`while true do … end`) + บล็อกมาตรฐาน controls_*
- **ตัวเลข**: math_* มาตรฐาน + แปลงช่วงค่า (map)
- ข้อความ, ตรรกะ, ตัวแปร, ฟังก์ชัน: บล็อกมาตรฐาน (ตัด `text_prompt*` ออก เพราะใช้ io.read)

dropdown ขา: ขาออกได้ `2,4,5,12–19,21–23,25–27,32,33` · ขาอนาล็อก/อินพุตเพิ่ม `34–39` · touch `T0–T9` แมปเป็น GPIO

## 7. หน้าเว็บ IDE

- แถบบน: ชื่อ/IP, สถานะ (กำลังทำงาน/ว่าง/ขาดการเชื่อมต่อ), ปุ่ม **▶ รัน** (Ctrl+Enter), **■ หยุด**
- ซ้าย: แท็บ **บล็อก** | **โค้ด Lua**
  - แท็บโค้ด: สร้างจากบล็อกทุกครั้งที่เปิด; ถ้าผู้ใช้แก้เอง → โหมด "โค้ดเขียนเอง" (กลับไปบล็อกต้องยืนยันว่าจะทิ้งการแก้)
  - ปุ่ม Run รันตามแท็บที่เปิดอยู่
- ขวา: การ์ดค่า live (ตัวเลข = มีกราฟเส้นเล็ก ~60 จุดล่าสุด) + console (สี: ปกติ/แดง error/เทา system, ปุ่มล้าง)
- เมนู: ตัวอย่างโปรแกรม · เปิด/บันทึก/ลบ บนบอร์ด · ตั้งเป็น autorun / ยกเลิก autorun · ตั้งค่า WiFi · ส่งออก/นำเข้าไฟล์ .json ลงเครื่อง
- autosave workspace ใน localStorage (ห่อ try/catch)
- ตัวอย่างในตัว: ไฟกะพริบ · กดปุ่ม BOOT แล้วไฟติด · อ่านอนาล็อกแสดงกราฟ · หรี่ไฟด้วย PWM · นับเวลา/จับเวลา · สัมผัสแล้วไฟติด
- Blockly options: `media:'/media/'`, `sounds:false`, `renderer:'zelos'` (บล็อกใหญ่ กดง่ายบนแท็บเล็ต), `zoom` เปิด, `trashcan:true`
- รองรับมือถือ/แท็บเล็ต: layout ซ้อนแนวตั้งเมื่อจอแคบ

## 8. WiFi

1. ใช้ SSID/รหัสจาก NVS (ตั้งผ่านเว็บ) ถ้าไม่มีใช้จาก `config.h`
2. ต่อไม่ได้ใน 15 วิ → เปิด AP `ESP32-LiveCode` / `12345678` (IP 192.168.4.1)
3. mDNS `http://esp32.local`
4. พิมพ์ IP ออก Serial และใส่ใน log เว็บ

## 9. ลำดับงาน (ทำทีละเฟส ทดสอบก่อนไปต่อ)

**เฟส 1 — Runner + API (ยังไม่มี Blockly)**
- [x] platformio + lib Lua คอมไพล์ผ่าน (`pio run -e esp32dev`)
- [x] main.cpp: WiFi/AP, /api/run, /api/stop, /api/log, bindings ทั้งหมด
- [x] index.html ชั่วคราว: textarea + Run/Stop + console (ฝังผ่าน tools/embed_web.py แล้ว)
- [ ] ทดสอบบนบอร์ดจริงตามเกณฑ์ด้านล่าง
- ✅ ผ่านเมื่อ: `while true do end` กด Stop ได้, เว็บไม่ค้าง, ไม่มี WDT reset; ไฟ GPIO2 กะพริบได้; error แสดงเลขบรรทัด

**เฟส 2 — Blockly**
- [x] tools/embed_web.py + extra_script, ฝัง blockly (v13.4.0, ~227KB gzip)
- [x] custom blocks + Lua generator ภาษาไทย, toolbox
- [x] แท็บโค้ด, ค่า live + กราฟ, ตัวอย่าง
- [x] ตัวอย่างทั้ง 6 รันบนบอร์ดจริงผ่าน `tools/dev_server.py --port COM3` (สะพาน Serial)
- [ ] ทดสอบผ่าน WiFi จริง: โหลดในโหมด AP ไม่มีเน็ต, เวลาโหลดหน้าแรก, การ์ดค่า live จากบอร์ด
- ✅ ผ่านเมื่อ: ตัวอย่างทุกตัวรันได้บนบอร์ดจริง, หน้าเว็บโหลดได้ในโหมด AP ไม่มีเน็ต, โหลดหน้าแรก < 3 วิ

**เฟส 3 — ไฟล์ + autorun + ตั้งค่า**
- [x] บันทึก/เปิด/ลบ, autorun, ตั้งค่า WiFi (+ค้นหา WiFi), export/import
- [x] autorun หลังรีเซ็ตด้วยขา EN (เหมือนถอดไฟ) ทำงานเอง — `tools/autorun_test.py`
- [x] API ไฟล์/WiFi บนบอร์ดจริงผ่าน WiFi — `tools/http_test.py` (หน้าแรกโหลด ~0.8-1 วิ, หน้าเว็บจริง ~2.1 วิ)
- [ ] ตั้ง WiFi บ้าน/ออฟฟิศผ่านเว็บแล้วบอร์ดต่อได้ (ต้องใช้รหัส WiFi จริง — ผู้ใช้ทดสอบเอง)
- ✅ ผ่านเมื่อ: ถอดไฟแล้วเสียบใหม่ โปรแกรม autorun ทำงานเอง; ตั้ง WiFi ผ่านเว็บแล้วต่อได้

**เฟส 4 — ขัดเกลา**
- [x] ข้อความ error ภาษาไทยเข้าใจง่าย (แปลในหน้าเว็บ ~35 แบบ + แสดงข้อความ Lua เดิมตัวเล็ก, คลิก error → ไปบรรทัดในแท็บโค้ด)
- [x] README วิธีแฟลช + `tools/make_release.py` → `docs/` (.bin รวมเขียนที่ 0x0, manifest.json, index.html ของ ESP Web Tools 10.4.0)
- [x] ทดสอบ .bin รวม: ลบแฟลชทั้งหมด → เขียน .bin ไฟล์เดียว → บูตได้, LittleFS format เอง, serial/autorun test ผ่าน
- [x] จอมือถือ (จำลอง 375px): เมนู/ไดอะล็อก/คอนโซลไม่ล้นจอ
- [ ] ติดตั้งผ่านปุ่มใน flash.html จริง (ต้องให้คนเลือกพอร์ต) และทดสอบบนมือถือจริง (สัมผัส/ลากบล็อก)

## เพิ่มหลังเฟส 4: เซนเซอร์ I2C + จอ OLED
- [x] `src/i2c_dev.cpp`: i2c (scan/begin/read/write/readReg/writeReg), aht20, sht31, bme280 (+bmp280), bh1750, mpu6050, oled (SSD1306 128x64 ผ่าน Adafruit SSD1306 2.5.17)
- [x] บล็อก 2 หมวด (I2C / เซนเซอร์, จอ OLED) + `round(x, digits)` + ตัวอย่าง 6 ตัว
- [x] ทดสอบบนบอร์ดแบบไม่ต่ออุปกรณ์: error ภาษาไทยพร้อมเลขบรรทัด, scan 15 ms, กดหยุดระหว่างอ่านเซนเซอร์ได้, สูตร BME280 ตรงกับตัวอย่างใน datasheet
- [x] จอ SH1106 1.3" ผ่าน `oled.begin('sh1106')`: ใช้ frame buffer เดิม + init/ส่งภาพแบบ SH1106 (ค่า init เดียวกับ Adafruit SH110X, เลื่อนคอลัมน์ 2)
- [x] จอ 0.91" 128x32 ผ่าน `oled.begin('ssd1306_32')` + `oled.height()` (Adafruit SSD1306 ตัวที่สองขนาด 128x32)
- [x] จอ LCD 16x2/20x4 + PCF8574: `lcd.begin/text/clear/backlight` (ไดรเวอร์เขียนเอง ลำดับคำสั่งเดียวกับ LiquidCrystal_I2C, ใช้ 100 kHz เฉพาะตอนคุยกับจอ)
- [x] HC-SR04: `ultrasonic.distance(trig, echo [, °C])` + บล็อก + ตัวอย่าง 2 ตัว (เว้น 60 ms ระหว่างวัด, -1 = เกินระยะ, error เมื่อไม่มี echo)
- [x] DHT22/DHT11: `dht.temperature/humidity(pin [, type])` อ่านช่วงบิตแบบปิด interrupt (~5 ms), cache 2 วินาที, ค่าปัด 0.1
- [x] DS18B20 (1-Wire เขียนเอง): หลายตัวต่อขา, ค้นหา ROM (AN187), CRC — ตรวจกับ test vector ของ Maxim และบัสจำลอง 1–8 ตัวบน PC
- [x] PIR: `pir.motion(pin)` (pull-down กันสายหลุด) + ตัวอย่างนับครั้ง/ไฟเปิดอัตโนมัติ — ทดสอบบนบอร์ดโดยจำลองสัญญาณด้วย pull-up/pull-down
- [x] เซนเซอร์อนาล็อก (soil/light/sensor.percent/raw, median 9 ครั้ง), บัซเซอร์ `tone/noTone`, `servo.write` — PWM แยกตัวจับเวลาต่อขา (ช่องคู่) วัดบนบอร์ดแล้ว: servo 495/1448/2401 µs, tone 440 Hz ตรง
- [x] ห้ามใช้ GPIO1/3 (USB Serial) จากโปรแกรม
- [x] MQ-2 ผ่านตัวแบ่งแรงดัน 10k/20k: `mq2.level/voltage/calibrate/ppm/config` (analogReadMilliVolts, R0 เก็บใน NVS, สูตร ppm ตรวจกับจุดบนกราฟ datasheet)
- [x] `relay.set` (LOW/HIGH-trigger), `motor.run/stop` (2 ขา PWM หรือ IN1/IN2 + EN) — หยุด/ error แล้วมอเตอร์หยุด รีเลย์ปิดเอง (วัดบนบอร์ดแล้ว)
- [x] เซนเซอร์ฝน `rain.level` และระดับน้ำ `water.level` (จ่ายไฟแผ่นเฉพาะตอนวัด) + ตัวอย่างปิดหน้าต่าง/เติมน้ำถัง (ล็อกกันน้ำล้น)
- [x] แก้บั๊ก: ฟังก์ชันไลบรารีทุกตัว (เซนเซอร์/จอ/มอเตอร์) เช็กสถานะหยุดก่อนทำงาน — ลูปที่เรียกฟังก์ชันช้า ๆ เคยใช้เวลาหยุดเกิน 1.5 วินาที (ds18b20 อาจเป็นนาที) ตอนนี้ 15-31 ms
- [x] ปรับหน้าเว็บ: แท็บ 'การต่อสาย' (อ่านบล็อก → อุปกรณ์/ขา/แผนผังบอร์ด 30 ขา/คำเตือนขาชนกัน), แกลเลอรีตัวอย่างแบบการ์ด, หน้าต้อนรับ, ค้นหาบล็อก (Ctrl+K), undo/redo, แผนผังขา, ธีมใหม่
- [ ] ทดสอบกับเซนเซอร์และจอจริง (ยังไม่มีอุปกรณ์ต่อ)
- [x] ภาษาไทยบนจอ OLED: ฟอนต์บิตแมป 20 จุด (K2D Bold (SIL OFL)) + วางสระ/วรรณยุกต์ซ้อน, `oled.width`, ตัวเลือก font — เทียบกับ PC ทีละพิกเซลผ่าน `tools/oled_font_test.py` ตรงทั้งหมด

## 10. ต่อยอดภายหลัง
- เลือกรุ่นบอร์ดตอนคอมไพล์ (ESP32-S3, C3, บอร์ดที่มีจอ) → แยก pin map ตามบอร์ดใน `boards/*.h` และ toolbox ฝั่งเว็บอ่านจาก `/api/info`
- บล็อก Modbus RTU (RS485), I2C sensor, UART, จอ OLED/AMOLED
- ไฮไลต์บล็อกที่กำลังทำงาน (Blockly `STATEMENT_PREFIX`)
- OTA อัปเดตเฟิร์มแวร์ผ่านเว็บ

## หมายเหตุสำหรับ Claude Code
- ในโฟลเดอร์มีโค้ดร่างแล้ว: `platformio.ini`, `src/config.h`, `src/main.cpp` (runner + API ครบตามข้อ 2/4/5 แต่ยังไม่ได้คอมไพล์ทดสอบ), `lib/lua/` — ให้ตรวจและแก้ให้คอมไพล์ผ่านก่อน
- ยังไม่มี: `src/web_assets.h`, `web/index.html`, `tools/embed_web.py`, ไฟล์ Blockly (ดึงด้วย `npm pack blockly`)
- Arduino-ESP32 core 2.x (PlatformIO espressif32 6.x) — `analogWrite`, `touchRead` มีให้ใช้

## บันทึกจากการทดสอบ (เฟส 3)
- LittleFS ของ core นี้ crash ถ้าพื้นที่เต็มกลางการเขียน → อัปโหลดหยุดก่อนเต็ม 12KB แล้วตอบ 507
- พาร์ติชัน min_spiffs ให้ LittleFS ~112KB ว่าง: เก็บโปรแกรมทั่วไปได้หลายสิบตัว แต่ไฟล์ใหญ่ 50KB+ ได้แค่ 1-2 ไฟล์
- ข้าม autorun อัตโนมัติถ้าบอร์ดรีเซ็ตเพราะ crash/WDT (กันบูตวน)
- ตอนบูตลบไฟล์ `.tmp` และ `.blk` ที่ไม่มี `.lua` คู่ (เศษจากไฟดับระหว่างบันทึก)

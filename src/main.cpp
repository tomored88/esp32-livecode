// ESP32 Live Code — drag blocks (or write Lua) in the browser, runs on the board instantly.
#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <LittleFS.h>
#include <Preferences.h>
#include <vector>
#include <map>
#include <algorithm>
#include <driver/gpio.h>
#include <soc/io_mux_reg.h>
#include <soc/gpio_periph.h>
#include "config.h"
#include "web_assets.h"
#include "i2c_dev.h"

extern "C" {
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
}

#define FW_VERSION "1.0.0"

WebServer server(80);
Preferences prefs;

// ---------------------------------------------------------------- log buffer
struct LogLine { uint32_t seq; uint8_t type; String text; };   // type 0=print 1=error 2=system
static std::vector<LogLine> logBuf;
static std::map<String, String> liveVals;
static uint32_t logSeq = 0, valsSeq = 0, clearSeq = 0;
static uint32_t bootId;   // random per boot: lets the page notice a reboot
static SemaphoreHandle_t logMux;
static const size_t LOG_MAX = 300;

static void logAdd(uint8_t type, const String &s) {
  Serial.println(s);
  xSemaphoreTake(logMux, portMAX_DELAY);
  logBuf.push_back({++logSeq, type, s});
  if (logBuf.size() > LOG_MAX) logBuf.erase(logBuf.begin(), logBuf.begin() + (logBuf.size() - LOG_MAX));
  xSemaphoreGive(logMux);
}

void logPrint(const String &s) { logAdd(0, s); }   // used by i2c_dev.cpp

static void jsonEsc(String &out, const String &s) {
  out += '"';
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    switch (c) {
      case '"':  out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': break;
      case '\t': out += "\\t"; break;
      default:
        if ((uint8_t)c < 0x20) { char b[8]; snprintf(b, sizeof b, "\\u%04x", c); out += b; }
        else out += c;
    }
  }
  out += '"';
}

// ---------------------------------------------------------------- Lua runner
static volatile bool stopReq = false;
static volatile bool running = false;
static volatile bool hasPending = false;
static String pendingCode;
static uint32_t lastYield = 0;

static void checkStop(lua_State *L) {
  if (stopReq) luaL_error(L, "__STOP__");
}

static void hook(lua_State *L, lua_Debug *) {
  checkStop(L);
  // let WiFi / web server breathe even inside a tight loop
  if (millis() - lastYield > 20) { lastYield = millis(); vTaskDelay(1); }
}

// keep some heap for WiFi + web server
static void *limAlloc(void *, void *ptr, size_t osize, size_t nsize) {
  if (nsize == 0) { free(ptr); return NULL; }
  if (nsize > osize && ESP.getFreeHeap() < 40000 + nsize) return NULL;
  return realloc(ptr, nsize);
}

static int l_print(lua_State *L) {
  int n = lua_gettop(L);
  String s;
  for (int i = 1; i <= n; i++) {
    size_t len; const char *p = luaL_tolstring(L, i, &len);
    if (i > 1) s += '\t';
    s += String(p).substring(0, len);
    lua_pop(L, 1);
  }
  logAdd(0, s);
  return 0;
}

static int l_clear(lua_State *L) {
  xSemaphoreTake(logMux, portMAX_DELAY);
  logBuf.clear(); clearSeq = logSeq; liveVals.clear(); valsSeq++;
  xSemaphoreGive(logMux);
  return 0;
}

static int l_show(lua_State *L) {
  String k = luaL_checkstring(L, 1);
  size_t len; const char *p = luaL_tolstring(L, 2, &len);
  String v = String(p).substring(0, len);
  lua_pop(L, 1);
  xSemaphoreTake(logMux, portMAX_DELAY);
  if (liveVals.size() < 24 || liveVals.count(k)) { liveVals[k] = v; valsSeq++; }
  xSemaphoreGive(logMux);
  return 0;
}

static int l_delay(lua_State *L) {
  i2cFlush();                 // drawing on the OLED shows up whenever the program waits
  lua_Number n = luaL_checknumber(L, 1);
  int32_t ms = n > 0 ? (int32_t)n : 0;
  while (ms > 0) {
    int d = ms > 20 ? 20 : (int)ms;
    vTaskDelay(pdMS_TO_TICKS(d));
    ms -= d;
    checkStop(L);
  }
  lastYield = millis();
  return 0;
}

static int l_millis(lua_State *L) { lua_pushinteger(L, millis()); return 1; }
static int l_micros(lua_State *L) { lua_pushinteger(L, micros()); return 1; }

static void pwmRelease(int p);

static int checkPin(lua_State *L, int idx) {
  int p = luaL_checkinteger(L, idx);
  if (p < 0 || p > 39 || (p >= 6 && p <= 11)) luaL_error(L, "ขา GPIO %d ใช้ไม่ได้", p);
  if (p == 1 || p == 3) luaL_error(L, "ขา GPIO %d ใช้สื่อสารผ่าน USB (Serial) อยู่ ใช้ขาอื่น", p);
  return p;
}

// pins this run has configured with pinMode; digitalWrite on a pin that was never
// configured makes it an output first (so blocks/loops over pins work without pinMode)
static uint64_t cfgPins = 0;

static int l_pinMode(lua_State *L) {
  int p = checkPin(L, 1);
  int m = luaL_optinteger(L, 2, OUTPUT);
  if ((m == OUTPUT) && p >= 34) luaL_error(L, "ขา %d เป็นขาอินพุตอย่างเดียว", p);
  pwmRelease(p);
  pinMode(p, m);
  cfgPins |= 1ULL << p;
  return 0;
}

static int8_t pwmChan[40];          // pin -> LEDC channel, -1 = none (see PWM below)

static int l_digitalWrite(lua_State *L) {
  int p = checkPin(L, 1);
  if (p >= 34) luaL_error(L, "ขา %d เป็นขาอินพุตอย่างเดียว", p);
  int v = lua_isboolean(L, 2) ? lua_toboolean(L, 2) : (luaL_checknumber(L, 2) != 0);
  pwmRelease(p);
  if (!(cfgPins & (1ULL << p))) { pinMode(p, OUTPUT); cfgPins |= 1ULL << p; }
  digitalWrite(p, v ? HIGH : LOW);
  return 0;
}

static int l_digitalRead(lua_State *L) { lua_pushinteger(L, digitalRead(checkPin(L, 1))); return 1; }

static int l_analogRead(lua_State *L) {
  int p = checkPin(L, 1);
  // ADC2 pins do not work while WiFi is on
  if (p < 32) luaL_error(L, "ขา %d อ่านอนาล็อกไม่ได้ขณะเปิด WiFi ใช้ขา 32-39", p);
  lua_pushinteger(L, analogRead(p));
  return 1;
}

static int l_touchRead(lua_State *L) {
  int p = checkPin(L, 1);
  static const uint8_t touchPins[] = {4, 0, 2, 15, 13, 12, 14, 27, 33, 32};  // T0..T9
  bool ok = false;
  for (uint8_t t : touchPins) ok |= (t == p);
  if (!ok) luaL_error(L, "ขา %d ไม่ใช่ขาสัมผัส (ใช้ 0,2,4,12,13,14,15,27,32,33)", p);
  lua_pushinteger(L, touchRead(p));
  return 1;
}

// PWM: own LEDC bookkeeping (instead of analogWrite) so every run can release the
// pins and a later digitalWrite on the same pin works again.
static uint8_t pwmUsed = 0;
// LEDC channels 2k and 2k+1 share a timer (= the same frequency), so every pin gets an
// even channel: PWM (5 kHz), buzzer tones and servos (50 Hz) can then run side by side.
enum { PWM_DUTY = 0, PWM_TONE = 1, PWM_SERVO = 2 };
static uint8_t pwmMode[40];
static const int PWM_MAX = 8;

static void pwmRelease(int p) {
  if (pwmChan[p] < 0) return;
  ledcWrite(pwmChan[p], 0);
  ledcDetachPin(p);
  pwmChan[p] = -1;
  pinMode(p, OUTPUT);        // back to a plain GPIO output, so digitalWrite/digitalRead work
  digitalWrite(p, LOW);
}

static void pwmReleaseAll() {
  for (int p = 0; p < 40; p++) pwmRelease(p);
  pwmUsed = 0;
  cfgPins = 0;
}

// attach pin p to its own LEDC channel in the given mode (re-configures if the mode changed)
static int pwmAttach(lua_State *L, int p, uint8_t mode, uint32_t freq, uint8_t bits) {
  if (p >= 34) luaL_error(L, "ขา %d เป็นขาอินพุตอย่างเดียว ใช้ส่งสัญญาณไม่ได้", p);
  if (pwmChan[p] < 0) {
    if (pwmUsed >= PWM_MAX) luaL_error(L, "ใช้ PWM/บัซเซอร์/เซอร์โว ได้พร้อมกันไม่เกิน %d ขา", PWM_MAX);
    pwmChan[p] = (int8_t)(pwmUsed++ * 2);
    ledcSetup(pwmChan[p], freq, bits);
    ledcAttachPin(p, pwmChan[p]);
    PIN_INPUT_ENABLE(GPIO_PIN_MUX_REG[p]);    // keep digitalRead working on the pin (reads the signal)
    pwmMode[p] = mode;
  } else if (pwmMode[p] != mode) {
    ledcSetup(pwmChan[p], freq, bits);
    pwmMode[p] = mode;
  }
  return pwmChan[p];
}

static int l_analogWrite(lua_State *L) {
  int p = checkPin(L, 1);
  int v = constrain((int)luaL_checknumber(L, 2), 0, 255);
  ledcWrite(pwmAttach(L, p, PWM_DUTY, 5000, 8), v);
  return 0;
}

// wait ms in 20 ms steps so Stop still works
static void sleepMs(lua_State *L, int32_t ms) {
  while (ms > 0) {
    int d = ms > 20 ? 20 : (int)ms;
    vTaskDelay(pdMS_TO_TICKS(d));
    ms -= d;
    checkStop(L);
  }
  lastYield = millis();
}

// ---- buzzer (passive): tone(pin, Hz [, ms]) — with ms it waits and then goes quiet
static int l_tone(lua_State *L) {
  int p = checkPin(L, 1);
  lua_Number f = luaL_checknumber(L, 2);
  int ch = pwmAttach(L, p, PWM_TONE, 1000, 10);
  if (f < 1) {                                   // 0 Hz = rest: silent (for the given time)
    ledcWrite(ch, 0);
    if (!lua_isnoneornil(L, 3)) { lua_Number ms = luaL_checknumber(L, 3); sleepMs(L, ms > 0 ? (int32_t)ms : 0); }
    return 0;
  }
  if (f < 20 || f > 20000) luaL_error(L, "ความถี่เสียงต้องอยู่ระหว่าง 20-20000 Hz");
  ledcWriteTone(ch, f);
  if (!lua_isnoneornil(L, 3)) {
    lua_Number ms = luaL_checknumber(L, 3);
    sleepMs(L, ms > 0 ? (int32_t)ms : 0);
    ledcWrite(ch, 0);
  }
  return 0;
}

static int l_noTone(lua_State *L) {
  int p = checkPin(L, 1);
  if (pwmChan[p] >= 0 && pwmMode[p] == PWM_TONE) ledcWrite(pwmChan[p], 0);
  return 0;
}

// a stopped/finished program must not leave a buzzer sounding
static void toneSilenceAll() {
  for (int p = 0; p < 40; p++)
    if (pwmChan[p] >= 0 && pwmMode[p] == PWM_TONE) ledcWrite(pwmChan[p], 0);
}

// ---- servo: servo.write(pin, angle [, minUs, maxUs]) — 50 Hz, 500-2400 us pulse (SG90/MG90S)
static int l_servo_write(lua_State *L) {
  int p = checkPin(L, 1);
  lua_Number a = luaL_checknumber(L, 2);
  lua_Number lo = luaL_optnumber(L, 3, 500), hi = luaL_optnumber(L, 4, 2400);
  if (lo < 300 || hi > 2700 || lo >= hi) luaL_error(L, "ช่วงพัลส์เซอร์โวต้องอยู่ใน 300-2700 us และ min < max");
  a = a < 0 ? 0 : a > 180 ? 180 : a;
  int ch = pwmAttach(L, p, PWM_SERVO, 50, 16);
  double us = lo + (hi - lo) * a / 180.0;
  ledcWrite(ch, (uint32_t)(us / 20000.0 * 65535.0 + 0.5));
  return 0;
}

// ---- analog sensors: median of 9 readings (ADC noise), mapped to 0-100 %
static void checkAdcPin(lua_State *L, int p) {
  if (p < 32) luaL_error(L, "ขา %d อ่านอนาล็อกไม่ได้ขณะเปิด WiFi ใช้ขา 32-39", p);
}
static int analogMedian(int p) {
  int v[9];
  for (int i = 0; i < 9; i++) {
    v[i] = analogRead(p);
    for (int j = i; j > 0 && v[j] < v[j - 1]; j--) std::swap(v[j], v[j - 1]);
  }
  return v[4];
}
static int percentOf(lua_State *L, int p, lua_Number at0, lua_Number at100) {
  if (at0 == at100) luaL_error(L, "ค่าปรับเทียบ 0%% กับ 100%% ต้องไม่เท่ากัน");
  double pct = (analogMedian(p) - at0) * 100.0 / (at100 - at0);
  pct = pct < 0 ? 0 : pct > 100 ? 100 : pct;
  lua_pushinteger(L, (lua_Integer)floor(pct + 0.5));
  return 1;
}
// sensor.raw(pin) -> 0-4095 (steady median) · sensor.percent(pin, rawAt0, rawAt100) -> 0-100
static int l_sensor_raw(lua_State *L) { int p = checkPin(L, 1); checkAdcPin(L, p); lua_pushinteger(L, analogMedian(p)); return 1; }
static int l_sensor_percent(lua_State *L) {
  int p = checkPin(L, 1); checkAdcPin(L, p);
  return percentOf(L, p, luaL_checknumber(L, 2), luaL_checknumber(L, 3));
}
// soil.moisture(pin [, dry, wet]) -> % (capacitive sensor v1.2 at 3.3 V: dry ~2800, in water ~1200)
static int l_soil(lua_State *L) {
  int p = checkPin(L, 1); checkAdcPin(L, p);
  return percentOf(L, p, luaL_optnumber(L, 2, 2800), luaL_optnumber(L, 3, 1200));
}
// light.level(pin [, dark, bright]) -> % (LDR module: brighter = lower voltage)
static int l_light(lua_State *L) {
  int p = checkPin(L, 1); checkAdcPin(L, p);
  return percentOf(L, p, luaL_optnumber(L, 2, 4095), luaL_optnumber(L, 3, 0));
}

// round(x [, digits]) -> x rounded to digits decimals (integer result when digits = 0)
static int l_round(lua_State *L) {
  lua_Number x = luaL_checknumber(L, 1);
  lua_Integer n = luaL_optinteger(L, 2, 0);
  if (n < 0 || n > 6) luaL_error(L, "จำนวนทศนิยมต้องเป็น 0-6");
  if (n == 0) { lua_pushinteger(L, (lua_Integer)floor(x + 0.5)); return 1; }
  double m = pow(10.0, (double)n);
  lua_pushnumber(L, floor(x * m + 0.5) / m);
  return 1;
}

// ultrasonic.distance(trig, echo [, tempC]) -> cm, or -1 when nothing is in range (> ~4 m)
// HC-SR04: 10 us pulse on trig, echo stays HIGH for the round trip of the sound.
// Errors when echo never goes HIGH (sensor missing / wrong pins / no 5 V).
static uint32_t usLastMs = 0;

static int l_us_distance(lua_State *L) {
  int trig = checkPin(L, 1), echo = checkPin(L, 2);
  if (trig >= 34) luaL_error(L, "ขา trig %d เป็นขาอินพุตอย่างเดียว ใช้ขาอื่น (เช่น 5)", trig);
  if (trig == echo) luaL_error(L, "ขา trig กับ echo ต้องเป็นคนละขา");
  lua_Number tempC = luaL_optnumber(L, 3, 20.0);
  // the sensor needs ~60 ms between measurements or old echoes are picked up
  uint32_t since = millis() - usLastMs;
  if (since < 60) sleepMs(L, 60 - since);
  pwmRelease(trig);
  pinMode(trig, OUTPUT);
  pinMode(echo, echo >= 34 ? INPUT : INPUT_PULLDOWN);   // 34-39 have no internal pull-down
  cfgPins |= (1ULL << trig) | (1ULL << echo);
  uint32_t t0 = micros();
  while (digitalRead(echo) == HIGH && micros() - t0 < 40000) {}   // previous echo still running
  digitalWrite(trig, LOW); delayMicroseconds(3);
  digitalWrite(trig, HIGH); delayMicroseconds(10);
  digitalWrite(trig, LOW);
  t0 = micros();
  while (digitalRead(echo) == LOW)
    if (micros() - t0 > 10000) {
      usLastMs = millis();
      return luaL_error(L, "ไม่มีสัญญาณจาก HC-SR04 — ตรวจสาย trig=GPIO%d echo=GPIO%d และไฟเลี้ยง 5V "
                           "(ขา echo ควรผ่านตัวต้านทานแบ่งแรงดันลงเหลือ 3.3V)", trig, echo);
    }
  uint32_t start = micros();
  while (digitalRead(echo) == HIGH)
    if (micros() - start > 26000) {          // > ~4.4 m: nothing in range
      usLastMs = millis();
      lua_pushinteger(L, -1);
      return 1;
    }
  uint32_t us = micros() - start;
  usLastMs = millis();
  double v = 331.3 + 0.606 * tempC;          // speed of sound in m/s at tempC
  lua_pushnumber(L, us * v / 20000.0);        // us * (m/s) / 2 -> cm
  return 1;
}

// ---- DHT22 / DHT11 (single-wire temperature + humidity)
// Start: host pulls the line LOW (DHT22 1 ms, DHT11 20 ms) and releases it. The sensor
// answers LOW 80 us + HIGH 80 us, then 40 bits: LOW ~50 us, HIGH ~27 us (0) or ~70 us (1).
// The ~5 ms of bit reading runs with interrupts off so WiFi cannot stretch the timing.
// The sensor may only be read every 2 s (DHT11: 1 s), so values are cached per pin.
struct DhtCache { int8_t pin = -1; uint8_t type; uint32_t at; float t, h; };
static DhtCache dhtCache[4];
static portMUX_TYPE dhtMux = portMUX_INITIALIZER_UNLOCKED;

static int32_t dhtWait(int pin, int level, uint32_t timeoutUs) {   // us spent at `level`, -1 on timeout
  uint32_t t0 = micros();
  while (digitalRead(pin) == level)
    if (micros() - t0 > timeoutUs) return -1;
  return (int32_t)(micros() - t0);
}

// 0 = ok, 1 = no answer, 2 = timing error mid-transfer, 3 = checksum error
static int dhtRead(int pin, bool dht11, float &t, float &h) {
  pwmRelease(pin);
  pinMode(pin, INPUT_PULLUP);
  delay(2);
  pinMode(pin, OUTPUT);
  digitalWrite(pin, LOW);
  if (dht11) delay(20); else delayMicroseconds(1100);
  int32_t lows[40], highs[40];
  int err = 0;
  pinMode(pin, INPUT_PULLUP);
  portENTER_CRITICAL(&dhtMux);
  if (dhtWait(pin, HIGH, 200) < 0 || dhtWait(pin, LOW, 120) < 0 || dhtWait(pin, HIGH, 120) < 0) err = 1;
  for (int i = 0; i < 40 && !err; i++) {
    lows[i] = dhtWait(pin, LOW, 100);
    highs[i] = dhtWait(pin, HIGH, 120);
    if (lows[i] < 0 || highs[i] < 0) err = 2;
  }
  portEXIT_CRITICAL(&dhtMux);
  cfgPins |= 1ULL << pin;
  if (err) return err;
  uint8_t d[5] = {0};
  for (int i = 0; i < 40; i++) d[i / 8] = (d[i / 8] << 1) | (highs[i] > lows[i] ? 1 : 0);
  if (((d[0] + d[1] + d[2] + d[3]) & 0xFF) != d[4]) return 3;
  if (dht11) {
    h = d[0] + d[1] / 10.0f;
    t = (d[2] & 0x7F) + d[3] / 10.0f;
    if (d[2] & 0x80) t = -t;
  } else {
    h = ((d[0] << 8) | d[1]) / 10.0f;
    t = (((d[2] & 0x7F) << 8) | d[3]) / 10.0f;
    if (d[2] & 0x80) t = -t;
  }
  return 0;
}

// the sensors report 0.1 steps; push 25.3, not the float's 25.299999237
static void pushTenths(lua_State *L, float v) { lua_pushnumber(L, round(v * 10.0) / 10.0); }

// dht.temperature(pin [, "dht22"|"dht11"]) / dht.humidity(...)
static int dhtGet(lua_State *L, bool wantTemp) {
  int pin = checkPin(L, 1);
  if (pin >= 34) luaL_error(L, "ขา %d ใช้กับ DHT ไม่ได้ (ต้องส่งสัญญาณออกได้) ใช้ขาอื่น เช่น 4", pin);
  static const char *const types[] = {"dht22", "dht11", NULL};
  int type = luaL_checkoption(L, 2, "dht22", types);
  const char *name = type ? "DHT11" : "DHT22";
  uint32_t minGap = type ? 1100 : 2100;
  DhtCache *c = nullptr;
  for (auto &e : dhtCache) if (e.pin == pin && e.type == type) c = &e;
  if (c && millis() - c->at < minGap) { pushTenths(L, wantTemp ? c->t : c->h); return 1; }
  if (c) { uint32_t gap = millis() - c->at; if (gap < minGap) sleepMs(L, minGap - gap); }
  if (millis() < 1500) sleepMs(L, 1500 - millis());    // sensor needs ~1 s after power-up
  float t, h;
  int err = dhtRead(pin, type == 1, t, h);
  if (err) {
    if (c && millis() - c->at < 10000) { pushTenths(L, wantTemp ? c->t : c->h); return 1; }   // brief glitch
    static const char *why[] = {"", "ไม่มีสัญญาณตอบ", "สัญญาณขาดระหว่างอ่าน", "ข้อมูลผิด (checksum)"};
    return luaL_error(L, "อ่าน %s ที่ GPIO%d ไม่ได้ (%s) — ตรวจสาย DATA, ไฟเลี้ยง 3.3V/5V, ตัวต้านทาน pull-up 10k และชนิดเซนเซอร์ (DHT22/DHT11)",
                      name, pin, why[err]);
  }
  if (!c) {                                         // take a free slot (or the oldest)
    c = &dhtCache[0];
    for (auto &e : dhtCache) { if (e.pin < 0) { c = &e; break; } if (e.at < c->at) c = &e; }
  }
  c->pin = pin; c->type = type; c->at = millis(); c->t = t; c->h = h;
  pushTenths(L, wantTemp ? t : h);
  return 1;
}
static int l_dht_t(lua_State *L) { return dhtGet(L, true); }
static int l_dht_h(lua_State *L) { return dhtGet(L, false); }

// ---- 1-Wire bus + DS18B20 temperature probes (several probes may share one pin)
// Open-drain GPIO: level 0 pulls the bus low, level 1 releases it to the pull-up resistor.
// Each time slot (60-480 us) runs with interrupts off so WiFi cannot stretch it.
static portMUX_TYPE owMux = portMUX_INITIALIZER_UNLOCKED;

static void owSetup(int pin) {
  pwmRelease(pin);
  pinMode(pin, INPUT_PULLUP);                          // routes the pad to plain GPIO
  gpio_set_direction((gpio_num_t)pin, GPIO_MODE_INPUT_OUTPUT_OD);
  gpio_set_pull_mode((gpio_num_t)pin, GPIO_PULLUP_ONLY);
  gpio_set_level((gpio_num_t)pin, 1);
  cfgPins |= 1ULL << pin;
}

static bool owReset(int pin) {                         // true = a device answered (presence pulse)
  gpio_num_t g = (gpio_num_t)pin;
  gpio_set_level(g, 0);
  delayMicroseconds(480);
  portENTER_CRITICAL(&owMux);
  gpio_set_level(g, 1);
  delayMicroseconds(70);
  bool present = gpio_get_level(g) == 0;
  portEXIT_CRITICAL(&owMux);
  delayMicroseconds(410);
  return present;
}

static void owWriteBit(int pin, int bit) {
  gpio_num_t g = (gpio_num_t)pin;
  portENTER_CRITICAL(&owMux);
  gpio_set_level(g, 0);
  delayMicroseconds(bit ? 6 : 60);
  gpio_set_level(g, 1);
  portEXIT_CRITICAL(&owMux);
  delayMicroseconds(bit ? 64 : 10);
}

static int owReadBit(int pin) {
  gpio_num_t g = (gpio_num_t)pin;
  portENTER_CRITICAL(&owMux);
  gpio_set_level(g, 0);
  delayMicroseconds(3);
  gpio_set_level(g, 1);
  delayMicroseconds(10);
  int b = gpio_get_level(g);
  portEXIT_CRITICAL(&owMux);
  delayMicroseconds(53);
  return b;
}

static void owWrite(int pin, uint8_t v) { for (int i = 0; i < 8; i++) owWriteBit(pin, (v >> i) & 1); }
static uint8_t owRead(int pin) { uint8_t v = 0; for (int i = 0; i < 8; i++) v |= owReadBit(pin) << i; return v; }

// Dallas/Maxim CRC-8 (x^8 + x^5 + x^4 + 1, LSB first)
static uint8_t owCrc8(const uint8_t *d, int n) {
  uint8_t crc = 0;
  for (int i = 0; i < n; i++) {
    uint8_t b = d[i];
    for (int j = 0; j < 8; j++) { uint8_t mix = (crc ^ b) & 1; crc >>= 1; if (mix) crc ^= 0x8C; b >>= 1; }
  }
  return crc;
}

// ROM search (Maxim application note 187): fills roms[] with every temperature probe found
static int owSearchAll(int pin, uint8_t roms[][8], int maxDev) {
  uint8_t rom[8] = {0};
  int count = 0, lastDisc = 0;
  bool lastDevice = false;
  while (!lastDevice && count < maxDev) {
    if (!owReset(pin)) break;
    owWrite(pin, 0xF0);
    int lastZero = 0;
    for (int bit = 1; bit <= 64; bit++) {
      int idx = (bit - 1) / 8;
      uint8_t mask = 1 << ((bit - 1) % 8);
      int b = owReadBit(pin), cb = owReadBit(pin);
      if (b && cb) return count;                        // nobody answered this bit
      int dir;
      if (b != cb) dir = b;
      else {
        dir = bit < lastDisc ? ((rom[idx] & mask) ? 1 : 0) : (bit == lastDisc);
        if (!dir) lastZero = bit;
      }
      if (dir) rom[idx] |= mask; else rom[idx] &= ~mask;
      owWriteBit(pin, dir);
    }
    lastDisc = lastZero;
    if (!lastDisc) lastDevice = true;
    bool tempProbe = rom[0] == 0x28 || rom[0] == 0x22 || rom[0] == 0x10;   // DS18B20, DS1822, DS18S20
    if (owCrc8(rom, 7) == rom[7] && tempProbe) memcpy(roms[count++], rom, 8);
  }
  return count;
}

static const int DS_MAX = 8;
struct DsBus { int8_t pin = -1; int count = 0; uint32_t at = 0; uint8_t roms[DS_MAX][8]; float t[DS_MAX]; };
static DsBus dsBus[2];

static bool dsReadOne(int pin, const uint8_t *rom, float &t) {
  for (int attempt = 0; attempt < 2; attempt++) {
    if (!owReset(pin)) continue;
    owWrite(pin, 0x55);                                // MATCH ROM
    for (int i = 0; i < 8; i++) owWrite(pin, rom[i]);
    owWrite(pin, 0xBE);                                // READ SCRATCHPAD
    uint8_t d[9];
    for (int i = 0; i < 9; i++) d[i] = owRead(pin);
    if (owCrc8(d, 8) != d[8]) continue;
    int16_t raw = (int16_t)((d[1] << 8) | d[0]);
    if (rom[0] == 0x10) t = (raw >> 1) - 0.25f + (16 - d[6]) / 16.0f;   // DS18S20: 9-bit + count remain
    else t = raw / 16.0f;
    return true;
  }
  return false;
}

// refresh all probes on a pin (search, convert all at once, read each); ~0.8 s
static DsBus *dsRefresh(lua_State *L, int pin) {
  DsBus *b = nullptr;
  for (auto &e : dsBus) if (e.pin == pin) b = &e;
  if (b && b->count && millis() - b->at < 1000) return b;
  if (!b) { b = &dsBus[0]; for (auto &e : dsBus) { if (e.pin < 0) { b = &e; break; } if (e.at < b->at) b = &e; } }
  b->pin = pin; b->count = 0;
  owSetup(pin);
  if (!owReset(pin)) {
    luaL_error(L, "ไม่พบ DS18B20 ที่ GPIO%d — ตรวจสาย DATA, ไฟเลี้ยง VDD 3.3V และตัวต้านทาน 4.7k ระหว่าง DATA กับ 3.3V", pin);
  }
  b->count = owSearchAll(pin, b->roms, DS_MAX);
  if (!b->count) luaL_error(L, "มีอุปกรณ์ 1-Wire ที่ GPIO%d แต่ไม่ใช่ DS18B20 หรือสัญญาณรบกวน (ตรวจตัวต้านทาน 4.7k)", pin);
  owReset(pin);
  owWrite(pin, 0xCC);                                  // SKIP ROM: every probe converts at once
  owWrite(pin, 0x44);                                  // CONVERT T (12-bit: up to 750 ms)
  uint32_t t0 = millis();
  do { sleepMs(L, 10); } while (!owReadBit(pin) && millis() - t0 < 800);   // reads 1 when done
  for (int i = 0; i < b->count; i++)
    if (!dsReadOne(pin, b->roms[i], b->t[i])) b->t[i] = NAN;
  b->at = millis();
  return b;
}

// ds18b20.temperature(pin [, n]) -> °C of probe n (1 = first; order is fixed by serial number)
static int l_ds_t(lua_State *L) {
  int pin = checkPin(L, 1);
  if (pin >= 34) luaL_error(L, "ขา %d ใช้กับ DS18B20 ไม่ได้ (ต้องส่งสัญญาณออกได้) ใช้ขาอื่น เช่น 4", pin);
  int n = (int)luaL_optinteger(L, 2, 1);
  DsBus *b = dsRefresh(L, pin);
  if (n < 1 || n > b->count) luaL_error(L, "ที่ GPIO%d มี DS18B20 %d ตัว (ขอตัวที่ %d)", pin, b->count, n);
  if (isnan(b->t[n - 1])) luaL_error(L, "อ่าน DS18B20 ตัวที่ %d ที่ GPIO%d ไม่สำเร็จ (ข้อมูลผิด — สายยาว/สัญญาณรบกวน ลองตัวต้านทาน 4.7k)", n, pin);
  lua_pushnumber(L, round(b->t[n - 1] * 100.0) / 100.0);
  return 1;
}

// ds18b20.count(pin) -> number of probes on that pin (0 if none)
static int l_ds_count(lua_State *L) {
  int pin = checkPin(L, 1);
  if (pin >= 34) luaL_error(L, "ขา %d ใช้กับ DS18B20 ไม่ได้", pin);
  owSetup(pin);
  if (!owReset(pin)) { lua_pushinteger(L, 0); return 1; }
  uint8_t roms[DS_MAX][8];
  lua_pushinteger(L, owSearchAll(pin, roms, DS_MAX));
  return 1;
}

// ---- relay modules and DC motors (through a driver board)
// When a program is stopped or fails, every motor it started stops and every relay it
// switched goes off, so a pump/motor never keeps running unattended. A program that
// finishes normally leaves them as it set them.
static uint64_t relayMask = 0, relayLowMask = 0;      // relays used by this run, LOW-trigger ones
struct MotorPins { int8_t in1, in2, en; };
static MotorPins motors[8];
static int motorCount = 0;

static void outLevel(int p, int level) {             // plain GPIO output without a glitch
  pwmRelease(p);
  digitalWrite(p, level);                              // output latch first ...
  pinMode(p, OUTPUT);                                  // ... then enable the driver
  digitalWrite(p, level);
  cfgPins |= 1ULL << p;
}

// relay.set(pin, on [, lowTrigger = true]) — most relay modules switch on when IN is LOW
static int l_relay_set(lua_State *L) {
  int p = checkPin(L, 1);
  if (p >= 34) luaL_error(L, "ขา %d เป็นขาอินพุตอย่างเดียว ใช้คุมรีเลย์ไม่ได้", p);
  bool on = lua_toboolean(L, 2);
  bool low = lua_isnoneornil(L, 3) ? true : lua_toboolean(L, 3);
  outLevel(p, (on != low) ? HIGH : LOW);               // on+low -> LOW, on+high -> HIGH
  relayMask |= 1ULL << p;
  if (low) relayLowMask |= 1ULL << p; else relayLowMask &= ~(1ULL << p);
  return 0;
}

static int motorPin(lua_State *L, int i) {
  int p = checkPin(L, i);
  if (p >= 34) luaL_error(L, "ขา %d เป็นขาอินพุตอย่างเดียว ใช้คุมมอเตอร์ไม่ได้", p);
  return p;
}

static void motorRemember(int in1, int in2, int en) {
  for (int i = 0; i < motorCount; i++) if (motors[i].in1 == in1 && motors[i].in2 == in2) { motors[i].en = en; return; }
  if (motorCount < 8) motors[motorCount++] = {(int8_t)in1, (int8_t)in2, (int8_t)en};
}

// motor.run(in1, in2, speed [, en]) — speed -100..100 (negative = reverse, 0 = stop)
//   without en: PWM on IN1/IN2 (L9110S, DRV8833, L298N with the ENA jumper fitted)
//   with en:    IN1/IN2 set the direction, PWM on EN (L298N ENA/ENB, TB6612 PWMA)
static int l_motor_run(lua_State *L) {
  int in1 = motorPin(L, 1), in2 = motorPin(L, 2);
  lua_Number sp = luaL_checknumber(L, 3);
  int en = lua_isnoneornil(L, 4) ? -1 : motorPin(L, 4);
  if (in1 == in2 || in1 == en || in2 == en) luaL_error(L, "ขามอเตอร์ต้องเป็นคนละขากัน");
  sp = sp < -100 ? -100 : sp > 100 ? 100 : sp;
  int duty = (int)lround(fabs(sp) * 255.0 / 100.0);
  if (en >= 0) {
    outLevel(in1, sp > 0 ? HIGH : LOW);
    outLevel(in2, sp < 0 ? HIGH : LOW);
    ledcWrite(pwmAttach(L, en, PWM_DUTY, 5000, 8), duty);
  } else {
    ledcWrite(pwmAttach(L, in1, PWM_DUTY, 5000, 8), sp > 0 ? duty : 0);
    ledcWrite(pwmAttach(L, in2, PWM_DUTY, 5000, 8), sp < 0 ? duty : 0);
  }
  motorRemember(in1, in2, en);
  return 0;
}

static void motorOff(const MotorPins &m) {
  for (int p : {(int)m.in1, (int)m.in2, (int)m.en}) {
    if (p < 0) continue;
    if (pwmChan[p] >= 0) ledcWrite(pwmChan[p], 0); else digitalWrite(p, LOW);
  }
}

// motor.stop(in1, in2 [, en]) — both inputs LOW: the motor coasts to a stop
static int l_motor_stop(lua_State *L) {
  int in1 = motorPin(L, 1), in2 = motorPin(L, 2);
  int en = lua_isnoneornil(L, 3) ? -1 : motorPin(L, 3);
  motorOff({(int8_t)in1, (int8_t)in2, (int8_t)en});
  return 0;
}

static void actuatorsSafe() {
  for (int i = 0; i < motorCount; i++) motorOff(motors[i]);
  for (int p = 0; p < 40; p++)
    if (relayMask & (1ULL << p)) digitalWrite(p, (relayLowMask & (1ULL << p)) ? HIGH : LOW);   // off
}
static void actuatorsReset() { relayMask = relayLowMask = 0; motorCount = 0; }

// ---- rain sensor board and water level strip (analog, ADC1 pins 32-39)
// rain.level(pin [, dry, wet]) -> 0-100 % wet (more water = lower voltage on the usual FC-37/YL-83 board)
static int l_rain(lua_State *L) {
  int p = checkPin(L, 1); checkAdcPin(L, p);
  return percentOf(L, p, luaL_optnumber(L, 2, 4095), luaL_optnumber(L, 3, 1500));
}

// water.level(pin [, powerPin [, empty, full]]) -> 0-100 % (more water = higher voltage on the usual
// 3-pin strip sensor). The strip's bare traces corrode when powered all the time, so with powerPin
// the sensor is powered from that GPIO only for ~20 ms around each reading.
static int l_water(lua_State *L) {
  int p = checkPin(L, 1); checkAdcPin(L, p);
  int pw = lua_isnoneornil(L, 2) ? -1 : checkPin(L, 2);
  lua_Number empty = luaL_optnumber(L, 3, 0), full = luaL_optnumber(L, 4, 2000);
  if (pw >= 34) luaL_error(L, "ขา %d เป็นขาอินพุตอย่างเดียว จ่ายไฟให้เซนเซอร์ไม่ได้ ใช้ขาอื่น เช่น 27", pw);
  if (pw == p) luaL_error(L, "ขาจ่ายไฟกับขาอ่านค่าต้องเป็นคนละขากัน");
  if (empty == full) luaL_error(L, "ค่าปรับเทียบ 'ว่าง' กับ 'เต็ม' ต้องไม่เท่ากัน");
  if (pw >= 0) { outLevel(pw, HIGH); delay(20); }       // let the strip settle
  int raw = analogMedian(p);
  if (pw >= 0) digitalWrite(pw, LOW);                    // off again right away
  double pct = (raw - empty) * 100.0 / (full - empty);
  pct = pct < 0 ? 0 : pct > 100 ? 100 : pct;
  lua_pushinteger(L, (lua_Integer)floor(pct + 0.5));
  return 1;
}

// ---- MQ-2 gas sensor module (5 V heater, AO up to ~5 V)
// Wiring: AO -> 10k -> GPIO(32-39) -> 20k -> GND, so the pin sees AO x 2/3 (max 3.33 V).
// voltage = AO volts (divider undone); level = AO as 0-100 % of 5 V (no calibration);
// ppm = datasheet curve ppm = a * (Rs/R0)^b with Rs = RL * (5 - AO) / AO and
// R0 = Rs in clean air / 9.83 (mq2.calibrate, saved on the board).
// Rough estimates only: not a certified gas detector.
static float mqDivider = 1.5f, mqRl = 1.0f;          // AO/pin ratio, module load resistor (kOhm)

static void mqReset() { mqDivider = 1.5f; mqRl = 1.0f; }

static float mqVolts(lua_State *L, int pin) {
  checkAdcPin(L, pin);
  int v[9];
  for (int i = 0; i < 9; i++) {
    v[i] = analogReadMilliVolts(pin);
    for (int j = i; j > 0 && v[j] < v[j - 1]; j--) std::swap(v[j], v[j - 1]);
  }
  float ao = v[4] / 1000.0f * mqDivider;
  // with the divider fitted, an unplugged sensor leaves the pin at 0 V; real AO in clean air is >= ~0.2 V
  if (ao < 0.075f)
    luaL_error(L, "ไม่มีสัญญาณจาก MQ-2 ที่ GPIO%d — ตรวจสาย AO ผ่านตัวแบ่งแรงดัน (10k/20k) และไฟเลี้ยง 5V", pin);
  return ao > 4.99f ? 4.99f : ao;
}
static float mqRs(float ao) { return mqRl * (5.0f - ao) / ao; }   // kOhm

// mq2.config(divider [, rlKOhm]) — divider = AO volts / pin volts (10k/20k = 1.5, 10k/15k = 1.667)
static int l_mq_config(lua_State *L) {
  lua_Number d = luaL_checknumber(L, 1), rl = luaL_optnumber(L, 2, 1.0);
  if (d < 1 || d > 4) luaL_error(L, "อัตราแบ่งแรงดันต้องอยู่ระหว่าง 1-4 (10k/20k = 1.5)");
  if (rl <= 0 || rl > 100) luaL_error(L, "ค่า RL ต้องเป็น kOhm 0.1-100 (โมดูลทั่วไป 1 = 1k)");
  mqDivider = (float)d; mqRl = (float)rl;
  return 0;
}
static int l_mq_voltage(lua_State *L) {
  float ao = mqVolts(L, checkPin(L, 1));
  lua_pushnumber(L, round(ao * 100.0) / 100.0);
  return 1;
}
static int l_mq_level(lua_State *L) {
  float ao = mqVolts(L, checkPin(L, 1));
  lua_pushinteger(L, (lua_Integer)floor(ao / 5.0f * 100.0f + 0.5f));
  return 1;
}
static String mqKey(int pin) { return "mq2r0_" + String(pin); }

// mq2.calibrate(pin) — in clean air after warm-up: averages Rs for 5 s, saves R0, returns it
static int l_mq_calibrate(lua_State *L) {
  int pin = checkPin(L, 1);
  double sum = 0;
  for (int i = 0; i < 50; i++) { sum += mqRs(mqVolts(L, pin)); sleepMs(L, 100); }
  float r0 = (float)(sum / 50 / 9.83);
  prefs.putFloat(mqKey(pin).c_str(), r0);
  lua_pushnumber(L, round(r0 * 1000.0) / 1000.0);
  return 1;
}

// mq2.ppm(pin, gas) — gas: "lpg" "co" "h2" "alcohol" "propane"; 0-10000 ppm (estimate)
static int l_mq_ppm(lua_State *L) {
  int pin = checkPin(L, 1);
  static const char *const gases[] = {"lpg", "co", "h2", "alcohol", "propane", NULL};
  static const float A[] = {574.25f, 36974.0f, 987.99f, 3616.1f, 658.71f};
  static const float Bx[] = {-2.222f, -3.109f, -2.162f, -2.675f, -2.168f};
  int g = luaL_checkoption(L, 2, "lpg", gases);
  float r0 = prefs.getFloat(mqKey(pin).c_str(), 0);
  if (r0 <= 0) luaL_error(L, "MQ-2 ที่ GPIO%d ยังไม่ได้ปรับเทียบ — รันตัวอย่าง \"ปรับเทียบ MQ-2\" ในอากาศสะอาดก่อน", pin);
  float ratio = mqRs(mqVolts(L, pin)) / r0;
  double ppm = A[g] * pow(ratio, Bx[g]);
  if (ppm > 10000) ppm = 10000;
  lua_pushinteger(L, (lua_Integer)floor(ppm + 0.5));
  return 1;
}

// ---- PIR motion sensor (HC-SR501, AM312, ...): output HIGH while motion is seen.
// pir.motion(pin) -> true/false. The input gets a pull-down so a loose wire reads "no motion".
static int l_pir_motion(lua_State *L) {
  int pin = checkPin(L, 1);
  if (!(cfgPins & (1ULL << pin))) {
    pwmRelease(pin);
    pinMode(pin, pin >= 34 ? INPUT : INPUT_PULLDOWN);   // 34-39 have no internal pull-down
    cfgPins |= 1ULL << pin;
  }
  lua_pushboolean(L, digitalRead(pin) == HIGH);
  return 1;
}

static int l_map(lua_State *L) {
  double x = luaL_checknumber(L, 1), a = luaL_checknumber(L, 2), b = luaL_checknumber(L, 3);
  double c = luaL_checknumber(L, 4), d = luaL_checknumber(L, 5);
  lua_pushnumber(L, b == a ? c : (x - a) * (d - c) / (b - a) + c);
  return 1;
}

// pcall/xpcall that cannot swallow a Stop: same as lbaselib.c, but when stopReq is set
// the error is re-raised so `while true do pcall(f) end` still stops.
static int finishPcall(lua_State *L, int status, lua_KContext extra) {
  if (status != LUA_OK && status != LUA_YIELD) {
    if (stopReq) return lua_error(L);
    lua_pushboolean(L, 0);
    lua_pushvalue(L, -2);
    return 2;
  }
  return lua_gettop(L) - (int)extra;
}

static int l_pcall(lua_State *L) {
  luaL_checkany(L, 1);
  lua_pushboolean(L, 1);
  lua_insert(L, 1);
  int status = lua_pcallk(L, lua_gettop(L) - 2, LUA_MULTRET, 0, 0, finishPcall);
  return finishPcall(L, status, 0);
}

static int l_xpcall(lua_State *L) {
  int n = lua_gettop(L);
  luaL_checktype(L, 2, LUA_TFUNCTION);
  lua_pushboolean(L, 1);
  lua_pushvalue(L, 1);
  lua_rotate(L, 3, 2);
  int status = lua_pcallk(L, n - 2, LUA_MULTRET, 2, 2, finishPcall);
  return finishPcall(L, status, 2);
}

// Library calls (sensors, displays, motors ...) can block for 20 ms - 2 s. The VM hook only
// runs every 1000 instructions, so a loop around such a call could take minutes to notice
// Stop. Every C function in the library tables is therefore wrapped: Stop is checked first.
static int stopWrapper(lua_State *L) {
  checkStop(L);
  return lua_tocfunction(L, lua_upvalueindex(1))(L);
}

static void wrapLib(lua_State *L, const char *name) {
  lua_getglobal(L, name);
  if (!lua_istable(L, -1)) { lua_pop(L, 1); return; }
  lua_pushnil(L);                                      // [table, nil]
  while (lua_next(L, -2)) {                            // [table, key, value]
    if (lua_iscfunction(L, -1)) {
      lua_CFunction f = lua_tocfunction(L, -1);
      lua_pop(L, 1);                                   // [table, key]
      lua_pushvalue(L, -1);                            // [table, key, key]
      lua_pushcfunction(L, f);
      lua_pushcclosure(L, stopWrapper, 1);             // [table, key, key, closure]
      lua_settable(L, -4);                             // table[key] = closure -> [table, key]
    } else {
      lua_pop(L, 1);
    }
  }
  lua_pop(L, 1);                                       // table
}

static int openLibs(lua_State *L) {
  static const luaL_Reg libs[] = {
    {LUA_GNAME, luaopen_base}, {LUA_TABLIBNAME, luaopen_table}, {LUA_STRLIBNAME, luaopen_string},
    {LUA_MATHLIBNAME, luaopen_math}, {LUA_COLIBNAME, luaopen_coroutine}, {LUA_UTF8LIBNAME, luaopen_utf8},
    {NULL, NULL}};
  for (const luaL_Reg *l = libs; l->func; l++) { luaL_requiref(L, l->name, l->func, 1); lua_pop(L, 1); }
  // remove file access from base lib
  lua_pushnil(L); lua_setglobal(L, "dofile");
  lua_pushnil(L); lua_setglobal(L, "loadfile");

  static const luaL_Reg fns[] = {
    {"print", l_print}, {"clear", l_clear}, {"show", l_show}, {"delay", l_delay},
    {"millis", l_millis}, {"micros", l_micros}, {"pinMode", l_pinMode},
    {"digitalWrite", l_digitalWrite}, {"digitalRead", l_digitalRead},
    {"analogRead", l_analogRead}, {"analogWrite", l_analogWrite},
    {"touchRead", l_touchRead}, {"map", l_map}, {"round", l_round},
    {"tone", l_tone}, {"noTone", l_noTone},
    {"pcall", l_pcall}, {"xpcall", l_xpcall}, {NULL, NULL}};
  for (const luaL_Reg *f = fns; f->func; f++) lua_register(L, f->name, f->func);

  struct { const char *n; int v; } consts[] = {
    {"HIGH", 1}, {"LOW", 0}, {"OUTPUT", OUTPUT}, {"INPUT", INPUT},
    {"INPUT_PULLUP", INPUT_PULLUP}, {"INPUT_PULLDOWN", INPUT_PULLDOWN}, {"LED_BUILTIN", 2}};
  for (auto &c : consts) { lua_pushinteger(L, c.v); lua_setglobal(L, c.n); }
  static const luaL_Reg usF[] = {{"distance", l_us_distance}, {NULL, NULL}};
  lua_newtable(L); luaL_setfuncs(L, usF, 0); lua_setglobal(L, "ultrasonic");
  static const luaL_Reg dhtF[] = {{"temperature", l_dht_t}, {"humidity", l_dht_h}, {NULL, NULL}};
  lua_newtable(L); luaL_setfuncs(L, dhtF, 0); lua_setglobal(L, "dht");
  static const luaL_Reg servoF[] = {{"write", l_servo_write}, {NULL, NULL}};
  lua_newtable(L); luaL_setfuncs(L, servoF, 0); lua_setglobal(L, "servo");
  static const luaL_Reg sensF[] = {{"raw", l_sensor_raw}, {"percent", l_sensor_percent}, {NULL, NULL}};
  lua_newtable(L); luaL_setfuncs(L, sensF, 0); lua_setglobal(L, "sensor");
  static const luaL_Reg soilF[] = {{"moisture", l_soil}, {NULL, NULL}};
  lua_newtable(L); luaL_setfuncs(L, soilF, 0); lua_setglobal(L, "soil");
  static const luaL_Reg lightF[] = {{"level", l_light}, {NULL, NULL}};
  lua_newtable(L); luaL_setfuncs(L, lightF, 0); lua_setglobal(L, "light");
  static const luaL_Reg rainF[] = {{"level", l_rain}, {NULL, NULL}};
  lua_newtable(L); luaL_setfuncs(L, rainF, 0); lua_setglobal(L, "rain");
  static const luaL_Reg waterF[] = {{"level", l_water}, {NULL, NULL}};
  lua_newtable(L); luaL_setfuncs(L, waterF, 0); lua_setglobal(L, "water");
  static const luaL_Reg relayF[] = {{"set", l_relay_set}, {NULL, NULL}};
  lua_newtable(L); luaL_setfuncs(L, relayF, 0); lua_setglobal(L, "relay");
  static const luaL_Reg motorF[] = {{"run", l_motor_run}, {"stop", l_motor_stop}, {NULL, NULL}};
  lua_newtable(L); luaL_setfuncs(L, motorF, 0); lua_setglobal(L, "motor");
  static const luaL_Reg mqF[] = {{"voltage", l_mq_voltage}, {"level", l_mq_level}, {"calibrate", l_mq_calibrate},
    {"ppm", l_mq_ppm}, {"config", l_mq_config}, {NULL, NULL}};
  lua_newtable(L); luaL_setfuncs(L, mqF, 0); lua_setglobal(L, "mq2");
  static const luaL_Reg pirF[] = {{"motion", l_pir_motion}, {NULL, NULL}};
  lua_newtable(L); luaL_setfuncs(L, pirF, 0); lua_setglobal(L, "pir");
  static const luaL_Reg dsF[] = {{"temperature", l_ds_t}, {"count", l_ds_count}, {NULL, NULL}};
  lua_newtable(L); luaL_setfuncs(L, dsF, 0); lua_setglobal(L, "ds18b20");
  i2cOpenLib(L);
  for (const char *n : {"i2c", "aht20", "sht31", "bme280", "bh1750", "mpu6050", "oled", "lcd", "ultrasonic", "dht",
                        "ds18b20", "pir", "soil", "light", "sensor", "rain", "water", "relay", "motor", "servo", "mq2"})
    wrapLib(L, n);
  return 0;
}

static int luaPanic(lua_State *L) {
  const char *m = lua_tostring(L, -1);
  Serial.printf("Lua panic: %s\n", m ? m : "?");
  return 0;  // abort() follows; only reachable on a bug, all script code runs under pcall
}

static void runScript(const String &code) {
  lua_State *L = lua_newstate(limAlloc, NULL);
  if (!L) { logAdd(1, "✖ หน่วยความจำไม่พอ"); return; }
  lua_atpanic(L, luaPanic);
  lua_pushcfunction(L, openLibs);
  if (lua_pcall(L, 0, 0, 0) != LUA_OK) { lua_close(L); logAdd(1, "✖ หน่วยความจำไม่พอ"); return; }
  lua_sethook(L, hook, LUA_MASKCOUNT, 1000);

  xSemaphoreTake(logMux, portMAX_DELAY);
  liveVals.clear(); valsSeq++;
  xSemaphoreGive(logMux);

  running = true;
  stopReq = false;
  lastYield = millis();
  uint32_t t0 = millis();
  logAdd(2, "▶ เริ่มทำงาน");

  int rc = luaL_loadbuffer(L, code.c_str(), code.length(), "=โปรแกรม");
  if (rc == LUA_OK) rc = lua_pcall(L, 0, 0, 0);

  if (rc != LUA_OK) {
    const char *msg = lua_tostring(L, -1);
    String m = msg ? msg : (lua_isnil(L, -1) ? "error(nil)" : String("error: ") + luaL_tolstring(L, -1, NULL));
    // stopReq set: Stop was pressed (an error raised at that moment counts as the stop)
    if (m.indexOf("__STOP__") >= 0 || stopReq) logAdd(2, "■ หยุดแล้ว");
    else if (rc == LUA_ERRMEM) logAdd(1, "✖ หน่วยความจำไม่พอ (ข้อมูลใหญ่เกิน หรือฟังก์ชันเรียกตัวเองไม่รู้จบ)");
    else logAdd(1, "✖ " + m);
  } else {
    logAdd(2, "✔ จบการทำงาน (" + String(millis() - t0) + " ms)");
  }
  lua_close(L);
  toneSilenceAll();
  if (rc != LUA_OK) actuatorsSafe();      // stopped or failed: motors stop, relays off
  i2cFlush();
  running = false;
}

static void luaTask(void *) {
  for (;;) {
    if (hasPending) {
      xSemaphoreTake(logMux, portMAX_DELAY);
      String code = pendingCode; pendingCode = ""; hasPending = false;
      xSemaphoreGive(logMux);
      actuatorsReset();
      pwmReleaseAll();
      mqReset();
      i2cRunStart();
      runScript(code);
    } else {
      vTaskDelay(pdMS_TO_TICKS(20));
    }
  }
}

static void startCode(const String &code) {
  xSemaphoreTake(logMux, portMAX_DELAY);
  pendingCode = code; hasPending = true;
  xSemaphoreGive(logMux);
  if (running) stopReq = true;
}

// ---------------------------------------------------------------- files
static bool validName(const String &n) {
  if (n.length() == 0 || n.length() > 24) return false;
  for (char c : n) if (!(isalnum(c) || c == '_' || c == '-')) return false;
  return true;
}

static String readFile(const String &path) {
  File f = LittleFS.open(path, "r");
  if (!f) return "";
  String s = f.readString(); f.close();
  return s;
}

static size_t fsWritable();
static bool writeFile(const String &path, const String &data) {
  if (data.length() > fsWritable()) return false;
  File f = LittleFS.open(path, "w");
  if (!f) return false;
  size_t n = f.print(data); f.close();
  return n == data.length();
}

// ---------------------------------------------------------------- web handlers
static void sendAsset(const uint8_t *data, size_t len, const char *type, bool cache) {
  server.sendHeader("Content-Encoding", "gzip");
  server.sendHeader("Cache-Control", cache ? "max-age=86400" : "no-cache");
  server.send_P(200, type, (const char *)data, len);
}

static void handleLog() {
  uint32_t since = server.arg("since").toInt();
  uint32_t vs = server.arg("vs").toInt();
  String out; out.reserve(2048);
  xSemaphoreTake(logMux, portMAX_DELAY);
  out += "{\"boot\":"; out += bootId;
  out += ",\"seq\":"; out += logSeq;
  out += ",\"clear\":"; out += clearSeq;
  out += ",\"running\":"; out += running ? "true" : "false";
  out += ",\"lines\":[";
  bool first = true; int cnt = 0;
  for (auto &l : logBuf) {
    if (l.seq <= since) continue;
    if (cnt++ >= 150) break;
    if (!first) out += ','; first = false;
    out += "{\"s\":"; out += l.seq; out += ",\"t\":"; out += l.type; out += ",\"m\":"; jsonEsc(out, l.text); out += '}';
  }
  out += "],\"vs\":"; out += valsSeq;
  if (vs != valsSeq) {
    out += ",\"vals\":{";
    first = true;
    for (auto &kv : liveVals) {
      if (!first) out += ','; first = false;
      jsonEsc(out, kv.first); out += ':'; jsonEsc(out, kv.second);
    }
    out += '}';
  }
  out += '}';
  xSemaphoreGive(logMux);
  server.send(200, "application/json", out);
}

static void handleInfo() {
  String out = "{\"ver\":\"" FW_VERSION "\",\"ip\":\"";
  out += (WiFi.getMode() & WIFI_STA) && WiFi.isConnected() ? WiFi.localIP().toString() : WiFi.softAPIP().toString();
  out += "\",\"mode\":\""; out += WiFi.isConnected() ? "sta" : "ap";
  out += "\",\"ssid\":"; jsonEsc(out, WiFi.isConnected() ? WiFi.SSID() : String(AP_SSID));
  out += ",\"saved\":"; jsonEsc(out, prefs.getString("ssid", WIFI_SSID));   // WiFi the board tries at boot
  out += ",\"heap\":"; out += ESP.getFreeHeap();
  out += ",\"autorun\":"; out += LittleFS.exists(AUTORUN_FILE) ? "true" : "false";
  out += ",\"fs\":"; out += (uint32_t)(LittleFS.totalBytes() - LittleFS.usedBytes());
  out += '}';
  server.send(200, "application/json", out);
}

static void handleFiles() {
  String out = "[";
  File dir = LittleFS.open("/p");
  bool first = true;
  if (dir && dir.isDirectory()) {
    for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
      String n = f.name();
      int slash = n.lastIndexOf('/'); if (slash >= 0) n = n.substring(slash + 1);
      if (n.endsWith(".lua")) {
        if (!first) out += ','; first = false;
        jsonEsc(out, n.substring(0, n.length() - 4));
      }
    }
  }
  out += ']';
  server.send(200, "application/json", out);
}

// /api/file?name=x&kind=lua|blk   (name "__autorun" = the autorun program)
//   GET reads, POST (multipart) writes, DELETE removes both .lua and .blk
static const size_t FILE_MAX = 65536;

static String filePath(const String &name, const String &kind) {
  if (name == "__autorun") return kind == "lua" ? String(AUTORUN_FILE) : String("/autorun.blk");
  if (!validName(name)) return "";
  return "/p/" + name + "." + kind;
}
static String argKind() { return server.arg("kind") == "blk" ? "blk" : "lua"; }
static void sendText(int code, const String &msg) { server.send(code, "text/plain; charset=utf-8", msg); }
static const char *BAD_NAME = "ชื่อไฟล์ใช้ได้เฉพาะ a-z 0-9 _ - ยาวไม่เกิน 24 ตัว";

static void handleFile() {
  String name = server.arg("name");
  String path = filePath(name, argKind());
  if (!path.length()) { sendText(400, BAD_NAME); return; }
  if (server.method() == HTTP_GET) {
    File f = LittleFS.open(path, "r");
    if (!f) { sendText(404, "ไม่พบไฟล์"); return; }
    server.streamFile(f, "text/plain; charset=utf-8");
    f.close();
  } else if (server.method() == HTTP_DELETE) {
    String base = name == "__autorun" ? "/autorun" : "/p/" + name;
    LittleFS.remove(base + ".lua"); LittleFS.remove(base + ".blk");
    sendText(200, "ok");
  } else {
    sendText(405, "method not allowed");
  }
}

// POST /api/file?name=x&kind=lua|blk  as multipart/form-data (one file field).
// The upload is streamed into a temp file (no RAM copy), then renamed over the old file,
// so a failed or oversized upload never destroys the saved program.
// (Multipart, not a raw text body: this core's raw-body path ignores the query string and
// waits out a 5 s read timeout on the last chunk.)
static File upFile;
static String upPath, upErr;
static int upCode;
static size_t upSize, upLimit;
static bool upSeen;

// LittleFS (this core's build) can crash inside its block allocator when the partition
// fills up mid-write, so never write closer than FS_RESERVE bytes to full.
static const size_t FS_RESERVE = 12288;
static size_t fsWritable() {
  size_t fr = LittleFS.totalBytes() - LittleFS.usedBytes();
  return fr > FS_RESERVE ? fr - FS_RESERVE : 0;
}
static const char *FS_FULL = "พื้นที่บนบอร์ดเต็ม ลบโปรแกรมที่ไม่ใช้ก่อน";

static void fileUpload(HTTPUpload &u) {
  if (u.status == UPLOAD_FILE_START) {
    upSeen = true; upErr = ""; upCode = 200; upSize = 0; upLimit = fsWritable();
    upPath = filePath(server.arg("name"), argKind());
    if (!upPath.length()) { upErr = BAD_NAME; upCode = 400; return; }
    if (!LittleFS.exists("/p")) LittleFS.mkdir("/p");
    upFile = LittleFS.open(upPath + ".tmp", "w");
    if (!upFile) { upErr = "บันทึกไม่สำเร็จ (เปิดไฟล์ไม่ได้)"; upCode = 500; }
    return;
  }
  if (upErr.length()) return;                     // keep draining the body, ignore it
  if (u.status == UPLOAD_FILE_WRITE) {
    upSize += u.currentSize;
    if (upSize > FILE_MAX) { upErr = "ไฟล์ใหญ่เกิน 64KB"; upCode = 413; }
    else if (upSize > upLimit) { upErr = FS_FULL; upCode = 507; }
    else if (upFile.write(u.buf, u.currentSize) != u.currentSize) { upErr = "บันทึกไม่สำเร็จ (พื้นที่เต็ม?)"; upCode = 507; }
    if (upErr.length()) { upFile.close(); LittleFS.remove(upPath + ".tmp"); }
  } else if (u.status == UPLOAD_FILE_END) {
    upFile.close();
    LittleFS.remove(upPath);
    if (!LittleFS.rename(upPath + ".tmp", upPath)) { upErr = "บันทึกไม่สำเร็จ"; upCode = 500; }
  } else {                                        // UPLOAD_FILE_ABORTED
    upFile.close(); LittleFS.remove(upPath + ".tmp");
    upErr = "ส่งข้อมูลไม่ครบ"; upCode = 400;
  }
}

// Own handler so a non-multipart POST can never reach the upload callback
// (canRaw stays false; such a request is answered with 400 instead).
class FileUploadHandler : public RequestHandler {
 public:
  bool canHandle(HTTPMethod m, String uri) override { return m == HTTP_POST && uri == "/api/file"; }
  bool canUpload(String uri) override { return uri == "/api/file"; }
  bool handle(WebServer &, HTTPMethod m, String uri) override {
    if (!canHandle(m, uri)) return false;
    if (!upSeen) sendText(400, "ส่งไฟล์แบบ multipart/form-data");
    else sendText(upCode, upErr.length() ? upErr : String("ok"));
    upSeen = false;
    return true;
  }
  void upload(WebServer &, String, HTTPUpload &u) override { fileUpload(u); }
};

// POST /api/wifi  form ssid, pass   (empty ssid = forget, start in AP mode)
static void handleWifi() {
  String ssid = server.arg("ssid"), pass = server.arg("pass");
  ssid.trim();
  if (ssid.length() > 32) { server.send(400, "text/plain; charset=utf-8", "ชื่อ WiFi ยาวเกิน 32 ตัวอักษร"); return; }
  if (pass.length() && (pass.length() < 8 || pass.length() > 63)) {
    server.send(400, "text/plain; charset=utf-8", "รหัส WiFi ต้องยาว 8-63 ตัวอักษร (หรือเว้นว่างถ้าไม่มีรหัส)"); return;
  }
  if (ssid.length()) { prefs.putString("ssid", ssid); prefs.putString("pass", pass); }
  else { prefs.remove("ssid"); prefs.remove("pass"); }
  server.send(200, "text/plain", "ok");
  delay(500);
  ESP.restart();
}

// GET /api/scan -> [{"ssid":"..","rssi":-50,"lock":true}, ...] strongest first, no duplicates
static void handleScan() {
  int n = WiFi.scanNetworks();
  String out = "[";
  std::vector<String> seen;
  for (int i = 0; i < n; i++) {
    String s = WiFi.SSID(i);
    if (!s.length() || std::find(seen.begin(), seen.end(), s) != seen.end()) continue;
    seen.push_back(s);
    if (out.length() > 1) out += ',';
    out += "{\"ssid\":"; jsonEsc(out, s);
    out += ",\"rssi\":"; out += WiFi.RSSI(i);
    out += ",\"lock\":"; out += WiFi.encryptionType(i) == WIFI_AUTH_OPEN ? "false" : "true";
    out += '}';
  }
  out += ']';
  WiFi.scanDelete();
  server.send(200, "application/json", out);
}

static void setupRoutes() {
  server.on("/", HTTP_GET, [] { sendAsset(INDEX_HTML_GZ, INDEX_HTML_GZ_LEN, "text/html; charset=utf-8", false); });
#if HAS_BLOCKLY
  server.on("/blockly.js", HTTP_GET, [] { sendAsset(BLOCKLY_JS_GZ, BLOCKLY_JS_GZ_LEN, "application/javascript", true); });
#endif
  for (size_t i = 0; i < MEDIA_COUNT; i++) {
    server.on(String("/media/") + MEDIA[i].name, HTTP_GET, [i] { sendAsset(MEDIA[i].data, MEDIA[i].len, MEDIA[i].type, true); });
  }
  server.on("/api/run", HTTP_POST, [] { startCode(server.arg("plain")); server.send(200, "text/plain", "ok"); });
  server.on("/api/stop", HTTP_POST, [] { if (running) stopReq = true; server.send(200, "text/plain", "ok"); });
  server.on("/api/log", HTTP_GET, handleLog);
  server.on("/api/info", HTTP_GET, handleInfo);
  server.on("/api/files", HTTP_GET, handleFiles);
  server.addHandler(new FileUploadHandler());          // POST /api/file, must be before the HTTP_ANY route
  server.on("/api/file", HTTP_ANY, handleFile);
  server.on("/api/wifi", HTTP_POST, handleWifi);
  server.on("/api/scan", HTTP_GET, handleScan);
  server.on("/api/reboot", HTTP_POST, [] { server.send(200, "text/plain", "ok"); delay(300); ESP.restart(); });
  server.onNotFound([] { server.send(404, "text/plain", "not found"); });
}

// Remove what an interrupted upload (power loss, reset) can leave behind:
// *.tmp files, and .blk files whose .lua is gone. None of these show up in /api/files.
static void cleanupFiles() {
  std::vector<String> junk;
  for (const char *dirName : {"/", "/p"}) {
    File dir = LittleFS.open(dirName);
    if (!dir || !dir.isDirectory()) continue;
    String base = String(dirName) == "/" ? "/" : String(dirName) + "/";
    for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
      String n = f.name();
      int slash = n.lastIndexOf('/'); if (slash >= 0) n = n.substring(slash + 1);
      String path = base + n;
      if (n.endsWith(".tmp")) junk.push_back(path);
      else if (n.endsWith(".blk") && !LittleFS.exists(path.substring(0, path.length() - 4) + ".lua")) junk.push_back(path);
    }
  }
  for (auto &p : junk) { LittleFS.remove(p); Serial.println("cleanup: removed " + p); }
}

// ---------------------------------------------------------------- WiFi
static void setupWifi() {
  String ssid = prefs.getString("ssid", WIFI_SSID);
  String pass = prefs.getString("pass", WIFI_PASS);
  if (ssid.length()) {
    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid.c_str(), pass.c_str());
    Serial.printf("Connecting to %s", ssid.c_str());
    uint32_t t0 = millis();
    while (!WiFi.isConnected() && millis() - t0 < 15000) { delay(300); Serial.print('.'); }
    Serial.println();
  }
  if (WiFi.isConnected()) {
    logAdd(2, "WiFi: " + ssid + "  เปิด http://" + WiFi.localIP().toString() + " หรือ http://" MDNS_NAME ".local");
  } else {
    WiFi.disconnect(true);
    WiFi.mode(WIFI_AP_STA);          // STA side stays idle; it is only used by /api/scan
    WiFi.softAP(AP_SSID, AP_PASS);
    logAdd(2, "โหมด AP: ต่อ WiFi \"" AP_SSID "\" (รหัส " AP_PASS ") แล้วเปิด http://" + WiFi.softAPIP().toString());
  }
  MDNS.begin(MDNS_NAME);
  MDNS.addService("http", "tcp", 80);
}

void setup() {
  Serial.begin(115200);
  delay(200);
  logMux = xSemaphoreCreateMutex();
  bootId = esp_random() & 0x7fffffff;
  memset(pwmChan, -1, sizeof pwmChan);
  if (!LittleFS.begin(true)) logAdd(1, "LittleFS ใช้งานไม่ได้");
  cleanupFiles();
  prefs.begin("livecode", false);
  setupWifi();
  setupRoutes();
  server.begin();
  xTaskCreatePinnedToCore(luaTask, "lua", 16384, NULL, 1, NULL, 1);

  if (LittleFS.exists(AUTORUN_FILE)) {
    esp_reset_reason_t r = esp_reset_reason();
    if (r == ESP_RST_PANIC || r == ESP_RST_INT_WDT || r == ESP_RST_TASK_WDT || r == ESP_RST_WDT) {
      logAdd(1, "ข้าม autorun เพราะบอร์ดรีเซ็ตจากความผิดพลาด (รหัส " + String((int)r) + ")");
    } else {
      logAdd(2, "รันโปรแกรมอัตโนมัติ (autorun)");
      startCode(readFile(AUTORUN_FILE));
    }
  }
}

// Serial console: type Lua lines, then a line "--run" to run them, "--stop" to stop,
// "--autorun" to save them as the autorun program, "--noautorun", "--reboot".
// Handy for testing over USB without joining the board's WiFi.
static void handleSerial() {
  static String line, code;
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\r') continue;
    if (c != '\n') { if (line.length() < 512) line += c; continue; }
    if (line == "--run") { startCode(code); code = ""; }
    else if (line == "--stop") { if (running) stopReq = true; code = ""; }
    else if (line == "--autorun") { Serial.println(writeFile(AUTORUN_FILE, code) ? "autorun saved" : "autorun save failed"); code = ""; }
    else if (line == "--noautorun") { LittleFS.remove(AUTORUN_FILE); LittleFS.remove("/autorun.blk"); Serial.println("autorun removed"); code = ""; }
    else if (line == "--reboot") { ESP.restart(); }
    else if (line.startsWith("--oledtest ")) { oledTestDump(line.substring(11), 1, 64); code = ""; }
    else if (line.startsWith("--oledtest32 ")) { oledTestDump(line.substring(13), 1, 32); code = ""; }
    else if (code.length() < 8192) { code += line; code += '\n'; }
    line = "";
  }
}

void loop() {
  server.handleClient();
  handleSerial();
  delay(1);
}

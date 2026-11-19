// I2C bus + sensor drivers + SSD1306 OLED for Lua.
// All calls run on the Lua task only, so no locking is needed.
// Sensors are initialised on first use and again after any failed read (hot-plug friendly).
#include <Arduino.h>
#include <Wire.h>
#include <math.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include "font_thai.h"
#include "i2c_dev.h"

extern "C" {
#include "lauxlib.h"
}

// ---------------------------------------------------------------- bus
static int busSda = 21, busScl = 22;
static uint32_t busFreq = 400000;
static bool busUp = false;
static uint32_t busGen = 1;            // bumps when the bus is restarted; sensors re-init

static void busEnsure() {
  if (busUp) return;
  Wire.begin(busSda, busScl, busFreq);
  Wire.setTimeOut(50);
  busUp = true;
}

static bool present(uint8_t a) {
  busEnsure();
  Wire.beginTransmission(a);
  return Wire.endTransmission() == 0;
}

static bool writeBytes(uint8_t a, const uint8_t *d, size_t n) {
  busEnsure();
  Wire.beginTransmission(a);
  Wire.write(d, n);
  return Wire.endTransmission() == 0;
}

static bool writeReg(uint8_t a, uint8_t reg, uint8_t v) {
  uint8_t b[2] = {reg, v};
  return writeBytes(a, b, 2);
}

static bool readBytes(uint8_t a, uint8_t *d, size_t n) {
  busEnsure();
  if (Wire.requestFrom((int)a, (int)n) != (int)n) return false;
  for (size_t i = 0; i < n; i++) d[i] = Wire.read();
  return true;
}

static bool readRegs(uint8_t a, uint8_t reg, uint8_t *d, size_t n) {
  busEnsure();
  Wire.beginTransmission(a);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  return readBytes(a, d, n);
}

static const char *WIRING = "ตรวจสาย SDA=GPIO%d SCL=GPIO%d และไฟเลี้ยง %s";

static int notFound(lua_State *L, const char *name, int addr, const char *vcc = "3.3V") {
  char hex[8]; snprintf(hex, sizeof hex, "0x%02X", addr);
  luaL_where(L, 1);                                    // "โปรแกรม:12: " like luaL_error
  lua_pushfstring(L, "ไม่พบ %s (I2C %s) — ", name, hex);
  lua_pushfstring(L, WIRING, busSda, busScl, vcc);
  lua_concat(L, 3);
  return lua_error(L);
}

// ---------------------------------------------------------------- i2c.*
static int checkAddr(lua_State *L, int i) {
  lua_Integer a = luaL_checkinteger(L, i);
  if (a < 1 || a > 127) luaL_error(L, "ที่อยู่ I2C ต้องอยู่ระหว่าง 1-127 (0x01-0x7F)");
  return (int)a;
}

static int l_i2c_begin(lua_State *L) {
  int sda = (int)luaL_optinteger(L, 1, 21), scl = (int)luaL_optinteger(L, 2, 22);
  uint32_t f = (uint32_t)luaL_optinteger(L, 3, 400000);
  for (int p : {sda, scl})
    if (p < 0 || p > 33 || (p >= 6 && p <= 11)) luaL_error(L, "ขา %d ใช้เป็น I2C ไม่ได้", p);
  if (f < 10000 || f > 1000000) luaL_error(L, "ความเร็ว I2C ต้องอยู่ระหว่าง 10000-1000000");
  if (busUp) Wire.end();
  busSda = sda; busScl = scl; busFreq = f; busUp = false; busGen++;
  busEnsure();
  return 0;
}

static const char *knownName(int a) {
  switch (a) {
    case 0x3C: case 0x3D: return "จอ OLED";
    case 0x38: return "AHT10/AHT20";
    case 0x44: case 0x45: return "SHT3x";
    case 0x76: case 0x77: return "BME280/BMP280";
    case 0x23: case 0x5C: return "BH1750";
    case 0x68: return "MPU6050 หรือ DS3231";
    case 0x69: return "MPU6050";
    case 0x27: case 0x3F: return "จอ LCD (PCF8574)";
    case 0x57: return "EEPROM (บนโมดูล DS3231)";
    case 0x40: return "INA219 / HTU21D / PCA9685";
    case 0x48: return "ADS1115 / PCF8591";
    default: return nullptr;
  }
}

extern void logPrint(const String &s);   // main.cpp: prints to the web console

// i2c.scan([print]) -> {addr, ...}; with print=true also lists them on the console
static int l_i2c_scan(lua_State *L) {
  bool say = lua_toboolean(L, 1);
  busEnsure();
  lua_newtable(L);
  String line;
  int n = 0;
  for (int a = 1; a < 127; a++) {
    if (!present(a)) continue;
    lua_pushinteger(L, a);
    lua_rawseti(L, -2, ++n);
    char buf[8]; snprintf(buf, sizeof buf, "0x%02X", a);
    if (line.length()) line += ", ";
    line += buf;
    if (const char *k = knownName(a)) { line += " ("; line += k; line += ")"; }
  }
  if (say) {
    char buf[64]; snprintf(buf, sizeof buf, "SDA=%d SCL=%d: ", busSda, busScl);
    logPrint(n ? "พบอุปกรณ์ I2C " + String(n) + " ตัว — " + line : String("ไม่พบอุปกรณ์ I2C — ") + buf + "ตรวจสายและไฟเลี้ยง");
  }
  return 1;
}

// i2c.write(addr, b1, b2, ...) -> true/false
static int l_i2c_write(lua_State *L) {
  int a = checkAddr(L, 1), n = lua_gettop(L) - 1;
  if (n > 64) luaL_error(L, "ส่งได้ครั้งละไม่เกิน 64 ไบต์");
  uint8_t b[64];
  for (int i = 0; i < n; i++) b[i] = (uint8_t)luaL_checkinteger(L, i + 2);
  lua_pushboolean(L, writeBytes(a, b, n));
  return 1;
}

// i2c.read(addr, n) -> {bytes} or nil
static int l_i2c_read(lua_State *L) {
  int a = checkAddr(L, 1), n = (int)luaL_checkinteger(L, 2);
  if (n < 1 || n > 64) luaL_error(L, "อ่านได้ครั้งละ 1-64 ไบต์");
  uint8_t b[64];
  if (!readBytes(a, b, n)) { lua_pushnil(L); return 1; }
  lua_createtable(L, n, 0);
  for (int i = 0; i < n; i++) { lua_pushinteger(L, b[i]); lua_rawseti(L, -2, i + 1); }
  return 1;
}

// i2c.readReg(addr, reg [, n]) -> byte (n=1) or {bytes}; error if the device does not answer
static int l_i2c_readReg(lua_State *L) {
  int a = checkAddr(L, 1), reg = (int)luaL_checkinteger(L, 2), n = (int)luaL_optinteger(L, 3, 1);
  if (n < 1 || n > 64) luaL_error(L, "อ่านได้ครั้งละ 1-64 ไบต์");
  uint8_t b[64];
  if (!readRegs(a, (uint8_t)reg, b, n)) return notFound(L, "อุปกรณ์", a);
  if (n == 1) { lua_pushinteger(L, b[0]); return 1; }
  lua_createtable(L, n, 0);
  for (int i = 0; i < n; i++) { lua_pushinteger(L, b[i]); lua_rawseti(L, -2, i + 1); }
  return 1;
}

static int l_i2c_writeReg(lua_State *L) {
  int a = checkAddr(L, 1), reg = (int)luaL_checkinteger(L, 2), v = (int)luaL_checkinteger(L, 3);
  if (!writeReg(a, (uint8_t)reg, (uint8_t)v)) return notFound(L, "อุปกรณ์", a);
  return 0;
}

// ---------------------------------------------------------------- shared sensor state
struct Sensor {
  uint8_t addr = 0;          // 0 = not initialised
  uint32_t gen = 0;          // bus generation it was initialised on
  uint32_t at = 0;           // millis() of the cached reading
  bool valid = false;
  float v[7];
};

static bool fresh(Sensor &s, uint32_t maxAge) {
  return s.addr && s.gen == busGen && s.valid && millis() - s.at < maxAge;
}
static void fail(Sensor &s) { s.addr = 0; s.valid = false; }

// ---------------------------------------------------------------- AHT10 / AHT20 (0x38)
static Sensor aht;

static bool ahtRead() {
  if (fresh(aht, 250)) return true;
  if (!aht.addr || aht.gen != busGen) {
    if (!present(0x38)) return false;
    uint8_t st = 0;
    readBytes(0x38, &st, 1);
    if (!(st & 0x08)) {                              // not calibrated: AHT20 / AHT10 init command
      const uint8_t c20[3] = {0xBE, 0x08, 0x00}, c10[3] = {0xE1, 0x08, 0x00};
      if (!writeBytes(0x38, c20, 3)) writeBytes(0x38, c10, 3);
      delay(10);
    }
    aht.addr = 0x38; aht.gen = busGen;
  }
  const uint8_t trig[3] = {0xAC, 0x33, 0x00};
  if (!writeBytes(0x38, trig, 3)) { fail(aht); return false; }
  uint8_t d[6];
  uint32_t t0 = millis();
  do {
    delay(20);
    if (!readBytes(0x38, d, 6)) { fail(aht); return false; }
  } while ((d[0] & 0x80) && millis() - t0 < 200);   // busy bit
  uint32_t rh = ((uint32_t)d[1] << 12) | ((uint32_t)d[2] << 4) | (d[3] >> 4);
  uint32_t rt = (((uint32_t)d[3] & 0x0F) << 16) | ((uint32_t)d[4] << 8) | d[5];
  aht.v[0] = rt * 200.0f / 1048576.0f - 50.0f;
  aht.v[1] = rh * 100.0f / 1048576.0f;
  aht.valid = true; aht.at = millis();
  return true;
}
static int l_aht_t(lua_State *L) { if (!ahtRead()) return notFound(L, "AHT10/AHT20", 0x38); lua_pushnumber(L, aht.v[0]); return 1; }
static int l_aht_h(lua_State *L) { if (!ahtRead()) return notFound(L, "AHT10/AHT20", 0x38); lua_pushnumber(L, aht.v[1]); return 1; }

// ---------------------------------------------------------------- SHT3x (0x44 / 0x45)
static Sensor sht;

static uint8_t crc8(const uint8_t *d, int n) {
  uint8_t c = 0xFF;
  for (int i = 0; i < n; i++) {
    c ^= d[i];
    for (int b = 0; b < 8; b++) c = (c & 0x80) ? (c << 1) ^ 0x31 : c << 1;
  }
  return c;
}

static bool shtRead() {
  if (fresh(sht, 250)) return true;
  if (!sht.addr || sht.gen != busGen) {
    if (present(0x44)) sht.addr = 0x44; else if (present(0x45)) sht.addr = 0x45; else return false;
    sht.gen = busGen;
  }
  const uint8_t cmd[2] = {0x24, 0x00};                 // single shot, high repeatability
  if (!writeBytes(sht.addr, cmd, 2)) { fail(sht); return false; }
  delay(16);
  uint8_t d[6];
  if (!readBytes(sht.addr, d, 6) || crc8(d, 2) != d[2] || crc8(d + 3, 2) != d[5]) { fail(sht); return false; }
  sht.v[0] = -45.0f + 175.0f * (((uint16_t)d[0] << 8) | d[1]) / 65535.0f;
  sht.v[1] = 100.0f * (((uint16_t)d[3] << 8) | d[4]) / 65535.0f;
  sht.valid = true; sht.at = millis();
  return true;
}
static int l_sht_t(lua_State *L) { if (!shtRead()) return notFound(L, "SHT3x", 0x44); lua_pushnumber(L, sht.v[0]); return 1; }
static int l_sht_h(lua_State *L) { if (!shtRead()) return notFound(L, "SHT3x", 0x44); lua_pushnumber(L, sht.v[1]); return 1; }

// ---------------------------------------------------------------- BME280 / BMP280 (0x76 / 0x77)
static Sensor bme;
static bool bmeHasHum;
static struct {                       // Bosch calibration constants (names as in the datasheet)
  uint16_t T1, P1; int16_t T2, T3, P2, P3, P4, P5, P6, P7, P8, P9;
  uint8_t H1, H3; int16_t H2, H4, H5; int8_t H6;
} cal;

static bool bmeInit() {
  uint8_t a = present(0x76) ? 0x76 : present(0x77) ? 0x77 : 0;
  if (!a) return false;
  uint8_t id;
  if (!readRegs(a, 0xD0, &id, 1)) return false;
  if (id != 0x60 && id != 0x58 && id != 0x56 && id != 0x57) return false;
  bmeHasHum = id == 0x60;
  uint8_t c[24];
  if (!readRegs(a, 0x88, c, 24)) return false;
  auto u16 = [&](int i) { return (uint16_t)(c[i] | (c[i + 1] << 8)); };
  cal.T1 = u16(0); cal.T2 = (int16_t)u16(2); cal.T3 = (int16_t)u16(4);
  cal.P1 = u16(6); cal.P2 = (int16_t)u16(8); cal.P3 = (int16_t)u16(10); cal.P4 = (int16_t)u16(12); cal.P5 = (int16_t)u16(14);
  cal.P6 = (int16_t)u16(16); cal.P7 = (int16_t)u16(18); cal.P8 = (int16_t)u16(20); cal.P9 = (int16_t)u16(22);
  if (bmeHasHum) {
    uint8_t h[7];
    if (!readRegs(a, 0xA1, &cal.H1, 1) || !readRegs(a, 0xE1, h, 7)) return false;
    cal.H2 = (int16_t)(h[0] | (h[1] << 8));
    cal.H3 = h[2];
    cal.H4 = (int16_t)(((int8_t)h[3]) * 16) | (h[4] & 0x0F);
    cal.H5 = (int16_t)(((int8_t)h[5]) * 16) | (h[4] >> 4);
    cal.H6 = (int8_t)h[6];
    writeReg(a, 0xF2, 0x01);                              // humidity x1 (must precede ctrl_meas)
  }
  writeReg(a, 0xF5, 0x00);                                // standby 0.5 ms, filter off
  writeReg(a, 0xF4, 0x27);                                // temp x1, press x1, normal mode
  delay(15);
  bme.addr = a; bme.gen = busGen;
  return true;
}

static bool bmeRead() {
  if (fresh(bme, 100)) return true;
  if ((!bme.addr || bme.gen != busGen) && !bmeInit()) return false;
  uint8_t d[8];
  if (!readRegs(bme.addr, 0xF7, d, bmeHasHum ? 8 : 6)) { fail(bme); return false; }
  int32_t adcP = ((int32_t)d[0] << 12) | ((int32_t)d[1] << 4) | (d[2] >> 4);
  int32_t adcT = ((int32_t)d[3] << 12) | ((int32_t)d[4] << 4) | (d[5] >> 4);
  // Bosch datasheet floating point compensation
  double v1 = (adcT / 16384.0 - cal.T1 / 1024.0) * cal.T2;
  double v2 = (adcT / 131072.0 - cal.T1 / 8192.0); v2 = v2 * v2 * cal.T3;
  double tFine = v1 + v2;
  bme.v[0] = tFine / 5120.0;
  v1 = tFine / 2.0 - 64000.0;
  v2 = v1 * v1 * cal.P6 / 32768.0;
  v2 = v2 + v1 * cal.P5 * 2.0;
  v2 = v2 / 4.0 + cal.P4 * 65536.0;
  v1 = (cal.P3 * v1 * v1 / 524288.0 + cal.P2 * v1) / 524288.0;
  v1 = (1.0 + v1 / 32768.0) * cal.P1;
  double p = 0;
  if (v1 != 0) {
    p = 1048576.0 - adcP;
    p = (p - v2 / 4096.0) * 6250.0 / v1;
    v1 = cal.P9 * p * p / 2147483648.0;
    v2 = p * cal.P8 / 32768.0;
    p = p + (v1 + v2 + cal.P7) / 16.0;
  }
  bme.v[2] = p / 100.0;                                   // hPa
  if (bmeHasHum) {
    int32_t adcH = ((int32_t)d[6] << 8) | d[7];
    double h = tFine - 76800.0;
    h = (adcH - (cal.H4 * 64.0 + cal.H5 / 16384.0 * h)) *
        (cal.H2 / 65536.0 * (1.0 + cal.H6 / 67108864.0 * h * (1.0 + cal.H3 / 67108864.0 * h)));
    h = h * (1.0 - cal.H1 * h / 524288.0);
    bme.v[1] = h < 0 ? 0 : h > 100 ? 100 : h;
  }
  bme.valid = true; bme.at = millis();
  return true;
}
static int l_bme_t(lua_State *L) { if (!bmeRead()) return notFound(L, "BME280/BMP280", 0x76); lua_pushnumber(L, bme.v[0]); return 1; }
static int l_bme_p(lua_State *L) { if (!bmeRead()) return notFound(L, "BME280/BMP280", 0x76); lua_pushnumber(L, bme.v[2]); return 1; }
static int l_bme_h(lua_State *L) {
  if (!bmeRead()) return notFound(L, "BME280/BMP280", 0x76);
  if (!bmeHasHum) luaL_error(L, "เซนเซอร์นี้เป็น BMP280 วัดความชื้นไม่ได้ (BME280 ถึงจะวัดได้)");
  lua_pushnumber(L, bme.v[1]);
  return 1;
}

// ---------------------------------------------------------------- BH1750 (0x23 / 0x5C)
static Sensor bh;

static bool bhRead() {
  if (fresh(bh, 150)) return true;
  if (!bh.addr || bh.gen != busGen) {
    uint8_t a = present(0x23) ? 0x23 : present(0x5C) ? 0x5C : 0;
    if (!a) return false;
    uint8_t on = 0x01, cont = 0x10;                      // power on, continuous high-res
    if (!writeBytes(a, &on, 1) || !writeBytes(a, &cont, 1)) return false;
    delay(180);                                          // first conversion
    bh.addr = a; bh.gen = busGen;
  }
  uint8_t d[2];
  if (!readBytes(bh.addr, d, 2)) { fail(bh); return false; }
  bh.v[0] = (((uint16_t)d[0] << 8) | d[1]) / 1.2f;
  bh.valid = true; bh.at = millis();
  return true;
}
static int l_bh_lux(lua_State *L) { if (!bhRead()) return notFound(L, "BH1750", 0x23); lua_pushnumber(L, bh.v[0]); return 1; }

// ---------------------------------------------------------------- MPU6050 (0x68 / 0x69)
static Sensor mpu;   // v: ax ay az (g), gx gy gz (deg/s), temp

static bool mpuRead() {
  if (fresh(mpu, 10)) return true;
  if (!mpu.addr || mpu.gen != busGen) {
    uint8_t a = 0;
    for (uint8_t c : {0x68, 0x69}) {
      uint8_t who;
      if (present(c) && readRegs(c, 0x75, &who, 1) && who != 0x00 && who != 0xFF) { a = c; break; }
    }
    if (!a) return false;
    writeReg(a, 0x6B, 0x00);       // wake up
    writeReg(a, 0x1A, 0x03);       // DLPF ~44 Hz
    writeReg(a, 0x1B, 0x00);       // gyro +-250 deg/s
    writeReg(a, 0x1C, 0x00);       // accel +-2 g
    delay(10);
    mpu.addr = a; mpu.gen = busGen;
  }
  uint8_t d[14];
  if (!readRegs(mpu.addr, 0x3B, d, 14)) { fail(mpu); return false; }
  auto s16 = [&](int i) { return (int16_t)((d[i] << 8) | d[i + 1]); };
  for (int i = 0; i < 3; i++) mpu.v[i] = s16(i * 2) / 16384.0f;
  mpu.v[6] = s16(6) / 340.0f + 36.53f;
  for (int i = 0; i < 3; i++) mpu.v[3 + i] = s16(8 + i * 2) / 131.0f;
  mpu.valid = true; mpu.at = millis();
  return true;
}

static int axisArg(lua_State *L) {
  static const char *const axes[] = {"x", "y", "z", NULL};
  return luaL_checkoption(L, 1, NULL, axes);
}
// mpu6050.accel("x"|"y"|"z") -> g;  mpu6050.accel() -> x, y, z
static int l_mpu_accel(lua_State *L) {
  if (!mpuRead()) return notFound(L, "MPU6050", 0x68);
  if (lua_isnoneornil(L, 1)) { for (int i = 0; i < 3; i++) lua_pushnumber(L, mpu.v[i]); return 3; }
  lua_pushnumber(L, mpu.v[axisArg(L)]);
  return 1;
}
static int l_mpu_gyro(lua_State *L) {
  if (!mpuRead()) return notFound(L, "MPU6050", 0x68);
  if (lua_isnoneornil(L, 1)) { for (int i = 0; i < 3; i++) lua_pushnumber(L, mpu.v[3 + i]); return 3; }
  lua_pushnumber(L, mpu.v[3 + axisArg(L)]);
  return 1;
}
// mpu6050.angle("pitch"|"roll") -> degrees of tilt from the accelerometer
static int l_mpu_angle(lua_State *L) {
  static const char *const which[] = {"pitch", "roll", NULL};
  int w = luaL_checkoption(L, 1, NULL, which);
  if (!mpuRead()) return notFound(L, "MPU6050", 0x68);
  float ax = mpu.v[0], ay = mpu.v[1], az = mpu.v[2];
  float deg = w == 0 ? atan2f(-ax, sqrtf(ay * ay + az * az)) : atan2f(ay, az);
  lua_pushnumber(L, deg * 180.0f / (float)M_PI);
  return 1;
}
static int l_mpu_temp(lua_State *L) { if (!mpuRead()) return notFound(L, "MPU6050", 0x68); lua_pushnumber(L, mpu.v[6]); return 1; }

// ---------------------------------------------------------------- OLED SSD1306 128x64
// one object per panel height; `oled` below always means the one in use
static Adafruit_SSD1306 oled64(128, 64, &Wire, -1);   // SSD1306 0.96" and SH1106 1.3" (frame buffer)
static Adafruit_SSD1306 oled32(128, 32, &Wire, -1);   // SSD1306 0.91"
static Adafruit_SSD1306 *oledp = &oled64;
#define oled (*oledp)
static uint8_t oledAddr = 0;      // 0 = not initialised
static uint32_t oledGen = 0;
static bool oledDirty = false;
// controller: 0 = SSD1306 (0.96"), 1 = SH1106 (1.3"). Both use the same 128x64 page buffer
// (Adafruit_SSD1306 draws into it); only init and the transfer to the panel differ.
enum { OLED_SSD1306 = 0, OLED_SH1106 = 1, OLED_SSD1306_32 = 2 };
static uint8_t oledType = OLED_SSD1306, oledWant = 0;   // oledWant: address asked for (0 = auto)

static bool oledCmds(const uint8_t *c, size_t n) {     // control byte 0x00 = command stream
  Wire.beginTransmission(oledAddr);
  Wire.write((uint8_t)0x00);
  Wire.write(c, n);
  return Wire.endTransmission() == 0;
}

// SH1106 init (same values as Adafruit_SH110X's SH1106G::begin)
static bool sh1106Init() {
  static const uint8_t init[] = {
    0xAE,             // display off
    0xD5, 0x80,       // clock divide
    0xA8, 0x3F,       // multiplex 64
    0xD3, 0x00,       // display offset
    0x40,             // start line 0
    0xAD, 0x8B,       // DC-DC on
    0xA1,             // segment remap
    0xC8,             // COM scan direction: remapped
    0xDA, 0x12,       // COM pins
    0x81, 0xFF,       // contrast
    0xD9, 0x1F,       // pre-charge
    0xDB, 0x40,       // VCOM deselect level
    0x33,             // pump voltage 9 V
    0xA6,             // normal (not inverted)
    0x20, 0x10,       // memory mode
    0xA4,             // display follows RAM
  };
  if (!oledCmds(init, sizeof init)) return false;
  delay(100);
  const uint8_t on = 0xAF;
  return oledCmds(&on, 1);
}

// SH1106 has 132 columns of RAM; the 128 visible ones start at column 2
static void sh1106Display() {
  const uint8_t *buf = oled.getBuffer();
  for (uint8_t p = 0; p < 8; p++) {
    const uint8_t cmd[3] = {(uint8_t)(0xB0 + p), 0x10, 0x02};   // page p, column 2
    oledCmds(cmd, 3);
    for (int i = 0; i < 128; i += 32) {
      Wire.beginTransmission(oledAddr);
      Wire.write((uint8_t)0x40);                                 // control byte: data
      Wire.write(buf + p * 128 + i, 32);
      Wire.endTransmission();
    }
  }
}

static void oledEnsure(lua_State *L) {
  if (oledAddr && oledGen == busGen) return;
  oledAddr = 0;
  uint8_t a = oledWant ? (present(oledWant) ? oledWant : 0) : present(0x3C) ? 0x3C : present(0x3D) ? 0x3D : 0;
  if (!a) {
    notFound(L, oledType == OLED_SH1106 ? "จอ OLED SH1106" : "จอ OLED", oledWant ? oledWant : 0x3C);
    return;
  }
  if (oledType == OLED_SH1106) {
    // the SSD1306 object only provides the frame buffer here: give it an address nobody
    // answers on, so its SSD1306 init commands do not reach the SH1106
    if (!oled.getBuffer() && !oled.begin(SSD1306_SWITCHCAPVCC, 0x7F, false, false))
      luaL_error(L, "เริ่มจอ OLED ไม่สำเร็จ (หน่วยความจำไม่พอ?)");
    oledAddr = a;
    if (!sh1106Init()) { oledAddr = 0; notFound(L, "จอ OLED SH1106", a); return; }
  } else {
    // periphBegin=false: the bus is already running with our pins and speed
    if (!oled.begin(SSD1306_SWITCHCAPVCC, a, false, false)) luaL_error(L, "เริ่มจอ OLED ไม่สำเร็จ (หน่วยความจำไม่พอ?)");
  }
  Wire.setClock(busFreq);          // the library drops the bus to 100 kHz after each transaction
  oled.clearDisplay();
  oled.setTextColor(SSD1306_WHITE, SSD1306_BLACK);
  oled.setTextWrap(false);
  oledAddr = a; oledGen = busGen;
  oledDirty = true;
}

// every run starts on the default bus (SDA 21, SCL 22, 400 kHz) unless it calls i2c.begin
static void lcdReset();           // character LCD section below

void i2cRunStart() {
  lcdReset();
  if (oledType != OLED_SSD1306 || oledWant) { oledType = OLED_SSD1306; oledWant = 0; oledAddr = 0; oledp = &oled64; }
  if (busSda == 21 && busScl == 22 && busFreq == 400000) return;
  if (busUp) Wire.end();
  busSda = 21; busScl = 22; busFreq = 400000; busUp = false; busGen++;
}

void i2cFlush() {
  if (!oledDirty || !oledAddr) return;
  oledDirty = false;
  if (oledType == OLED_SH1106) sh1106Display(); else oled.display();
  Wire.setClock(busFreq);          // display() may change the clock
}

static int color(lua_State *L, int i) {          // 1/true = on (default), 0/false = off
  if (lua_isnoneornil(L, i)) return SSD1306_WHITE;
  bool on = lua_isboolean(L, i) ? lua_toboolean(L, i) : luaL_checknumber(L, i) != 0;
  return on ? SSD1306_WHITE : SSD1306_BLACK;
}
static int16_t ci(lua_State *L, int i) {
  lua_Number n = luaL_checknumber(L, i);
  return (int16_t)(n < -32000 ? -32000 : n > 32000 ? 32000 : lround(n));
}

// oled.height() -> 64 or 32 (the display chosen with oled.begin; no display needed)
static int l_oled_height(lua_State *L) { lua_pushinteger(L, oled.height()); return 1; }

static int l_oled_clear(lua_State *L) { oledEnsure(L); oled.clearDisplay(); oledDirty = true; return 0; }
static int l_oled_show(lua_State *L) { oledEnsure(L); oledDirty = true; i2cFlush(); return 0; }

// ---- Thai font (tools/make_thai_font.py -> font_thai.h), 20 px line per size step.
// Glyphs are drawn at pen + offset like TrueType: vowels/tone marks have advance 0 and a
// negative x offset, so they land on the consonant before them.
static bool isAbove(uint32_t c) { return c == 0x0E31 || (c >= 0x0E34 && c <= 0x0E37) || (c >= 0x0E47 && c <= 0x0E4E); }
static bool isTone(uint32_t c)  { return c >= 0x0E48 && c <= 0x0E4C; }
static bool isUpperVowel(uint32_t c) { return c == 0x0E31 || (c >= 0x0E34 && c <= 0x0E37) || c == 0x0E47 || c == 0x0E4D; }
static bool isTall(uint32_t c)  { return c == 0x0E1B || c == 0x0E1D || c == 0x0E1F || c == 0x0E2C; }

static const ThaiGlyph *thaiGlyph(uint32_t cp) {
  int lo = 0, hi = THAI_FONT_COUNT - 1;
  while (lo <= hi) {
    int mid = (lo + hi) / 2;
    if (THAI_FONT[mid].cp == cp) return &THAI_FONT[mid];
    if (THAI_FONT[mid].cp < cp) lo = mid + 1; else hi = mid - 1;
  }
  return cp == '?' ? nullptr : thaiGlyph('?');
}

// UTF-8 -> code points (invalid bytes become '?'); returns count
static int utf8Decode(const char *s, size_t len, uint32_t *out, int max) {
  int n = 0;
  for (size_t i = 0; i < len && n < max;) {
    uint8_t c = s[i];
    uint32_t cp; int k;
    if (c < 0x80) { cp = c; k = 1; }
    else if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; k = 2; }
    else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; k = 3; }
    else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; k = 4; }
    else { out[n++] = '?'; i++; continue; }
    if (i + k > len) { out[n++] = '?'; break; }
    for (int j = 1; j < k; j++) cp = (cp << 6) | (s[i + j] & 0x3F);
    i += k;
    if (cp < 0x20) continue;              // control characters are skipped
    out[n++] = cp;
  }
  return n;
}

// draws (when draw=true) and returns the width in pixels
static int thaiText(int16_t x, int16_t y, const uint32_t *cp, int n, int size, bool draw) {
  int pen = 0;
  bool prevUpper = false, baseTall = false;
  for (int i = 0; i < n; i++) {
    const ThaiGlyph *g = thaiGlyph(cp[i]);
    if (!g) continue;
    int dx = 0, dy = 0;
    if (isTone(cp[i]) && !prevUpper && !(i + 1 < n && cp[i + 1] == 0x0E33)) dy = THAI_TONE_DROP;
    if (isAbove(cp[i]) && baseTall) dx = -THAI_TALL_SHIFT;
    if (draw && g->w) {
      int bpr = (g->w + 7) / 8;
      const uint8_t *bits = THAI_FONT_BITS + g->off;
      for (int r = 0; r < g->h; r++)
        for (int c = 0; c < g->w; c++)
          if (bits[r * bpr + c / 8] & (0x80 >> (c % 8))) {
            int16_t px = x + (pen + g->xo + dx + c) * size, py = y + (g->yo + dy + r) * size;
            if (size == 1) oled.drawPixel(px, py, SSD1306_WHITE);
            else oled.fillRect(px, py, size, size, SSD1306_WHITE);
          }
    }
    pen += g->adv;
    if (isUpperVowel(cp[i])) prevUpper = true;
    else if (g->adv) { prevUpper = false; baseTall = isTall(cp[i]); }
  }
  return pen * size;
}

// font argument: "auto" (default: Thai font when the text has non-English characters),
// "thai" (always the 20 px Thai font) or "small" (classic 6x8 font, English only)
// (read the options before luaL_tolstring: it pushes onto the stack and would shift
// a missing optional argument onto the converted string)
static int fontOpt(lua_State *L, int i) {
  static const char *const names[] = {"auto", "thai", "small", NULL};
  return luaL_checkoption(L, i, "auto", names);
}
static int resolveFont(int f, const uint32_t *cp, int n) {
  if (f) return f;
  for (int k = 0; k < n; k++) if (cp[k] >= 0x80) return 1;
  return 2;
}

static const int TEXT_MAX = 160;

// oled.text(x, y, value [, size 1-4 [, font]])
static int l_oled_text(lua_State *L) {
  oledEnsure(L);
  int16_t x = ci(L, 1), y = ci(L, 2);
  int size = (int)luaL_optinteger(L, 4, 1);
  if (size < 1 || size > 4) luaL_error(L, "ขนาดตัวอักษรต้องเป็น 1-4");
  int font = fontOpt(L, 5);
  size_t len; const char *s = luaL_tolstring(L, 3, &len);
  uint32_t cp[TEXT_MAX];
  int n = utf8Decode(s, len, cp, TEXT_MAX);
  if (resolveFont(font, cp, n) == 1) {
    thaiText(x, y, cp, n, size, true);
  } else {
    String out;
    for (int k = 0; k < n; k++) out += cp[k] < 0x80 ? (char)cp[k] : '?';
    oled.setTextSize(size);
    oled.setCursor(x, y);
    oled.print(out);
  }
  lua_pop(L, 1);
  oledDirty = true;
  return 0;
}

// Dev aid (serial "--oledtest <text>"): draw text at 0,0 into the frame buffer and print
// the 128x64 pixels as '#'/'.', so the renderer can be checked without a display attached.
void oledTestDump(const String &text, int size, int rows) {
  oledp = rows == 32 ? &oled32 : &oled64;
  if (!oled.getBuffer()) oled.begin(SSD1306_SWITCHCAPVCC, 0x3C, false, false);
  if (!oled.getBuffer()) { Serial.println("oledtest: no buffer"); return; }
  oled.clearDisplay();
  uint32_t cp[TEXT_MAX];
  int n = utf8Decode(text.c_str(), text.length(), cp, TEXT_MAX);
  int w = thaiText(0, 0, cp, n, size, true);
  Serial.printf("oledtest width=%d\n", w);
  const uint8_t *buf = oled.getBuffer();
  char line[129]; line[128] = 0;
  for (int y = 0; y < rows; y++) {
    for (int x = 0; x < 128; x++) line[x] = (buf[x + (y / 8) * 128] >> (y % 8)) & 1 ? '#' : '.';
    Serial.println(line);
  }
  oled.clearDisplay();
  oledp = oledType == OLED_SSD1306_32 ? &oled32 : &oled64;   // back to the display in use
  Serial.println("oledtest end");
}

// oled.width(value [, size [, font]]) -> width in pixels (for centering / right-aligning)
static int l_oled_width(lua_State *L) {
  int size = (int)luaL_optinteger(L, 2, 1);
  if (size < 1 || size > 4) luaL_error(L, "ขนาดตัวอักษรต้องเป็น 1-4");
  int font = fontOpt(L, 3);
  size_t len; const char *s = luaL_tolstring(L, 1, &len);
  uint32_t cp[TEXT_MAX];
  int n = utf8Decode(s, len, cp, TEXT_MAX);
  int w = resolveFont(font, cp, n) == 1 ? thaiText(0, 0, cp, n, size, false) : n * 6 * size;
  lua_pop(L, 1);
  lua_pushinteger(L, w);
  return 1;
}
static int l_oled_pixel(lua_State *L) { oledEnsure(L); oled.drawPixel(ci(L, 1), ci(L, 2), color(L, 3)); oledDirty = true; return 0; }
static int l_oled_line(lua_State *L) {
  oledEnsure(L); oled.drawLine(ci(L, 1), ci(L, 2), ci(L, 3), ci(L, 4), color(L, 5)); oledDirty = true; return 0;
}
static int l_oled_rect(lua_State *L) {
  oledEnsure(L);
  int16_t x = ci(L, 1), y = ci(L, 2), w = ci(L, 3), h = ci(L, 4);
  if (lua_toboolean(L, 5)) oled.fillRect(x, y, w, h, SSD1306_WHITE); else oled.drawRect(x, y, w, h, SSD1306_WHITE);
  oledDirty = true;
  return 0;
}
static int l_oled_circle(lua_State *L) {
  oledEnsure(L);
  int16_t x = ci(L, 1), y = ci(L, 2), r = ci(L, 3);
  if (lua_toboolean(L, 4)) oled.fillCircle(x, y, r, SSD1306_WHITE); else oled.drawCircle(x, y, r, SSD1306_WHITE);
  oledDirty = true;
  return 0;
}
// oled.bar(x, y, w, h, percent) — outlined bar filled to percent (0-100)
static int l_oled_bar(lua_State *L) {
  oledEnsure(L);
  int16_t x = ci(L, 1), y = ci(L, 2), w = ci(L, 3), h = ci(L, 4);
  lua_Number p = luaL_checknumber(L, 5);
  p = p < 0 ? 0 : p > 100 ? 100 : p;
  oled.fillRect(x, y, w, h, SSD1306_BLACK);
  oled.drawRect(x, y, w, h, SSD1306_WHITE);
  int16_t fw = (int16_t)lround((w - 4) * p / 100.0);
  if (fw > 0 && h > 4) oled.fillRect(x + 2, y + 2, fw, h - 4, SSD1306_WHITE);
  oledDirty = true;
  return 0;
}
static int l_oled_invert(lua_State *L) {
  oledEnsure(L);
  if (oledType == OLED_SH1106) { const uint8_t c = lua_toboolean(L, 1) ? 0xA7 : 0xA6; oledCmds(&c, 1); }
  else oled.invertDisplay(lua_toboolean(L, 1));
  return 0;
}

// oled.begin("ssd1306" | "sh1106" | "ssd1306_32" [, addr]) — choose the display (per run;
// without it the SSD1306 0.96" 128x64 display at 0x3C/0x3D is assumed)
static int l_oled_begin(lua_State *L) {
  static const char *const types[] = {"ssd1306", "sh1106", "ssd1306_32", NULL};
  int t = luaL_checkoption(L, 1, "ssd1306", types);
  int a = (int)luaL_optinteger(L, 2, 0);
  if (a && a != 0x3C && a != 0x3D) luaL_error(L, "ที่อยู่จอ OLED ต้องเป็น 0x3C (60) หรือ 0x3D (61)");
  oledType = (uint8_t)t; oledWant = (uint8_t)a; oledAddr = 0;
  oledp = t == OLED_SSD1306_32 ? &oled32 : &oled64;
  oledEnsure(L);
  return 0;
}

// ---------------------------------------------------------------- character LCD (HD44780 + PCF8574 backpack)
// PCF8574 pins: P0=RS P1=RW P2=EN P3=backlight P4..P7=D4..D7 (same as LiquidCrystal_I2C).
// The PCF8574 is a 100 kHz part, so LCD traffic temporarily runs the bus at 100 kHz.
// HD44780 has no Thai characters: Thai shows as '?' ('°' maps to the LCD's degree sign).
static uint8_t lcdAddr = 0, lcdWant = 0, lcdCols = 16, lcdRows = 2, lcdBl = 0x08;
static uint32_t lcdGen = 0;

static bool lcdNibble(uint8_t v) {             // one nibble = data, data|EN, data in one transfer
  v |= lcdBl;
  uint8_t b[3] = {v, (uint8_t)(v | 0x04), v};
  Wire.beginTransmission(lcdAddr);
  Wire.write(b, 3);
  return Wire.endTransmission() == 0;
}
static bool lcdSend(uint8_t v, uint8_t rs) {
  return lcdNibble((v & 0xF0) | rs) && lcdNibble(((v << 4) & 0xF0) | rs);
}
static void lcdSlow() { Wire.setClock(100000); }
static void lcdFast() { Wire.setClock(busFreq); }

static const char *LCD_NAME = "จอ LCD I2C (PCF8574)";

static void lcdEnsure(lua_State *L) {
  if (lcdAddr && lcdGen == busGen) return;
  lcdAddr = 0;
  busEnsure();
  uint8_t a = 0;
  if (lcdWant) a = present(lcdWant) ? lcdWant : 0;
  else {
    static const uint8_t tryFirst[] = {0x27, 0x3F};
    for (uint8_t c : tryFirst) if (!a && present(c)) a = c;
    for (uint8_t c = 0x20; !a && c <= 0x26; c++) if (present(c)) a = c;   // PCF8574
    for (uint8_t c = 0x38; !a && c <= 0x3E; c++) if (present(c)) a = c;   // PCF8574A
  }
  if (!a) { notFound(L, LCD_NAME, lcdWant ? lcdWant : 0x27, "5V"); return; }
  lcdAddr = a;
  lcdSlow();
  bool ok = true;
  Wire.beginTransmission(a); Wire.write(lcdBl); ok &= Wire.endTransmission() == 0;
  delay(50);
  // wake up in 8-bit mode three times, then switch to 4-bit (HD44780 datasheet fig. 24)
  ok &= lcdNibble(0x30); delay(5);
  ok &= lcdNibble(0x30); delay(5);
  ok &= lcdNibble(0x30); delayMicroseconds(150);
  ok &= lcdNibble(0x20);
  ok &= lcdSend(lcdRows > 1 ? 0x28 : 0x20, 0);   // 4-bit, 2 lines (or 1), 5x8 dots
  ok &= lcdSend(0x0C, 0);                        // display on, cursor off, blink off
  ok &= lcdSend(0x01, 0); delay(2);              // clear
  ok &= lcdSend(0x06, 0);                        // entry mode: left to right
  lcdFast();
  if (!ok) { lcdAddr = 0; notFound(L, LCD_NAME, a, "5V"); return; }
  lcdGen = busGen;
}

// lcd.begin([cols, rows [, addr]]) — 16x2 (default), 20x4, 16x1 ...; addr: 0x27/0x3F/... (auto if omitted)
static int l_lcd_begin(lua_State *L) {
  int c = (int)luaL_optinteger(L, 1, 16), r = (int)luaL_optinteger(L, 2, 2), a = (int)luaL_optinteger(L, 3, 0);
  if (c < 8 || c > 40 || r < 1 || r > 4) luaL_error(L, "ขนาดจอ LCD ต้องเป็น 8-40 ตัวอักษร x 1-4 บรรทัด เช่น 16, 2");
  if (a && (a < 0x20 || a > 0x3F)) luaL_error(L, "ที่อยู่จอ LCD ต้องอยู่ระหว่าง 0x20-0x3F (ส่วนใหญ่ 0x27 หรือ 0x3F)");
  lcdCols = c; lcdRows = r; lcdWant = a; lcdAddr = 0;
  lcdEnsure(L);
  return 0;
}

static int l_lcd_clear(lua_State *L) {
  lcdEnsure(L);
  lcdSlow(); lcdSend(0x01, 0); delay(2); lcdFast();
  return 0;
}

// lcd.text(col, row, value) — col 0..cols-1, row 0..rows-1; text past the edge is cut off
static int l_lcd_text(lua_State *L) {
  int col = (int)luaL_checkinteger(L, 1), row = (int)luaL_checkinteger(L, 2);
  lcdEnsure(L);
  if (row < 0 || row >= lcdRows) luaL_error(L, "บรรทัดต้องเป็น 0-%d", lcdRows - 1);
  if (col < 0 || col >= lcdCols) luaL_error(L, "ตำแหน่งต้องเป็น 0-%d", lcdCols - 1);
  size_t len; const char *s = luaL_tolstring(L, 3, &len);
  uint32_t cp[TEXT_MAX];
  int n = utf8Decode(s, len, cp, TEXT_MAX);
  lua_pop(L, 1);
  static const uint8_t rowAddr[4] = {0x00, 0x40, 0x14, 0x54};
  lcdSlow();
  lcdSend(0x80 | (rowAddr[row] + col), 0);
  for (int i = 0; i < n && col + i < lcdCols; i++) {
    uint32_t c = cp[i];
    uint8_t ch = c == 0xB0 ? 0xDF : (c >= 0x20 && c < 0x7F) ? (uint8_t)c : '?';
    if (c >= 0x0E31 && c <= 0x0E4E && c != 0x0E32 && c != 0x0E33 && !(c >= 0x0E3F && c <= 0x0E46)) continue;  // Thai marks: no cell of their own
    lcdSend(ch, 1);
  }
  lcdFast();
  return 0;
}

// every run starts as a 16x2 LCD at an auto-detected address with the backlight on
static void lcdReset() {
  if (lcdCols != 16 || lcdRows != 2 || lcdWant) { lcdCols = 16; lcdRows = 2; lcdWant = 0; lcdAddr = 0; }
  lcdBl = 0x08;
}

static int l_lcd_backlight(lua_State *L) {
  lcdEnsure(L);
  lcdBl = (lua_isnone(L, 1) || lua_toboolean(L, 1)) ? 0x08 : 0x00;
  lcdSlow();
  Wire.beginTransmission(lcdAddr); Wire.write(lcdBl); Wire.endTransmission();
  lcdFast();
  return 0;
}

// ---------------------------------------------------------------- registration
static void lib(lua_State *L, const char *name, const luaL_Reg *fns) {
  lua_newtable(L);
  luaL_setfuncs(L, fns, 0);
  lua_setglobal(L, name);
}

void i2cOpenLib(lua_State *L) {
  static const luaL_Reg i2c[] = {{"begin", l_i2c_begin}, {"scan", l_i2c_scan}, {"write", l_i2c_write},
    {"read", l_i2c_read}, {"readReg", l_i2c_readReg}, {"writeReg", l_i2c_writeReg}, {NULL, NULL}};
  static const luaL_Reg ahtF[] = {{"temperature", l_aht_t}, {"humidity", l_aht_h}, {NULL, NULL}};
  static const luaL_Reg shtF[] = {{"temperature", l_sht_t}, {"humidity", l_sht_h}, {NULL, NULL}};
  static const luaL_Reg bmeF[] = {{"temperature", l_bme_t}, {"humidity", l_bme_h}, {"pressure", l_bme_p}, {NULL, NULL}};
  static const luaL_Reg bhF[] = {{"lux", l_bh_lux}, {NULL, NULL}};
  static const luaL_Reg mpuF[] = {{"accel", l_mpu_accel}, {"gyro", l_mpu_gyro}, {"angle", l_mpu_angle},
    {"temperature", l_mpu_temp}, {NULL, NULL}};
  static const luaL_Reg oledF[] = {{"begin", l_oled_begin}, {"clear", l_oled_clear}, {"show", l_oled_show}, {"text", l_oled_text},
    {"pixel", l_oled_pixel}, {"line", l_oled_line}, {"rect", l_oled_rect}, {"circle", l_oled_circle},
    {"bar", l_oled_bar}, {"invert", l_oled_invert}, {"width", l_oled_width}, {"height", l_oled_height}, {NULL, NULL}};
  lib(L, "i2c", i2c);
  lib(L, "aht20", ahtF);
  lib(L, "sht31", shtF);
  lib(L, "bme280", bmeF);
  lib(L, "bh1750", bhF);
  lib(L, "mpu6050", mpuF);
  lib(L, "oled", oledF);
  static const luaL_Reg lcdF[] = {{"begin", l_lcd_begin}, {"clear", l_lcd_clear}, {"text", l_lcd_text},
    {"backlight", l_lcd_backlight}, {NULL, NULL}};
  lib(L, "lcd", lcdF);
}

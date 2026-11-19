#pragma once
// I2C bus, sensor drivers and the OLED displays (SSD1306 128x64 / 128x32, SH1106),
// exposed to Lua as the tables i2c, aht20, sht31, bme280, bh1750, mpu6050 and oled.
#include <Arduino.h>
extern "C" {
#include "lua.h"
}

void i2cOpenLib(lua_State *L);   // register the Lua tables (call for every new lua_State)
void i2cFlush();                 // push the OLED buffer to the screen if something was drawn
void i2cRunStart();              // reset bus pins and display type to the defaults before each run
// dev aid (serial --oledtest / --oledtest32): draw text into the frame buffer of the
// 128x64 or 128x32 display (no display needed) and print the pixels
void oledTestDump(const String &text, int size, int rows);

#pragma once
// ---- WiFi settings ----
// If WIFI_SSID is empty or the connection fails within 15 s,
// the board starts its own access point instead.
#define WIFI_SSID     ""
#define WIFI_PASS     ""

#define AP_SSID       "ESP32-LiveCode"
#define AP_PASS       "12345678"      // at least 8 characters
#define MDNS_NAME     "esp32"         // open http://esp32.local

// Script saved as /autorun.lua runs automatically after boot
#define AUTORUN_FILE  "/autorun.lua"

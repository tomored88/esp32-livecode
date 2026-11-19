# Third-party notices

This project (MIT, see `LICENSE`) bundles or uses the following components.

| Component | Where | License |
|---|---|---|
| Lua 5.4.6 — Lua.org, PUC-Rio | `lib/lua/` | MIT (license text kept in `lib/lua/`) |
| Blockly 13.4.0 — Google LLC | `web/vendor/blockly/` | Apache License 2.0 (`web/vendor/blockly/LICENSE`) |
| K2D Bold font — Copyright 2018 The K2D Project Authors (https://github.com/cadsondemak/K2D) | `tools/fonts/K2D-Bold.ttf`, converted to the bitmap table in `src/font_thai.h` | SIL Open Font License 1.1 (https://openfontlicense.org) |
| ESP Web Tools 10.4.0 — Open Home Foundation | loaded from unpkg CDN by the install page (`docs/index.html`); not redistributed here | Apache License 2.0 |
| Adafruit SSD1306 / Adafruit GFX / BusIO | downloaded by PlatformIO at build time (`platformio.ini`); not redistributed here | BSD |
| Arduino-ESP32 core (Espressif) | linked into the firmware image in `docs/*.bin` | LGPL-2.1 / Apache-2.0 (see https://github.com/espressif/arduino-esp32) |

The K2D font is licensed under the SIL Open Font License, Version 1.1. The full text is available at
https://openfontlicense.org/open-font-license-official-text/ . The bitmap font in `src/font_thai.h` is a derivative
of K2D and remains under the OFL; the font name "K2D" is not used for it.

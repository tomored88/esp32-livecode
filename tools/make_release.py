"""Build a release that customers can flash from a web browser (ESP Web Tools).

    python tools/make_release.py            # builds the firmware first (pio run -e esp32dev)

Writes docs/ (served by GitHub Pages):
    esp32-livecode-<ver>.bin   one merged image, flash at offset 0
                               (bootloader 0x1000 + partitions 0x8000 + boot_app0 0xe000 + app 0x10000)
    manifest.json              ESP Web Tools manifest
    index.html                 page with the "install" button

The page needs https:// or http://localhost (Web Serial rule), e.g.
    python -m http.server 8000 -d docs      ->  http://localhost:8000/
The data partition (saved programs) is not part of the image; the firmware formats it
on first boot if it is empty or invalid.
"""
import json
import os
import re
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD = os.path.join(ROOT, ".pio", "build", "esp32dev")
OUT = os.path.join(ROOT, "docs")
PIO_HOME = os.path.join(os.path.expanduser("~"), ".platformio", "packages")
ESPTOOL = os.path.join(PIO_HOME, "tool-esptoolpy", "esptool.py")
BOOT_APP0 = os.path.join(PIO_HOME, "framework-arduinoespressif32", "tools", "partitions", "boot_app0.bin")
ESP_WEB_TOOLS = "https://unpkg.com/esp-web-tools@10.4.0/dist/web/install-button.js?module"


def version():
    src = open(os.path.join(ROOT, "src", "main.cpp"), encoding="utf-8").read()
    return re.search(r'#define FW_VERSION "([^"]+)"', src).group(1)


def main():
    if "--no-build" not in sys.argv:
        subprocess.run(["pio", "run", "-e", "esp32dev"], cwd=ROOT, check=True)
    ver = version()
    name = f"esp32-livecode-{ver}.bin"
    os.makedirs(OUT, exist_ok=True)
    for old in os.listdir(OUT):                      # drop earlier images/manifest, keep docs/img etc.
        if old.endswith(".bin") or old in ("manifest.json", "index.html"):
            os.remove(os.path.join(OUT, old))
    subprocess.run([sys.executable, ESPTOOL, "--chip", "esp32", "merge_bin", "-o", os.path.join(OUT, name),
                    "--flash_mode", "dio", "--flash_freq", "40m", "--flash_size", "4MB",
                    "0x1000", os.path.join(BUILD, "bootloader.bin"),
                    "0x8000", os.path.join(BUILD, "partitions.bin"),
                    "0xe000", BOOT_APP0,
                    "0x10000", os.path.join(BUILD, "firmware.bin")], check=True)

    manifest = {"name": "ESP32 Live Code", "version": ver, "new_install_prompt_erase": True,
                "builds": [{"chipFamily": "ESP32", "parts": [{"path": name, "offset": 0}]}]}
    with open(os.path.join(OUT, "manifest.json"), "w", encoding="utf-8") as f:
        json.dump(manifest, f, indent=2)

    page = open(os.path.join(ROOT, "tools", "flash_template.html"), encoding="utf-8").read()
    page = page.replace("{{VERSION}}", ver).replace("{{BIN}}", name).replace("{{ESP_WEB_TOOLS}}", ESP_WEB_TOOLS)
    with open(os.path.join(OUT, "index.html"), "w", encoding="utf-8", newline="\n") as f:
        f.write(page)
    size = os.path.getsize(os.path.join(OUT, name))
    print(f"docs/{name} ({size // 1024} KB), manifest.json, index.html")


if __name__ == "__main__":
    main()

"""Develop the web UI on a PC without reflashing.

    python tools/dev_server.py                 # mock board (no hardware)
    python tools/dev_server.py --port COM3     # real board over USB serial

Serves web/index.html, /blockly.js and /media/* straight from web/ (edit + reload),
and implements the board's /api/* routes. Files and WiFi settings are kept in memory
(never on the board), so file/WiFi dialogs can be tried safely.
With --port, code is sent through the firmware's serial console ("--run" / "--stop")
and every serial line becomes a console line. Live values (show) are not sent over
serial, so cards stay empty in serial mode; mock mode fakes a couple.
"""
import argparse
import http.server
import json
import math
import os
import threading
import time
import urllib.parse

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
WEB = os.path.join(ROOT, "web")
BLOCKLY = os.path.join(WEB, "vendor", "blockly")
PARTS = ["blockly_compressed.js", "blocks_compressed.js", "lua_compressed.js", os.path.join("msg", "en.js"), "=SNAPSHOT_EN", os.path.join("msg", "th.js")]
# English messages are kept in window.BLOCKLY_MSG_EN (block text uses them), Thai fills the rest of the UI
SNAPSHOT_EN = b"window.BLOCKLY_MSG_EN = Object.assign({}, Blockly.Msg);\n"
TYPES = {".png": "image/png", ".gif": "image/gif", ".svg": "image/svg+xml", ".cur": "image/x-icon"}


BOOT_ID = int(time.time()) & 0x7fffffff


class Board:
    def __init__(self):
        self.lock = threading.Lock()
        self.lines = []      # (seq, type, text)
        self.seq = 0
        self.running = False
        self.vals = {}
        self.vs = 0

    def add(self, t, m):
        with self.lock:
            self.seq += 1
            self.lines.append((self.seq, t, m))
            del self.lines[:-300]

    def log(self, since, vs):
        with self.lock:
            out = {"boot": BOOT_ID, "seq": self.seq, "clear": 0, "running": self.running,
                   "lines": [{"s": s, "t": t, "m": m} for s, t, m in self.lines if s > since][:150], "vs": self.vs}
            if vs != self.vs:
                out["vals"] = dict(self.vals)
            return out


class SerialBoard(Board):
    def __init__(self, port):
        super().__init__()
        import serial  # pyserial
        self.ser = serial.Serial()
        self.ser.port, self.ser.baudrate, self.ser.timeout = port, 115200, 0.2
        self.ser.dtr = self.ser.rts = False   # do not reset the board on open
        self.ser.open()
        self.port = port
        threading.Thread(target=self.reader, daemon=True).start()

    def reader(self):
        buf = b""
        while True:
            buf += self.ser.read(256)
            while b"\n" in buf:
                raw, buf = buf.split(b"\n", 1)
                m = raw.decode("utf-8", "replace").rstrip("\r")
                if not m:
                    continue
                t = 1 if m.startswith("✖") else 2 if m[:1] in "▶■✔" else 0
                if m.startswith("▶"):
                    self.running = True
                elif m[:1] in "■✔✖":
                    self.running = False
                self.add(t, m)

    def run(self, code):
        lines = [l for l in code.replace("\r", "").split("\n") if l.strip() not in ("--run", "--stop")]
        self.ser.write(("\n".join(lines) + "\n--run\n").encode())

    def stop(self):
        self.ser.write(b"--stop\n")


class MockBoard(Board):
    port = "mock"

    def run(self, code):
        self.running = True
        self.add(2, "▶ เริ่มทำงาน (จำลอง)")
        for l in code.split("\n")[:5]:
            self.add(0, l)
        t0 = time.time()

        def tick():
            while self.running:
                t = time.time() - t0
                with self.lock:
                    self.vals = {"อนาล็อก GPIO34": str(int(2048 + 1800 * math.sin(t))), "ข้อความ": "สวัสดี"}
                    self.vs += 1
                time.sleep(0.2)
        threading.Thread(target=tick, daemon=True).start()

    def stop(self):
        if self.running:
            self.running = False
            self.add(2, "■ หยุดแล้ว")


def multipart_file(ctype, raw):
    """Content of the first file part of a multipart/form-data body, or None."""
    if "multipart/form-data" not in ctype or "boundary=" not in ctype:
        return None
    boundary = b"--" + ctype.split("boundary=", 1)[1].strip().strip('"').encode()
    for part in raw.split(boundary)[1:-1]:
        head, _, data = part.partition(b"\r\n\r\n")
        if b"filename=" in head:
            return data[:-2] if data.endswith(b"\r\n") else data
    return None


def blockly_js():
    out = b""
    for p in PARTS:
        if p == "=SNAPSHOT_EN":
            out += SNAPSHOT_EN
            continue
        with open(os.path.join(BLOCKLY, p), "rb") as f:
            out += f.read() + b"\n;\n"
    return out


class MockFiles:
    """In-memory stand-in for the board's LittleFS + WiFi settings (same rules as main.cpp)."""
    def __init__(self):
        self.files = {}      # "name.kind" -> text   ("__autorun.lua" for autorun)
        self.wifi = ""

    @staticmethod
    def valid(name):
        return name == "__autorun" or (0 < len(name) <= 24 and all(c.isalnum() or c in "_-" for c in name))

    def names(self):
        return sorted({k[:-4] for k in self.files if k.endswith(".lua") and not k.startswith("__autorun")})


def make_handler(board, fs):
    class H(http.server.BaseHTTPRequestHandler):
        def log_message(self, *a):
            pass

        def send(self, code, body, ctype="text/plain; charset=utf-8"):
            if isinstance(body, str):
                body = body.encode()
            self.send_response(code)
            self.send_header("Content-Type", ctype)
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Cache-Control", "no-cache")
            self.end_headers()
            self.wfile.write(body)

        def do_GET(self):
            u = urllib.parse.urlparse(self.path)
            q = urllib.parse.parse_qs(u.query)
            if u.path == "/":
                with open(os.path.join(WEB, "index.html"), "rb") as f:
                    return self.send(200, f.read(), "text/html; charset=utf-8")
            if u.path == "/blockly.js":
                return self.send(200, blockly_js(), "application/javascript")
            if u.path.startswith("/media/"):
                name = os.path.basename(u.path)
                p = os.path.join(BLOCKLY, "media", name)
                if os.path.isfile(p):
                    with open(p, "rb") as f:
                        return self.send(200, f.read(), TYPES.get(os.path.splitext(name)[1], "application/octet-stream"))
            if u.path == "/api/log":
                since = int(q.get("since", ["0"])[0] or 0)
                vs = int(q.get("vs", ["-1"])[0] or -1)
                return self.send(200, json.dumps(board.log(since, vs), ensure_ascii=False), "application/json")
            if u.path == "/api/info":
                return self.send(200, json.dumps({"ver": "dev", "ip": "localhost", "mode": "sta", "ssid": board.port,
                                                  "saved": fs.wifi, "heap": 0, "autorun": "__autorun.lua" in fs.files,
                                                  "fs": 0}), "application/json")
            if u.path == "/api/files":
                return self.send(200, json.dumps(fs.names()), "application/json")
            if u.path == "/api/file":
                return self.file("GET", q)
            if u.path == "/api/scan":
                time.sleep(1)
                return self.send(200, json.dumps([{"ssid": "MyHome", "rssi": -48, "lock": True},
                                                  {"ssid": "Cafe Free WiFi", "rssi": -71, "lock": False}]), "application/json")
            self.send(404, "not found")

        def file(self, method, q, body=""):
            name = q.get("name", [""])[0]
            kind = "blk" if q.get("kind", [""])[0] == "blk" else "lua"
            if not fs.valid(name):
                return self.send(400, "ชื่อไฟล์ใช้ได้เฉพาะ a-z 0-9 _ - ยาวไม่เกิน 24 ตัว")
            key = name + "." + kind
            if method == "GET":
                return self.send(200, fs.files[key]) if key in fs.files else self.send(404, "ไม่พบไฟล์")
            if method == "POST":
                if len(body.encode()) > 65536:
                    return self.send(413, "ไฟล์ใหญ่เกิน 64KB")
                fs.files[key] = body
            else:
                fs.files.pop(name + ".lua", None)
                fs.files.pop(name + ".blk", None)
            self.send(200, "ok")

        def do_DELETE(self):
            u = urllib.parse.urlparse(self.path)
            if u.path == "/api/file":
                return self.file("DELETE", urllib.parse.parse_qs(u.query))
            self.send(404, "not found")

        def do_POST(self):
            n = int(self.headers.get("Content-Length") or 0)
            raw = self.rfile.read(n)
            body = raw.decode("utf-8", "replace")
            u = urllib.parse.urlparse(self.path)
            if u.path == "/api/file":
                data = multipart_file(self.headers.get("Content-Type", ""), raw)
                if data is None:
                    return self.send(400, "ส่งไฟล์แบบ multipart/form-data")
                return self.file("POST", urllib.parse.parse_qs(u.query), data.decode("utf-8", "replace"))
            if u.path == "/api/wifi":
                f = urllib.parse.parse_qs(body, keep_blank_values=True)
                ssid, pw = f.get("ssid", [""])[0].strip(), f.get("pass", [""])[0]
                if len(ssid) > 32:
                    return self.send(400, "ชื่อ WiFi ยาวเกิน 32 ตัวอักษร")
                if pw and not 8 <= len(pw) <= 63:
                    return self.send(400, "รหัส WiFi ต้องยาว 8-63 ตัวอักษร (หรือเว้นว่างถ้าไม่มีรหัส)")
                fs.wifi = ssid
                board.add(2, "(จำลอง) บันทึก WiFi '%s' แล้วรีบูต" % ssid)
            elif u.path == "/api/reboot":
                board.add(2, "(จำลอง) รีบูต")
            elif u.path == "/api/run":
                board.run(body)
            elif u.path == "/api/stop":
                board.stop()
            else:
                return self.send(404, "not found")
            self.send(200, "ok")
    return H


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", help="serial port of the board, e.g. COM3")
    ap.add_argument("--http", type=int, default=8080)
    a = ap.parse_args()
    board = SerialBoard(a.port) if a.port else MockBoard()
    srv = http.server.ThreadingHTTPServer(("127.0.0.1", a.http), make_handler(board, MockFiles()))
    print(f"open http://localhost:{a.http}  (board: {board.port})")
    srv.serve_forever()


if __name__ == "__main__":
    main()

"""HTTP API test against a real board:  python -X utf8 tools/http_test.py [http://192.168.4.1]

Join the board's WiFi first. Leaves no files behind (deletes what it creates) and does
not change the saved WiFi (only tries invalid values, which the board must reject).
"""
import json
import sys
import time
import urllib.error
import urllib.parse
import urllib.request

BASE = (sys.argv[1] if len(sys.argv) > 1 else "http://192.168.4.1").rstrip("/")
results = []


def put(name, kind, text, timeout=30):
    """Save a file the way the web page does: multipart/form-data."""
    b = "----lc" + str(time.time()).replace(".", "")
    data = text.encode() if isinstance(text, str) else text
    CRLF = "\r\n"
    head = (f"--{b}{CRLF}Content-Disposition: form-data; name=\"file\"; filename=\"{name}.{kind}\"{CRLF}"
            f"Content-Type: text/plain{CRLF}{CRLF}")
    body = head.encode() + data + f"{CRLF}--{b}--{CRLF}".encode()
    return req("POST", f"/api/file?name={urllib.parse.quote(name)}&kind={kind}", body,
               f"multipart/form-data; boundary={b}", timeout)


def req(method, path, body=None, ctype="text/plain; charset=utf-8", timeout=15):
    data = body.encode() if isinstance(body, str) else body
    r = urllib.request.Request(BASE + path, data=data, method=method, headers={"Content-Type": ctype} if data is not None else {})
    t0 = time.time()
    try:
        with urllib.request.urlopen(r, timeout=timeout) as resp:
            return resp.status, resp.read(), time.time() - t0, dict(resp.headers)
    except urllib.error.HTTPError as e:
        return e.code, e.read(), time.time() - t0, dict(e.headers)


def check(name, ok, detail=""):
    results.append(ok)
    print(("PASS " if ok else "FAIL ") + name + (("  [" + str(detail)[:300] + "]") if detail != "" else ""))


# ---- page load (raw transfer of what a browser fetches first)
total = 0
for path in ["/", "/blockly.js", "/media/sprites.svg"]:
    st, body, dt, hd = req("GET", path)
    total += dt
    check(f"GET {path} {len(body)//1024}KB gzip in {dt:.2f}s", st == 200 and hd.get("Content-Encoding") == "gzip", st)
check(f"first page assets in {total:.2f}s (< 3s)", total < 3.0, round(total, 2))

st, body, _, _ = req("GET", "/api/info")
info = json.loads(body)
check("GET /api/info", st == 200 and info["mode"] in ("ap", "sta") and "saved" in info, info)

# ---- run / log / live values over HTTP
st, body, _, _ = req("GET", "/api/log?since=0&vs=-1")
seq = json.loads(body)["seq"]
req("POST", "/api/run", 'for i = 1, 5 do show("n", i) show("txt", "สวัสดี") delay(50) end print("done", 7)')
time.sleep(1.0)
j = json.loads(req("GET", f"/api/log?since={seq}&vs=-1")[1])
msgs = [l["m"] for l in j["lines"]]
check("run over HTTP prints to log", "done\t7" in msgs, msgs)
check("show() values in /api/log", j.get("vals", {}).get("n") == "5" and j["vals"].get("txt") == "สวัสดี", j.get("vals"))
vs = j["vs"]
check("vals omitted when unchanged", "vals" not in json.loads(req("GET", f"/api/log?since={j['seq']}&vs={vs}")[1]))

req("POST", "/api/run", "while true do end")
time.sleep(1.0)
t0 = time.time()
st, _, dt, _ = req("GET", "/api/info")
check(f"web answers during tight loop ({dt*1000:.0f} ms)", st == 200 and dt < 1.0, dt)
req("POST", "/api/stop")
time.sleep(0.5)
j = json.loads(req("GET", "/api/log?since=0&vs=0")[1])
check("stop over HTTP", not j["running"] and any("หยุดแล้ว" in l["m"] for l in j["lines"][-3:]), j["lines"][-2:])

# ---- files
name = "httptest_x"
st, _, _, _ = put(name, "lua", 'print("from file")')
st2, _, _, _ = put(name, "blk", json.dumps({"format": "esp32-livecode", "v": 1, "name": name, "blocks": {}, "manual": None}))
check("save .lua + .blk", st == 200 and st2 == 200, (st, st2))
files = json.loads(req("GET", "/api/files")[1])
check("file listed", name in files, files)
st, body, _, _ = req("GET", f"/api/file?name={name}&kind=lua")
check("read back .lua", st == 200 and body.decode() == 'print("from file")', body)
check("bad name rejected", put("Bad Name!", "lua", "x")[0] == 400)
check("plain-text POST rejected (needs multipart)", req("POST", f"/api/file?name={name}&kind=lua", "x")[0] == 400)
check("missing file -> 404", req("GET", "/api/file?name=nope_nope&kind=lua")[0] == 404)
check("70KB file -> 413 (size limit)", put(name + "2", "blk", "x" * 70000)[0] == 413)
st, _, dt, _ = put(name, "blk", "y" * 60000)
check(f"60KB file saves ({dt:.2f}s)", st == 200, st)
st, body, _, _ = req("GET", f"/api/file?name={name}&kind=blk")
check("60KB file reads back intact", st == 200 and body == b"y" * 60000, len(body))
check("overwrite with 70KB rejected (413/507)", put(name, "blk", "x" * 70000)[0] in (413, 507))
check("old file kept after rejected upload", req("GET", f"/api/file?name={name}&kind=blk")[1] == b"y" * 60000)
req("DELETE", f"/api/file?name={name}&kind=lua")
files = json.loads(req("GET", "/api/files")[1])
check("delete removes file", name not in files and req("GET", f"/api/file?name={name}&kind=blk")[0] == 404, files)

# ---- disk full: must answer 507, not crash the board (LittleFS crashes if it fills mid-write)
seq_before = json.loads(req("GET", "/api/log?since=0&vs=0")[1])["seq"]
filled = []
for i in range(6):
    st, body, _, _ = put(f"fill_{i}", "blk", "f" * 50000)
    if st != 200:
        break
    filled.append(f"fill_{i}")
check(f"disk full -> 507 after {len(filled)} x 50KB", st == 507, (st, body.decode("utf-8", "replace")[:60]))
j = json.loads(req("GET", "/api/log?since=0&vs=0")[1])
check("board did not reboot", j["seq"] >= seq_before, (seq_before, j["seq"]))
check("earlier files intact", all(req("GET", f"/api/file?name={n}&kind=blk")[1] == b"f" * 50000 for n in filled))
for n in filled:
    req("DELETE", f"/api/file?name={n}&kind=lua")
check("space freed again", json.loads(req("GET", "/api/info")[1])["fs"] > 100000)

# ---- autorun flag through HTTP (removed again at the end)
had_auto = info["autorun"]
if not had_auto:
    put("__autorun", "lua", 'print("auto")')
    check("autorun flag on", json.loads(req("GET", "/api/info")[1])["autorun"] is True)
    req("DELETE", "/api/file?name=__autorun&kind=lua")
    check("autorun flag off", json.loads(req("GET", "/api/info")[1])["autorun"] is False)

# ---- WiFi: scan + validation only (no reboot)
st, body, dt, _ = req("GET", "/api/scan", timeout=20)
nets = json.loads(body) if st == 200 else []
check(f"scan found {len(nets)} networks in {dt:.1f}s", st == 200 and isinstance(nets, list), [n["ssid"] for n in nets][:5])
form = "application/x-www-form-urlencoded"
check("short WiFi password rejected", req("POST", "/api/wifi", urllib.parse.urlencode({"ssid": "x", "pass": "short"}), form)[0] == 400)
check("long SSID rejected", req("POST", "/api/wifi", urllib.parse.urlencode({"ssid": "s" * 40, "pass": ""}), form)[0] == 400)
check("board still up after scan", req("GET", "/api/info")[0] == 200)

print("ALL PASS" if all(results) else "SOME FAILED")

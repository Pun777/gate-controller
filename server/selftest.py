"""End-to-end check of a running compile server: python server/selftest.py http://localhost:8080"""
import base64
import json
import os
import sys
import urllib.error
import urllib.request

BASE = (sys.argv[1] if len(sys.argv) > 1 else "http://localhost:8080").rstrip("/")
# Same password the server uses (IDE_PASSWORD), if any
if os.environ.get("IDE_PASSWORD"):
    token = base64.b64encode(f"admin:{os.environ['IDE_PASSWORD']}".encode()).decode()
    opener = urllib.request.build_opener()
    opener.addheaders = [("Authorization", "Basic " + token)]
    urllib.request.install_opener(opener)


def call(path, body=None):
    req = urllib.request.Request(BASE + path, data=json.dumps(body).encode() if body is not None else None,
                                 headers={"Content-Type": "application/json"})
    if os.environ.get("IDE_PASSWORD"):
        req.add_header("Authorization", "Basic " + base64.b64encode(f"admin:{os.environ['IDE_PASSWORD']}".encode()).decode())
    with urllib.request.urlopen(req, timeout=900) as r:
        return json.loads(r.read().decode())


def check(cond, msg):
    print(("PASS " if cond else "FAIL ") + msg, flush=True)
    if not cond:
        sys.exit(1)


info = call("/api/info")
check(info["mode"] == "server", f"/api/info answers ({info})")

code = call("/api/code")["code"]
check("setOpenGate" in code, "/api/code returns the gate sketch")

r = call("/api/compile", {"code": code, "publish": False})
check(r["ok"], f"verify compiles the sketch in {r['seconds']} s, size {r['size']}")

broken = code.replace("setCloseGate(); // RELAY_UP", "setCloseGat(); // RELAY_UP", 1)
r = call("/api/compile", {"code": broken, "publish": False})
lines = [e["line"] for e in r["errors"]]
check(not r["ok"] and r["errors"] and r["errors"][0]["file"] == "firmware.ino",
      f"broken code reports errors with line numbers {lines}")

r = call("/api/compile", {"code": code, "publish": True})
check(r["ok"] and r.get("version"), f"publish builds version {r.get('version')} in {r['seconds']} s")

m = json.loads(urllib.request.urlopen(BASE + "/firmware/firmware.json").read())
parts = m["builds"]["ESP32"]["parts"]
check([p["offset"] for p in parts] == [0x1000, 0x8000, 0xE000, 0x10000], "manifest lists 4 parts at the right offsets")
for p in parts:
    data = urllib.request.urlopen(BASE + "/firmware/" + p["path"]).read()
    check(len(data) > 1000, f"/firmware/{p['path']} served ({len(data)} bytes)")
app = urllib.request.urlopen(BASE + "/firmware/" + parts[3]["path"]).read()
check(app[0] == 0xE9, "app image starts with the ESP32 image magic byte")

page = urllib.request.urlopen(BASE + "/").read().decode()
check("Gate Controller IDE" in page, "IDE page served at /")

# Update over LAN + IP add-on
check(m.get("lanUpdate") is True and m["app"]["size"] == len(app), f"build includes the LAN update add-on (app {m['app']})")
check(m.get("nvs") == {"offset": 0x9000, "size": 0x5000}, f"NVS partition found {m.get('nvs')}")
tok = call("/api/info")["lanUpdate"]["token"]
plain = urllib.request.build_opener()  # no password: what a board's update page uses


def status(url):
    try:
        return plain.open(url).status
    except urllib.error.HTTPError as e:
        return e.code


check(status(BASE + "/ota/firmware.json?t=wrong") == 403, "/ota refuses a wrong token")
r = plain.open(BASE + "/ota/firmware.json?t=" + tok)
j = json.loads(r.read())
check(j["version"] == m["version"] and r.headers.get("Access-Control-Allow-Origin") == "*", f"/ota/firmware.json with token {j}")
check(plain.open(BASE + "/ota/app.bin?t=" + tok).read() == app, "/ota/app.bin serves the same app image")
check(status(BASE + "/healthz") == 200, "/healthz answers without a password")

b = call("/api/boards", {"mac": "aa:bb:cc:00:11:22", "ip": "10.99.99.77", "version": m["version"]})
check(b["mac"] == "AA:BB:CC:00:11:22" and any(x["ip"] == "10.99.99.77" for x in call("/api/boards")), "board registry saves MAC -> IP")
r = call("/api/reports", {"name": "selftest", "title": "test report", "log": "x" * 100})
check(any(x["id"] == r["id"] for x in call("/api/reports")), "problem report stored")


def delete(path):
    req = urllib.request.Request(BASE + path, method="DELETE")
    if os.environ.get("IDE_PASSWORD"):
        req.add_header("Authorization", "Basic " + base64.b64encode(f"admin:{os.environ['IDE_PASSWORD']}".encode()).decode())
    urllib.request.urlopen(req)


delete("/api/reports/" + r["id"])
delete("/api/boards/AA:BB:CC:00:11:22")
check(all(x["id"] != r["id"] for x in call("/api/reports")), "problem report deleted")
print("ALL PASSED")

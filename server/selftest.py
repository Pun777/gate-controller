"""End-to-end check of a running compile server: python server/selftest.py http://localhost:8080"""
import json
import sys
import urllib.request

BASE = (sys.argv[1] if len(sys.argv) > 1 else "http://localhost:8080").rstrip("/")


def call(path, body=None):
    req = urllib.request.Request(BASE + path, data=json.dumps(body).encode() if body is not None else None,
                                 headers={"Content-Type": "application/json"})
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
print("ALL PASSED")

"""Gate Controller compile server.

Serves the IDE page and compiles the sketch with arduino-cli, so editing and
compiling work like the desktop Arduino IDE (no GitHub token, 20-60 s builds).

  GET  /api/info      server mode, project name, published version
  GET  /api/code      the saved sketch
  POST /api/compile   {"code": "...", "publish": false}  -> verify only
                      {"code": "...", "publish": true}   -> save + publish for upload
  GET  /firmware/...  published images + firmware.json (same format as GitHub Pages)
  GET  /api/boards    boards installed from the IDE (MAC -> IP, version)
  POST /api/boards    {"mac", "ip", "version"} record an install
  GET/POST /api/reports, DELETE /api/reports/{id}   problem reports from technicians
  GET  /ota/firmware.json?t=TOKEN, /ota/app.bin?t=TOKEN   for a board's update page (no password, token only)
  GET  /              the IDE (web/index.html)

Set IDE_PASSWORD to require HTTP Basic auth (user "admin") on every request
except /healthz and /ota/ (those check the update token instead).

Every build gets addons/gc_lan_update.cpp next to the sketch (the sketch text is
not changed): update over LAN on port 81, automatic rollback, and the
"GC SETIP" serial command the install wizard uses to set the board's IP.
"""
import asyncio
import base64
import datetime as dt
import glob
import hashlib
import json
import os
import re
import secrets
import shutil
from pathlib import Path

from fastapi import FastAPI, HTTPException, Request
from fastapi.responses import FileResponse, JSONResponse, Response
from fastapi.staticfiles import StaticFiles
from pydantic import BaseModel

APP_DIR = Path(__file__).resolve().parent
WEB_DIR = Path(os.environ.get("WEB_DIR", APP_DIR / "web"))
SEED_DIR = Path(os.environ.get("SEED_DIR", APP_DIR / "firmware"))   # sketch shipped with the image
DATA_DIR = Path(os.environ.get("DATA_DIR", "/data"))
SKETCH_DIR = DATA_DIR / "sketch"        # saved code (survives container restarts)
HISTORY_DIR = DATA_DIR / "history"      # every published version of firmware.ino
OUT_DIR = DATA_DIR / "firmware"         # what /firmware/ serves
BUILD_DIR = Path(os.environ.get("BUILD_DIR", "/tmp/gc-build"))  # kept between builds = faster
PROJECT = json.loads((APP_DIR / "project.json").read_text(encoding="utf-8"))
FQBN = os.environ.get("FQBN", "esp32:esp32:esp32")
CHIP = os.environ.get("CHIP", "ESP32")
BOOT_OFFSET = 0x1000  # ESP32 classic; S3/C3 would be 0x0
UPDATE_BASE = os.environ.get("UPDATE_BASE", "")  # empty = update-over-LAN disabled in these builds
PASSWORD = os.environ.get("IDE_PASSWORD", "")
MAX_CODE = 512 * 1024
ADDON = APP_DIR / "addons" / "gc_lan_update.cpp"
ADDON_NAME = "zz_gc_lan_update.cpp"
IP_CFG = PROJECT.get("ipConfig", {})
LAN_PORT = int(PROJECT.get("lanUpdatePort", 81))
BOARDS_FILE = DATA_DIR / "boards.json"
REPORT_DIR = DATA_DIR / "reports"
MAX_REPORTS = 500
OPEN_PATHS = ("/healthz", "/ota/")  # no password; /ota/ checks the update token
IP_RE = re.compile(r"^(?:(?:25[0-5]|2[0-4]\d|1?\d?\d)\.){3}(?:25[0-5]|2[0-4]\d|1?\d?\d)$")
MAC_RE = re.compile(r"^[0-9A-F]{2}(?::[0-9A-F]{2}){5}$")

ERROR_RE = re.compile(r"(?:^|[\\/])([\w.-]+\.(?:ino|h|cpp|c)):(\d+):(\d+): (?:fatal )?(error|warning): (.+)")

app = FastAPI(title="Gate Controller compile server", docs_url=None, redoc_url=None)
compile_lock = asyncio.Lock()


def init_dirs():
    for d in (SKETCH_DIR, HISTORY_DIR, OUT_DIR, BUILD_DIR, REPORT_DIR):
        d.mkdir(parents=True, exist_ok=True)
    if not (SKETCH_DIR / "firmware.ino").exists():
        for f in SEED_DIR.iterdir():
            if f.is_file() and f.name != "version.h":
                shutil.copy2(f, SKETCH_DIR / f.name)


def load_token():
    """Update token compiled into each build. Kept in /data so it never changes
    (boards in the field only accept the token they were built with)."""
    env = os.environ.get("OTA_TOKEN", "").strip()
    if env:
        return env
    f = DATA_DIR / "ota_token"
    if not f.exists():
        f.write_text(secrets.token_hex(16), encoding="utf-8")
    return f.read_text(encoding="utf-8").strip()


init_dirs()
OTA_TOKEN = load_token()


@app.middleware("http")
async def basic_auth(request: Request, call_next):
    if PASSWORD and not request.url.path.startswith(OPEN_PATHS):
        expected = "Basic " + base64.b64encode(f"admin:{PASSWORD}".encode()).decode()
        if not secrets.compare_digest(request.headers.get("authorization", ""), expected):
            return Response(status_code=401, headers={"WWW-Authenticate": 'Basic realm="Gate Controller IDE"'})
    return await call_next(request)


def published():
    f = OUT_DIR / "firmware.json"
    return json.loads(f.read_text(encoding="utf-8")) if f.exists() else None


@app.get("/api/info")
def info():
    m = published()
    return {"mode": "server", "name": PROJECT.get("name"), "chip": CHIP, "fqbn": FQBN,
            "version": m["version"] if m else None, "updateOverLan": bool(UPDATE_BASE),
            "lanUpdate": {"port": LAN_PORT, "token": OTA_TOKEN, "inBuild": bool(m and m.get("lanUpdate"))},
            "ipConfig": {k: IP_CFG.get(k) for k in ("gateway", "subnet", "suggestFrom")}}


@app.get("/api/code")
def get_code():
    text = (SKETCH_DIR / "firmware.ino").read_text(encoding="utf-8")
    m = published()
    return {"code": text, "hash": hashlib.sha256(text.encode()).hexdigest(),
            "publishedHash": m.get("codeHash") if m else None}


class CompileRequest(BaseModel):
    code: str
    publish: bool = False


def boot_app0_path():
    data = os.environ.get("ARDUINO_DIRECTORIES_DATA", os.path.expanduser("~/.arduino15"))
    hits = sorted(glob.glob(os.path.join(data, "packages", "esp32", "hardware", "esp32", "*",
                                         "tools", "partitions", "boot_app0.bin")))
    if not hits:
        raise HTTPException(500, "boot_app0.bin not found: ESP32 core is not installed in this image")
    return hits[-1]


def addon_skip_reason(code: str):
    """The add-on defines initVariant() and verifyRollbackLater(); a sketch that
    defines them itself (or opts out) is built without it."""
    if "GC_NO_LAN_UPDATE" in code:
        return "โค้ดมีคำว่า GC_NO_LAN_UPDATE"
    for name in ("initVariant", "verifyRollbackLater"):
        if re.search(r"\b" + name + r"\s*\(\s*(?:void)?\s*\)\s*\{", code):
            return f"โค้ดมีฟังก์ชัน {name}() อยู่แล้ว"
    return None


def c_str(s) -> str:
    return '"' + str(s).replace("\\", "\\\\").replace('"', '\\"') + '"'


def version_header(version: str) -> str:
    lines = ["// Generated by the compile server",
             f"#define FW_VERSION {c_str(version)}",
             f"#define FW_UPDATE_BASE {c_str(UPDATE_BASE)}",
             f"#define GC_OTA_TOKEN {c_str(OTA_TOKEN)}",
             f"#define GC_UPDATE_PORT {LAN_PORT}"]
    for macro, key, default in (("GC_PREF_NS", "namespace", "eth-config"), ("GC_KEY_MODE", "modeKey", "ipMode"),
                                ("GC_KEY_IP", "ipKey", "staticIP"), ("GC_KEY_GW", "gatewayKey", "gateway"),
                                ("GC_KEY_MASK", "subnetKey", "subnet"), ("GC_VAL_STATIC", "staticValue", "Static"),
                                ("GC_VAL_DHCP", "dhcpValue", "DHCP")):
        lines.append(f"#define {macro} {c_str(IP_CFG.get(key, default))}")
    return "\n".join(lines) + "\n"


async def run_compile(code: str, version: str):
    """Compile in a scratch copy of the sketch. Returns (ok, log, errors, warnings, size, addon)."""
    work = BUILD_DIR / "sketch" / "firmware"
    if work.exists():
        shutil.rmtree(work)
    work.mkdir(parents=True)
    for f in SKETCH_DIR.iterdir():
        if f.is_file() and f.name not in ("firmware.ino", "version.h", ADDON_NAME):
            shutil.copy2(f, work / f.name)
    (work / "firmware.ino").write_text(code, encoding="utf-8")
    (work / "version.h").write_text(version_header(version), encoding="utf-8")
    skip = addon_skip_reason(code) if ADDON.exists() else "ไม่มีไฟล์เสริมในเซิร์ฟเวอร์"
    if not skip:
        shutil.copyfile(ADDON, work / ADDON_NAME)

    proc = await asyncio.create_subprocess_exec(
        "arduino-cli", "compile", "--fqbn", FQBN, "--build-path", str(BUILD_DIR / "out"), str(work),
        stdout=asyncio.subprocess.PIPE, stderr=asyncio.subprocess.STDOUT)
    raw, _ = await asyncio.wait_for(proc.communicate(), timeout=600)
    log = raw.decode("utf-8", "replace").replace(str(work), "firmware")
    errors, warnings, seen = [], [], set()
    for m in ERROR_RE.finditer(log):
        item = {"file": m.group(1), "line": int(m.group(2)), "col": int(m.group(3)), "message": m.group(5).strip()}
        key = (item["file"], item["line"], item["message"])
        if key in seen:
            continue
        seen.add(key)
        (errors if m.group(4) == "error" else warnings).append(item)
    size = re.search(r"Sketch uses (\d+) bytes \((\d+)%\)", log)
    return proc.returncode == 0, log, errors, warnings, (size.group(1), size.group(2)) if size else None, skip


def nvs_partition(parts_bin: Path):
    """Find the NVS partition (where the IP settings live) in partitions.bin."""
    data = parts_bin.read_bytes()
    for i in range(0, len(data) - 31, 32):
        e = data[i:i + 32]
        if e[:2] != b"\xaa\x50":
            break
        if e[2] == 1 and e[3] == 2:  # data / nvs
            return {"offset": int.from_bytes(e[4:8], "little"), "size": int.from_bytes(e[8:12], "little")}
    return None


def publish(code: str, version: str, addon: bool):
    out = BUILD_DIR / "out"
    dest = OUT_DIR / "esp32"
    tmp = OUT_DIR / ".new"
    if tmp.exists():
        shutil.rmtree(tmp)
    tmp.mkdir()
    parts = [
        ("bootloader.bin", out / "firmware.ino.bootloader.bin", BOOT_OFFSET),
        ("partitions.bin", out / "firmware.ino.partitions.bin", 0x8000),
        ("boot_app0.bin", Path(boot_app0_path()), 0xE000),
        ("app.bin", out / "firmware.ino.bin", 0x10000),
    ]
    for name, src, _ in parts:
        shutil.copyfile(src, tmp / name)
    if dest.exists():
        shutil.rmtree(dest)
    tmp.rename(dest)
    shutil.copyfile(out / "firmware.ino.bin", OUT_DIR / "esp32.app.bin")
    (OUT_DIR / "esp32.version").write_text(version + "\n", encoding="utf-8")

    (SKETCH_DIR / "firmware.ino").write_text(code, encoding="utf-8")
    (HISTORY_DIR / f"{version}.ino").write_text(code, encoding="utf-8")
    app_bin = (OUT_DIR / "esp32.app.bin").read_bytes()
    manifest = {
        "name": PROJECT.get("name", "ESP32 firmware"),
        "title": PROJECT.get("title", ""),
        "version": version,
        "commit": version,
        "repo": "",
        "codeHash": hashlib.sha256(code.encode()).hexdigest(),
        "lanUpdate": addon,
        "app": {"size": len(app_bin), "md5": hashlib.md5(app_bin).hexdigest()},
        "nvs": nvs_partition(dest / "partitions.bin"),
        "beforeInstall": PROJECT.get("beforeInstall", []),
        "afterInstall": PROJECT.get("afterInstall", []),
        "builds": {CHIP: {"parts": [{"path": f"esp32/{n}", "offset": o} for n, _, o in parts]}},
    }
    (OUT_DIR / "firmware.json").write_text(json.dumps(manifest, ensure_ascii=False, indent=2), encoding="utf-8")
    return manifest


@app.post("/api/compile")
async def compile_endpoint(req: CompileRequest):
    if len(req.code.encode()) > MAX_CODE:
        raise HTTPException(413, "โค้ดใหญ่เกิน 512 KB")
    if compile_lock.locked():
        return JSONResponse({"ok": False, "busy": True, "message": "มีการคอมไพล์อื่นอยู่ รอสักครู่แล้วลองใหม่"}, status_code=409)
    async with compile_lock:
        version = dt.datetime.now().strftime("%Y.%m.%d-%H%M%S") + "-local"
        started = dt.datetime.now()
        ok, log, errors, warnings, size, skip = await run_compile(req.code, version)
        result = {"ok": ok, "errors": errors, "warnings": warnings[:50], "log": log[-20000:],
                  "seconds": round((dt.datetime.now() - started).total_seconds(), 1),
                  "size": {"bytes": int(size[0]), "percent": int(size[1])} if size else None,
                  "lanUpdate": not skip, "lanUpdateSkip": skip}
        if ok and req.publish:
            result["manifest"] = publish(req.code, version, not skip)
            result["version"] = version
        return result


@app.get("/healthz")
def healthz():
    return {"ok": True}


# ---------- update over LAN: files for a board's own update page ----------
# The page runs on the board (http://<board>:81/) and downloads from here, so these
# answer cross-origin and check the update token instead of the IDE password.
OTA_HEADERS = {"Access-Control-Allow-Origin": "*", "Cache-Control": "no-store"}


def check_token(t: str):
    if not t or not secrets.compare_digest(t.encode(), OTA_TOKEN.encode()):
        raise HTTPException(403, "wrong token", headers=OTA_HEADERS)


@app.get("/ota/firmware.json")
def ota_manifest(t: str = ""):
    check_token(t)
    m = published()
    if not m or not m.get("app") or not (OUT_DIR / "esp32.app.bin").exists():
        raise HTTPException(404, "nothing published", headers=OTA_HEADERS)
    return JSONResponse({"version": m["version"], **m["app"]}, headers=OTA_HEADERS)


@app.get("/ota/app.bin")
def ota_app(t: str = ""):
    check_token(t)
    f = OUT_DIR / "esp32.app.bin"
    if not f.exists():
        raise HTTPException(404, "nothing published", headers=OTA_HEADERS)
    return FileResponse(f, media_type="application/octet-stream", headers=OTA_HEADERS)


# ---------- boards installed from the IDE ----------
def read_boards():
    try:
        return json.loads(BOARDS_FILE.read_text(encoding="utf-8"))
    except (FileNotFoundError, ValueError):
        return {}


class BoardRecord(BaseModel):
    mac: str
    ip: str = ""
    version: str = ""
    note: str = ""


@app.get("/api/boards")
def list_boards():
    return sorted(read_boards().values(), key=lambda b: b.get("updated", ""), reverse=True)


@app.post("/api/boards")
def save_board(b: BoardRecord):
    mac = b.mac.strip().upper()
    if not MAC_RE.match(mac):
        raise HTTPException(400, "bad MAC")
    if b.ip and not IP_RE.match(b.ip):
        raise HTTPException(400, "bad IP")
    boards = read_boards()
    now = dt.datetime.now().isoformat(timespec="seconds")
    rec = boards.get(mac, {"mac": mac, "first": now})
    rec.update({"updated": now, **({"ip": b.ip} if b.ip else {}), **({"version": b.version[:64]} if b.version else {}),
                **({"note": b.note[:200]} if b.note else {})})
    boards[mac] = rec
    BOARDS_FILE.write_text(json.dumps(boards, ensure_ascii=False, indent=1), encoding="utf-8")
    return rec


@app.delete("/api/boards/{mac}")
def delete_board(mac: str):
    boards = read_boards()
    if boards.pop(mac.strip().upper(), None) is not None:
        BOARDS_FILE.write_text(json.dumps(boards, ensure_ascii=False, indent=1), encoding="utf-8")
    return {"ok": True}


# ---------- problem reports from technicians ----------
@app.post("/api/reports")
async def add_report(request: Request):
    raw = await request.body()
    if len(raw) > 400 * 1024:
        raise HTTPException(413, "report too large")
    try:
        rep = json.loads(raw)
        assert isinstance(rep, dict)
    except Exception:
        raise HTTPException(400, "bad report")
    now = dt.datetime.now()
    rid = now.strftime("%Y%m%d-%H%M%S-") + secrets.token_hex(3)
    rep = {"id": rid, "time": now.isoformat(timespec="seconds"),
           "client": request.headers.get("x-forwarded-for", request.client.host if request.client else ""), **rep}
    rep["id"] = rid
    (REPORT_DIR / f"{rid}.json").write_text(json.dumps(rep, ensure_ascii=False), encoding="utf-8")
    for old in sorted(REPORT_DIR.glob("*.json"))[:-MAX_REPORTS]:
        old.unlink(missing_ok=True)
    return {"ok": True, "id": rid}


@app.get("/api/reports")
def list_reports():
    out = []
    for f in sorted(REPORT_DIR.glob("*.json"), reverse=True)[:200]:
        try:
            out.append(json.loads(f.read_text(encoding="utf-8")))
        except ValueError:
            pass
    return out


@app.delete("/api/reports/{rid}")
def delete_report(rid: str):
    if not re.fullmatch(r"[\w-]{1,64}", rid):
        raise HTTPException(400, "bad id")
    (REPORT_DIR / f"{rid}.json").unlink(missing_ok=True)
    return {"ok": True}


app.mount("/firmware", StaticFiles(directory=OUT_DIR, check_dir=False), name="firmware")
app.mount("/", StaticFiles(directory=WEB_DIR, html=True), name="web")

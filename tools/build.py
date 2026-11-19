#!/usr/bin/env python3
"""Compile firmware/firmware.ino for every target in project.json and publish
the results to web/firmware/:

  <target>/bootloader.bin, partitions.bin, boot_app0.bin, app.bin
      flashed separately by the USB installer page, so the NVS area that holds
      saved settings (Static IP, DNS) is never overwritten
  <target>.app.bin + <target>.version
      what boards download when someone presses "อัปเดตโปรแกรม" over LAN
  firmware.json
      what the installer page reads

Needs arduino-cli on PATH.

    python tools/build.py               # install core + libraries, compile, package
    python tools/build.py --skip-setup  # compile + package only (faster re-runs)
"""
import argparse
import datetime as dt
import glob
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SKETCH = ROOT / "firmware"
# Short path outside the project: Windows compilers fail on very long paths.
BUILD = Path(tempfile.gettempdir()) / "esp32-oneclick-build"
OUT = ROOT / "web" / "firmware"
CORE_URL = "https://espressif.github.io/arduino-esp32/package_esp32_index.json"

# target -> (chip name reported by esptool-js, bootloader offset)
TARGETS = {
    "esp32":   ("ESP32",    0x1000),
    "esp32s2": ("ESP32-S2", 0x1000),
    "esp32s3": ("ESP32-S3", 0x0),
    "esp32c3": ("ESP32-C3", 0x0),
    "esp32c6": ("ESP32-C6", 0x0),
}
# Offsets of the default Arduino partition table (4MB with OTA). NVS lives at
# 0x9000-0xDFFF and is deliberately left out so settings survive a reinstall.
PARTITIONS_OFFSET = 0x8000
OTADATA_OFFSET = 0xE000
APP_OFFSET = 0x10000


def run(*args, capture=False):
    print("$ " + " ".join(args), flush=True)
    res = subprocess.run(args, check=True, text=True,
                         stdout=subprocess.PIPE if capture else None)
    return res.stdout if capture else None


def setup(core_version):
    run("arduino-cli", "core", "update-index", "--additional-urls", CORE_URL)
    core = "esp32:esp32" if core_version in ("", "latest") else f"esp32:esp32@{core_version}"
    run("arduino-cli", "core", "install", core, "--additional-urls", CORE_URL)
    libs_file = SKETCH / "libraries.txt"
    if libs_file.exists():
        for line in libs_file.read_text(encoding="utf-8").splitlines():
            line = line.strip()
            if line and not line.startswith("#"):
                run("arduino-cli", "lib", "install", line)


def compile_sketch(fqbn, out_dir):
    """Compile, echo the compiler output, and turn errors into GitHub annotations
    (the web IDE reads these to mark the failing lines in its editor)."""
    args = ["arduino-cli", "compile", "--fqbn", fqbn, "--build-path", str(out_dir), str(SKETCH)]
    print("$ " + " ".join(args), flush=True)
    res = subprocess.run(args, text=True, encoding="utf-8", errors="replace",
                         stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    print(res.stdout, flush=True)
    if res.returncode == 0:
        return
    if os.environ.get("GITHUB_ACTIONS"):
        seen = set()
        for m in re.finditer(r"firmware[\\/]([\w.-]+\.(?:ino|h|cpp|c)):(\d+):(\d+): (?:fatal )?error: (.+)",
                             res.stdout):
            key = m.group(0)
            if key in seen:
                continue
            seen.add(key)
            msg = m.group(4).replace("%", "%25").replace("\r", "").replace("\n", " ")
            print(f"::error file=firmware/{m.group(1)},line={m.group(2)},col={m.group(3)}::{msg}", flush=True)
    sys.exit(f"Compile failed for {fqbn}")


def boot_app0():
    cfg = json.loads(run("arduino-cli", "config", "dump", "--format", "json", capture=True))
    data_dir = (cfg.get("config", cfg).get("directories", {}).get("data")
                or os.path.expanduser("~/.arduino15"))
    hits = sorted(glob.glob(os.path.join(data_dir, "packages", "esp32", "hardware", "esp32",
                                         "*", "tools", "partitions", "boot_app0.bin")))
    if not hits:
        sys.exit("boot_app0.bin not found; is the esp32 core installed?")
    return hits[-1]


def update_base(project):
    """Web folder boards download updates from, e.g. https://me.github.io/gate/firmware/"""
    url = project.get("updateUrl", "").strip()
    repo = os.environ.get("GITHUB_REPOSITORY", "")  # "owner/name", set by GitHub Actions
    if not url and repo:
        owner, name = repo.split("/", 1)
        site = f"https://{owner.lower()}.github.io/"
        url = site if name.lower() == f"{owner.lower()}.github.io" else f"{site}{name}/"
    if url and not url.endswith("/"):
        url += "/"
    return url + "firmware/" if url else ""


def version_string():
    stamp = dt.datetime.now(dt.timezone.utc).strftime("%Y.%m.%d-%H%M")
    sha = os.environ.get("GITHUB_SHA", "")[:7]
    return f"{stamp}-{sha}" if sha else stamp


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--skip-setup", action="store_true")
    args = ap.parse_args()

    project = json.loads((ROOT / "project.json").read_text(encoding="utf-8"))
    targets = project.get("targets", ["esp32"])
    unknown = [t for t in targets if t not in TARGETS]
    if unknown:
        sys.exit(f"Unknown target(s) in project.json: {unknown}. Use: {list(TARGETS)}")

    if not args.skip_setup:
        setup(project.get("esp32Core", "latest"))

    version = version_string()
    base = update_base(project)
    (SKETCH / "version.h").write_text(
        "// Generated by tools/build.py - do not edit.\n"
        f'#define FW_VERSION "{version}"\n'
        f'#define FW_UPDATE_BASE "{base}"\n', encoding="utf-8")
    print(f"Version {version}; boards will update from: {base or '(disabled - no updateUrl)'}")

    shutil.rmtree(OUT, ignore_errors=True)
    OUT.mkdir(parents=True)
    ota_data = boot_app0()
    builds = {}
    for target in targets:
        out_dir = BUILD / target
        compile_sketch(f"esp32:esp32:{target}", out_dir)
        chip, boot_offset = TARGETS[target]
        dest = OUT / target
        dest.mkdir()
        parts = [
            ("bootloader.bin", out_dir / "firmware.ino.bootloader.bin", boot_offset),
            ("partitions.bin", out_dir / "firmware.ino.partitions.bin", PARTITIONS_OFFSET),
            ("boot_app0.bin", Path(ota_data), OTADATA_OFFSET),  # makes the board boot the new app
            ("app.bin", out_dir / "firmware.ino.bin", APP_OFFSET),
        ]
        for name, src, _ in parts:
            shutil.copyfile(src, dest / name)
        builds[chip] = {"parts": [{"path": f"{target}/{name}", "offset": off} for name, _, off in parts]}
        # Files for update-over-LAN
        shutil.copyfile(out_dir / "firmware.ino.bin", OUT / f"{target}.app.bin")
        (OUT / f"{target}.version").write_text(version + "\n", encoding="utf-8")

    manifest = {
        "name": project.get("name", "ESP32 firmware"),
        "title": project.get("title", ""),
        "version": version,
        "commit": os.environ.get("GITHUB_SHA", ""),
        "repo": os.environ.get("GITHUB_REPOSITORY", ""),
        "beforeInstall": project.get("beforeInstall", []),
        "afterInstall": project.get("afterInstall", []),
        "builds": builds,
    }
    (OUT / "firmware.json").write_text(json.dumps(manifest, ensure_ascii=False, indent=2), encoding="utf-8")
    print(f"\nPackaged {len(builds)} target(s) into {OUT.relative_to(ROOT)}: {', '.join(builds)}")


if __name__ == "__main__":
    main()

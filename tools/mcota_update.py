import json
import os
import stat
import sys

import mcota_net
from mcota_version import VERSION

REPO = "funkmesh-deutschland/meshcore-ota"
LATEST_URL = f"https://api.github.com/repos/{REPO}/releases/latest"
TIMEOUT = 6.0


def numbers(text):
    return [int(part) for part in text.lstrip("v").split(".") if part.isdigit()]


def asset_name():
    return "meshcore-ota-windows.exe" if os.name == "nt" else "meshcore-ota-linux"


def check(timeout=TIMEOUT):
    try:
        data = json.loads(mcota_net.fetch(LATEST_URL, timeout, {"Accept": "application/vnd.github+json"}).decode())
    except Exception:
        return None
    version = data.get("tag_name", "")
    url = next((asset["browser_download_url"] for asset in data.get("assets", []) if asset.get("name") == asset_name()), None)
    if not version or numbers(version) <= numbers(VERSION):
        return None
    return {"version": version, "url": url, "notes": data.get("body") or ""}


def cleanup():
    if not getattr(sys, "frozen", False):
        return
    old = sys.executable + ".old"
    try:
        if os.path.exists(old):
            os.remove(old)
    except OSError:
        pass


def install(found, timeout=60.0):
    if not getattr(sys, "frozen", False):
        raise SystemExit("The self-update only works for the packaged program file. When running from source, please update with git.")
    if not found.get("url"):
        raise SystemExit(f"Version {found['version']} contains no file for this operating system.")
    target = sys.executable
    fresh = target + ".new"
    try:
        with mcota_net.open_url(found["url"], timeout) as response, open(fresh, "wb") as fh:
            while True:
                chunk = response.read(1 << 16)
                if not chunk:
                    break
                fh.write(chunk)
    except Exception as error:
        if os.path.exists(fresh):
            os.remove(fresh)
        raise SystemExit(f"The download failed: {mcota_net.reason(error)}.")
    if os.path.getsize(fresh) < 1 << 20:
        os.remove(fresh)
        raise SystemExit("The downloaded file is incomplete.")
    os.chmod(fresh, os.stat(fresh).st_mode | stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH)
    old = target + ".old"
    if os.path.exists(old):
        os.remove(old)
    os.rename(target, old)
    try:
        os.rename(fresh, target)
    except OSError:
        os.rename(old, target)
        raise
    if os.name != "nt":
        os.remove(old)
    return target

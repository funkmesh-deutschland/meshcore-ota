import glob
import hashlib
import io
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import tarfile
import time
import zipfile

import mcota_net

SOURCE_URL = "https://github.com/meshcore-dev/MeshCore/archive/{commit}.tar.gz"
PAGE = 4096

TARGETS = {
    "Xiao_nrf52_repeater": {
        "board": "xiao_nrf52", "env": "Xiao_nrf52_repeater_mcota", "env_test": "Xiao_nrf52_repeater_mcota_test",
        "layout": "S140_V7", "define": "XIAO_NRF52", "loader": 0x27000, "loader_pages": 1, "compact": False,
        "status": 0xEA000, "status_pages": 2, "device_type": 0x0052, "sd_req": 0x0123, "family": 0xADA52840,
    },
    "RAK_4631_repeater": {
        "board": "rak4631", "env": "RAK_4631_repeater_mcota", "env_test": "RAK_4631_repeater_mcota_test",
        "layout": "S140_V6", "define": "RAK4631", "loader": 0x26000, "loader_pages": 3, "compact": True,
        "status": 0xEA000, "status_pages": 2, "device_type": 0x0052, "sd_req": 0x00B6, "family": 0xADA52840,
    },
}
for _target in TARGETS.values():
    _target["app"] = _target["loader"] + _target["loader_pages"] * PAGE
    _target["slot_pages"] = (_target["status"] - _target["app"]) // PAGE


def parse_name(path):
    name = os.path.basename(path)
    match = re.match(r"^(?:[0-9a-f]{8}-)?(?P<target>[A-Za-z0-9_]+?)-(?P<version>v\d+(?:\.\d+)*)-(?P<commit>[0-9a-f]{7,40})\.zip$", name)
    if not match:
        return None
    return match.group("target"), match.group("version"), match.group("commit")


def resources():
    base = getattr(sys, "_MEIPASS", None)
    if base and os.path.isdir(os.path.join(base, "sources")):
        return os.path.join(base, "sources")
    return os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def say(text):
    print(text, flush=True)


def run(command, cwd=None, env=None, log=None):
    with open(log, "ab") if log else open(os.devnull, "wb") as out:
        result = subprocess.run(command, cwd=cwd, env=env, stdout=out, stderr=subprocess.STDOUT)
    return result.returncode


def find_pio(tools):
    override = os.environ.get("MCOTA_PIO")
    if override:
        return override
    found = shutil.which("pio") or shutil.which("platformio")
    if found:
        return found
    venv = os.path.join(tools, "venv")
    pio = os.path.join(venv, "Scripts" if os.name == "nt" else "bin", "pio.exe" if os.name == "nt" else "pio")
    if os.path.exists(pio):
        return pio
    python = None if getattr(sys, "frozen", False) else sys.executable
    python = python or shutil.which("python3") or shutil.which("python") or shutil.which("py")
    if not python:
        raise SystemExit("Building the firmware needs Python 3 on this computer once (python.org). "
                         "With it meshcore-ota sets up the build tool PlatformIO by itself.")
    say("Setting up the build tool PlatformIO (once, a few minutes) ...")
    log = os.path.join(tools, "setup.log")
    if run([python, "-m", "venv", venv], log=log) != 0:
        raise SystemExit(f"Python could not create its own environment. Details in {log}")
    pip = os.path.join(venv, "Scripts" if os.name == "nt" else "bin", "pip")
    if run([pip, "install", "platformio"], log=log) != 0:
        raise SystemExit(f"PlatformIO could not be set up. Is there an internet connection? Details in {log}")
    return pio


def fetch_source(commit, tools):
    target = os.path.join(tools, "sources", commit)
    if os.path.isdir(target):
        return target
    say(f"Downloading the MeshCore source code, commit {commit} ...")
    try:
        data = mcota_net.fetch(SOURCE_URL.format(commit=commit), 120)
    except Exception as error:
        raise SystemExit(f"The MeshCore source code could not be downloaded: {mcota_net.reason(error)}.")
    temp = target + ".part"
    shutil.rmtree(temp, ignore_errors=True)
    os.makedirs(temp)
    with tarfile.open(fileobj=io.BytesIO(data), mode="r:gz") as archive:
        root = os.path.realpath(temp)
        for member in archive.getmembers():
            path = os.path.realpath(os.path.join(temp, member.name))
            if not path.startswith(root + os.sep) or member.issym() or member.islnk():
                continue
            archive.extract(member, temp)
    inner = [d for d in os.listdir(temp) if os.path.isdir(os.path.join(temp, d))]
    if len(inner) != 1:
        raise SystemExit("The MeshCore archive does not have the expected layout.")
    os.rename(os.path.join(temp, inner[0]), target)
    shutil.rmtree(temp, ignore_errors=True)
    return target


def apply_patch(root, text):
    files, current, hunk = [], None, None
    for line in text.splitlines():
        if line.startswith("diff --git "):
            current = {"path": None, "hunks": []}
            files.append(current)
            hunk = None
        elif line.startswith("+++ b/") and current is not None:
            current["path"] = line[6:]
        elif line.startswith("@@") and current is not None:
            start = int(re.match(r"@@ -(\d+)", line).group(1))
            hunk = {"start": start, "old": [], "new": []}
            current["hunks"].append(hunk)
        elif hunk is not None and line[:1] in (" ", "+", "-") and not line.startswith(("+++", "---")):
            if line[0] in " -":
                hunk["old"].append(line[1:])
            if line[0] in " +":
                hunk["new"].append(line[1:])
        elif hunk is not None and line == "":
            hunk["old"].append("")
            hunk["new"].append("")
    for entry in files:
        path = os.path.join(root, entry["path"])
        with open(path, encoding="utf-8") as fh:
            lines = fh.read().split("\n")
        shift = 0
        for hunk in entry["hunks"]:
            old = hunk["old"]
            while old and old[-1] == "" and hunk["new"] and hunk["new"][-1] == "":
                old = old[:-1]
                hunk["new"] = hunk["new"][:-1]
            guess = hunk["start"] - 1 + shift
            place = None
            for distance in range(0, len(lines) + 1):
                for candidate in (guess - distance, guess + distance):
                    if 0 <= candidate <= len(lines) - len(old) and lines[candidate:candidate + len(old)] == old:
                        place = candidate
                        break
                if place is not None:
                    break
            if place is None:
                raise SystemExit(f"The MeshCore-OTA patch does not fit this MeshCore version (file {entry['path']}). "
                                 "This version is not supported yet.")
            lines[place:place + len(old)] = hunk["new"]
            shift += len(hunk["new"]) - len(old)
        with open(path, "w", encoding="utf-8", newline="\n") as fh:
            fh.write("\n".join(lines))


TOOLCHAIN = "platformio/toolchain-gccarmnoneeabi@"
TOOLCHAIN_DEFAULT = "1.70201.0"


def pin_compiler(work, tools, switch):
    path = os.path.join(work, "platformio.ini")
    with open(path, encoding="utf-8") as fh:
        text = fh.read()
    match = re.search(re.escape(TOOLCHAIN) + r"(\S+)", text)
    wanted = match.group(1) if match else TOOLCHAIN_DEFAULT
    record = os.path.join(tools, "compiler.txt")
    pinned = open(record).read().strip() if os.path.exists(record) else None
    if pinned is None or switch:
        with open(record, "w") as fh:
            fh.write(wanted + "\n")
        if pinned and pinned != wanted:
            say(f"Switching the compiler from {pinned} to {wanted}. This makes the next update large.")
        return wanted
    if pinned != wanted:
        say(f"MeshCore uses the compiler {wanted} for this version. Staying with {pinned} so the update stays small.")
        if match:
            text = text.replace(match.group(0), TOOLCHAIN + pinned)
        else:
            text = re.sub(r"(\[nrf52_base\][^\[]*?platform_packages\s*=[^\n]*\n)", lambda m: m.group(1) + "  " + TOOLCHAIN + pinned + "\n", text, count=1, flags=re.S)
            if TOOLCHAIN + pinned not in text:
                raise SystemExit("The compiler could not be pinned for this MeshCore version.")
        with open(path, "w", encoding="utf-8", newline="\n") as fh:
            fh.write(text)
    return pinned


def crc16(data):
    crc = 0xFFFF
    for value in data:
        crc = ((crc >> 8) | (crc << 8)) & 0xFFFF
        crc ^= value
        crc ^= (crc & 0xFF) >> 4
        crc ^= (crc << 12) & 0xFFFF
        crc ^= ((crc & 0xFF) << 5) & 0xFFFF
    return crc


def write_dfu_zip(path, image, target):
    check = crc16(image)
    init = struct.pack("<HHIHHH", target["device_type"], 0xFFFF, 0xFFFFFFFF, 1, target["sd_req"], check)
    manifest = {"manifest": {"application": {"bin_file": "firmware.bin", "dat_file": "firmware.dat", "init_packet_data": {
        "application_version": 0xFFFFFFFF, "device_revision": 0xFFFF, "device_type": target["device_type"],
        "firmware_crc16": check, "softdevice_req": [target["sd_req"]]}}, "dfu_version": 0.5}}
    with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED) as archive:
        archive.writestr("firmware.bin", image)
        archive.writestr("firmware.dat", init)
        archive.writestr("manifest.json", json.dumps(manifest, indent=4))


def write_uf2(path, parts, family):
    blocks = []
    for addr, data in parts:
        data += bytes([0xFF]) * (-len(data) % 256)
        for pos in range(0, len(data), 256):
            blocks.append((addr + pos, data[pos:pos + 256]))
    out = bytearray()
    for number, (addr, chunk) in enumerate(blocks):
        out += struct.pack("<IIIIIIII", 0x0A324655, 0x9E5D5157, 0x00002000, addr, 256, number, len(blocks), family)
        out += chunk + bytes(476 - 256) + struct.pack("<I", 0x0AB16F30)
    with open(path, "wb") as fh:
        fh.write(out)


def toolchain(pio_env):
    homes = [pio_env.get("PLATFORMIO_CORE_DIR"), os.path.join(os.path.expanduser("~"), ".platformio")]
    for home in filter(None, homes):
        for found in sorted(glob.glob(os.path.join(home, "packages", "toolchain-gccarmnoneeabi*", "bin", "arm-none-eabi-gcc*"))):
            if os.path.basename(found).split(".")[0] == "arm-none-eabi-gcc":
                return found[:found.rindex("gcc")]
    raise SystemExit("The compiler for the loader was not found.")


REFS_URL = "https://github.com/meshcore-dev/MeshCore.git/info/refs?service=git-upload-pack"
REFS_KEEP = 3600
releases_error = ""


def natural(text):
    return [int(part) if part.isdigit() else part.lower() for part in re.split(r"(\d+)", text)]


def parse_refs(text):
    tags = {}
    for sha, name, peeled in re.findall(r"([0-9a-f]{40}) refs/tags/([^\s^\x00]+)(\^\{\})?", text):
        if peeled or name not in tags:
            tags[name] = sha
    return sorted(tags.items())


def cached_tags(cache):
    if not os.path.exists(cache):
        return None
    try:
        with open(cache) as fh:
            data = json.load(fh)
    except ValueError:
        return None
    if isinstance(data, dict):
        return [tuple(item) for item in data.get("tags", [])]
    return [(entry.get("name", ""), entry.get("commit", {}).get("sha", "")) for entry in data]


def releases(tools, kind="repeater", fresh=True):
    global releases_error
    cache = os.path.join(tools, "versions.json")
    tags = cached_tags(cache)
    recent = tags is not None and time.time() - os.path.getmtime(cache) < REFS_KEEP
    releases_error = ""
    if fresh and not recent:
        try:
            loaded = parse_refs(mcota_net.fetch(REFS_URL, 30).decode(errors="replace"))
            if not loaded:
                raise ValueError("the answer contained no versions")
            tags = loaded
            os.makedirs(tools, exist_ok=True)
            with open(cache, "w") as fh:
                json.dump({"tags": tags}, fh)
        except Exception as error:
            releases_error = mcota_net.reason(error)
    found = []
    for name, sha in tags or []:
        match = re.match(r"^" + kind + r"-(v\d+(?:\.\d+)*)$", name)
        if match:
            found.append((match.group(1), sha[:7]))
    return sorted(found, key=lambda item: natural(item[0]))


def releases_problem():
    return "The list of MeshCore versions could not be loaded: " + (releases_error or "no versions found") + "."


LAYOUTS = {
    "nrf52840_s140_v6.ld": ("S140_V6", 0x26000, 0x00B6, "boards/nrf52840_s140_v6_mcota_compact.ld"),
    "nrf52840_s140_v7.ld": ("S140_V7", 0x27000, 0x0123, "boards/nrf52840_s140_v7_mcota_compact.ld"),
}


def board_id(name):
    short = re.sub(r"_repeater$", "", name, flags=re.I).lower()
    if len(short) > 15:
        short = short[:10] + "_" + hashlib.sha256(name.encode()).hexdigest()[:4]
    return short


def core_dir():
    return os.environ.get("PLATFORMIO_CORE_DIR") or os.path.join(os.path.expanduser("~"), ".platformio")


def board_file(source, board):
    for path in [os.path.join(source, "boards", board + ".json")] + glob.glob(os.path.join(core_dir(), "platforms", "*", "boards", board + ".json")):
        if os.path.exists(path):
            with open(path, encoding="utf-8") as fh:
                return json.load(fh)
    return None


def catalog(tools, commit):
    cache = os.path.join(tools, f"devices-{commit}.json")
    if os.path.exists(cache):
        with open(cache) as fh:
            found = json.load(fh)
        if all("scripts" in entry and (entry["ld"] or entry["mcu"] != "nrf52840") for entry in found.values()):
            return found
    pio = find_pio(tools)
    source = fetch_source(commit, tools)
    result = subprocess.run([pio, "project", "config", "--json-output"], cwd=source, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
    if result.returncode != 0:
        raise SystemExit("PlatformIO could not read the MeshCore device list.")
    found = {}
    for section, options in json.loads(result.stdout.decode()):
        name = section[4:]
        if not section.startswith("env:") or not name.lower().endswith("repeater"):
            continue
        options = dict(options)
        board = options.get("board") or ""
        data = board_file(source, board) or {}
        platform = str(options.get("platform") or "")
        kind = "nrf52" if "nordicnrf52" in platform else "esp32" if "espressif32" in platform else "rp2040" if "raspberrypi" in platform else "stm32" if "ststm32" in platform else "other"
        mcu = data.get("build", {}).get("mcu") or ("nrf52840" if kind == "nrf52" else kind)
        ld = options.get("board_build.ldscript") or data.get("build", {}).get("arduino", {}).get("ldscript") or ""
        scripts = options.get("extra_scripts") or []
        scripts = [scripts] if isinstance(scripts, str) else list(scripts)
        found[name] = {"kind": kind, "mcu": mcu, "ld": os.path.basename(str(ld)), "scripts": [str(item) for item in scripts]}
    os.makedirs(tools, exist_ok=True)
    with open(cache, "w") as fh:
        json.dump(found, fh)
    return found


def support(name, entry):
    if name in TARGETS:
        return "ready", "supported"
    if entry["kind"] == "nrf52" and entry["mcu"] == "nrf52840":
        if entry["ld"] in LAYOUTS or not entry["ld"]:
            return "ready", "supported"
        return "missing", "custom memory layout, not supported"
    if entry["kind"] == "esp32":
        return "ready", "supported"
    return "missing", "this processor is not supported"


def saved_targets(folder):
    found = {}
    for path in glob.glob(os.path.join(folder, "firmware", "*.json")):
        with open(path) as fh:
            target = json.load(fh)
        if "name" in target and "board" in target:
            found[target["name"]] = target
    return found


def find_target(name, folder, commit):
    if name in TARGETS:
        return dict(TARGETS[name], name=name)
    saved = saved_targets(folder)
    if name in saved:
        return saved[name]
    tools = os.path.join(folder, "cache")
    entry = catalog(tools, commit).get(name)
    if entry is None:
        raise SystemExit(f"{name} does not exist as a repeater in this MeshCore version. 'meshcore-ota devices' shows the list.")
    state, text = support(name, entry)
    if state == "missing":
        raise SystemExit(f"{name}: {text}.")
    if entry["kind"] == "esp32":
        return {"name": name, "board": board_id(name), "env": name + "_mcota", "env_test": name + "_mcota_test", "kind": "esp32", "mcu": entry["mcu"],
                "scripts": entry.get("scripts", []), "base_env": name, "app": 0, "max_pages": ESP_MAX_PAGES}
    if entry["ld"] not in LAYOUTS:
        raise SystemExit(f"{name}: the memory layout of this device could not be determined. Please build another nRF52 device first, then try again.")
    layout, loader, sd_req, ldscript = LAYOUTS[entry["ld"]]
    target = {"name": name, "board": board_id(name), "env": name + "_mcota", "env_test": name + "_mcota_test", "layout": layout, "define": "GENERIC",
              "loader": loader, "loader_pages": 3, "compact": True, "status": 0xEA000, "status_pages": 2, "device_type": 0x0052, "sd_req": sd_req,
              "family": 0xADA52840, "ldscript": ldscript, "base_env": name}
    target["app"] = target["loader"] + target["loader_pages"] * PAGE
    target["slot_pages"] = (target["status"] - target["app"]) // PAGE
    return target


ESP_MAX_PAGES = 448
ESP_TAIL_MIN = 0x4000
ESP_TAIL_MAX = 0x100000
ESP_SCRIPT = "src/helpers/mcota/link_order.py"


def esp_environment(target):
    base = target["base_env"]
    scripts = "".join(f"  {item}\n" for item in target.get("scripts", []) if ESP_SCRIPT not in item)
    return f"""

[env:{target['env']}]
extends = env:{base}
build_flags =
  ${{env:{base}.build_flags}}
  -D MCOTA_ENABLED
  -D MCOTA_MAX_PAGES={target['max_pages']}u
  -D MCOTA_BOARD='"{target['board']}"'
  -I src/helpers/mcota
build_src_filter = ${{env:{base}.build_src_filter}}
  +<helpers/mcota/*.cpp>
  +<helpers/mcota/*.c>
extra_scripts =
{scripts}  post:{ESP_SCRIPT}

[env:{target['env_test']}]
extends = env:{target['env']}
build_flags =
  ${{env:{target['env']}.build_flags}}
  -D MCOTA_TEST
  -D MCOTA_BUDGET_PPM=50000
"""


def esp_slots(path):
    with open(path, "rb") as fh:
        table = fh.read()
    sizes = []
    for pos in range(0, len(table) - 31, 32):
        magic, kind, subtype, offset, size = struct.unpack_from("<HBBII", table, pos)
        if magic != 0x50AA:
            break
        if kind == 0 and 0x10 <= subtype < 0x20:
            sizes.append(size)
    return sizes


def esp_slot_pages(sizes, max_pages):
    size = min(sizes)
    tail = min(max(size - max_pages * PAGE, ESP_TAIL_MIN), ESP_TAIL_MAX)
    return min((size - tail) // PAGE, max_pages)


def install_files(target, folder, name, label):
    stem = os.path.join(folder, "install", f"{name}-{label}-mcota")
    if target.get("kind") == "esp32":
        return [("USB with an ESP flasher, from address 0", stem + "-merged.bin"), ("MeshCore Wi-Fi update (start ota)", stem + ".bin")]
    return [("Bluetooth (DFU app on the phone) or USB serial", stem + ".zip"), ("USB drive (double click on reset)", stem + ".uf2")]


def add_environment(work, target):
    base = target["base_env"]
    text = esp_environment(target) if target.get("kind") == "esp32" else f"""

[env:{target['env']}]
extends = env:{base}
board_build.ldscript = {target['ldscript']}
build_flags =
  ${{env:{base}.build_flags}}
  -D MCOTA_ENABLED
  -D MCOTA_LAYOUT_{target['layout']}
  -D MCOTA_BOARD='"{target['board']}"'
  -I src/helpers/mcota
build_src_filter = ${{env:{base}.build_src_filter}}
  +<helpers/mcota/*.cpp>
  +<helpers/mcota/*.c>

[env:{target['env_test']}]
extends = env:{target['env']}
build_flags =
  ${{env:{target['env']}.build_flags}}
  -D MCOTA_TEST
  -D MCOTA_BUDGET_PPM=50000
"""
    with open(os.path.join(work, "platformio.ini"), "a", encoding="utf-8", newline="\n") as fh:
        fh.write(text)


def target_for_board(board, folder=None):
    for name, target in TARGETS.items():
        if target["board"] == board:
            return name
    for name, target in (saved_targets(folder) if folder else {}).items():
        if target["board"] == board:
            return name
    return None


def board_base(board, folder=None):
    for target in list(TARGETS.values()) + list((saved_targets(folder) if folder else {}).values()):
        if target["board"] == board:
            return target["app"]
    return 0


SETTINGS_FILE = "settings.json"
VERSIONS_FILE = "releases.json"
BLOCKED_KEYS = ("name", "password", "lat", "lon", "prv.key", "owner")
KNOWN_SETTINGS = [
    ("tx", "transmit power in dBm"),
    ("advert.interval", "advert to neighbours, interval in minutes (0 means off)"),
    ("flood.advert.interval", "advert to the whole mesh, interval in hours (0 means off)"),
    ("flood.max", "highest number of hops for forwarded flood packets"),
    ("flood.max.unscoped", "highest number of hops for flood packets without a region"),
    ("flood.max.advert", "highest number of hops for forwarded adverts"),
    ("af", "airtime factor (pause after each transmission)"),
    ("rxdelay", "delay before forwarding, depending on the signal"),
    ("txdelay", "random delay when forwarding flood packets"),
    ("direct.txdelay", "random delay when forwarding on a fixed route"),
    ("multi.acks", "additional acknowledgements (0 or 1)"),
    ("path.hash.mode", "length of the hops in routes (0 means 1 byte, 1 means 2 bytes, 2 means 3 bytes)"),
    ("loop.detect", "loop detection (off, minimal, moderate, strict)"),
    ("agc.reset.interval", "reset the receiver gain, interval in seconds (0 means off)"),
]


def firmware_file(folder, board, label):
    return os.path.join(folder, "firmware", f"{board}-{label}.bin")


def firmware_labels(folder, board):
    found = []
    for path in glob.glob(os.path.join(folder, "firmware", glob.escape(board) + "-v*.bin")):
        label = os.path.basename(path)[len(board) + 1:-4]
        if parse_label(label):
            found.append(label)
    return found


def firmware_boards(folder):
    found = set()
    for path in glob.glob(os.path.join(folder, "firmware", "*.bin")):
        match = re.fullmatch(r"(.+)-(v[\d.]+(?:\+\d+)?(?:-test)?)\.bin", os.path.basename(path))
        if match:
            found.add(match.group(1))
    return found


def key_index(folder):
    path = os.path.join(folder, "firmware", "keys.json")
    if not os.path.exists(path):
        return {}
    with open(path, encoding="utf-8") as fh:
        return json.load(fh)


def note_key(folder, board, label):
    key = public_key(folder)
    data = key_index(folder)
    data[f"{board}-{label}"] = hashlib.sha256(key).digest()[:8].hex() if key else ""
    os.makedirs(os.path.join(folder, "firmware"), exist_ok=True)
    with open(os.path.join(folder, "firmware", "keys.json"), "w", encoding="utf-8") as fh:
        json.dump(data, fh, indent=2)


def retired_keys(folder):
    path = os.path.join(folder, "keys", "retired.json")
    if not os.path.exists(path):
        return []
    with open(path, encoding="utf-8") as fh:
        return json.load(fh)


def settings_path(folder):
    return os.path.join(folder, SETTINGS_FILE)


def public_key(folder):
    path = os.path.join(folder, "keys", "public.txt")
    if not os.path.exists(path):
        return None
    match = re.search(r"Public key: ([0-9a-f]{64})", open(path, encoding="utf-8").read())
    return bytes.fromhex(match.group(1)) if match else None


def check_preset(preset):
    name = preset.get("name", "")
    if not re.fullmatch(r"[a-z0-9_-]{1,15}", name):
        raise SystemExit(f"Preset '{name}': the name needs 1 to 15 characters from a to z, digits, - and _.")
    for key, value in preset.get("values", {}).items():
        if not re.fullmatch(r"[a-z0-9._]{1,32}", key) or any(part in key for part in BLOCKED_KEYS):
            raise SystemExit(f"Preset '{name}': '{key}' does not belong in a preset. Name, location and passwords stay per device.")
        if not re.fullmatch(r"[A-Za-z0-9.,:_+-]{1,40}", str(value)):
            raise SystemExit(f"Preset '{name}': the value for '{key}' contains characters that are not allowed.")
        if len(f"set {key} {value}") > 90:
            raise SystemExit(f"Preset '{name}': '{key}' is too long.")


def preset_commands(preset):
    return "\n".join(f"set {key} {value}" for key, value in preset.get("values", {}).items())


def load_settings(folder):
    path = settings_path(folder)
    data = {"revision": 0, "digest": "", "presets": []}
    if os.path.exists(path):
        with open(path, encoding="utf-8") as fh:
            data.update(json.load(fh))
    names = [preset.get("name") for preset in data["presets"]]
    if len(set(names)) != len(names):
        raise SystemExit("Two presets have the same name.")
    if len(names) > 8:
        raise SystemExit("At most 8 presets.")
    for preset in data["presets"]:
        check_preset(preset)
    key = public_key(folder)
    image = hashlib.sha256(json.dumps([key.hex() if key else "", [entry["public"] for entry in retired_keys(folder)],
                                       [(p["name"], preset_commands(p)) for p in data["presets"]]]).encode()).hexdigest()[:16]
    if image != data.get("digest"):
        data["revision"] = int(data.get("revision", 0)) + 1
        data["digest"] = image
        save_settings(folder, data)
    return data


def save_settings(folder, data):
    os.makedirs(folder, exist_ok=True)
    with open(settings_path(folder), "w", encoding="utf-8") as fh:
        json.dump(data, fh, indent=2, ensure_ascii=False)


def config_header(folder):
    data = load_settings(folder)
    key = public_key(folder)
    lines = ["#pragma once", ""]
    if key:
        lines.append("#define MCOTA_BUILTIN_KEY {" + ", ".join(f"0x{value:02x}" for value in key) + "}")
    retired = [bytes.fromhex(entry["public"]) for entry in retired_keys(folder)][-4:]
    if retired:
        lines.append(f"#define MCOTA_RETIRED_COUNT {len(retired)}")
        lines.append("#define MCOTA_RETIRED_KEYS {" + ", ".join("{" + ", ".join(f"0x{value:02x}" for value in key) + "}" for key in retired) + "}")
    lines.append(f"#define MCOTA_PRESET_COUNT {len(data['presets'])}")
    if data["presets"]:
        quote = lambda text: '"' + text.replace("\\", "\\\\").replace('"', '\\"').replace("\n", "\\n") + '"'
        lines.append("#define MCOTA_PRESET_NAMES {" + ", ".join(quote(p["name"]) for p in data["presets"]) + "}")
        lines.append("#define MCOTA_PRESET_COMMANDS {" + ", ".join(quote(preset_commands(p)) for p in data["presets"]) + "}")
    return "\n".join(lines) + "\n"


def stand_label(version, folder, test=False):
    return f"{version}+{load_settings(folder)['revision']}" + ("-test" if test else "")


def parse_label(label):
    match = re.fullmatch(r"(v[\d.]+)(?:\+(\d+))?(-test)?", label)
    if not match:
        return None
    return match.group(1), int(match.group(2) or 0), bool(match.group(3))


def allowed_versions(folder):
    path = os.path.join(folder, VERSIONS_FILE)
    if not os.path.exists(path):
        return None
    with open(path, encoding="utf-8") as fh:
        return sorted(set(json.load(fh).get("released", [])), key=natural)


def save_allowed(folder, versions):
    with open(os.path.join(folder, VERSIONS_FILE), "w", encoding="utf-8") as fh:
        json.dump({"released": sorted(set(versions), key=natural)}, fh, indent=2)


def build(archive, folder, test=False, switch=False):
    parsed = parse_name(archive)
    if not parsed:
        raise SystemExit(f"{os.path.basename(archive)}: the file name does not have the meshcore.io form, such as Xiao_nrf52_repeater-v1.18.0-81dbae5.zip.")
    return build_version(parsed[0], parsed[1], parsed[2], folder, test, switch)


def build_version(name, version, commit, folder, test=False, switch=False):
    target = find_target(name, folder, commit)
    label = stand_label(version, folder, test)
    tools = os.path.join(folder, "cache")
    os.makedirs(tools, exist_ok=True)
    firmware_path = firmware_file(folder, target["board"], label)
    first_dir = os.path.join(folder, "install")
    if os.path.exists(firmware_path):
        return target["board"], label, firmware_path, False
    say(f"Building {name} {label} with MeshCore-OTA from the MeshCore source code (commit {commit}).")
    pio = find_pio(tools)
    source = fetch_source(commit, tools)
    work = os.path.join(tools, "build", f"{name}-{label}")
    shutil.rmtree(work, ignore_errors=True)
    shutil.copytree(source, work)
    base = resources()
    with open(os.path.join(base, "firmware", "meshcore.patch"), encoding="utf-8") as fh:
        apply_patch(work, fh.read())
    helper = os.path.join(work, "src", "helpers", "mcota")
    os.makedirs(helper, exist_ok=True)
    for pattern in ("core/*.c", "core/*.h", "firmware/mcota/*", "firmware/esp32/*.py", "loader/nrf52/layout.h", "loader/nrf52/xflash.h"):
        for path in glob.glob(os.path.join(base, *pattern.split("/"))):
            shutil.copy(path, helper)
    with open(os.path.join(helper, "mcota_config.h"), "w", encoding="utf-8") as fh:
        fh.write(config_header(folder))
    for path in glob.glob(os.path.join(base, "firmware", "boards", "*.ld")):
        shutil.copy(path, os.path.join(work, "boards"))
    if target.get("base_env"):
        add_environment(work, target)
    esp = target.get("kind") == "esp32"
    compiler = None if esp else pin_compiler(work, tools, switch)
    env = dict(os.environ)
    env["PLATFORMIO_BUILD_FLAGS"] = "-DMCOTA_TAG='\"%s\"'" % label
    environment = target["env_test"] if test else target["env"]
    log = os.path.join(tools, f"build-{name}-{label}.log")
    if os.path.exists(log):
        os.remove(log)
    say("Compiling the firmware. The first time PlatformIO downloads about 1 GB for this and needs a few minutes ...")
    if run([pio, "run", "-e", environment], cwd=work, env=env, log=log) != 0:
        text = open(log, errors="replace").read()
        if "will not fit in region" in text or "overflowed by" in text:
            over = re.search(r"overflowed by (\d+) bytes", text)
            raise SystemExit(f"{name} {version} with MeshCore-OTA is too large for this device" + (f", {int(over.group(1)) / 1024:.0f} KB too much" if over else "") +
                             ". This version cannot be distributed with MeshCore-OTA here.")
        if esp:
            raise SystemExit(f"The build failed. Details in {log}")
        raise SystemExit(f"The build with the compiler {compiler} failed. Details in {log}. "
                         "If this version only builds with the newer compiler, the option --switch-compiler helps.")
    if esp:
        return finish_esp(target, name, version, label, folder, work, environment, pio, env, log, firmware_path)
    cross = toolchain(env)
    elf = os.path.join(work, ".pio", "build", environment, "firmware.elf")
    app_bin = os.path.join(work, "firmware-mcota.bin")
    loader_dir = os.path.join(work, "loader-build")
    os.makedirs(loader_dir, exist_ok=True)
    loader_src = os.path.join(base, "loader", "nrf52")
    core = os.path.join(base, "core")
    loader_elf = os.path.join(loader_dir, "loader.elf")
    loader_bin = os.path.join(loader_dir, "loader.bin")
    steps = [
        [cross + "objcopy", "-O", "binary", elf, app_bin],
        [cross + "gcc", "-mcpu=cortex-m4", "-mthumb", "-Os", "-ffreestanding", "-fno-builtin", "-ffunction-sections", "-fdata-sections", "-Wall", "-Wextra",
         "-Werror", "-std=c99", f"-DMCOTA_LAYOUT_{target['layout']}", f"-DMCOTA_BOARD_{target['define']}"] + (["-DMCOTA_BOOT_COMPACT"] if target["compact"] else []) +
        ["-nostdlib", "-nostartfiles", "-Wl,--gc-sections", f"-Wl,--defsym=LOADER_ORIGIN={target['loader']:#x}",
         f"-Wl,--defsym=LOADER_SIZE={target['loader_pages'] * PAGE}", "-T", os.path.join(loader_src, "loader.ld"), "-o", loader_elf,
         os.path.join(loader_src, "main.c"), os.path.join(core, "mcota_boot.c"), os.path.join(core, "mcota_sha256.c")] +
        ([os.path.join(core, name) for name in ("mcota_compact.c", "mcota_manifest.c", "mcota_patch.c")] if target["compact"] else []) + ["-lgcc"],
        [cross + "objcopy", "-O", "binary", loader_elf, loader_bin],
    ]
    for step in steps:
        if run(step, cwd=loader_src, log=log) != 0:
            raise SystemExit(f"The build failed. Details in {log}")
    app = open(app_bin, "rb").read()
    loader = open(loader_bin, "rb").read()
    if len(app) > target["slot_pages"] * PAGE or len(loader) > target["app"] - target["loader"]:
        raise SystemExit(f"{name} {version} with MeshCore-OTA is too large for this device.")
    os.makedirs(os.path.dirname(firmware_path), exist_ok=True)
    os.makedirs(first_dir, exist_ok=True)
    with open(firmware_path, "wb") as fh:
        fh.write(app)
    note_key(folder, target["board"], label)
    with open(os.path.join(os.path.dirname(firmware_path), target["board"] + ".json"), "w") as fh:
        json.dump(dict(target, name=name), fh)
    image = loader + bytes([0xFF]) * (target["app"] - target["loader"] - len(loader)) + app
    stem = os.path.join(first_dir, f"{name}-{label}-mcota")
    files = install_files(target, folder, name, label)
    write_dfu_zip(stem + ".zip", image, target)
    write_uf2(stem + ".uf2", [(target["loader"], loader), (target["app"], app), (target["status"], bytes([0xFF]) * PAGE * target["status_pages"])], target["family"])
    shutil.rmtree(work, ignore_errors=True)
    say(f"Done: {len(app)} bytes, {(len(app) + PAGE - 1) // PAGE} of {target['slot_pages']} pages.")
    say(f"  For updates by radio:       {firmware_path}")
    for text, path in files:
        say(f"  For the first installation: {path} ({text})")
    return target["board"], label, firmware_path, True


def finish_esp(target, name, version, label, folder, work, environment, pio, env, log, firmware_path):
    out = os.path.join(work, ".pio", "build", environment)
    if run([pio, "run", "-e", environment, "-t", "mergebin"], cwd=work, env=env, log=log) != 0 or not os.path.exists(os.path.join(out, "firmware-merged.bin")):
        raise SystemExit(f"The image for the first installation over USB could not be created. Details in {log}")
    sizes = esp_slots(os.path.join(out, "partitions.bin"))
    if len(sizes) != 2:
        raise SystemExit(f"{name} does not have two app partitions in flash. Without a second partition MeshCore-OTA cannot switch there.")
    slot_pages = esp_slot_pages(sizes, target["max_pages"])
    with open(os.path.join(out, "firmware.bin"), "rb") as fh:
        app = fh.read()
    if len(app) > slot_pages * PAGE:
        raise SystemExit(f"{name} {version} with MeshCore-OTA is too large for this device, {(len(app) - slot_pages * PAGE) / 1024:.0f} KB too much. "
                         "This version cannot be distributed with MeshCore-OTA here.")
    os.makedirs(os.path.dirname(firmware_path), exist_ok=True)
    os.makedirs(os.path.join(folder, "install"), exist_ok=True)
    with open(firmware_path, "wb") as fh:
        fh.write(app)
    note_key(folder, target["board"], label)
    with open(os.path.join(os.path.dirname(firmware_path), target["board"] + ".json"), "w") as fh:
        json.dump(dict(target, name=name, slot_pages=slot_pages), fh)
    files = install_files(target, folder, name, label)
    shutil.copy(os.path.join(out, "firmware-merged.bin"), files[0][1])
    shutil.copy(firmware_path, files[1][1])
    shutil.rmtree(work, ignore_errors=True)
    say(f"Done: {len(app)} bytes, {(len(app) + PAGE - 1) // PAGE} of {slot_pages} pages.")
    say(f"  For updates by radio:       {firmware_path}")
    for text, path in files:
        say(f"  For the first installation: {path} ({text})")
    return target["board"], label, firmware_path, True

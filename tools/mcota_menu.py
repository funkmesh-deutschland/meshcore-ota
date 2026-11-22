import argparse
import asyncio
import json
import os
import threading
import time

import mcota
import mcota_build
import mcota_ui
import mcota_update
from mcota_version import VERSION
from rich.table import Table
from rich.text import Text

console = mcota_ui.console
TEAL, ORANGE, SOFT = mcota_ui.TEAL, mcota_ui.ORANGE, mcota_ui.SOFT
CONFIG_FILE = "config.json"


def ask(text, default=""):
    try:
        answer = input(f"{text}{' [' + default + ']' if default else ''}: ").strip()
    except EOFError:
        raise KeyboardInterrupt
    return answer or default


def pause():
    ask("Press Enter to continue")


def head(title):
    console.print()
    console.print(Text(title, style=f"bold {TEAL}"))
    console.print(Text("─" * max(len(title), 40), style=SOFT))


def load_config(home):
    path = os.path.join(home, CONFIG_FILE)
    data = {"serial": "", "ble": "", "airtime": None, "test": False, "hops": 2}
    if os.path.exists(path):
        with open(path, encoding="utf-8") as fh:
            data.update(json.load(fh))
    return data


def save_config(home, data):
    with open(os.path.join(home, CONFIG_FILE), "w", encoding="utf-8") as fh:
        json.dump(data, fh, indent=2)


def namespace(home, config, **extra):
    values = dict(dir=home, serial=config["serial"], ble=config["ble"], airtime=config["airtime"], test=config["test"], hops=config["hops"],
                  plain=False, route=[], offline=False, manual_apply=False, no_rollback=False, switch_compiler=False, board=None, build_only=False,
                  version=None, device=None)
    values.update(extra)
    return argparse.Namespace(**values)


def ensure_key(home):
    key_path = os.path.join(home, "keys", "signing.key")
    if os.path.exists(key_path):
        return
    head("First start")
    console.print(f"There is no key in {home} yet. Creating the folders and a key pair.")
    mcota.cmd_init(argparse.Namespace(dir=home))
    pause()


def status_line(home, config):
    key = mcota_build.public_key(home)
    settings = mcota_build.load_settings(home)
    allowed = mcota_build.allowed_versions(home)
    link = f"USB {config['serial']}" if config["serial"] else f"Bluetooth {config['ble']}" if config["ble"] else "not chosen yet"
    grid = Table.grid(padding=(0, 2))
    grid.add_column(style=SOFT)
    grid.add_column()
    grid.add_row("Version", VERSION)
    grid.add_row("Folder", home)
    grid.add_row("Key", f"ID {mcota.key_id(key).hex()}" if key else "missing")
    grid.add_row("Companion", Text(link, style=TEAL if config["serial"] or config["ble"] else ORANGE))
    grid.add_row("Released", ", ".join(allowed) if allowed else "no version selected yet" if allowed is not None else "all built versions")
    grid.add_row("Settings", f"revision {settings['revision']}, presets: {', '.join(p['name'] for p in settings['presets']) or 'none'}")
    grid.add_row("Airtime", f"{config['airtime'] or (5.0 if config['test'] else 0.5):g} %" + (" (test mode)" if config["test"] else ""))
    console.print(grid)


def menu_overview(home, config):
    if not config["serial"] and not config["ble"]:
        console.print(Text("Choose the companion first (item 6).", style=ORANGE))
        pause()
        return
    console.print("Starting the dashboard. Ctrl+C returns to the menu.")
    time.sleep(1.0)
    try:
        mcota.cmd_update(namespace(home, config))
    except SystemExit as error:
        if error.code not in (None, 0):
            console.print(Text(str(error), style=ORANGE))
        pause()


def menu_versions(home, config):
    while True:
        head("Release versions")
        known = mcota_build.releases(os.path.join(home, "cache"))
        if not known:
            console.print(Text("The list of MeshCore versions could not be loaded. Is there an internet connection?", style=ORANGE))
            pause()
            return
        allowed = set(mcota_build.allowed_versions(home) or [])
        shown = known[-12:]
        table = Table(box=None, padding=(0, 2))
        for title in ("No.", "Version", "Commit", "is sent"):
            table.add_column(title, style=SOFT if title == "Commit" else None)
        for index, (version, commit) in enumerate(shown, 1):
            table.add_row(str(index), version, commit, Text("yes", style=f"bold {TEAL}") if version in allowed else Text("no", style=SOFT))
        console.print(table)
        answer = ask("Number to toggle, Enter to go back")
        if not answer:
            return
        if answer.isdigit() and 1 <= int(answer) <= len(shown):
            version = shown[int(answer) - 1][0]
            allowed ^= {version}
            mcota_build.save_allowed(home, allowed)


def pick_device(home, commit):
    names = mcota_build.catalog(os.path.join(home, "cache"), commit)
    while True:
        wanted = ask("Search device (part of the name, for example xiao, rak, t114, heltec), Enter to go back").lower()
        if not wanted:
            return None
        hits = sorted(name for name in names if wanted in name.lower())
        if not hits:
            console.print(Text("No device with this name.", style=ORANGE))
            continue
        table = Table(box=None, padding=(0, 2))
        for title in ("No.", "Device", "Processor", "Status"):
            table.add_column(title)
        for index, name in enumerate(hits, 1):
            state, text = mcota_build.support(name, names[name])
            table.add_row(str(index), name, names[name]["mcu"], Text(text, style=TEAL if state == "ready" else ORANGE))
        console.print(table)
        answer = ask("Choose a number, Enter for a new search")
        if answer.isdigit() and 1 <= int(answer) <= len(hits):
            name = hits[int(answer) - 1]
            if mcota_build.support(name, names[name])[0] == "missing":
                console.print(Text(f"meshcore-ota cannot build {name} yet: {mcota_build.support(name, names[name])[1]}.", style=ORANGE))
                continue
            return name


def menu_firmware(home, config):
    head("Build firmware")
    known = mcota_build.releases(os.path.join(home, "cache"))
    if not known:
        console.print(Text("The list of MeshCore versions could not be loaded. Is there an internet connection?", style=ORANGE))
        pause()
        return
    shown = known[-8:]
    for index, (version, commit) in enumerate(shown, 1):
        console.print(f"  {index}  {version}")
    answer = ask("Choose version", str(len(shown)))
    if not answer.isdigit() or not 1 <= int(answer) <= len(shown):
        return
    version, commit = shown[int(answer) - 1]
    name = pick_device(home, commit)
    if not name:
        return
    settings = mcota_build.load_settings(home)
    console.print(f"Building {name} {version} with your key and settings revision {settings['revision']}.")
    try:
        board, label, path, _ = mcota_build.build_version(name, version, commit, home, config["test"], False)
    except SystemExit as error:
        console.print(Text(str(error), style=ORANGE))
        pause()
        return
    console.print()
    console.print(Text("Done. Put these files on the repeater:", style=f"bold {TEAL}"))
    for text, file in mcota_build.install_files(mcota_build.find_target(name, home, commit), home, name, label):
        console.print(f"  {text + ':':<48}{file}")
    console.print(f"The base firmware is in {path}. All later updates build on it, so keep the folder.")
    allowed = mcota_build.allowed_versions(home)
    if allowed is not None and version not in allowed and ask(f"Also release {version} for sending? (y/n)", "y").lower().startswith("y"):
        mcota_build.save_allowed(home, set(allowed) | {version})
    pause()


def show_preset(preset):
    table = Table(box=None, padding=(0, 2))
    for title in ("No.", "Setting", "Value", "Meaning"):
        table.add_column(title, style=SOFT if title == "Meaning" else None)
    known = dict(mcota_build.KNOWN_SETTINGS)
    keys = [key for key, _ in mcota_build.KNOWN_SETTINGS] + [key for key in preset["values"] if key not in known]
    for index, key in enumerate(keys, 1):
        value = preset["values"].get(key)
        table.add_row(str(index), key, Text(str(value), style=f"bold {TEAL}") if value is not None else Text("unchanged", style=SOFT), known.get(key, "custom setting"))
    console.print(table)
    return keys


def edit_preset(home, data, preset):
    while True:
        head(f"Preset {preset['name']}")
        keys = show_preset(preset)
        answer = ask("Number to change, n for a custom setting, Enter to go back")
        if not answer:
            return
        if answer == "n":
            key = ask("Name of the setting as in the MeshCore command 'set'")
        elif answer.isdigit() and 1 <= int(answer) <= len(keys):
            key = keys[int(answer) - 1]
        else:
            continue
        if not key:
            continue
        value = ask(f"Value for {key} (leave empty to remove the entry)")
        before = dict(preset["values"])
        if value:
            preset["values"][key] = value
        else:
            preset["values"].pop(key, None)
        try:
            mcota_build.check_preset(preset)
        except SystemExit as error:
            preset["values"] = before
            console.print(Text(str(error), style=ORANGE))
            pause()
            continue
        mcota_build.save_settings(home, data)


def menu_settings(home, config):
    while True:
        data = mcota_build.load_settings(home)
        head("Settings and presets")
        console.print(f"Revision {data['revision']}")
        for index, preset in enumerate(data["presets"], 1):
            values = ", ".join(f"{k}={v}" for k, v in preset["values"].items()) or "no values"
            console.print(f"  {index}  {preset['name']:<16} {values}")
        if not data["presets"]:
            console.print(Text("  no preset yet", style=SOFT))
        answer = ask("Number to edit, n for new, d for delete, Enter to go back")
        if not answer:
            return
        if answer == "n":
            name = ask("Name of the preset (a to z, digits, - and _, at most 15 characters)").lower()
            preset = {"name": name, "values": {}}
            try:
                mcota_build.check_preset(preset)
            except SystemExit as error:
                console.print(Text(str(error), style=ORANGE))
                pause()
                continue
            if any(p["name"] == name for p in data["presets"]) or len(data["presets"]) >= 8:
                console.print(Text("The name already exists, or there are already 8 presets.", style=ORANGE))
                pause()
                continue
            data["presets"].append(preset)
            mcota_build.save_settings(home, data)
            edit_preset(home, data, preset)
        elif answer == "d":
            number = ask("Number of the preset to delete")
            if number.isdigit() and 1 <= int(number) <= len(data["presets"]):
                del data["presets"][int(number) - 1]
                mcota_build.save_settings(home, data)
        elif answer.isdigit() and 1 <= int(answer) <= len(data["presets"]):
            edit_preset(home, data, data["presets"][int(answer) - 1])


def menu_repeaters(home, config):
    while True:
        head("My repeaters")
        mine = mcota.load_repeaters(home)
        for index, (ident, entry) in enumerate(sorted(mine.items(), key=lambda item: item[1].get("name", "")), 1):
            seen = time.strftime("%Y-%m-%d %H:%M", time.localtime(entry["seen"])) if entry.get("seen") else "never reported"
            console.print(f"  {index}  {entry.get('name') or ident:<28} {ident}  {entry.get('firmware') or '':<18} {seen}")
        if not mine:
            console.print(Text("  none yet", style=SOFT))
        answer = ask("a to add from the contacts of the companion, d for delete, Enter to go back")
        if not answer:
            return
        if answer == "d":
            number = ask("Number")
            ordered = sorted(mine.items(), key=lambda item: item[1].get("name", ""))
            if number.isdigit() and 1 <= int(number) <= len(ordered):
                del mine[ordered[int(number) - 1][0]]
                mcota.save_repeaters(home, mine)
        elif answer == "a":
            if not config["serial"] and not config["ble"]:
                console.print(Text("Choose the companion first (item 6).", style=ORANGE))
                pause()
                continue
            try:
                mcota_companion, link = mcota.open_link(namespace(home, config))
                contacts = asyncio.run(mcota_companion.with_companion(link, lambda companion: companion.contacts()))
            except (SystemExit, Exception) as error:
                console.print(Text(f"The companion cannot be reached: {error}", style=ORANGE))
                pause()
                continue
            found = sorted((c for c in contacts if c["type"] == mcota_companion.TYPE_REPEATER and c["key"][:4].hex() not in mine), key=lambda c: c["name"].lower())
            wanted = ask("Part of the name, Enter shows all").lower()
            found = [c for c in found if wanted in c["name"].lower()]
            for index, contact in enumerate(found, 1):
                console.print(f"  {index}  {contact['name']:<28} {contact['key'][:4].hex()}")
            numbers = ask("Numbers, separated by commas")
            for number in numbers.replace(" ", "").split(","):
                if number.isdigit() and 1 <= int(number) <= len(found):
                    contact = found[int(number) - 1]
                    mine[contact["key"][:4].hex()] = {"name": contact["name"], "firmware": "", "seen": 0}
            mcota.save_repeaters(home, mine)


def menu_link(home, config):
    head("Companion connection")
    from serial.tools import list_ports
    choices = []
    for port in list_ports.comports():
        if port.vid is not None:
            choices.append(("serial", port.device, f"USB {port.device}  {port.description}"))
    console.print("Scanning for companions over Bluetooth, this takes a few seconds ...")
    try:
        import mcota_companion
        found = asyncio.run(mcota_companion.scan_ble())
        known = asyncio.run(mcota_companion.known_ble())
        for address, name, rssi in found:
            choices.append(("ble", address, f"Bluetooth {address}  {name}  {rssi} dBm"))
        for address, name in known:
            if address not in {item[1] for item in choices}:
                choices.append(("ble", address, f"Bluetooth {address}  {name}  (paired)"))
    except Exception as error:
        console.print(Text(f"Bluetooth scan not possible: {error}", style=SOFT))
    for index, (_, _, text) in enumerate(choices, 1):
        console.print(f"  {index}  {text}")
    answer = ask("Choose a number, or enter the port by hand (COM5, /dev/ttyACM0, AA:BB:CC:DD:EE:FF), Enter to go back")
    if answer.isdigit() and 1 <= int(answer) <= len(choices):
        kind, value, _ = choices[int(answer) - 1]
    elif answer:
        kind, value = ("ble", answer) if answer.count(":") == 5 else ("serial", answer)
    else:
        kind = None
    if kind:
        config["serial"], config["ble"] = (value, "") if kind == "serial" else ("", value)
    percent = ask("Airtime of the companion in per cent, 0.05 to 5", f"{config['airtime'] or 0.5:g}")
    try:
        config["airtime"] = min(5.0, max(0.05, float(percent.replace(",", "."))))
    except ValueError:
        pass
    config["test"] = ask("Test mode with test firmware and short intervals? (y/n)", "y" if config["test"] else "n").lower().startswith("y")
    save_config(home, config)


def menu_key(home, config):
    head("Key")
    key = mcota_build.public_key(home)
    console.print(f"Active key: ID {mcota.key_id(key).hex()}")
    console.print(f"Private key: {os.path.join(home, 'keys', 'signing.key')}")
    for entry in mcota_build.retired_keys(home):
        console.print(Text(f"Previous key: ID {entry['id']}, is replaced on the repeaters with the next update", style=SOFT))
    path = ask("Take over a private key from a file: enter the path, Enter to go back")
    if not path:
        return
    try:
        lines = mcota.import_key(home, os.path.expanduser(path))
    except (SystemExit, OSError) as error:
        lines = [str(error)]
    for line in lines:
        console.print(line)
    pause()


def menu_update(home, config):
    head("Update program")
    console.print(f"Installed is {VERSION}. Checking GitHub ...")
    found = mcota_update.check()
    if not found:
        console.print("No newer version found, or GitHub cannot be reached right now.")
        pause()
        return
    console.print(Text(f"New: {found['version']}", style=f"bold {TEAL}"))
    if found["notes"]:
        console.print(found["notes"])
    console.print("Key, firmware, packages and settings stay, only the program file is replaced.")
    if ask("Update now? (y/n)", "y").lower().startswith("y"):
        try:
            mcota_update.install(found)
        except (SystemExit, OSError) as error:
            console.print(Text(str(error), style=ORANGE))
            pause()
            return
        console.print(Text("Updated. Please restart the program.", style=f"bold {TEAL}"))
        raise SystemExit(0)


ENTRIES = [
    ("1", "Dashboard", menu_overview),
    ("2", "Release versions", menu_versions),
    ("3", "Build firmware", menu_firmware),
    ("4", "Settings and presets", menu_settings),
    ("5", "My repeaters", menu_repeaters),
    ("6", "Companion connection", menu_link),
    ("7", "Key", menu_key),
    ("8", "Update program", menu_update),
]


def run(home):
    home = os.path.abspath(home)
    os.makedirs(home, exist_ok=True)
    ensure_key(home)
    config = load_config(home)
    mcota_update.cleanup()
    news = {}
    threading.Thread(target=lambda: news.update(mcota_update.check() or {}), daemon=True).start()
    while True:
        mcota_ui.show_banner()
        status_line(home, config)
        if news.get("version"):
            console.print(Text(f"Version {news['version']} is available (item 8).", style=f"bold {ORANGE}"))
        console.print()
        for key, title, _ in ENTRIES:
            console.print(Text.assemble((f"  {key}  ", f"bold {TEAL}"), (title, "bold")))
        console.print(Text.assemble(("  0  ", f"bold {TEAL}"), ("Quit", "bold")))
        try:
            answer = ask("\nChoice")
            if answer == "0":
                return
            for key, _, action in ENTRIES:
                if answer == key:
                    action(home, config)
        except KeyboardInterrupt:
            console.print()
            if answer_is_exit():
                return


def answer_is_exit():
    try:
        return ask("Quit? (y/n)", "n").lower().startswith("y")
    except KeyboardInterrupt:
        return True

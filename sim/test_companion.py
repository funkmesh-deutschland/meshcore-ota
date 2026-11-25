import os
import pty
import struct
import subprocess
import sys
import threading
import time

tools = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "tools")
fixture = sys.argv[1]
master, slave = pty.openpty()
sent, logins, commands, failures = [], [], [], 0
asked = []
hello = []
info = bytes([5, 1, 22, 22]) + bytes(range(32)) + bytes(12) + struct.pack("<II", 869618, 62500) + bytes([8, 8]) + b"Testcompanion"
target_key = bytes([0x77, 0x01, 0x02, 0x03]) + bytes(range(28))
other_key = bytes([0x55]) * 32
waiting = []


def frame(data):
    return b">" + struct.pack("<H", len(data)) + data


def contact(key, kind, path_len, path, name):
    return bytes([3]) + key + bytes([kind, 0, path_len]) + path.ljust(64, b"\0") + name.ljust(32, b"\0") + bytes(16)


def device():
    buf = b""
    while True:
        try:
            chunk = os.read(master, 4096)
        except OSError:
            return
        buf += chunk
        while True:
            start = buf.find(b"<")
            if start < 0 or len(buf) < start + 3:
                break
            size = struct.unpack("<H", buf[start + 1:start + 3])[0]
            if len(buf) < start + 3 + size:
                break
            data = buf[start + 3:start + 3 + size]
            buf = buf[start + 3 + size:]
            if data[0] == 1:
                os.write(master, frame(info))
            elif data[0] == 22:
                os.write(master, frame(bytes([13, 9, 100, 40]) + bytes(4) + b"1 Nov 2026\0\0" + b"T1000"))
            elif data[0] == 4:
                os.write(master, frame(bytes([2]) + struct.pack("<I", 3)))
                os.write(master, frame(contact(target_key, 2, 0x41, bytes.fromhex("aa01"), b"Mast North")))
                os.write(master, frame(contact(other_key, 2, 0xFF, b"", b"Mast South")))
                os.write(master, frame(contact(bytes([0x66]) * 32, 1, 0, b"", b"Phone")))
                os.write(master, frame(bytes([4]) + struct.pack("<I", 0)))
            elif data[0] == 26:
                logins.append((data[1:33], data[33:]))
                os.write(master, frame(bytes([6, 0]) + data[1:5] + struct.pack("<I", 500)))
                os.write(master, frame(bytes([0x85 if data[33:] == b"secret" else 0x86, 1]) + data[1:7]))
            elif data[0] == 2:
                text = data[13:].decode()
                commands.append((data[1], data[7:13], text))
                os.write(master, frame(bytes([6, 0, 0, 0, 0, 0]) + struct.pack("<I", 500)))
                reply = "OK fw=a1 loading v2 40/93 w=40 old=1 id=0011223344556677" if text == "ota info" else "OK 5615d82a:53:12s"
                waiting.append(bytes([7]) + data[7:13] + bytes([0xFF, 1]) + struct.pack("<I", 1) + reply.encode())
                os.write(master, frame(bytes([0x83])))
            elif data[0] == 42:
                if data[2:34] == target_key:
                    os.write(master, frame(bytes([22]) + struct.pack("<I", 1) + bytes([0x42]) + bytes.fromhex("bb02aa01")))
                else:
                    os.write(master, frame(bytes([1, 2])))
            elif data[0] == 10:
                os.write(master, frame(waiting.pop(0) if waiting else bytes([10])))
            elif data[0] == 25:
                hops = data[1] & 63
                width = (data[1] >> 6) + 1
                payload = data[2 + hops * width:]
                sent.append((data[1], data[2:2 + hops * width], payload))
                os.write(master, frame(bytes([0])))
                if payload[:3] == b"L2\x07" and hello:
                    os.write(master, frame(bytes([0x84, 40, 0xC4, 0xFF]) + hello[0]))
                if payload[:3] == b"L2\x01" and not asked:
                    asked.append(1)
                    body = payload[4:]
                    request = b"L2\x02\x00" + body[0:4] + b"\x77\x01\x02\x03" + body[4:12] + bytes([0]) + struct.pack("<I", 0xFFFFFFFF)
                    os.write(master, frame(bytes([0x84, 40, 0xC4, 0xFF]) + request))


def check(ok, text):
    global failures
    if not ok:
        failures += 1
        print("ERROR:", text)


def run(args, until=None, limit=300.0):
    proc = subprocess.Popen([sys.executable, os.path.join(tools, "mcota.py")] + args, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    if until:
        deadline = time.monotonic() + limit
        while not until() and time.monotonic() < deadline and proc.poll() is None:
            time.sleep(0.2)
        time.sleep(6.0)
        proc.terminate()
    return proc.communicate()[0]


def mcota_kinds(routed=None):
    return [p[2] for _, path, p in list(sent) if p[:2] == b"L2" and (routed is None or bool(path) == routed)]


threading.Thread(target=device, daemon=True).start()
port = os.ttyname(slave)

out = run(["repeaters", "--serial", port])
check("Mast North" in out and "via aa01" in out, "repeater with a route is not shown")
check("Mast South" in out and "no route known" in out, "repeater without a route is not shown")
check("Phone" not in out, "contact that is not a repeater is shown")

out = run(["send", "--serial", port, "--test", os.path.join(fixture, "update.mcota")], lambda: 3 in mcota_kinds(False) and 7 in mcota_kinds(True))
kinds = [p[2] for _, _, p in sent if p[:2] == b"L2"]
check(1 in kinds, "no offer sent")
check(kinds.count(3) >= 1, "no manifest data sent")
check(sent and all(head == 0 and path == b"" for head, path, p in sent if p[2] != 7), "offer and data do not use path length 0")
check(any(head == 0x02 and path == bytes.fromhex("aabb") and p[2] == 7 and p[16:20] == target_key[:4] and p[20:24] == bytes([1, 2]) + bytes.fromhex("bbaa")
          for head, path, p in sent), "no targeted query over 2 hops with 1-byte hops to the repeater with a known route")
check("Route to Mast North: via aa > bb" in out, "learned route is not shortened for companion version 9")
check("Testcompanion" in out and "SF8" in out and "CR 4/8" in out, "companion details not read")
check("requests 1" in out, "request not processed")
check(all(2 + len(path) + len(p) <= 176 for _, path, p in sent), "packet too long for the companion")
if failures:
    print(out[-800:])

import hashlib
import shutil
import tempfile
work = tempfile.mkdtemp(prefix="meshcore-ota-test-")
run(["--dir", work, "init"])
os.makedirs(os.path.join(work, "firmware"), exist_ok=True)
shutil.copy(os.path.join(fixture, "old.bin"), os.path.join(work, "firmware", "sim_board-v1.0.0+1-test.bin"))
shutil.copy(os.path.join(fixture, "new.bin"), os.path.join(work, "firmware", "sim_board-v1.0.1+1-test.bin"))
key_line = [l for l in open(os.path.join(work, "keys", "public.txt")).read().splitlines() if l.startswith("Key ID")][0]
old_image = open(os.path.join(fixture, "old.bin"), "rb").read()
hello[:] = [b"L2\x08\x00" + bytes.fromhex("77010203") + bytes.fromhex(key_line.split()[-1]) + hashlib.sha256(b"sim_board").digest()[:4]
            + struct.pack("<I", len(old_image)) + hashlib.sha256(old_image).digest()[:8] + b"1.0.0"]
sent.clear()
asked.clear()
import base64
import json
import socket
import urllib.error
import urllib.request
probe = socket.socket()
probe.bind(("127.0.0.1", 0))
web_port = probe.getsockname()[1]
probe.close()
web = {}


def web_checked():
    if 3 not in mcota_kinds(True):
        return False
    base = f"http://127.0.0.1:{web_port}"
    auth = {"Authorization": "Basic " + base64.b64encode(b"user:open sesame").decode()}
    try:
        try:
            urllib.request.urlopen(base + "/api/state", timeout=3)
            web["open"] = True
        except urllib.error.HTTPError as error:
            web["open"] = error.code != 401
        web["state"] = json.load(urllib.request.urlopen(urllib.request.Request(base + "/api/state", headers=auth), timeout=3))
        web["page"] = urllib.request.urlopen(urllib.request.Request(base + "/", headers=auth), timeout=3).read()
        body = json.dumps({"id": "99887766", "name": "Added by hand", "lat": 51.5, "lon": 7.5}).encode()
        web["saved"] = json.load(urllib.request.urlopen(urllib.request.Request(base + "/api/repeater", data=body, headers=dict(auth, **{"Content-Type": "application/json"})), timeout=3))
        try:
            urllib.request.urlopen(urllib.request.Request(base + "/api/repeater", data=b'{"id": "xyz"}', headers=dict(auth, **{"Content-Type": "application/json"})), timeout=3)
            web["refused"] = False
        except urllib.error.HTTPError as error:
            web["refused"] = error.code == 400
        web["file"] = json.load(open(os.path.join(work, "repeaters.json")))
    except Exception as error:
        web["error"] = repr(error)
    return True


out = run(["--dir", work, "update", "--serial", port, "--test", "--offline", "--plain", "--web", str(web_port), "--web-password", "open sesame"], web_checked)
check("error" not in web, f"web interface failed: {web.get('error')}")
if "state" in web:
    state = web["state"]
    mast = next((r for r in state["repeaters"] if r["id"] == "77010203"), None)
    check(web["open"] is False, "the web interface answers without the password")
    check(state["companion"] and state["companion"]["name"] == "Testcompanion" and state["airtime"]["sent"] > 0, "web state lacks the companion or the airtime")
    check(mast is not None and mast["name"] == "Mast North" and mast["state"] == "loading" and mast["progress"] and mast["via"] and mast["hops"] == 2, f"web state of the repeater is wrong: {mast}")
    check(any(l["kind"] == "data" and l["to"] == "77010203" for l in state["links"]) and any(l["kind"] == "route" for l in state["links"]), "web state lacks the links")
    check(state["packages"] and state["packages"][0]["to"] == "v1.0.1+1-test", "web state lacks the offered update")
    check(b"MeshCore" in web.get("page", b""), "the page is not served")
    check(web.get("saved") == {"ok": True} and web.get("file", {}).get("99887766", {}).get("lat") == 51.5 and web.get("refused"), "a repeater cannot be added through the web interface")
kinds = [p[2] for _, _, p in sent if p[:2] == b"L2"]
check(7 in kinds, "query for repeaters was not sent")
check(1 in kinds, "no offer follows the hello of the repeater")
check(os.path.exists(os.path.join(work, "packages", "sim_board-v1.0.0+1-test__v1.0.1+1-test.mcota")), "package for the reported firmware was not built")
check("gets v1.0.1+1-test" in out, "reported repeater is not matched to the update")
check(any(path and p[2] == 3 for _, path, p in sent), "no data sent over the route")
check(all(2 + len(path) + len(p) <= 176 for _, path, p in sent), "packet with route too long for the companion")
if failures:
    print(out[-900:])
hello[:] = []
shutil.rmtree(work, ignore_errors=True)

out = run(["status", "--serial", port, "--to", "north", "--password", "secret"])
check(logins and logins[-1] == (target_key, b"secret"), "login not sent")
check([c[2] for c in commands] == ["ota info", "ota peers"] and all(c[0] == 1 and c[1] == target_key[:6] for c in commands), "commands not sent")
check("40/93" in out and "5615d82a:53" in out, "replies of the repeater not shown")
out = run(["status", "--serial", port, "--to", "north", "--password", "wrong"])
check("failed" in out and len(commands) == 2, "wrong password is not reported")

print(f"Companion link: {len(sent)} packets handed to the companion, {failures} errors")
sys.exit(1 if failures else 0)

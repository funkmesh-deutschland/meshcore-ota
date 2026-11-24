import os
import random
import subprocess
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "tools"))
import mcota
from mcota_provider import Budget, Finder, Provider, lora_airtime_ms

fixture, binary = sys.argv[1], sys.argv[2]
manifest, blocks = mcota.read_package(os.path.join(fixture, "update.mcota"))
new = open(os.path.join(fixture, "new.bin"), "rb").read()
failures = 0
routes = []


def check(ok, text):
    global failures
    if not ok:
        failures += 1
        print("ERROR:", text)


def run(pub, board, loss, corrupt, target=None, hops=()):
    clock = [0.0]
    rng = random.Random(3)
    provider = Provider(manifest, blocks, bytes.fromhex("a1b2c3d4"), lambda n: 80 + n * 5.6, budget_ppm=50000,
                        offer_min=10.0, offer_max=120.0, now=lambda: clock[0], rng=rng.random, target=target, hops=hops)
    out = os.path.join(fixture, "slot_b.bin")
    proc = subprocess.Popen([binary, os.path.join(fixture, "old.bin"), os.path.join(fixture, pub), board, out],
                            stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)

    def command(text):
        proc.stdin.write(text + "\n")
        proc.stdin.flush()
        sent = []
        while True:
            line = proc.stdout.readline().strip()
            if line.startswith("S "):
                sent.append(bytes.fromhex(line[2:]))
            elif line.startswith("V "):
                parts = line.split()
                routes.append((int(parts[1]), parts[2]))
                sent.append(bytes.fromhex(parts[3]))
            else:
                return line, sent

    state = None
    pinged = False
    routes.clear()
    for step in range(200000):
        clock[0] += 0.1
        line, sent = command("T 100")
        for packet in sent:
            if rng.random() >= loss:
                provider.receive(packet)
        packet = provider.poll()
        if packet is not None and rng.random() >= loss:
            if rng.random() < corrupt and len(packet) > 22:
                pos = 20 + rng.randrange(len(packet) - 20)
                packet = packet[:pos] + bytes([packet[pos] ^ 0x21]) + packet[pos + 1:]
            command("R " + packet.hex())
        if step % 600 == 599:
            state = command("Q")[0].split()
            if state[1] == "2" and state[5] != "1":
                for ping in command("P")[1]:
                    provider.receive(ping)
                pinged = True
            if state[1] == "2" and state[5] == "1":
                break
            if clock[0] > 6 * 3600:
                break
    state = command("Q")[0].split()
    proc.stdin.close()
    proc.wait()
    data = open(out, "rb").read()
    return state, data, provider, clock[0]


state, data, provider, seconds = run("good.pub", "sim_board", float(os.environ.get("LOSS", "0.2")), float(os.environ.get("CORRUPT", "0.02")))
check(state[1] == "2", f"receiver did not complete: {state}")
check(data[:len(new)] == new, "received firmware is wrong")
check(int(state[4]) > 0, "nothing was bundled")
check(state[5] == "1", "ping of the receiver was not answered")
check(provider.sent_ms <= seconds * 1000 * 0.05 + 2000, f"budget exceeded: {provider.sent_ms:.0f} ms in {seconds:.0f} s")
print(f"Provider on the computer: {seconds / 60:.0f} min, {provider.sent} packets sent, {provider.requests} requests, {state[4]} bundles at the receiver")

state, data, provider, seconds = run("other.pub", "sim_board", 0.0, 0.0)
check(state[1] == "0" and provider.requests == 0, "receiver with a foreign key sent a request")
state, data, provider, seconds = run("good.pub", "other_board", 0.0, 0.0)
check(state[1] == "0" and provider.requests == 0, "receiver with a different device sent a request")

state, data, provider, seconds = run("good.pub", "sim_board", 0.1, 0.0, target=bytes.fromhex("77010203"), hops=[bytes.fromhex("aabb"), bytes.fromhex("ccdd")])
check(state[1] == "2", f"receiver over 2 hops did not complete: {state}")
check(data[:len(new)] == new, "firmware received over 2 hops is wrong")
check(provider.requests > 0 and routes and all(r == (2, "ccddaabb") for r in routes), f"requests did not use the return route: {routes[:3]}")
print(f"Provider over 2 hops: {seconds / 60:.0f} min, {provider.sent} packets sent, {provider.requests} requests over the return route")

import hashlib


def ask(target=None, path=b"", first=3.0, direct=True):
    clock = [0.0]
    finder = Finder(bytes.fromhex("a1b2c3d4"), hashlib.sha256(open(os.path.join(fixture, "good.pub"), "rb").read()).digest()[:8],
                    lambda n: 80 + n * 5.6, Budget(50000, lambda: clock[0]), first=first, now=lambda: clock[0], rng=random.Random(5).random)
    if target:
        finder.route(target, path, 2)
    proc = subprocess.Popen([binary, os.path.join(fixture, "old.bin"), os.path.join(fixture, "good.pub"), "sim_board", os.path.join(fixture, "slot_b.bin")],
                            stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
    found, back, asked = None, [], []
    for step in range(3000):
        clock[0] += 0.1
        proc.stdin.write("T 100\n"); proc.stdin.flush()
        while True:
            line = proc.stdout.readline().strip()
            if line.startswith("S "):
                found = (finder.receive(bytes.fromhex(line[2:])) if direct else None) or found
            elif line.startswith("V "):
                parts = line.split()
                back.append(parts[2])
                found = finder.receive(bytes.fromhex(parts[3])) or found
            else:
                break
        item = finder.poll()
        if item:
            asked.append(item[1].hex())
            proc.stdin.write("R " + item[0].hex() + "\n"); proc.stdin.flush(); proc.stdout.readline()
        if found:
            break
    proc.stdin.close(); proc.wait()
    return found, back, asked, clock[0]


found, back, asked, seconds = ask()
old_image = open(os.path.join(fixture, "old.bin"), "rb").read()
check(found is not None and not back, "repeater does not answer the query of the computer directly")
if found:
    check(found["size"] == len(old_image) and found["hash"] == hashlib.sha256(old_image).digest()[:8], "reported firmware is wrong")
    check(found["board"] == hashlib.sha256(b"sim_board").digest()[:4] and found["tag"] == "1.0.0", "reported device type or firmware tag is wrong")
    check(found["node"] == 0 and found["loader"] == 0 and found["job"] == bytes(4), f"state in the hello is wrong: {found['node']} {found['loader']} {found['job']}")
    print(f"Hello of the repeater: firmware {found['tag']}, {found['size']} bytes, after {seconds:.0f} s")

found, back, asked, seconds = ask(bytes.fromhex("77010203"), bytes.fromhex("aabbccdd"), first=1e9)
check(found is not None and found["hops"] == 2 and back == ["ccddaabb"] and asked[:1] == ["aabbccdd"],
      f"hello over 2 hops wrong: {found and found['hops']}, return route {back}, outbound route {asked[:1]}")
if found:
    print(f"Hello over 2 hops after {seconds:.0f} s, return route {back[0]}")


def both_queries():
    finder = Finder(bytes.fromhex("a1b2c3d4"), hashlib.sha256(open(os.path.join(fixture, "good.pub"), "rb").read()).digest()[:8],
                    lambda n: 1.0, Budget(50000, lambda: 0.0), now=lambda: 0.0, rng=random.Random(5).random)
    finder.ask_at = 0.0
    finder.route(bytes.fromhex("77010203"), bytes.fromhex("aabbccdd"), 2)
    finder.routes["77010203"]["ask_at"] = 1e9
    plain = finder.poll()[0]
    finder.ask_at = 1e9
    finder.routes["77010203"]["ask_at"] = 0.0
    routed = finder.poll()[0]
    proc = subprocess.Popen([binary, os.path.join(fixture, "old.bin"), os.path.join(fixture, "good.pub"), "sim_board", os.path.join(fixture, "slot_b.bin")],
                            stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
    replies = []
    for packet in (plain, routed):
        proc.stdin.write("R " + packet.hex() + "\n"); proc.stdin.flush(); proc.stdout.readline()
    for step in range(600):
        proc.stdin.write("T 100\n"); proc.stdin.flush()
        while True:
            line = proc.stdout.readline().strip()
            if line.startswith("S ") or line.startswith("V "):
                replies.append(line.split()[:3] if line.startswith("V ") else ["S"])
            else:
                break
        if replies:
            break
    proc.stdin.close(); proc.wait()
    return replies


pinger = Finder(bytes.fromhex("a1b2c3d4"), bytes(8), lambda n: 1.0, Budget(50000, lambda: 0.0), first=1e9, now=lambda: 0.0)
check(pinger.receive(b"L2\x04\x00" + bytes.fromhex("7701020311223344")) is None and len(pinger.pongs) == 1, "a ping is not noted")
answer = pinger.poll()
check(answer is not None and answer[0][2] == 5 and answer[0][4:] == bytes.fromhex("77010203a1b2c3d411223344") and answer[1] == b"",
      f"a ping is not answered with a pong: {answer}")
check(pinger.poll() is None, "a pong is sent twice")

two = Finder(bytes.fromhex("a1b2c3d4"), [bytes([1]) * 8, bytes([2]) * 8], lambda n: 1.0, Budget(50000, lambda: 0.0), first=0.0, now=lambda: 0.0)
first_query = two.poll()
check(first_query is not None and first_query[0][8:16] == bytes([1]) * 8 and two.ask_at <= 60.0, "the first query does not use the current key or the second key is not asked soon")

replies = both_queries()
check(replies[:1] == [["V", "2", "ccddaabb"]], f"targeted query after a general one is not answered over the return route (repeater with a weak power amplifier): {replies}")
found, back, asked, seconds = ask(bytes.fromhex("deadbeef"), bytes.fromhex("aabbccdd"), first=1e9)
check(found is None and not back and len(asked) > 0, "repeater answered a query addressed to another one")

check(abs(lora_airtime_ms(170, 8, 62500, 8) - 1575) < 60, f"airtime calculated wrongly: {lora_airtime_ms(170, 8, 62500, 8):.0f} ms")
print(f"Provider test: {failures} errors")
sys.exit(1 if failures else 0)

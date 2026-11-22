import hashlib
import math
import random
import struct
import time

CHUNK = 152
HEADER = 4
MAGIC = b"L2"
MSG_OFFER, MSG_REQUEST, MSG_DATA, MSG_PING, MSG_PONG, MSG_SEEK, MSG_QUERY, MSG_HELLO = 1, 2, 3, 4, 5, 6, 7, 8
PEER_LIMIT = 16
REPAIR_LIMIT = 16
ROUTE_TRIES = 3


def lora_airtime_ms(length, sf, bw_hz, cr, preamble=16):
    symbol = (1 << sf) / bw_hz * 1000.0
    low_rate = 1 if symbol > 16.0 else 0
    payload = 8 + max(math.ceil((8 * length - 4 * sf + 28 + 16) / (4 * (sf - 2 * low_rate))) * cr, 0)
    return (preamble + 4.25 + payload) * symbol


def crc16(data):
    crc = 0xFFFF
    for value in data:
        crc ^= value << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc


class Budget:
    def __init__(self, ppm, now):
        self.ppm = ppm
        self.now = now
        self.ms = 2000.0
        self.at = now()

    def take(self, cost):
        now = self.now()
        self.ms = min(10000.0, self.ms + (now - self.at) * 1000.0 * self.ppm / 1e6)
        self.at = now
        if cost > self.ms:
            return False
        self.ms -= cost
        return True


class Finder:
    def __init__(self, self_id, key_id, airtime_ms, budget, first=3.0, least=30.0, most=1800.0, now=time.monotonic, rng=random.random):
        self.self_id = bytes(self_id[:4])
        self.key_ids = [bytes(key_id)] if isinstance(key_id, (bytes, bytearray)) else [bytes(k) for k in key_id]
        self.key_turn = 0
        self.airtime_ms = airtime_ms
        self.budget = budget
        self.now = now
        self.rng = rng
        self.least = least
        self.interval = least
        self.most = most
        self.ask_at = now() + first
        self.sequence = int(rng() * 256) & 0xFF
        self.sent = 0
        self.sent_ms = 0.0
        self.seen = {}
        self.routes = {}

    def route(self, target, path, hop_size):
        target, path = bytes(target[:4]), bytes(path)
        if not path or len(path) % hop_size or len(path) > 6:
            return False
        old = self.routes.get(target.hex())
        if old and old["path"] == path and old["hop_size"] == hop_size:
            if old["tries"] >= ROUTE_TRIES and target.hex() not in self.seen:
                old["tries"] = 0
                old["ask_at"] = self.now() + self.least * self.rng()
            return False
        self.routes[target.hex()] = {"target": target, "path": path, "hop_size": hop_size, "hops": len(path) // hop_size,
                                     "interval": self.least, "tries": 0, "ask_at": self.now() + self.least * self.rng()}
        return True

    def receive(self, payload, snr=None, rssi=None):
        if len(payload) < HEADER + 28 or payload[:2] != MAGIC or payload[2] != MSG_HELLO:
            return None
        body = payload[HEADER:]
        if body[4:12] not in self.key_ids:
            return None
        key = body[0:4].hex()
        route = self.routes.get(key)
        hello = {"id": key, "board": body[12:16], "size": struct.unpack("<I", body[16:20])[0], "hash": body[20:28],
                 "tag": body[28:].decode(errors="replace"), "rssi": None if route else rssi, "snr": None if route else snr, "at": self.now(),
                 "route": route, "hops": route["hops"] if route else 0}
        if route:
            route["tries"] = 0
            route["interval"] = self.most
            route["ask_at"] = self.now() + self.most
        fresh = key not in self.seen or self.seen[key]["hash"] != hello["hash"] or self.seen[key]["hops"] != hello["hops"]
        self.seen[key] = hello
        return hello if fresh else None

    def _key(self):
        self.key_turn += 1
        return self.key_ids[self.key_turn % len(self.key_ids)]

    def _take(self, packet, hops):
        cost = self.airtime_ms(len(packet) + 2 + hops * 3) * (hops + 1)
        if not self.budget.take(cost):
            return False
        self.sequence += 1
        self.sent += 1
        self.sent_ms += cost
        return True

    def poll(self):
        now = self.now()
        for route in sorted(self.routes.values(), key=lambda r: r["ask_at"]):
            if now < route["ask_at"]:
                break
            back = b"".join(reversed([route["path"][i:i + route["hop_size"]] for i in range(0, len(route["path"]), route["hop_size"])]))
            packet = (MAGIC + bytes([MSG_QUERY, self.sequence & 0xFF]) + self.self_id + self._key()
                      + route["target"] + bytes([route["hop_size"], route["hops"]]) + back)
            if not self._take(packet, route["hops"]):
                return None
            route["tries"] += 1
            route["interval"] = min(self.most, route["interval"] * 2)
            route["ask_at"] = now + route["interval"] * (0.5 + 0.5 * self.rng()) if route["tries"] < ROUTE_TRIES else math.inf
            return packet, route["path"], route["hop_size"]
        if now < self.ask_at:
            return None
        packet = MAGIC + bytes([MSG_QUERY, self.sequence & 0xFF]) + self.self_id + self._key()
        if not self._take(packet, 0):
            return None
        self.interval = min(self.most, self.interval * 2)
        self.ask_at = now + self.interval * (0.5 + 0.5 * self.rng())
        return packet, b"", 1

    def ask_again(self, within=5.0):
        self.interval = max(30.0, self.interval / 4)
        self.ask_at = min(self.ask_at, self.now() + within)
        for route in self.routes.values():
            route["tries"] = 0
            route["interval"] = max(30.0, route["interval"] / 4)
            route["ask_at"] = min(route["ask_at"], self.now() + within)


class Provider:
    def __init__(self, manifest, blocks, self_id, airtime_ms, budget_ppm=5000, offer_min=60.0, offer_max=3600.0,
                 now=time.monotonic, rng=random.random, target=None, hops=(), hop_size=2, budget=None):
        self.manifest = manifest
        self.blocks = blocks
        self.self_id = bytes(self_id[:4])
        self.airtime_ms = airtime_ms
        self.budget_ppm = budget_ppm
        self.offer_min = offer_min
        self.offer_max = offer_max
        self.now = now
        self.rng = rng
        self.pages = struct.unpack("<H", manifest[6:8])[0]
        self.map_bytes = (self.pages + 7) // 8
        self.campaign = hashlib.sha256(manifest).digest()[:8]
        self.target = bytes(target[:4]) if target else None
        self.hops = [bytes(h) for h in hops]
        self.hop_size = hop_size
        if any(len(h) != hop_size for h in self.hops):
            raise ValueError("Each hop needs exactly %d bytes" % hop_size)
        if self.hops and not self.target:
            raise ValueError("A route needs a target")
        self.path = b"".join(self.hops)
        board = manifest[20:36].split(b"\0")[0]
        full = bytearray(self.map_bytes)
        for page in range(self.pages):
            full[page >> 3] |= 1 << (page & 7)
        body = (self.self_id + self.campaign + manifest[116:124] + hashlib.sha256(board).digest()[:4]
                + manifest[12:16] + manifest[84:92] + struct.pack("<HH", self.pages, len(manifest)))
        if self.hops:
            body += bytes([1]) + bytes(full) + self.target + bytes([hop_size, len(self.hops)]) + b"".join(reversed(self.hops))
        else:
            body += bytes([0]) + bytes(full)
        self.offer_body = body
        self.sequence = int(rng() * 256) & 0xFF
        self.budget = budget or Budget(budget_ppm, now)
        self.last_path = b""
        self.interval = offer_min
        self.offer_at = now() + (2.0 if self.hops else offer_min * (0.5 + 0.5 * rng()))
        self.repairs = []
        self.wants = {}
        self.pongs = []
        self.sent = 0
        self.sent_ms = 0.0
        self.received = 0
        self.requests = 0
        self.seeks = 0
        self.last_request = None
        self.peers = {}

    def _packet(self, kind, body):
        packet = MAGIC + bytes([kind, self.sequence & 0xFF]) + body
        self.sequence += 1
        return packet

    def _data(self, kind, index, chunk, payload):
        body = self.campaign[:4] + bytes([kind]) + struct.pack("<H", index) + bytes([chunk]) + payload
        return self._packet(MSG_DATA, body + struct.pack("<H", crc16(body)))

    def _item(self, kind, index):
        if kind == 0:
            return self.manifest
        return self.blocks[index] if 0 <= index < self.pages else None

    def _repair(self, kind, index, mask):
        item = self._item(kind, index)
        if item is None:
            return
        mask &= (1 << ((len(item) + CHUNK - 1) // CHUNK)) - 1
        if not mask:
            return
        for entry in self.repairs:
            if entry[0] == kind and entry[1] == index:
                entry[2] |= mask
                return
        if len(self.repairs) < REPAIR_LIMIT:
            self.repairs.append([kind, index, mask])

    def receive(self, payload, snr=None, rssi=None):
        if len(payload) < HEADER or payload[:2] != MAGIC:
            return
        self.received += 1
        kind, body = payload[2], payload[HEADER:]
        if kind == MSG_PING and len(body) >= 8:
            self.pongs.append(body[0:4] + self.self_id + body[4:8])
        elif kind == MSG_SEEK and len(body) >= 12 and body[4:12] == self.campaign and not self.hops:
            self.seeks += 1
            self.interval = self.offer_min
            self.offer_at = min(self.offer_at, self.now() + self.offer_min * 0.5 * self.rng())
        elif kind == MSG_REQUEST and len(body) >= 17:
            if body[0:4] != self.self_id or body[8:16] != self.campaign:
                return
            peer = body[4:8]
            if self.target and peer != self.target:
                return
            self.requests += 1
            self.last_request = self.now()
            note = {"kind": body[16], "missing": None, "snr": snr, "rssi": rssi, "at": self.now()}
            if body[16] == 0 and len(body) >= 21:
                self._repair(0, 0, struct.unpack("<I", body[17:21])[0])
            elif body[16] == 1 and len(body) >= 18 + self.map_bytes:
                if peer not in self.wants and len(self.wants) >= PEER_LIMIT:
                    del self.wants[min(self.wants, key=lambda k: self.peers.get(k.hex(), {}).get("at", 0))]
                if peer not in self.wants:
                    self.interval = self.offer_min
                bits = body[17:17 + self.map_bytes]
                want = {page for page in range(self.pages) if bits[page >> 3] >> (page & 7) & 1}
                note["missing"] = len(want)
                want -= {entry[1] for entry in self.repairs if entry[0] == 1}
                count, pos = body[17 + self.map_bytes], 18 + self.map_bytes
                for _ in range(count):
                    if pos + 6 > len(body):
                        break
                    index, mask = struct.unpack("<HI", body[pos:pos + 6])
                    pos += 6
                    if index < self.pages:
                        self._repair(1, index, mask)
                        want.discard(index)
                self.wants[peer] = want
            self.peers[peer.hex()] = note

    def _next_page(self):
        best, best_count = None, 0
        for page in range(self.pages):
            count = sum(1 for want in self.wants.values() if page in want)
            if count > best_count:
                best, best_count = page, count
        return best

    def _served(self, page):
        for want in self.wants.values():
            want.discard(page)

    def _next_packet(self):
        if self.pongs:
            return self._packet(MSG_PONG, self.pongs[0]), ("pong",)
        if not self.repairs:
            page = self._next_page()
            if page is not None and len(self.blocks[page]) > CHUNK:
                self._repair(1, page, 0xFFFFFFFF)
                self._served(page)
            elif page is not None:
                count, total = 0, 0
                while page + count < self.pages and count < 255 and total + len(self.blocks[page + count]) <= CHUNK:
                    if count > 0 and not any(page + count in want for want in self.wants.values()):
                        break
                    total += len(self.blocks[page + count])
                    count += 1
                return self._data(2, page, count, b"".join(self.blocks[page:page + count])), ("bundle", page, count)
        if self.repairs:
            kind, index, mask = self.repairs[0]
            chunk = (mask & -mask).bit_length() - 1
            item = self._item(kind, index)
            return self._data(kind, index, chunk, item[chunk * CHUNK:(chunk + 1) * CHUNK]), ("chunk",)
        if self.now() >= self.offer_at:
            return self._packet(MSG_OFFER, self.offer_body), ("offer",)
        return None, None

    def busy(self):
        return bool(self.pongs or self.repairs or any(self.wants.values()))

    def poll(self):
        packet, what = self._next_packet()
        if packet is None:
            return None
        routed = bool(self.hops) and what[0] != "pong"
        cost = self.airtime_ms(len(packet) + 2 + (len(self.path) if routed else 0)) * (len(self.hops) + 1 if routed else 1)
        if not self.budget.take(cost):
            self.sequence -= 1
            return None
        self.sent += 1
        self.sent_ms += cost
        self.last_path = self.path if routed else b""
        if what[0] == "pong":
            self.pongs.pop(0)
        elif what[0] == "offer":
            self.interval = min(self.offer_max, self.interval * 2)
            self.offer_at = self.now() + self.interval * (0.5 + 0.5 * self.rng())
        elif what[0] == "bundle":
            for page in range(what[1], what[1] + what[2]):
                self._served(page)
        else:
            entry = self.repairs[0]
            entry[2] &= entry[2] - 1
            if not entry[2]:
                self.repairs.pop(0)
        return packet

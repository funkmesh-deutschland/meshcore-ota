import hashlib
import math
import random
import struct
import time

CHUNK = 152
HEADER = 4
MAGIC = b"L2"
MSG_OFFER, MSG_REQUEST, MSG_DATA, MSG_PING, MSG_PONG, MSG_SEEK, MSG_QUERY, MSG_HELLO = 1, 2, 3, 4, 5, 6, 7, 8
MSG_NOTE, MSG_ASK, MSG_PART = 9, 10, 11
LOAD_WINDOW = 60.0
LOAD_BUSY_PPM = 20000
LOAD_CALM_PPM = 5000
HELLO_CARRYING, HELLO_NEEDS_COMMAND, HELLO_NO_ROLLBACK = 1, 2, 4
NODE_IDLE, NODE_FETCHING, NODE_COMPLETE, NODE_APPLIED, NODE_FAILED = 0, 1, 2, 3, 4
PEER_LIMIT = 16
REPAIR_LIMIT = 16
ROUTE_TRIES = 3
KEY_GAP = 45.0


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
    def __init__(self, ppm, now, most=None):
        self.base = self.ppm = ppm
        self.most = max(ppm, most or ppm)
        self.now = now
        self.ms = 2000.0
        self.at = now()
        self.window_at = self.at
        self.busy_ms = 0.0
        self.heard_ms = 0.0
        self.claimed_at = None

    def claim(self):
        self.claimed_at = self.now()

    def claimed(self):
        return self.claimed_at is not None and self.now() - self.claimed_at < 2.0

    def _fill(self):
        now = self.now()
        self.ms = min(10000.0, self.ms + (now - self.at) * 1000.0 * self.ppm / 1e6)
        self.at = now

    def heard(self, ms):
        self.heard_ms += ms
        self.busy_ms += ms
        if self.most > self.base and self.busy_ms * 1e6 >= LOAD_BUSY_PPM * LOAD_WINDOW * 1000.0:
            self._fill()
            self.ppm = self.base
            self.ms = min(self.ms, 2000.0)
            self.window_at = self.now()
            self.busy_ms = 0.0

    def adapt(self):
        if self.most <= self.base or self.heard_ms == 0.0 or self.now() - self.window_at < LOAD_WINDOW:
            return
        if self.busy_ms * 1e6 <= LOAD_CALM_PPM * LOAD_WINDOW * 1000.0:
            self._fill()
            self.ppm = min(self.most, self.ppm * 2)
        self.window_at = self.now()
        self.busy_ms = 0.0

    def take(self, cost, spare=False):
        self.adapt()
        self._fill()
        if cost > self.ms or (spare and self.claimed()):
            return False
        self.ms -= cost
        return True

    def wait(self, cost):
        have = min(10000.0, self.ms + (self.now() - self.at) * 1000.0 * self.ppm / 1e6)
        return max(0.0, (cost - have) * 1000.0 / self.ppm)


def read_status(extra):
    status = {"node": None, "loader": None, "job": b"", "config": None, "level": None, "campaign": b"", "have": 0, "pages": 0,
              "from": "", "apply_in": None, "flags": 0, "stored": [], "peers": []}
    if len(extra) < 6:
        return status
    status.update(node=extra[0], loader=extra[1], job=extra[2:6])
    more = extra[6:]
    if len(more) < 20:
        return status
    config, level = struct.unpack("<IB", more[0:5])
    have, pages = struct.unpack("<HH", more[9:13])
    wait, flags = struct.unpack("<HB", more[17:20])
    status.update(config=config, level=level * 1000, campaign=more[5:9], have=have, pages=pages, flags=flags,
                  apply_in=None if wait == 0xFFFF else wait * 60.0)
    status["from"] = more[13:17].hex() if any(more[13:17]) else ""
    pos = 20
    if pos < len(more):
        count = more[pos]
        pos += 1
        status["stored"] = [more[pos + 4 * i:pos + 4 * i + 4] for i in range(count) if pos + 4 * i + 4 <= len(more)]
        pos += 4 * count
    if pos < len(more):
        count = more[pos]
        pos += 1
        for i in range(count):
            if pos + 6 > len(more):
                break
            status["peers"].append((more[pos:pos + 4].hex(), struct.unpack("<H", more[pos + 4:pos + 6])[0]))
            pos += 6
    return status


class Watcher:
    def __init__(self, now=time.monotonic):
        self.now = now
        self.links = {}
        self.offers = {}

    def observe(self, payload):
        if len(payload) < HEADER + 17 or payload[:2] != MAGIC:
            return
        kind, body = payload[2], payload[HEADER:]
        if kind == MSG_REQUEST:
            entry = {"from": body[0:4].hex(), "campaign": body[8:12], "at": self.now(), "missing": None}
            if body[16] == 1:
                pages = self.offers.get(body[0:4].hex(), {}).get(body[8:12].hex(), {}).get("pages")
                size = (pages + 7) // 8 if pages else max(0, len(body) - 18)
                entry["missing"] = sum(bin(value).count("1") for value in body[17:17 + size])
                entry["pages"] = pages
            self.links[body[4:8].hex()] = entry
        elif kind == MSG_OFFER and len(body) >= 41:
            self.offers.setdefault(body[0:4].hex(), {})[body[4:8].hex()] = {"board": body[20:24], "pages": struct.unpack("<H", body[36:38])[0], "at": self.now()}


class Settings:
    def __init__(self, blob, self_id, airtime_ms, budget, least=60.0, most=3600.0, now=time.monotonic, rng=random.random):
        self.blob = bytes(blob)
        self.self_id = bytes(self_id[:4])
        self.airtime_ms = airtime_ms
        self.budget = budget
        self.now = now
        self.rng = rng
        self.revision = struct.unpack("<I", blob[8:12])[0]
        self.key_id = blob[12:20]
        self.count = (len(blob) + CHUNK - 1) // CHUNK
        self.least, self.most, self.interval = least, most, least
        self.note_at = now() + 5.0 + least * 0.5 * rng()
        self.queue = []
        self.pushed = {}
        self.sequence = int(rng() * 256) & 0xFF
        self.sent = 0
        self.sent_ms = 0.0
        self.sent_at = now() - most
        self.asks = 0

    def receive(self, payload):
        if len(payload) < HEADER + 18 or payload[:2] != MAGIC:
            return
        kind, body = payload[2], payload[HEADER:]
        if kind == MSG_NOTE and body[4:12] == self.key_id and struct.unpack("<I", body[12:16])[0] < self.revision:
            self.interval = self.least
            self.note_at = min(self.note_at, self.now() + 2.0 + 3.0 * self.rng())
        elif kind == MSG_ASK and len(body) >= 20 and body[4:12] == self.key_id and struct.unpack("<I", body[12:16])[0] == self.revision:
            self.asks += 1
            mask = struct.unpack("<I", body[16:20])[0]
            for index in range(self.count):
                if mask >> index & 1 and (index, b"", 1) not in self.queue:
                    self.queue.append((index, b"", 1))

    def push(self, target, path, hop_size, again=1800.0):
        if self.now() - self.pushed.get(target, -again) < again:
            return False
        self.pushed[target] = self.now()
        self.queue += [(index, bytes(path), hop_size) for index in range(self.count)]
        return True

    def announce(self):
        self.interval = self.least
        self.note_at = min(self.note_at, self.now() + 2.0)

    def busy(self):
        return bool(self.queue)

    def poll(self):
        if self.queue:
            index, path, hop_size = self.queue[0]
            packet = (MAGIC + bytes([MSG_PART, self.sequence & 0xFF]) + self.key_id[:4] + struct.pack("<IHB", self.revision, len(self.blob), index)
                      + self.blob[index * CHUNK:(index + 1) * CHUNK])
        elif self.now() >= self.note_at:
            path, hop_size = b"", 1
            packet = MAGIC + bytes([MSG_NOTE, self.sequence & 0xFF]) + self.self_id + self.key_id + struct.pack("<IH", self.revision, len(self.blob))
        else:
            return None
        hops = len(path) // hop_size
        cost = self.airtime_ms(len(packet) + 2 + len(path)) * (hops + 1)
        if not self.budget.take(cost, self.now() - self.sent_at < self.most):
            return None
        self.sent_at = self.now()
        self.sequence += 1
        self.sent += 1
        self.sent_ms += cost
        if self.queue:
            self.queue.pop(0)
        else:
            self.interval = min(self.most, self.interval * 2)
            self.note_at = self.now() + self.interval * (0.5 + 0.5 * self.rng())
        return packet, path, hop_size


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
        self.pongs = []
        self.asked_at = now()

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
        if len(payload) >= HEADER + 8 and payload[:2] == MAGIC and payload[2] == MSG_PING:
            if len(self.pongs) < PEER_LIMIT:
                self.pongs.append(payload[HEADER:HEADER + 4] + self.self_id + payload[HEADER + 4:HEADER + 8])
            return None
        if len(payload) < HEADER + 28 or payload[:2] != MAGIC or payload[2] != MSG_HELLO:
            return None
        body = payload[HEADER:]
        if body[4:12] not in self.key_ids:
            return None
        key = body[0:4].hex()
        route = self.routes.get(key)
        tag, _, extra = body[28:].partition(b"\0")
        hello = {"id": key, "key": body[4:12], "board": body[12:16], "size": struct.unpack("<I", body[16:20])[0], "hash": body[20:28],
                 "tag": tag.decode(errors="replace"), "rssi": None if route else rssi, "snr": None if route else snr, "at": self.now(),
                 "route": route, "hops": route["hops"] if route else 0}
        hello.update(read_status(extra))
        if route:
            route["tries"] = 0
            route["interval"] = self.most
            route["ask_at"] = self.now() + self.most
        fresh = key not in self.seen or any(self.seen[key].get(field) != hello[field] for field in ("hash", "hops", "node", "loader"))
        self.seen[key] = hello
        return hello if fresh else None

    def _take(self, packet, hops):
        cost = self.airtime_ms(len(packet) + 2 + hops * 3) * (hops + 1)
        if not self.budget.take(cost, packet[2] == MSG_QUERY and self.quiet() < self.most):
            return False
        if packet[2] == MSG_QUERY:
            self.asked_at = self.now()
        self.sequence += 1
        self.sent += 1
        self.sent_ms += cost
        return True

    def quiet(self):
        return self.now() - self.asked_at

    def poll(self):
        now = self.now()
        if self.pongs:
            packet = MAGIC + bytes([MSG_PONG, self.sequence & 0xFF]) + self.pongs[0]
            if not self._take(packet, 0):
                return None
            self.pongs.pop(0)
            return packet, b"", 1
        for route in sorted(self.routes.values(), key=lambda r: r["ask_at"]):
            if now < route["ask_at"]:
                break
            back = b"".join(reversed([route["path"][i:i + route["hop_size"]] for i in range(0, len(route["path"]), route["hop_size"])]))
            packet = (MAGIC + bytes([MSG_QUERY, self.sequence & 0xFF]) + self.self_id + self.key_ids[route.get("turn", 0) % len(self.key_ids)]
                      + route["target"] + bytes([route["hop_size"], route["hops"]]) + back)
            if not self._take(packet, route["hops"]):
                return None
            route["turn"] = (route.get("turn", 0) + 1) % len(self.key_ids)
            if route["turn"]:
                route["ask_at"] = now + KEY_GAP
                return packet, route["path"], route["hop_size"]
            route["tries"] += 1
            route["interval"] = min(self.most, route["interval"] * 2)
            route["ask_at"] = now + route["interval"] * (0.5 + 0.5 * self.rng()) if route["tries"] < ROUTE_TRIES else math.inf
            return packet, route["path"], route["hop_size"]
        if now < self.ask_at:
            return None
        packet = MAGIC + bytes([MSG_QUERY, self.sequence & 0xFF]) + self.self_id + self.key_ids[self.key_turn % len(self.key_ids)]
        if not self._take(packet, 0):
            return None
        self.key_turn = (self.key_turn + 1) % len(self.key_ids)
        if self.key_turn:
            self.ask_at = now + KEY_GAP
            return packet, b"", 1
        self.interval = min(self.most, self.interval * 2)
        self.ask_at = now + self.interval * (0.5 + 0.5 * self.rng())
        return packet, b"", 1

    def refresh(self, key, within=5.0):
        route = self.routes.get(key)
        if route:
            route["tries"] = 0
            route["ask_at"] = min(route["ask_at"], self.now() + within)
        else:
            self.ask_at = min(self.ask_at, self.now() + within)

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
        self.offered_at = now() - offer_max
        self.repairs = []
        self.wants = {}
        self.answer_pings = True
        self.pongs = []
        self.sent = 0
        self.sent_ms = 0.0
        self.waiting = 0.0
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
        if kind == MSG_PING and len(body) >= 8 and self.answer_pings:
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
        loading = bool(self.repairs or any(self.wants.values()))
        if self.now() >= self.offer_at and (not loading or (not self.hops and self.now() - self.offered_at >= self.offer_max)):
            return self._packet(MSG_OFFER, self.offer_body), ("offer",)
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
        return None, None

    def busy(self):
        return bool(self.pongs or self.repairs or any(self.wants.values()))

    def parts(self, kind, index):
        item = self._item(kind, index)
        return (len(item) + CHUNK - 1) // CHUNK if item else 0

    def _packets(self, pages):
        small = sorted(page for page in pages if len(self.blocks[page]) <= CHUNK)
        count, i = sum(self.parts(1, page) for page in pages if len(self.blocks[page]) > CHUNK), 0
        while i < len(small):
            total, k = len(self.blocks[small[i]]), i + 1
            while k < len(small) and k - i < 255 and small[k] == small[k - 1] + 1 and total + len(self.blocks[small[k]]) <= CHUNK:
                total += len(self.blocks[small[k]])
                k += 1
            count += 1
            i = k
        return count

    def packets_total(self):
        return self._packets({page for page in range(self.pages) if self.blocks[page]})

    def packets_left(self, peer=None):
        queued = [entry for entry in self.repairs if entry[0] == 1]
        wanted = set(self.wants.get(peer, ())) if peer else set().union(*self.wants.values())
        return sum(bin(entry[2]).count("1") for entry in queued) + self._packets(wanted - {entry[1] for entry in queued})

    def current(self):
        if not self.repairs:
            return None
        kind, index, mask = self.repairs[0]
        total = self.parts(kind, index)
        return kind, index, total - min(total, bin(mask).count("1")), total

    def packet_cost(self):
        length = HEADER + 10 + CHUNK + 2
        return self.airtime_ms(length + len(self.path)) * (len(self.hops) + 1)

    def next_in(self):
        return self.budget.wait(self.waiting) if self.waiting else None

    def time_left(self, peer=None):
        left = self.packets_left(peer)
        return self.budget.wait(left * self.packet_cost()) if left else 0.0

    def poll(self):
        packet, what = self._next_packet()
        if packet is None:
            self.waiting = 0.0
            return None
        routed = bool(self.hops) and what[0] != "pong"
        cost = self.airtime_ms(len(packet) + 2 + (len(self.path) if routed else 0)) * (len(self.hops) + 1 if routed else 1)
        if not self.budget.take(cost):
            self.sequence -= 1
            self.waiting = cost
            if what[0] in ("bundle", "chunk"):
                self.budget.claim()
            return None
        self.waiting = 0.0
        self.sent += 1
        self.sent_ms += cost
        self.last_path = self.path if routed else b""
        if what[0] == "pong":
            self.pongs.pop(0)
        elif what[0] == "offer":
            self.offered_at = self.now()
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

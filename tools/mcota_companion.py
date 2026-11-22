import asyncio
import struct
import time

CMD_APP_START = 1
CMD_SEND_TXT_MSG = 2
CMD_GET_CONTACTS = 4
CMD_SYNC_NEXT_MESSAGE = 10
CMD_DEVICE_QUERY = 22
CMD_SEND_RAW_DATA = 25
CMD_SEND_LOGIN = 26
CMD_GET_ADVERT_PATH = 42
CMD_PATH_DISCOVERY = 52
RESP_OK = 0
RESP_ERR = 1
RESP_CONTACT = 3
RESP_END_OF_CONTACTS = 4
RESP_SELF_INFO = 5
RESP_SENT = 6
RESP_CONTACT_MSG = 7
RESP_NO_MORE_MESSAGES = 10
RESP_DEVICE_INFO = 13
RESP_CONTACT_MSG_V3 = 16
RESP_ADVERT_PATH = 22
PUSH_ADVERT = 0x80
PUSH_MSG_WAITING = 0x83
PUSH_RAW_DATA = 0x84
PUSH_LOGIN_SUCCESS = 0x85
PUSH_LOGIN_FAIL = 0x86
PUSH_NEW_ADVERT = 0x8A
PUSH_PATH_DISCOVERY = 0x8D
DISCOVER_GAP = 90.0
DISCOVER_AGAIN = 3600.0
MAX_HOPS = 2
WIDE_HOPS_VERSION = 14
TYPE_REPEATER = 2
TXT_CLI = 1
PATH_UNKNOWN = 0xFF
NUS_RX = "6e400002-b5a3-f393-e0a9-e50e24dcca9e"
NUS_TX = "6e400003-b5a3-f393-e0a9-e50e24dcca9e"


class SerialLink:
    def __init__(self, port):
        import serial
        self.port = serial.Serial(port, 115200, timeout=0)
        self.buffer = b""

    async def open(self):
        return None

    async def write(self, frame):
        self.port.write(b"<" + struct.pack("<H", len(frame)) + frame)

    async def frames(self):
        self.buffer += self.port.read(4096)
        out = []
        while True:
            start = self.buffer.find(b">")
            if start < 0:
                self.buffer = b""
                break
            self.buffer = self.buffer[start:]
            if len(self.buffer) < 3:
                break
            size = struct.unpack("<H", self.buffer[1:3])[0]
            if size > 300:
                self.buffer = self.buffer[1:]
                continue
            if len(self.buffer) < 3 + size:
                break
            out.append(self.buffer[3:3 + size])
            self.buffer = self.buffer[3 + size:]
        return out

    async def close(self):
        self.port.close()


class BleLink:
    def __init__(self, address):
        self.address = address
        self.queue = []
        self.client = None

    async def open(self):
        from bleak import BleakClient, BleakScanner
        device = await self._known()
        if device is None:
            device = await BleakScanner.find_device_by_address(self.address, timeout=12.0)
        if device is None:
            raise SystemExit(f"The companion {self.address} cannot be found over Bluetooth. Is it switched on, nearby and paired with this computer?")
        self.client = BleakClient(device)
        try:
            await self.client.connect()
            await self.client.start_notify(NUS_TX, lambda _, data: self.queue.append(bytes(data)))
        except Exception as error:
            raise SystemExit(f"The Bluetooth connection to the companion {self.address} could not be established: {error or type(error).__name__}. "
                             "Is it paired with this computer, and is no other app connected to it?")

    async def _known(self):
        try:
            from bleak.backends.bluezdbus.manager import get_global_bluez_manager
            from bleak.backends.device import BLEDevice
            manager = await get_global_bluez_manager()
            for path, interfaces in manager._properties.items():
                props = interfaces.get("org.bluez.Device1")
                if props and str(props.get("Address", "")).upper() == self.address.upper():
                    return BLEDevice(self.address, props.get("Alias"), {"path": path, "props": props})
        except Exception:
            pass
        return None

    async def write(self, frame):
        await self.client.write_gatt_char(NUS_RX, frame, response=False)

    async def frames(self):
        out, self.queue = self.queue, []
        return out

    async def close(self):
        if self.client:
            await self.client.disconnect()


class Companion:
    def __init__(self, link):
        self.link = link
        self.pending = []
        self.public_key = b""
        self.name = ""
        self.sf, self.bw, self.cr, self.freq, self.tx_power = 8, 62500, 5, 0, 0
        self.firmware = "unknown"
        self.version = 0
        self.names = {}
        self.finder = None
        self.states = {}
        self.heard = []
        self.paths = []
        self.last_error = ""

    async def _wait(self, codes, timeout):
        deadline = time.monotonic() + timeout
        while True:
            self.pending += await self.link.frames()
            for i, frame in enumerate(self.pending):
                if frame and frame[0] in codes:
                    return self.pending.pop(i)
            if time.monotonic() >= deadline:
                return None
            await asyncio.sleep(0.02)

    async def start(self):
        await self.link.open()
        await self.link.write(bytes([CMD_APP_START, 3]) + b" " * 6 + b"mcota")
        info = await self._wait((RESP_SELF_INFO,), 5.0)
        if info is None or len(info) < 58:
            raise SystemExit("The companion does not answer. Is the port right, and does it run companion firmware?")
        self.tx_power = info[2]
        self.public_key = info[4:36]
        self.freq, self.bw = struct.unpack("<II", info[48:56])
        self.sf, self.cr = info[56], info[57]
        self.name = info[58:].decode(errors="replace")
        await self.link.write(bytes([CMD_DEVICE_QUERY, 3]))
        device = await self._wait((RESP_DEVICE_INFO, RESP_ERR), 3.0)
        if device is not None and device[0] == RESP_DEVICE_INFO and len(device) >= 20:
            self.firmware = f"version {device[1]}, {device[8:20].split(bytes([0]))[0].decode(errors='replace')}"
            self.version = device[1]

    async def send_raw(self, payload, path=b"", hop_size=1):
        hops = len(path) // hop_size
        head = ((hop_size - 1) << 6) | hops if hops else 0
        self.pending = [f for f in self.pending if not f or f[0] not in (RESP_OK, RESP_ERR)]
        await self.link.write(bytes([CMD_SEND_RAW_DATA, head]) + path + payload)
        reply = await self._wait((RESP_OK, RESP_ERR), 8.0)
        if reply is None:
            self.last_error = "no confirmation within 8 s, the packet was probably sent anyway"
            return False
        if reply[0] != RESP_OK:
            names = {1: "command not supported", 2: "not found", 3: "send buffer full", 4: "wrong state", 6: "invalid argument"}
            code = reply[1] if len(reply) > 1 else 0
            self.last_error = f"error {code} ({names.get(code, 'unknown')})"
            return False
        return True

    async def contacts(self):
        await self.link.write(bytes([CMD_GET_CONTACTS]))
        found = []
        while True:
            frame = await self._wait((RESP_CONTACT, RESP_END_OF_CONTACTS, RESP_ERR), 5.0)
            if frame is None or frame[0] != RESP_CONTACT:
                break
            if len(frame) < 132:
                continue
            path_len = frame[35]
            hop_size, hops = (path_len >> 6) + 1, path_len & 63
            found.append({
                "key": frame[1:33], "type": frame[33], "name": frame[100:132].split(b"\0")[0].decode(errors="replace"),
                "known": path_len != PATH_UNKNOWN, "hop_size": hop_size if path_len != PATH_UNKNOWN else 0,
                "path": frame[36:36 + hop_size * hops] if path_len != PATH_UNKNOWN else b"",
            })
        return found

    async def login(self, key, password, timeout=None):
        await self.link.write(bytes([CMD_SEND_LOGIN]) + key + password.encode())
        sent = await self._wait((RESP_SENT, RESP_ERR), 3.0)
        if sent is None or sent[0] != RESP_SENT:
            return False
        wait = timeout or max(10.0, struct.unpack("<I", sent[6:10])[0] / 1000.0 * 2 + 5.0)
        reply = await self._wait((PUSH_LOGIN_SUCCESS, PUSH_LOGIN_FAIL), wait)
        return reply is not None and reply[0] == PUSH_LOGIN_SUCCESS

    async def command(self, key, text, timeout=None):
        await self.link.write(bytes([CMD_SEND_TXT_MSG, TXT_CLI, 0]) + struct.pack("<I", int(time.time())) + key[:6] + text.encode())
        sent = await self._wait((RESP_SENT, RESP_ERR), 3.0)
        if sent is None or sent[0] != RESP_SENT:
            return None
        deadline = time.monotonic() + (timeout or max(15.0, struct.unpack("<I", sent[6:10])[0] / 1000.0 * 2 + 10.0))
        while time.monotonic() < deadline:
            await self.link.write(bytes([CMD_SYNC_NEXT_MESSAGE]))
            frame = await self._wait((RESP_CONTACT_MSG, RESP_CONTACT_MSG_V3, RESP_NO_MORE_MESSAGES), 3.0)
            if frame is None or frame[0] == RESP_NO_MORE_MESSAGES:
                await self._wait((PUSH_MSG_WAITING,), 2.0)
                continue
            body = frame[4:] if frame[0] == RESP_CONTACT_MSG_V3 else frame[1:]
            if len(body) >= 12 and body[:6] == key[:6] and body[7] == TXT_CLI:
                return body[12:].decode(errors="replace")
        return None

    async def advert_path(self, key):
        self.pending = [f for f in self.pending if not f or f[0] != RESP_ADVERT_PATH]
        await self.link.write(bytes([CMD_GET_ADVERT_PATH, 0]) + bytes(key[:32]))
        frame = await self._wait((RESP_ADVERT_PATH, RESP_ERR), 3.0)
        if frame is None or frame[0] != RESP_ADVERT_PATH or len(frame) < 6:
            return None
        path_len = frame[5]
        hop_size, hops = (path_len >> 6) + 1, path_len & 63
        if len(frame) < 6 + hop_size * hops:
            return None
        inbound = [frame[6 + i * hop_size:6 + (i + 1) * hop_size] for i in range(hops)]
        return b"".join(reversed(inbound)), hop_size

    async def discover(self, key):
        self.pending = [f for f in self.pending if not f or f[0] not in (RESP_SENT, RESP_ERR)]
        await self.link.write(bytes([CMD_PATH_DISCOVERY, 0]) + bytes(key[:32]))
        reply = await self._wait((RESP_SENT, RESP_ERR), 3.0)
        return reply is not None and reply[0] == RESP_SENT

    async def received(self):
        frames, self.pending = self.pending + await self.link.frames(), []
        out = []
        for frame in frames:
            if len(frame) > 4 and frame[0] == PUSH_RAW_DATA:
                out.append((frame[4:], struct.unpack("b", frame[1:2])[0] / 4.0, struct.unpack("b", frame[2:3])[0]))
            elif len(frame) >= 33 and frame[0] in (PUSH_ADVERT, PUSH_NEW_ADVERT):
                self.heard.append((frame[1:33], frame[33] if frame[0] == PUSH_NEW_ADVERT and len(frame) > 33 else None))
            elif len(frame) >= 9 and frame[0] == PUSH_PATH_DISCOVERY:
                hop_size, hops = (frame[8] >> 6) + 1, frame[8] & 63
                if len(frame) >= 9 + hop_size * hops:
                    self.paths.append((frame[2:8], frame[9:9 + hop_size * hops], hop_size))
        return out


def find_contact(contacts, wanted):
    wanted = wanted.strip().lower()
    hits = [c for c in contacts if c["key"].hex().startswith(wanted)] or [c for c in contacts if c["name"].lower() == wanted]
    if not hits:
        hits = [c for c in contacts if wanted in c["name"].lower()]
    if len(hits) != 1:
        names = ", ".join(c["name"] for c in hits) or "none"
        raise SystemExit(f"'{wanted}' does not match exactly one contact of the companion (matches: {names}).")
    return hits[0]


def hops_text(path, hop_size):
    return " > ".join(path[i:i + hop_size].hex() for i in range(0, len(path), hop_size))


def fit_route(companion, path, hop_size):
    if hop_size > 1 and companion.version < WIDE_HOPS_VERSION:
        return b"".join(path[i:i + 1] for i in range(0, len(path), hop_size)), 1
    return path, hop_size


async def learn_route(companion, finder, key, contact, max_hops, report, providers):
    found = await companion.advert_path(key)
    if found is None and contact is not None and contact["known"] and contact["path"]:
        found = (contact["path"], contact["hop_size"])
    if found is None or not found[0] or len(found[0]) // found[1] > max_hops:
        return
    path, hop_size = fit_route(companion, *found)
    if finder.route(key[:4], path, hop_size):
        name = companion.names.get(key[:4].hex(), key[:4].hex())
        report(companion, providers, False, f"Route to {name}: via {hops_text(path, hop_size)}")


async def serve(link, packages, budget_ppm, offer_min, offer_max, report, every=5.0, key_id=None, resolve=None, max_hops=MAX_HOPS, routes=(), mine=None):
    from mcota_provider import Budget, Finder, Provider, lora_airtime_ms
    companion = Companion(link)
    await companion.start()
    contacts = await companion.contacts()
    companion.names = {c["key"][:4].hex(): c["name"] for c in contacts}
    repeaters = {c["key"]: c for c in contacts if c["type"] == TYPE_REPEATER}
    budget = Budget(budget_ppm, time.monotonic)
    airtime = lambda n: lora_airtime_ms(n, companion.sf, companion.bw, companion.cr)

    def provider_for(manifest, blocks, route=None):
        extra = {"target": route["target"], "hops": [route["path"][i:i + route["hop_size"]] for i in range(0, len(route["path"]), route["hop_size"])],
                 "hop_size": route["hop_size"]} if route else {}
        return Provider(manifest, blocks, companion.public_key[:4], airtime, budget_ppm=budget_ppm, offer_min=offer_min, offer_max=offer_max, budget=budget, **extra)

    providers = [provider_for(manifest, blocks) for manifest, blocks in packages]
    finder = Finder(companion.public_key[:4], key_id, airtime, budget, least=offer_min / 2, most=offer_max) if key_id else None
    companion.finder = finder
    companion.states = {}
    pending = []
    report(companion, providers, True)
    fixed = {target for target, _, _ in routes}
    if routes and not finder:
        raise SystemExit("--route needs packages with exactly one key")
    for target, path, hop_size in routes:
        path, hop_size = fit_route(companion, path, hop_size)
        finder.route(target, path, hop_size)
        report(companion, providers, False, f"Route to {companion.names.get(target.hex(), target.hex())}: via {hops_text(path, hop_size)} (by hand)")
    if finder and max_hops > 0:
        for key, contact in repeaters.items():
            if key[:4] not in fixed:
                await learn_route(companion, finder, key, contact, max_hops, report, providers)
    last, turn = 0.0, 0
    searched, search_at = {}, time.monotonic() + 20.0
    try:
        while True:
            for payload, snr, rssi in await companion.received():
                for provider in providers:
                    provider.receive(payload, snr, rssi)
                hello = finder.receive(payload, snr, rssi) if finder else None
                if hello and resolve:
                    hello["name"] = companion.names.get(hello["id"], "")
                    companion.states[hello["id"]] = "checking"
                    pending.append((hello, asyncio.ensure_future(asyncio.to_thread(resolve, hello, companion.states))))
            heard, companion.heard = companion.heard, []
            for key, kind in heard:
                if finder and max_hops > 0 and key[:4] not in fixed and (kind == TYPE_REPEATER or key in repeaters):
                    await learn_route(companion, finder, key, repeaters.get(key), max_hops, report, providers)
            found, companion.paths = companion.paths, []
            for prefix, path, hop_size in found:
                key = next((k for k in repeaters if k[:6] == prefix), None)
                if key is None or not finder or key[:4] in fixed:
                    continue
                name = companion.names.get(key[:4].hex(), key[:4].hex())
                if not path:
                    report(companion, providers, False, f"Path discovery: {name} is directly reachable")
                elif len(path) // hop_size > max_hops:
                    report(companion, providers, False, f"Path discovery: {name} is {len(path) // hop_size} hops away, that is too far")
                else:
                    path, hop_size = fit_route(companion, path, hop_size)
                    finder.route(key[:4], path, hop_size)
                    finder.ask_again(3.0)
                    report(companion, providers, False, f"Route to {name}: via {hops_text(path, hop_size)} (discovered)")
            wanted = mine() if mine else ()
            if finder and max_hops > 0 and wanted and time.monotonic() >= search_at and not any(p.busy() for p in providers):
                now = time.monotonic()
                for key in repeaters:
                    ident = key[:4].hex()
                    if ident not in wanted or ident in finder.seen or ident in finder.routes or key[:4] in fixed or now - searched.get(key, -DISCOVER_AGAIN) < DISCOVER_AGAIN:
                        continue
                    searched[key] = now
                    search_at = now + DISCOVER_GAP
                    if await companion.discover(key):
                        report(companion, providers, False, f"Discovering the route to {companion.names.get(ident, ident)}")
                    break
            for item in list(pending):
                hello, task = item
                if not task.done():
                    continue
                pending.remove(item)
                try:
                    result = task.result()
                except SystemExit as error:
                    result = str(error)
                except Exception as error:
                    result = f"Error: {error}"
                name = companion.names.get(hello["id"], hello["id"])
                if isinstance(result, str):
                    companion.states[hello["id"]] = result
                    report(companion, providers, False, f"{name}: {result}")
                else:
                    manifest, blocks, text = result
                    route = hello.get("route")
                    companion.states[hello["id"]] = text
                    if route:
                        if all(p.manifest != manifest or p.target != route["target"] for p in providers):
                            providers.append(provider_for(manifest, blocks, route))
                            providers[-1].offer_at = time.monotonic() + 1.0
                        text += f", via {hops_text(route['path'], route['hop_size'])}"
                    elif all(p.manifest != manifest or p.target for p in providers):
                        providers.append(provider_for(manifest, blocks))
                        providers[-1].offer_at = time.monotonic() + 1.0
                    report(companion, providers, False, f"{name}: {text}")
            sent = False
            for step in range(len(providers)):
                provider = providers[(turn + step) % len(providers)]
                packet = provider.poll()
                if packet is None:
                    continue
                turn = (turn + step + 1) % len(providers)
                if not await companion.send_raw(packet, provider.last_path, provider.hop_size):
                    report(companion, providers, False, f"Companion: {companion.last_error}")
                sent = True
                break
            if not sent and finder and not any(p.busy() for p in providers):
                asked = finder.poll()
                if asked is not None and not await companion.send_raw(*asked):
                    report(companion, providers, False, f"Companion: {companion.last_error}")
            if time.monotonic() - last > every:
                last = time.monotonic()
                report(companion, providers, False)
            await asyncio.sleep(0.05)
    finally:
        await link.close()


async def with_companion(link, action):
    companion = Companion(link)
    await companion.start()
    try:
        return await action(companion)
    finally:
        await link.close()


async def known_ble():
    import shutil
    tool = shutil.which("bluetoothctl")
    if not tool:
        return []
    try:
        proc = await asyncio.create_subprocess_exec(tool, "devices", "Connected", stdout=asyncio.subprocess.PIPE, stderr=asyncio.subprocess.DEVNULL)
        text, _ = await asyncio.wait_for(proc.communicate(), 5.0)
    except (OSError, asyncio.TimeoutError):
        return []
    out = []
    for line in text.decode(errors="replace").splitlines():
        parts = line.split(" ", 2)
        if len(parts) == 3 and parts[0] == "Device":
            out.append((parts[1], parts[2]))
    return out


async def scan_ble(seconds=6.0):
    from bleak import BleakScanner
    found = await BleakScanner.discover(timeout=seconds, return_adv=True)
    out = []
    for device, adv in found.values():
        services = [u.lower() for u in (adv.service_uuids or [])]
        name = adv.local_name or device.name or ""
        if "6e400001-b5a3-f393-e0a9-e50e24dcca9e" in services or name.lower().startswith("meshcore"):
            out.append((device.address, name, adv.rssi))
    return out

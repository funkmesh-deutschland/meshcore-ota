#!/usr/bin/env python3
import argparse
import bisect
import collections
import hashlib
import os
import struct
import sys
import zlib

import bsdiff4.core
from cryptography.hazmat.primitives import serialization
from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey, Ed25519PublicKey

PAGE = 4096
CHUNK = 152
ITEM_MAX = 32 * CHUNK
OPS_MAX = 80
BLOCK_MAX = 4800
MAGIC = b"LOTA"
FORMAT = 5


def pack(data):
    packer = zlib.compressobj(9, zlib.DEFLATED, -15, 9)
    return packer.compress(bytes(data)) + packer.flush()


def unpack(data):
    return zlib.decompress(bytes(data), -15)


def page_ops(old, new):
    control, diff, extra = bsdiff4.core.diff(old, new)
    ops = []
    old_pos = new_pos = d = e = 0
    for i in range(0, len(control), 24):
        add, ext, seek = (-(v & 0x7FFFFFFFFFFFFFFF) if v >> 63 else v for v in struct.unpack("<QQQ", control[i:i + 24]))
        piece = diff[d:d + add]
        lead = min(add, max(0, -old_pos))
        if lead:
            ops.append((new_pos, 0, b"", piece[:lead]))
        tail = max(lead, min(add, len(old) - old_pos)) if old_pos < len(old) else lead
        if tail > lead:
            ops.append((new_pos + lead, old_pos + lead, piece[lead:tail], b""))
        if add > tail:
            ops.append((new_pos + tail, 0, b"", piece[tail:]))
        ops.append((new_pos + add, 0, b"", extra[e:e + ext]))
        d += add
        e += ext
        new_pos += add + ext
        old_pos += add + seek
    return ops


def varint(value):
    out = bytearray()
    while True:
        low = value & 0x7F
        value >>= 7
        if value:
            out.append(low | 0x80)
        else:
            out.append(low)
            return bytes(out)


def zigzag(value):
    return ((value << 1) ^ (value >> 31)) & 0xFFFFFFFF


def read_varint(data, pos):
    value = shift = 0
    while True:
        byte = data[pos]
        pos += 1
        value |= (byte & 0x7F) << shift
        if not byte & 0x80:
            return value, pos
        shift += 7


def unzigzag(value):
    return (value >> 1) ^ -(value & 1)


def encode_block(index, merged, sparse):
    head = bytearray([len(merged) | (0x80 if sparse else 0)])
    expect = index * PAGE
    for opos, diff, extra in merged:
        head += varint(len(diff)) + varint(len(extra))
        if diff:
            head += varint(zigzag(opos - expect))
            expect = opos + len(diff)
        expect += len(extra)
    diff = b"".join(d for _, d, _ in merged)
    if sparse:
        gaps, values, run = bytearray(), bytearray(), 0
        for byte in diff:
            if byte:
                gaps += varint(run)
                values.append(byte)
                run = 0
            else:
                run += 1
        diff = varint(len(values)) + bytes(gaps) + bytes(values)
    return bytes(head) + diff + b"".join(x for _, _, x in merged)


def decode_block(index, raw):
    count, sparse = raw[0] & 0x7F, raw[0] >> 7
    pos, expect, ops, total = 1, index * PAGE, [], 0
    for _ in range(count):
        add, pos = read_varint(raw, pos)
        ext, pos = read_varint(raw, pos)
        opos = 0
        if add:
            step, pos = read_varint(raw, pos)
            opos = expect + unzigzag(step)
            expect = opos + add
        expect += ext
        total += add
        ops.append((opos, add, ext))
    if sparse:
        count, pos = read_varint(raw, pos)
        diff, at = bytearray(total), -1
        places = []
        for _ in range(count):
            gap, pos = read_varint(raw, pos)
            at += gap + 1
            places.append(at)
        for place in places:
            diff[place] = raw[pos]
            pos += 1
    else:
        diff = raw[pos:pos + total]
        pos += total
    return ops, bytes(diff), raw[pos:]


def build_blocks(old, new, direction=None, ops=None):
    ops = ops or page_ops(old, new)
    pages = (len(new) + PAGE - 1) // PAGE
    per_page = [[] for _ in range(pages)]
    for new_pos, old_pos, diff, extra in ops:
        pos, opos, rest = new_pos, old_pos, diff
        while rest:
            page = pos // PAGE
            take = min(len(rest), (page + 1) * PAGE - pos)
            per_page[page].append([opos, rest[:take], b""])
            pos += take
            opos += take
            rest = rest[take:]
        rest = extra
        while rest:
            page = pos // PAGE
            take = min(len(rest), (page + 1) * PAGE - pos)
            per_page[page].append([0, b"", rest[:take]])
            pos += take
            rest = rest[take:]
    blocks = []
    for index, entries in enumerate(per_page):
        if direction is not None:
            limited, cursor = [], index * PAGE
            low, high = (index * PAGE, 1 << 40) if direction == 0 else (0, (index + 1) * PAGE)
            for opos, diff, extra in entries:
                count = len(diff)
                if count:
                    first, last = max(opos, low), min(opos + count, high)
                    if last <= first:
                        limited.append([0, b"", new[cursor:cursor + count]])
                    else:
                        if first > opos:
                            limited.append([0, b"", new[cursor:cursor + first - opos]])
                        limited.append([first, diff[first - opos:last - opos], b""])
                        if last < opos + count:
                            limited.append([0, b"", new[cursor + last - opos:cursor + count]])
                    cursor += count
                if extra:
                    limited.append([0, b"", extra])
                    cursor += len(extra)
            entries = limited
        merged = []
        for opos, diff, extra in entries:
            if merged and not diff:
                merged[-1][2] += extra
            else:
                merged.append([opos, diff, extra])
        page = new[index * PAGE:(index + 1) * PAGE]
        candidates = [pack(encode_block(index, [[0, b"", page]], False))]
        if len(merged) <= OPS_MAX:
            for sparse in (False, True):
                raw = encode_block(index, merged, sparse)
                if len(raw) <= BLOCK_MAX:
                    candidates.append(pack(raw))
        block = min(candidates, key=len)
        if len(block) > BLOCK_MAX:
            raise SystemExit(f"Page {index} cannot be packed small enough")
        blocks.append(block)
    return blocks


def apply_blocks(old, blocks, new_size, direction=None):
    out = bytearray()
    for index, block in enumerate(blocks):
        low, high = (0, 1 << 40) if direction is None else ((index * PAGE, 1 << 40) if direction == 0 else (0, (index + 1) * PAGE))
        ops, diff, extra = decode_block(index, unpack(block))
        d = x = 0
        for pos, add, ext in ops:
            if add and (pos < low or min(pos + add, len(old)) > high):
                raise SystemExit("Internal error: block reads pages that are already overwritten")
            for k in range(add):
                base = old[pos + k] if pos + k < len(old) else 0
                out.append((base + diff[d]) & 0xFF)
                d += 1
            out += extra[x:x + ext]
            x += ext
    return bytes(out[:new_size])


def key_id(public_bytes):
    return hashlib.sha256(public_bytes).digest()[:8]


def load_private(path):
    raw = open(path, "rb").read().strip()
    return Ed25519PrivateKey.from_private_bytes(bytes.fromhex(raw.decode()))


def public_bytes(private):
    return private.public_key().public_bytes(serialization.Encoding.Raw, serialization.PublicFormat.Raw)


def fixed(text, size):
    raw = text.encode()
    if len(raw) >= size:
        raise SystemExit(f"'{text}' is too long (at most {size - 1} characters)")
    return raw + b"\0" * (size - len(raw))


SHIFT_MAX = 100
POINTER_SHIFT_MAX = 80
SHIFT_BYTES = 1200
RAM_LOW, RAM_HIGH = 0x20000000, 0x20100000


def is_branch(data, i):
    return (data[i + 1] & 0xF8) == 0xF0 and (data[i + 3] & 0xF8) in (0xF8, 0xB8)


def branch_get(data, i):
    return (((data[i + 1] & 7) << 19) | (data[i] << 11) | ((data[i + 3] & 7) << 8) | data[i + 2]) << 1


def branch_put(data, i, value):
    value = (value & 0x7FFFFF) >> 1
    data[i + 1] = 0xF0 | ((value >> 19) & 7)
    data[i] = (value >> 11) & 0xFF
    data[i + 3] = (data[i + 3] & 0xF8) | ((value >> 8) & 7)
    data[i + 2] = value & 0xFF


def branches(image, change):
    out = bytearray(image)
    for page in range(0, len(out), PAGE):
        end = min(page + PAGE, len(out))
        i = page
        while i + 4 <= end:
            if is_branch(out, i):
                branch_put(out, i, change(branch_get(out, i), i + 4))
                i += 4
            else:
                i += 2
    return bytes(out)


def shift_lookup(shifts):
    starts = [start for start, _, _ in shifts]

    def lookup(target):
        k = bisect.bisect_right(starts, target) - 1
        if k >= 0 and target - shifts[k][0] < shifts[k][1]:
            return shifts[k][2]
        return 0
    return lookup


def pointer_predict(image, shifts, base):
    if not shifts:
        return image
    lookup = shift_lookup(shifts)
    low, high = shifts[0][0], shifts[-1][0] + shifts[-1][1]
    out = bytearray(image)
    for i in range(0, len(out) - 3, 4):
        value = out[i] | out[i + 1] << 8 | out[i + 2] << 16 | out[i + 3] << 24
        target = ((value & ~1) - base) & 0xFFFFFFFF
        if low <= target < high:
            delta = lookup(target)
            if delta:
                out[i:i + 4] = ((value + delta) & 0xFFFFFFFF).to_bytes(4, "little")
    return bytes(out)


def branch_predict(image, shifts, base=0):
    lookup = shift_lookup(shifts)

    def change(value, pos):
        target = (value + pos) & 0x7FFFFF
        return target + lookup(target)
    return branches(pointer_predict(image, shifts, base), change)


def branch_absolute(image):
    return branches(image, lambda value, pos: (value + pos) & 0x7FFFFF)


def branch_restore(image):
    return branches(image, lambda value, pos: value - pos)


def literal_loads(image, low, high, lookup):
    out = bytearray(image)
    for page in range(low - low % PAGE, high, PAGE):
        i, end = max(page, low), min(page + PAGE, high, len(out))
        while i + 3 <= end:
            op = out[i] & 0x0F
            if op >= 8:
                i += 2
                continue
            if op == 1:
                imm, at = out[i + 1] | out[i + 2] << 8, i + 3
                if lookup is None:
                    value = imm - (at >> 2)
                else:
                    value = imm + (at >> 2) + (lookup(((at & ~3) + ((imm | 0xFFFF0000) << 2)) & 0xFFFFFFFF) >> 2)
                out[i + 1], out[i + 2] = value & 0xFF, (value >> 8) & 0xFF
            i += 3
    return bytes(out)


def esp_code(image):
    if image[:1] != b"\xe9" or len(image) < 24 or struct.unpack_from("<H", image, 12)[0] not in (0, 2, 9):
        return None
    pos, best = 24, None
    for _ in range(image[1]):
        if pos + 8 > len(image):
            return None
        address, size = struct.unpack_from("<II", image, pos)
        if 0x40000000 <= address < 0x50000000 and (best is None or size > best[1]):
            best = (pos + 8, size, address)
        pos += 8 + size
    return best


class View:
    def __init__(self, old, new, base):
        self.kind, self.base, self.ranges = "thumb", base, (0, 0, 0, 0)
        self.low, self.high, self.step = RAM_LOW, RAM_HIGH, 2
        if new[:1] == b"\xe9":
            self.kind, self.base = None, 0
            before, after = esp_code(old), esp_code(new)
            if before and after and before[2] - before[0] == after[2] - after[0] and (before[2] - before[0]) % PAGE == 0:
                self.kind, self.base = "xtensa", before[2] - before[0]
                self.ranges = (before[0], before[1], after[0], after[1])
                self.low, self.high, self.step = 0x3C000000, 0x60100000, 4

    def predict(self, old, shifts):
        if self.kind == "thumb":
            return branch_predict(old, shifts, self.base)
        out = pointer_predict(old, shifts, self.base)
        if self.kind == "xtensa":
            out = literal_loads(out, self.ranges[0], self.ranges[0] + self.ranges[1], shift_lookup(shifts))
        return out

    def absolute(self, new):
        if self.kind == "thumb":
            return branch_absolute(new)
        if self.kind == "xtensa":
            return literal_loads(new, self.ranges[2], self.ranges[2] + self.ranges[3], lambda target: 0)
        return new

    def restore(self, image):
        if self.kind == "thumb":
            return branch_restore(image)
        if self.kind == "xtensa":
            return literal_loads(image, self.ranges[2], self.ranges[2] + self.ranges[3], None)
        return image

    def tail(self):
        return b"".join(varint(v) for v in self.ranges) if self.kind == "xtensa" else b""

    def flags(self):
        return {"thumb": 16, "xtensa": 32}.get(self.kind, 0)


def find_shifts(old, new, view):
    if view.kind is None:
        return []
    control, _, _ = bsdiff4.core.diff(view.predict(old, []), view.absolute(new))
    low, high = (view.ranges[0], view.ranges[0] + view.ranges[1]) if view.kind == "xtensa" else (0, len(old))
    found, old_pos, new_pos = [], 0, 0
    for i in range(0, len(control), 24):
        add, ext, seek = (-(v & 0x7FFFFFFFFFFFFFFF) if v >> 63 else v for v in struct.unpack("<QQQ", control[i:i + 24]))
        delta = new_pos - old_pos
        if add >= 48 and old_pos >= low and old_pos + add <= high and delta % view.step == 0:
            if found and found[-1][2] == delta and old_pos - found[-1][1] < PAGE:
                found[-1][1] = old_pos + add
            else:
                found.append([old_pos, old_pos + add, delta])
        old_pos += add + seek
        new_pos += add + ext
    found.sort()
    clean = []
    for start, end, delta in found:
        if clean and start < clean[-1][0] + clean[-1][1]:
            start = clean[-1][0] + clean[-1][1]
        if end > start and delta != 0 and end < 1 << 24 and abs(delta) < 1 << 22:
            clean.append((start, end - start, delta))
    if len(clean) > SHIFT_MAX:
        clean = sorted(sorted(clean, key=lambda item: -item[1])[:SHIFT_MAX])
    return clean


def find_pointer_shifts(old_view, new_view, ops, view, code):
    votes = {}
    for new_pos, old_pos, diff, _ in ops:
        if not diff or (new_pos - old_pos) % 4:
            continue
        first = (new_pos + 3) & ~3
        for p in range(first, new_pos + len(diff) - 3, 4):
            q = old_pos + p - new_pos
            if q < 0 or q + 4 > len(old_view):
                continue
            before = int.from_bytes(old_view[q:q + 4], "little")
            after = int.from_bytes(new_view[p:p + 4], "little")
            if view.low <= before < view.high and view.low <= after < view.high and abs(after - before) < 1 << 16:
                votes.setdefault(before, collections.Counter())[after - before] += 1
    runs = []
    for address in sorted(votes):
        delta, count = votes[address].most_common(1)[0]
        if runs and runs[-1][2] == delta:
            runs[-1][1] = address + 1
            runs[-1][3] += count
        else:
            runs.append([address, address + 1, delta, count])
    for k in range(len(runs) - 1):
        runs[k][1] = runs[k + 1][0]
    covered = [(start, start + length) for start, length, _ in code]
    found = []
    for start, end, delta, count in runs:
        start, end = (start - view.base) & 0xFFFFFFFF, (end - view.base) & 0xFFFFFFFF
        if count < 3 or not delta or end <= start:
            continue
        if any(start < high and low < end for low, high in covered):
            continue
        found.append((start, end - start, delta, count))
    found = sorted(found, key=lambda item: -item[3])[:POINTER_SHIFT_MAX]
    return [item[:3] for item in found]


def shift_table(shifts):
    out, last = bytearray([len(shifts)]), 0
    for start, length, delta in shifts:
        out += varint(start - last) + varint(length) + varint(zigzag(delta))
        last = start + length
    return bytes(out)


def fit_shifts(shifts, room):
    shifts = list(shifts)
    while shifts and (len(shifts) > 255 or len(shift_table(sorted(shifts))) > room):
        shifts.remove(min(shifts, key=lambda item: item[1]))
    return sorted(shifts)


def board_base(board, home=None):
    import mcota_build
    return mcota_build.board_base(board, home)


def make_package(old, new, private, board, version, build, auto_apply=False, no_rollback=False, base=0):
    view = View(old, new, base)
    count = (len(new) + PAGE - 1) // PAGE
    changed = sum(1 for i in range(count) if not (i * PAGE + len(new[i * PAGE:(i + 1) * PAGE]) <= len(old) and old[i * PAGE:i * PAGE + len(new[i * PAGE:(i + 1) * PAGE])] == new[i * PAGE:(i + 1) * PAGE]))
    room = min(SHIFT_BYTES, ITEM_MAX - 64 - 124 - (count + 7) // 8 - 10 * changed) - 24
    if room < 1:
        raise SystemExit("Too many changed pages for one manifest. This firmware is too large for this method.")
    new_view = view.absolute(new)
    shifts = code = fit_shifts(find_shifts(old, new, view), room)
    old_view = view.predict(old, shifts)
    ops = page_ops(old_view, new_view)
    if view.kind:
        shifts = fit_shifts(code + find_pointer_shifts(old_view, new_view, ops, view, code), room)
        if shifts != code:
            old_view = view.predict(old, shifts)
            ops = page_ops(old_view, new_view)
    choices = [build_blocks(old_view, new_view, direction, ops) for direction in (0, 1)]
    direction = 0 if sum(map(len, choices[0])) <= sum(map(len, choices[1])) else 1
    blocks = choices[direction]
    flags = (1 if auto_apply else 0) | 2 | (4 if direction else 0) | (8 if no_rollback else 0) | view.flags()
    if view.restore(apply_blocks(old_view, blocks, len(new), direction)) != new:
        raise SystemExit("Internal error: the patch does not produce the new firmware")
    pub = public_bytes(private)
    bits = bytearray((len(blocks) + 7) // 8)
    for index in range(len(blocks)):
        page = new[index * PAGE:(index + 1) * PAGE]
        if index * PAGE + len(page) <= len(old) and old[index * PAGE:index * PAGE + len(page)] == page:
            bits[index >> 3] |= 1 << (index & 7)
            blocks[index] = b""
    if all(not b for b in blocks):
        raise SystemExit("Old and new firmware are identical, there is nothing to transfer.")
    head = MAGIC + struct.pack("<BBHIII", FORMAT, flags, len(blocks), len(new), len(old), build)
    head += fixed(board, 16) + fixed(version, 16)
    head += hashlib.sha256(new).digest() + hashlib.sha256(old).digest() + key_id(pub)
    head += bytes(bits)
    for index, block in enumerate(blocks):
        if block:
            head += struct.pack("<H", len(block)) + hashlib.sha256(new[index * PAGE:(index + 1) * PAGE]).digest()[:8]
    head += varint(view.base >> 12) + shift_table(shifts) + view.tail()
    manifest = head + private.sign(head)
    return manifest, blocks


def manifest_lens(manifest):
    pages = struct.unpack("<H", manifest[6:8])[0]
    size = (pages + 7) // 8
    bits = manifest[124:124 + size]
    lens, pos = [], 124 + size
    for index in range(pages):
        if bits[index >> 3] >> (index & 7) & 1:
            lens.append(0)
        else:
            lens.append(struct.unpack("<H", manifest[pos:pos + 2])[0])
            pos += 10
    return lens


def write_package(path, manifest, blocks):
    with open(path, "wb") as fh:
        fh.write(struct.pack("<H", len(manifest)) + manifest + b"".join(blocks))


def read_package(path):
    raw = open(path, "rb").read()
    size = struct.unpack("<H", raw[:2])[0]
    manifest = raw[2:2 + size]
    if manifest[:4] != MAGIC or manifest[4] != FORMAT:
        raise SystemExit(f"{path} is not a package in the current format. Please rebuild it.")
    blocks, pos = [], 2 + size
    for n in manifest_lens(manifest):
        blocks.append(raw[pos:pos + n])
        pos += n
    return manifest, blocks


def describe(manifest, blocks):
    pages, new_size, old_size, build = struct.unpack("<HIII", manifest[6:20])
    board = manifest[20:36].split(b"\0")[0].decode()
    version = manifest[36:52].split(b"\0")[0].decode()
    patch = sum(len(b) for b in blocks)
    packets, room = (len(manifest) + CHUNK - 1) // CHUNK, 0
    for b in blocks:
        if not b:
            continue
        if len(b) > CHUNK:
            packets, room = packets + (len(b) + CHUNK - 1) // CHUNK, 0
        elif len(b) > room:
            packets, room = packets + 1, CHUNK - len(b)
        else:
            room -= len(b)
    print(f"Device:       {board}")
    print(f"Version:      {version} (build {build})")
    print(f"Apply:        {'automatic, once complete and distributed' if manifest[5] & 1 else 'on command only (ota apply)'}")
    print(f"Rollback:     {'devices without room for a rollback copy still apply automatically' if manifest[5] & 8 else 'devices without room for a rollback copy wait for the command ota apply'}")
    print(f"Firmware:     {new_size} bytes, {pages} pages (previously {old_size} bytes)")
    print(f"Unchanged:    {sum(1 for b in blocks if not b)} of {pages} pages, they are not transferred")
    print(f"Key:          {manifest[116:124].hex()}")
    print(f"Campaign:     {hashlib.sha256(manifest).digest()[:8].hex()}")
    print(f"Manifest:     {len(manifest)} bytes")
    print(f"Patch:        {patch} bytes")
    print(f"Data packets: {packets}")


def cmd_keygen(args):
    if os.path.exists(args.file):
        raise SystemExit(f"{args.file} already exists. An existing key is never overwritten.")
    private = Ed25519PrivateKey.generate()
    raw = private.private_bytes(serialization.Encoding.Raw, serialization.PrivateFormat.Raw, serialization.NoEncryption())
    fd = os.open(args.file, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    with os.fdopen(fd, "w") as fh:
        fh.write(raw.hex() + "\n")
    pub = public_bytes(private)
    print(f"Private key: {args.file} (keep it secret, store a copy separately)")
    print(f"Public key: {pub.hex()}")
    print(f"Key ID: {key_id(pub).hex()}")


def cmd_pubkey(args):
    pub = public_bytes(load_private(args.key))
    print(pub.hex())
    print(f"Key ID: {key_id(pub).hex()}")


def cmd_pack(args):
    old = open(args.old, "rb").read()
    new = open(args.new, "rb").read()
    manifest, blocks = make_package(old, new, load_private(args.key), args.board, args.version, args.build, args.auto_apply, args.no_rollback,
                                   board_base(args.board) if args.basis is None else int(args.basis, 0))
    write_package(args.out, manifest, blocks)
    describe(manifest, blocks)
    print(f"Package:      {args.out}")


def cmd_info(args):
    manifest, blocks = read_package(args.package)
    describe(manifest, blocks)
    if args.pubkey:
        try:
            Ed25519PublicKey.from_public_bytes(bytes.fromhex(args.pubkey)).verify(manifest[-64:], manifest[:-64])
            print("Signature:    valid")
        except Exception:
            print("Signature:    INVALID")
            sys.exit(1)


def find_port(name):
    from serial.tools import list_ports
    for port in list_ports.comports():
        if port.serial_number and port.serial_number.lower() == name.lower():
            return port.device
    return name


def port_lock(path):
    import tempfile
    handle = open(os.path.join(tempfile.gettempdir(), "meshcore-ota-" + os.path.basename(path) + ".lock"), "w")
    try:
        import fcntl
        fcntl.flock(handle, fcntl.LOCK_EX)
    except ImportError:
        pass
    return handle


def serial_command(port, line, wait=3.0):
    import time
    port.reset_input_buffer()
    port.write((line + "\r").encode())
    deadline = time.time() + wait
    text = b""
    while time.time() < deadline:
        chunk = port.read(256)
        if chunk:
            text += chunk
            if b"\n" in text and (b"OK" in text or b"ERR" in text or b"->" in text):
                time.sleep(0.05)
                text += port.read(4096)
                break
    return text.decode(errors="replace")


def cmd_load(args):
    import serial
    manifest, blocks = read_package(args.package)
    args.port = find_port(args.port)
    lock = port_lock(args.port)
    with serial.Serial(args.port, 115200, timeout=0.2) as port:
        def run(line, wait=4.0):
            reply = serial_command(port, line, wait)
            if "OK" not in reply:
                raise SystemExit(f"The repeater reports an error for '{line[:40]}': {reply.strip()}")
            return reply
        run("ota load begin")
        for pos in range(0, len(manifest), 64):
            run("ota load data " + manifest[pos:pos + 64].hex())
        run("ota load manifest", 60.0)
        for index, block in enumerate(blocks):
            if not block:
                continue
            run("ota load begin")
            for pos in range(0, len(block), 64):
                run("ota load data " + block[pos:pos + 64].hex())
            run(f"ota load block {index}", 10.0)
            print(f"\rPage {index + 1} of {len(blocks)}", end="", flush=True)
        print()
        print(serial_command(port, "ota info").strip())


def cmd_uf2(args):
    blocks = []
    for part in args.part:
        addr_text, path = part.split(":", 1)
        addr = int(addr_text, 0)
        data = bytes([0xFF]) * int(path[5:], 0) if path.startswith("fill=") else open(path, "rb").read()
        data += bytes([0xFF]) * (-len(data) % 256)
        for pos in range(0, len(data), 256):
            blocks.append((addr + pos, data[pos:pos + 256]))
    out = bytearray()
    for number, (addr, chunk) in enumerate(blocks):
        head = struct.pack("<IIIIIIII", 0x0A324655, 0x9E5D5157, 0x00002000, addr, 256, number, len(blocks), args.family)
        out += head + chunk + bytes(476 - 256) + struct.pack("<I", 0x0AB16F30)
    open(args.out, "wb").write(out)
    print(f"{args.out}: {len(blocks)} blocks, {min(a for a, _ in blocks):#x} to {max(a for a, _ in blocks) + 256:#x}")


def open_link(args):
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import mcota_companion
    if bool(args.serial) == bool(args.ble):
        raise SystemExit("Please give exactly one port: --serial COM5 (Linux: /dev/ttyACM0) or --ble AA:BB:CC:DD:EE:FF. 'meshcore-ota ports' lists what is available.")
    return mcota_companion, (mcota_companion.SerialLink(args.serial) if args.serial else mcota_companion.BleLink(args.ble))


def package_version(manifest):
    return manifest[36:52].split(b"\0")[0].decode()


def airtime_ppm(args):
    percent = args.airtime if args.airtime is not None else (5.0 if args.test else 0.5)
    if not 0.05 <= percent <= 5.0:
        raise SystemExit("--airtime accepts values from 0.05 to 5 per cent.")
    return int(percent * 10000)


def package_resolver(packages):
    import hashlib

    def resolve(hello, states):
        for manifest, blocks in packages:
            if hello["board"] == hashlib.sha256(manifest[20:36].split(b"\0")[0]).digest()[:4] and hello["hash"] == manifest[84:92]:
                return manifest, blocks, f"gets {package_version(manifest)}"
        return f"no matching package for firmware {hello['tag'] or 'unknown'}"
    return resolve


def parse_routes(texts, max_hops):
    routes = []
    for text in texts:
        target, _, hops = text.partition("=")
        try:
            target = bytes.fromhex(target.strip())
            parts = [bytes.fromhex(h.strip()) for h in hops.replace(">", ",").split(",") if h.strip()]
        except ValueError:
            raise SystemExit(f"--route {text}: target and hops must be hex, for example 1a2b3c4d=5e")
        if len(target) < 4 or not parts or len({len(h) for h in parts}) != 1 or len(parts[0]) > 3:
            raise SystemExit(f"--route {text}: the target needs 4 bytes, each hop 1 to 3 bytes, all of the same length")
        if len(parts) > max_hops:
            raise SystemExit(f"--route {text}: at most {max_hops} hops")
        routes.append((target[:4], b"".join(parts), len(parts[0])))
    return routes


def cmd_serve(args):
    packages = [read_package(path) for path in args.package]
    keys = {manifest[116:124] for manifest, _ in packages}
    send_packages(args, packages, {}, keys.pop() if len(keys) == 1 else None, package_resolver(packages))


def send_packages(args, packages, old_versions, key_id=None, resolve=None, mine=None):
    import asyncio
    import time
    mcota_companion, link = open_link(args)
    import mcota_ui
    ppm = airtime_ppm(args)
    offers = (15.0, 120.0) if args.test else (60.0, 3600.0)
    board = None if args.plain or not mcota_ui.fancy() else mcota_ui.Dashboard(ppm, args.test)
    if board is None:
        for manifest, blocks in packages:
            describe(manifest, blocks)
            print()

    def report(companion, providers, first, note=None):
        if board is not None:
            board.names = companion.names
            if note:
                if note.startswith("Target "):
                    board.target = note[7:]
                board.note(note, mcota_ui.ACCENT if note.startswith("Target ") else mcota_ui.WARN)
            for provider in providers or ():
                provider.old_version = old_versions.get(provider.campaign, "")
            if first:
                board.note(f"Connected to {companion.name}, " + ("asking which repeaters are there" if not providers else "offering " + ("one package" if len(providers) == 1 else f"{len(providers)} packages")), mcota_ui.GOOD)
            board.update(companion, providers or board.providers)
            return
        if first:
            print(f"Companion:    {companion.name} ({companion.public_key[:4].hex()}), {companion.freq / 1000:.3f} MHz, "
                  f"{companion.bw / 1000:g} kHz, SF{companion.sf}, CR 4/{companion.cr}, {companion.tx_power} dBm")
            print(f"Firmware:     {companion.firmware}")
            print(f"Airtime:      at most {ppm / 10000:g} %" + (" (test mode)" if args.test else ""))
            print("Asking which repeaters are there. Quit with Ctrl+C." if not providers else "Offering. Quit with Ctrl+C.")
            return
        if note:
            print(time.strftime("%H:%M:%S"), note, flush=True)
            return
        parts = []
        if companion.finder:
            for key, hello in companion.finder.seen.items():
                parts.append(f"{key} reports {hello['tag'] or 'unknown'}: {companion.states.get(key, '')}")
        for provider in providers:
            version = package_version(provider.manifest)
            for k, v in provider.peers.items():
                left = len(provider.wants.get(bytes.fromhex(k), ()))
                state = "fetching manifest" if v["kind"] == 0 else f"{left} of {provider.pages} pages still to send"
                parts.append(f"{k} loading {version}: {state} ({v['rssi']} dBm, {provider.now() - v['at']:.0f} s ago)")
        print(f"{time.strftime('%H:%M:%S')} sent {sum(p.sent for p in providers)} packets ({sum(p.sent_ms for p in providers) / 1000:.0f} s airtime), "
              f"requests {sum(p.requests for p in providers)}, repeaters: {'; '.join(parts) or 'nobody yet'}", flush=True)

    problem = None
    if board is not None:
        board.live.start()
    try:
        asyncio.run(mcota_companion.serve(link, packages, ppm, offers[0], offers[1], report, 1.0 if board is not None else 5.0, key_id, resolve, args.hops, parse_routes(args.route, args.hops), mine))
    except KeyboardInterrupt:
        problem = "Stopped."
    except SystemExit as error:
        problem = str(error)
    finally:
        if board is not None:
            board.live.stop()
    if board is not None and board.providers:
        sent = sum(p.sent for p in board.providers)
        print(f"Sent: {sent} packets in {sum(p.sent_ms for p in board.providers) / 1000:.0f} s airtime.")
        for stamp, text, _ in reversed(board.events):
            print(f"  {stamp} {text}")
    if problem:
        print(problem)
        if problem != "Stopped.":
            sys.exit(1)


def natural(text):
    import re
    return [int(part) if part.isdigit() else part.lower() for part in re.split(r"(\d+)", text)]


def folder(args, *parts):
    return os.path.join(os.path.abspath(args.dir), *parts)


def write_public(home, private):
    pub = public_bytes(private)
    with open(os.path.join(home, "keys", "public.txt"), "w", encoding="utf-8") as fh:
        fh.write(f"Public key: {pub.hex()}\nKey ID: {key_id(pub).hex()}\n\n"
                 f"Firmware built by this program already contains the key.\nFor firmware built elsewhere, store it once over USB:\nota key set 1 {pub.hex()}\n")
    return pub


def write_private(path, private):
    raw = private.private_bytes(serialization.Encoding.Raw, serialization.PrivateFormat.Raw, serialization.NoEncryption())
    fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
    with os.fdopen(fd, "w") as fh:
        fh.write(raw.hex() + "\n")


def cmd_init(args):
    home = os.path.abspath(args.dir)
    for name in ("keys", "firmware", "packages", "install"):
        os.makedirs(folder(args, name), exist_ok=True)
    key_path = folder(args, "keys", "signing.key")
    if os.path.exists(key_path):
        private = load_private(key_path)
        print(f"Key:          present, {key_path}")
    else:
        private = Ed25519PrivateKey.generate()
        write_private(key_path, private)
        print(f"Key:          newly created, {key_path}")
    pub = write_public(home, private)
    print(f"Public:       {pub.hex()}")
    print(f"Key ID:       {key_id(pub).hex()}")
    print()
    print("The file signing.key is secret. Whoever holds it can supply your repeaters with firmware. Keep a copy in a separate place.")
    print("Firmware built by MeshCore-OTA already contains the public key.")


def read_key_file(path):
    data = open(path, "rb").read()
    text = data.strip()
    try:
        raw = bytes.fromhex(text.decode()) if len(text) == 64 else data
    except (ValueError, UnicodeDecodeError):
        raw = data
    if len(raw) != 32:
        raise SystemExit(f"{path} is not a private Ed25519 key. Expected are 32 bytes, raw or as 64 hex characters.")
    return Ed25519PrivateKey.from_private_bytes(raw)


def import_key(home, path):
    import json
    import mcota_build
    new = read_key_file(path)
    os.makedirs(os.path.join(home, "keys"), exist_ok=True)
    key_path = os.path.join(home, "keys", "signing.key")
    lines = []
    if os.path.exists(key_path):
        old = load_private(key_path)
        old_pub = public_bytes(old)
        if old_pub == public_bytes(new):
            return ["This key is already the active one."]
        ident = key_id(old_pub).hex()
        if ident in mcota_build.key_index(home).values():
            write_private(os.path.join(home, "keys", f"retired-{ident}.key"), old)
            retired = [entry for entry in mcota_build.retired_keys(home) if entry["id"] != ident and entry["public"] != public_bytes(new).hex()]
            retired.append({"id": ident, "public": old_pub.hex()})
            with open(os.path.join(home, "keys", "retired.json"), "w", encoding="utf-8") as fh:
                json.dump(retired, fh, indent=2)
            lines += [f"The previous key {ident} is now in keys/retired-{ident}.key.",
                      "Your repeaters do not know the new key yet. MeshCore-OTA distributes it with the next update:",
                      "The update is signed with the previous key and carries the new one. After that only the new key is valid on the repeater.",
                      "Keep the previous key until all repeaters have changed over.",
                      "Repeaters that cannot be reached by radio need the new base firmware (menu: Build firmware)."]
    write_private(key_path, new)
    pub = write_public(home, new)
    return [f"New key is active, ID {key_id(pub).hex()}."] + lines


def cmd_version(args):
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    from mcota_version import VERSION
    print(VERSION)


def cmd_self_update(args):
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import mcota_update
    found = mcota_update.check()
    if not found:
        print("No newer version found, or GitHub cannot be reached right now.")
        return
    mcota_update.install(found)
    print(f"Updated to {found['version']}. Key, firmware, packages and settings are unchanged.")


def cmd_key_import(args):
    for line in import_key(os.path.abspath(args.dir), args.file):
        print(line)


def private_for(home, ident):
    current = load_private(os.path.join(home, "keys", "signing.key"))
    if not ident or key_id(public_bytes(current)).hex() == ident:
        return current
    path = os.path.join(home, "keys", f"retired-{ident}.key")
    if not os.path.exists(path):
        raise SystemExit(f"The key {ident}, which these repeaters still use, is missing in keys. Without it no update can be signed for them.")
    return load_private(path)


def known_key_ids(home):
    import mcota_build
    current = key_id(public_bytes(load_private(os.path.join(home, "keys", "signing.key"))))
    used = set(mcota_build.key_index(home).values())
    return [current] + [bytes.fromhex(entry["id"]) for entry in mcota_build.retired_keys(home) if entry["id"] in used and bytes.fromhex(entry["id"]) != current]


def load_repeaters(home):
    import json
    path = os.path.join(home, "repeaters.json")
    if not os.path.exists(path):
        return {}
    with open(path, encoding="utf-8") as fh:
        return json.load(fh)


def save_repeaters(home, data):
    import json
    with open(os.path.join(home, "repeaters.json"), "w", encoding="utf-8") as fh:
        json.dump(data, fh, indent=2, ensure_ascii=False)


def note_repeater(home, hello, firmware, name=""):
    import time
    data = load_repeaters(home)
    entry = data.setdefault(hello["id"], {"name": name, "firmware": "", "seen": 0})
    entry.update(firmware=firmware, seen=int(time.time()))
    if name:
        entry["name"] = name
    save_repeaters(home, data)


def firmware_files(args, board):
    import mcota_build
    found = []
    for label in mcota_build.firmware_labels(os.path.abspath(args.dir), board):
        parsed = mcota_build.parse_label(label)
        if parsed[2] == bool(args.test):
            found.append((parsed, label))
    return sorted(found, key=lambda item: (natural(item[0][0]), item[0][1]))


def next_step(args, board, running, known):
    import mcota_build
    home = os.path.abspath(args.dir)
    version = mcota_build.parse_label(running)[0]
    have = {parsed[0] for parsed, _ in firmware_files(args, board)}
    allowed = mcota_build.allowed_versions(home)
    if allowed is None:
        allowed = sorted(have | ({known[-1][0]} if known else set()), key=natural)
    later = [v for v in allowed if natural(v) > natural(version)]
    target = later[0] if later else version
    return target, mcota_build.stand_label(target, home, args.test)


def package_for(args, board, old_label, new_label, build):
    import mcota_build
    home = os.path.abspath(args.dir)
    old_path = mcota_build.firmware_file(home, board, old_label)
    new_path = mcota_build.firmware_file(home, board, new_label)
    target = folder(args, "packages", f"{board}-{old_label}__{new_label}.mcota")
    wanted = bytes([FORMAT, (0 if args.manual_apply else 1) | 2 | (8 if args.no_rollback else 0)])
    private = private_for(home, mcota_build.key_index(home).get(f"{board}-{old_label}", ""))
    if os.path.exists(target) and os.path.getmtime(target) >= max(os.path.getmtime(old_path), os.path.getmtime(new_path)) and open(target, "rb").read(8)[6:8] == wanted:
        manifest, blocks = read_package(target)
        if manifest[116:124] == key_id(public_bytes(private)):
            return manifest, blocks
    old = open(old_path, "rb").read()
    new = open(new_path, "rb").read()
    os.makedirs(os.path.dirname(target), exist_ok=True)
    manifest, blocks = make_package(old, new, private, board, new_label, build, not args.manual_apply, args.no_rollback, board_base(board, home))
    write_package(target, manifest, blocks)
    return manifest, blocks


def collect_updates(args, quiet=False):
    import mcota_build
    home = os.path.abspath(args.dir)
    if not os.path.exists(folder(args, "keys", "signing.key")):
        raise SystemExit(f"There is no key in {home}. Run 'meshcore-ota init' first.")
    packages, old_versions, rows = [], {}, []
    for board in sorted(mcota_build.firmware_boards(home)):
        files = firmware_files(args, board)
        if not files or (args.board and board != args.board):
            continue
        labels = {label for _, label in files}
        for index, (parsed, running) in enumerate(files, 1):
            _, label = next_step(args, board, running, [])
            if label == running:
                rows.append((board, "", running, "is the newest version"))
                continue
            if label not in labels:
                rows.append((board, running, label, "target is not built yet"))
                continue
            manifest, blocks = package_for(args, board, running, label, len(files) + index)
            packages.append((manifest, blocks))
            old_versions[hashlib.sha256(manifest).digest()[:8]] = running
            changed = sum(1 for b in blocks if b)
            rows.append((board, running, label, f"{changed} of {len(blocks)} pages changed, patch {sum(len(b) for b in blocks) / 1024:.1f} KB"))
    if not quiet:
        for board, old, new, text in rows:
            print(f"{board:<16} {old + ' > ' if old else ''}{new:<18} {text}")
    return packages, old_versions


def build_downloads(args):
    import mcota_build
    source = folder(args, "downloads")
    built = []
    for name in sorted(os.listdir(source)) if os.path.isdir(source) else []:
        if not name.lower().endswith(".zip"):
            continue
        if not mcota_build.parse_name(name):
            print(f"{name}: skipped, the file name does not have the meshcore.io form")
            continue
        try:
            built.append(mcota_build.build(os.path.join(source, name), os.path.abspath(args.dir), args.test, args.switch_compiler))
        except SystemExit as error:
            print(f"{name}: {error}")
    return built


def cmd_build(args):
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import mcota_build
    if args.archive:
        for path in args.archive:
            mcota_build.build(path, os.path.abspath(args.dir), args.test, args.switch_compiler)
    else:
        build_downloads(args)


def make_resolver(args, old_versions):
    import threading
    import mcota_build
    lock = threading.Lock()
    home = os.path.abspath(args.dir)
    tools = folder(args, "cache")

    def resolve(hello, states):
        with lock:
            boards = mcota_build.firmware_boards(home) | {target["board"] for target in mcota_build.TARGETS.values()}
            board = next((b for b in boards if hashlib.sha256(b.encode()).digest()[:4] == hello["board"]), None)
            if board is None:
                return "unknown device type. The firmware of this repeater was not built by this program"
            running = None
            files = firmware_files(args, board)
            for _, label in files:
                data = open(mcota_build.firmware_file(home, board, label), "rb").read()
                if len(data) == hello["size"] and hashlib.sha256(data).digest()[:8] == hello["hash"]:
                    running = label
            note_repeater(home, hello, running or hello["tag"], hello.get("name", ""))
            if running is None:
                return (f"runs {hello['tag'] or 'unknown firmware'}, which was not built here. Without the base firmware no update can be built. "
                        f"The repeater needs the base firmware once (menu: Build firmware)")
            known = mcota_build.releases(tools, fresh=not args.offline)
            version, label = next_step(args, board, running, known)
            if label == running:
                return f"is up to date ({running})"
            if not os.path.exists(mcota_build.firmware_file(home, board, label)):
                target_name = mcota_build.target_for_board(board, home)
                commit = dict(known).get(version)
                if not target_name or not commit:
                    return f"should get {label}, but the source commit of {version} is not known. Is there an internet connection?"
                states[hello["id"]] = f"building {label}"
                mcota_build.build_version(target_name, version, commit, home, args.test, args.switch_compiler)
            states[hello["id"]] = f"building the package {running} to {label}"
            manifest, blocks = package_for(args, board, running, label, len(files) + 1)
            old_versions[hashlib.sha256(manifest).digest()[:8]] = running
            return manifest, blocks, f"gets {label} (runs {running})"

    return resolve


def cmd_update(args):
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    home = os.path.abspath(args.dir)
    if not os.path.exists(folder(args, "keys", "signing.key")):
        raise SystemExit(f"There is no key in {home}. Run 'meshcore-ota init' first.")
    build_downloads(args)
    if args.build_only:
        collect_updates(args)
        return
    old_versions = {}
    send_packages(args, [], old_versions, known_key_ids(home), make_resolver(args, old_versions), lambda: set(load_repeaters(home)))


def newest_release(args, wanted=None):
    import mcota_build
    found = mcota_build.releases(folder(args, "cache"), fresh=not getattr(args, "offline", False))
    if not found:
        raise SystemExit("The list of MeshCore versions could not be loaded. Is there an internet connection?")
    if wanted:
        match = [item for item in found if item[0] == wanted or item[0] == "v" + wanted]
        if not match:
            raise SystemExit(f"Version {wanted} does not exist. Available: {', '.join(item[0] for item in found[-8:])}")
        return match[0]
    return found[-1]


def cmd_devices(args):
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import mcota_build
    version, commit = newest_release(args, args.version)
    found = mcota_build.catalog(folder(args, "cache"), commit)
    have = mcota_build.saved_targets(os.path.abspath(args.dir))
    print(f"Repeater devices in MeshCore {version}:")
    order = {"ready": 0, "missing": 1}
    rows = sorted(((mcota_build.support(name, entry), name, entry) for name, entry in found.items()), key=lambda row: (order[row[0][0]], row[1].lower()))
    marks = {"ready": "yes ", "missing": "no  "}
    for (state, text), name, entry in rows:
        if args.buildable and state == "missing":
            continue
        built = "  (base firmware is built)" if name in have or (name in mcota_build.TARGETS and mcota_build.TARGETS[name]["board"] in mcota_build.firmware_boards(os.path.abspath(args.dir))) else ""
        print(f"  {marks[state]} {name:<42} {entry['mcu']:<10} {text}{built}")
    counts = collections.Counter(row[0][0] for row in rows)
    print(f"{counts['ready']} supported, {counts['missing']} not supported.")
    print("Build the first installation for a device: meshcore-ota install <device>")


def cmd_first(args):
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import mcota_build
    version, commit = newest_release(args, args.version)
    names = mcota_build.catalog(folder(args, "cache"), commit)
    wanted = args.device.lower()
    match = [name for name in names if name.lower() in (wanted, wanted + "_repeater")]
    if not match:
        near = [name for name in names if wanted in name.lower()]
        raise SystemExit(f"{args.device} is not a known device." + (f" Did you mean: {', '.join(sorted(near)[:8])}?" if near else " 'meshcore-ota devices' shows the list."))
    mcota_build.build_version(match[0], version, commit, os.path.abspath(args.dir), args.test, args.switch_compiler)


def cmd_ports(args):
    import asyncio
    from serial.tools import list_ports
    ports = [p for p in list_ports.comports() if p.vid is not None or p.description not in ("n/a", "")]
    print("Serial ports:")
    for port in ports:
        print(f"  {port.device}  {port.description}  {port.serial_number or ''}")
    if not ports:
        print("  none found")
    if args.no_ble:
        return
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import mcota_companion
    print("Bluetooth (the scan takes a few seconds):")
    try:
        found = asyncio.run(mcota_companion.scan_ble())
    except Exception as error:
        print(f"  Scan not possible: {error}")
        return
    for address, name, rssi in found:
        print(f"  {address}  {name}  {rssi} dBm")
    known = [k for k in asyncio.run(mcota_companion.known_ble()) if k[0] not in {f[0] for f in found}]
    for address, name in known:
        print(f"  {address}  {name}  (already connected to this computer)")
    if not found and not known:
        print("  none found")


def cmd_repeaters(args):
    import asyncio
    mcota_companion, link = open_link(args)
    contacts = asyncio.run(mcota_companion.with_companion(link, lambda c: c.contacts()))
    import mcota_ui
    rows = [c for c in contacts if c["type"] == mcota_companion.TYPE_REPEATER]
    if not rows:
        print("The companion knows no repeaters.")
        return
    grid = mcota_ui.table("Repeater", [("ID", {"style": mcota_ui.DIM}), ("Name", {"style": "bold"}), ("Route", {})])
    for c in sorted(rows, key=lambda c: c["name"].lower()):
        if not c["known"]:
            way = "no route known"
        elif not c["path"]:
            way = "directly in range"
        else:
            size = c["hop_size"]
            way = "via " + " > ".join(c["path"][i:i + size].hex() for i in range(0, len(c["path"]), size))
        if mcota_ui.fancy():
            grid.add_row(c["key"][:4].hex(), c["name"], way)
        else:
            print(f"{c['key'][:4].hex()}  {c['name']:<24} {way}")
    if mcota_ui.fancy():
        mcota_ui.console.print(mcota_ui.panel(grid, f"Repeaters in the contacts ({len(rows)})"))


def cmd_status(args):
    import asyncio
    mcota_companion, link = open_link(args)

    async def action(companion):
        contact = mcota_companion.find_contact(await companion.contacts(), args.to)
        if not await companion.login(contact["key"], args.password):
            raise SystemExit(f"Login to {contact['name']} failed (password, range or no route).")
        for text in args.command and [" ".join(args.command)] or ["ota info", "ota peers"]:
            reply = await companion.command(contact["key"], text)
            print(f"{contact['name']} > {text}")
            print(f"  {reply if reply is not None else 'no reply'}")

    asyncio.run(mcota_companion.with_companion(link, action))


def cmd_cli(args):
    import serial
    args.port = find_port(args.port)
    lock = port_lock(args.port)
    with serial.Serial(args.port, 115200, timeout=0.2) as port:
        print(serial_command(port, " ".join(args.command), args.wait).strip())
    lock.close()


def main():
    parser = argparse.ArgumentParser(description="MeshCore-OTA: keys, packages, sending through a companion and querying repeaters")
    parser.add_argument("--dir", default=os.path.dirname(os.path.abspath(sys.executable)) if getattr(sys, "frozen", False) else ".", help="directory with keys, firmware, packages and the other data (default: next to the program)")
    sub = parser.add_subparsers(dest="cmd")
    p = sub.add_parser("keygen", help="create a new signing key")
    p.add_argument("file")
    p.set_defaults(func=cmd_keygen)
    p = sub.add_parser("version", help="show the program version")
    p.set_defaults(func=cmd_version)
    p = sub.add_parser("self-update", help="fetch a new version of the program from GitHub")
    p.set_defaults(func=cmd_self_update)
    p = sub.add_parser("key-import", help="take over a private key from a file. The previous one is kept until all repeaters have changed over")
    p.add_argument("file", help="file with 32 bytes, raw or as 64 hex characters")
    p.set_defaults(func=cmd_key_import)
    p = sub.add_parser("devices", help="show all devices MeshCore knows as repeaters, and whether meshcore-ota can build them")
    p.add_argument("--version", help="MeshCore version, otherwise the newest")
    p.add_argument("--buildable", action="store_true", help="only show devices meshcore-ota can build")
    p.add_argument("--offline", action="store_true", help="do not look for new versions")
    p.set_defaults(func=cmd_devices)
    p = sub.add_parser("install", help="build firmware with MeshCore-OTA for a device, for the first installation over USB or Bluetooth")
    p.add_argument("device", help="name from 'meshcore-ota devices', such as RAK_4631 or Heltec_t114")
    p.add_argument("--version", help="MeshCore version, otherwise the newest")
    p.add_argument("--test", action="store_true", help="test build with short waiting times and 5 per cent airtime")
    p.add_argument("--switch-compiler", action="store_true", help="adopt the compiler of this MeshCore version")
    p.add_argument("--offline", action="store_true", help="do not look for new versions")
    p.set_defaults(func=cmd_first)
    p = sub.add_parser("pubkey", help="show the public key for a private key")
    p.add_argument("key")
    p.set_defaults(func=cmd_pubkey)
    p = sub.add_parser("pack", help="build and sign an update package from old and new firmware")
    p.add_argument("--key", required=True)
    p.add_argument("--old", required=True)
    p.add_argument("--new", required=True)
    p.add_argument("--board", required=True)
    p.add_argument("--version", required=True)
    p.add_argument("--build", type=int, required=True)
    p.add_argument("--out", required=True)
    p.add_argument("--basis", help="start address of the firmware in flash, such as 0x28000. Without it the value of the device type applies")
    p.add_argument("--auto-apply", action="store_true", help="repeaters apply the update automatically")
    p.add_argument("--no-rollback", action="store_true", help="devices without room for a rollback copy still apply automatically. Without this option they wait for the command ota apply")
    p.set_defaults(func=cmd_pack)
    p = sub.add_parser("info", help="show the contents of a package")
    p.add_argument("package")
    p.add_argument("--pubkey")
    p.set_defaults(func=cmd_info)
    p = sub.add_parser("load", help="load a package onto a repeater over USB")
    p.add_argument("--port", required=True)
    p.add_argument("package")
    p.set_defaults(func=cmd_load)
    def send_args(p):
        p.add_argument("--airtime", type=float, help="share of airtime in per cent, 0.05 to 5. Default 0.5, in test mode 5")
        p.add_argument("--test", action="store_true", help="test mode matching the test firmware: short intervals, 5 %% airtime")
        p.add_argument("--plain", action="store_true", help="plain line output instead of the dashboard")
        p.add_argument("--hops", type=int, choices=(0, 1, 2), default=2, help="reach repeaters up to this many hops from the companion, default 2. 0 means direct neighbours only")
        p.add_argument("--route", action="append", default=[], metavar="TARGET=HOP,HOP", help="route to a repeater by hand, for example 1a2b3c4d=5e or 1a2b3c4d=5e01,77aa. The target is the start of its key, hops as hex seen from the companion")

    def link_args(p):
        p.add_argument("--serial", help="companion over USB, for example COM5 or /dev/ttyACM0")
        p.add_argument("--ble", help="companion over Bluetooth, address like AA:BB:CC:DD:EE:FF")

    p = sub.add_parser("ports", help="find serial ports and companions over Bluetooth")
    p.add_argument("--no-ble", action="store_true", help="only show serial ports")
    p.set_defaults(func=cmd_ports)
    p = sub.add_parser("repeaters", help="show repeaters from the contacts of the companion")
    link_args(p)
    p.set_defaults(func=cmd_repeaters)
    p = sub.add_parser("send", help="send an update by radio through a companion")
    link_args(p)
    send_args(p)
    p.add_argument("package", nargs="+", help="one or more packages")
    p.set_defaults(func=cmd_serve)
    p = sub.add_parser("init", help="create folders and key")
    p.set_defaults(func=cmd_init)
    p = sub.add_parser("build", help="build firmware with MeshCore-OTA from the meshcore.io files")
    p.add_argument("--test", action="store_true", help="build test firmware with 5 %% airtime")
    p.add_argument("--switch-compiler", action="store_true", help="adopt the compiler MeshCore intends for this version")
    p.add_argument("archive", nargs="*", help="ZIP files from meshcore.io. Without it everything from downloads")
    p.set_defaults(func=cmd_build)
    p = sub.add_parser("update", help="build, sign and send firmware and packages")
    link_args(p)
    send_args(p)
    p.add_argument("--board", help="only build this device type")
    p.add_argument("--offline", action="store_true", help="do not check MeshCore for new versions, only use the folder")
    p.add_argument("--manual-apply", action="store_true", help="repeaters do not apply automatically but wait for 'ota apply'")
    p.add_argument("--build-only", action="store_true", help="only build and sign packages, do not send")
    p.add_argument("--no-rollback", action="store_true", help="devices without room for a rollback copy still apply automatically. Without this option they wait for the command ota apply")
    p.add_argument("--switch-compiler", action="store_true", help="adopt the compiler MeshCore intends for this version")
    p.set_defaults(func=cmd_update)
    p = sub.add_parser("status", help="query the state of a repeater through the companion")
    link_args(p)
    p.add_argument("--to", required=True, help="repeater: name or start of its key")
    p.add_argument("--password", required=True, help="admin password of the repeater")
    p.add_argument("command", nargs="*", help="own command instead of 'ota info' and 'ota peers'")
    p.set_defaults(func=cmd_status)
    p = sub.add_parser("uf2", help="build a UF2 file for the first installation")
    p.add_argument("--out", required=True)
    p.add_argument("--family", type=lambda v: int(v, 0), default=0xADA52840)
    p.add_argument("--part", action="append", required=True, help="address:file or address:fill=length")
    p.set_defaults(func=cmd_uf2)
    p = sub.add_parser("cli", help="send a command to the repeater over USB")
    p.add_argument("--port", required=True)
    p.add_argument("--wait", type=float, default=3.0)
    p.add_argument("command", nargs="+")
    p.set_defaults(func=cmd_cli)
    args = parser.parse_args()
    if not args.cmd:
        sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
        import mcota_menu
        mcota_menu.run(args.dir)
        return
    if args.cmd not in ("send", "update", "cli", "uf2"):
        sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
        try:
            import mcota_ui
            mcota_ui.show_banner()
        except ImportError:
            pass
    args.func(args)


if __name__ == "__main__":
    main()

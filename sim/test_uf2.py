import os
import struct
import sys
import tempfile

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "tools"))
import mcota_build

MBR_SIZE = 0x1000
MAX_BLOCKS = 1024 * 1024 // 256 + 100
failures = 0


def check(ok, text):
    global failures
    if not ok:
        failures += 1
        print("ERROR:", text)


def accepted(path, flash_end, family):
    data = open(path, "rb").read()
    written, total = set(), None
    for pos in range(0, len(data), 512):
        start0, start1, flags, addr, size, number, count, block_family = struct.unpack("<IIIIIIII", data[pos:pos + 32])
        end = struct.unpack("<I", data[pos + 508:pos + 512])[0]
        if (start0, start1, end) != (0x0A324655, 0x9E5D5157, 0x0AB16F30) or not flags & 0x2000 or size != 256 or addr & 0xFF or block_family != family:
            continue
        if not MBR_SIZE <= addr < flash_end:
            continue
        total = count if total in (None, count) and count < MAX_BLOCKS else 0xFFFFFFFF
        if number < MAX_BLOCKS:
            written.add(number)
    return total is not None and len(written) >= total


for name, target in mcota_build.TARGETS.items():
    path = os.path.join(tempfile.mkdtemp(prefix="meshcore-ota-test-"), "test.uf2")
    loader = bytes(range(256)) * 12
    app = bytes(target["slot_pages"] * mcota_build.PAGE)
    mcota_build.write_uf2(path, [(target["loader"], loader), (target["app"], app)], target["family"])
    check(accepted(path, target["status"], target["family"]), f"{name}: the bootloader would not finish this UF2 file")
    mcota_build.write_uf2(path, [(target["loader"], loader), (target["app"], app), (target["status"], bytes(4096))], target["family"])
    check(not accepted(path, target["status"], target["family"]), f"{name}: a block in the reserved area is not noticed by this test")

print(f"UF2 files: {failures} errors")
sys.exit(1 if failures else 0)

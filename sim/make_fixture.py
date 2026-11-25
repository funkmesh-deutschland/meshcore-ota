import os
import random
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "tools"))
import mcota
from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey

out = sys.argv[1]
os.makedirs(out, exist_ok=True)
rng = random.Random(7)
words = [bytes(rng.randrange(256) for _ in range(rng.randrange(4, 40))) for _ in range(300)]
old = bytearray()
while len(old) < 60000:
    old += rng.choice(words)
old = bytes(old[:60000])
new = bytearray(old)
for _ in range(40):
    pos = rng.randrange(24000, len(new) - 64)
    new[pos:pos + rng.randrange(1, 24)] = bytes(rng.randrange(256) for _ in range(rng.randrange(1, 24)))
new[30000:30000] = bytes(rng.randrange(256) for _ in range(700))
new += bytes(rng.randrange(256) for _ in range(5000))
new = bytes(new)

for name, seed in (("good", 1), ("other", 2)):
    key = Ed25519PrivateKey.from_private_bytes(bytes([seed]) * 32)
    open(os.path.join(out, name + ".pub"), "wb").write(mcota.public_bytes(key))
    if name == "good":
        manifest, blocks = mcota.make_package(old, new, key, "sim_board", "1.0.1", 2)
        mcota.write_package(os.path.join(out, "update.mcota"), manifest, blocks)
        assert mcota.read_package(os.path.join(out, "update.mcota")) == (manifest, blocks)
        manifest, blocks = mcota.make_package(old, new, key, "sim_board", "1.0.1", 2, auto_apply=True)
        mcota.write_package(os.path.join(out, "auto.mcota"), manifest, blocks)
        print("Patch", sum(len(b) for b in blocks), "bytes for", len(new), "bytes of firmware,", len(blocks), "pages")
key = Ed25519PrivateKey.from_private_bytes(bytes([1]) * 32)
small = [("standard", ["set tx 20", "set advert.interval 120"]), ("dense", ["set tx 14", "set flood.max 8"])]
large = small + [("field%d" % i, ["set advert.interval %d" % (100 + i), "set flood.advert.interval %d" % (12 + i), "set txdelay 1.%d" % i]) for i in range(6)]
open(os.path.join(out, "settings.bin"), "wb").write(mcota.make_settings(key, 5, small))
open(os.path.join(out, "settings2.bin"), "wb").write(mcota.make_settings(key, 6, large))
open(os.path.join(out, "settings-other.bin"), "wb").write(mcota.make_settings(Ed25519PrivateKey.from_private_bytes(bytes([2]) * 32), 9, small))
assert mcota.read_settings(mcota.make_settings(key, 6, large)) == (6, mcota.key_id(mcota.public_bytes(key)), large)
newer = bytearray(new)
for _ in range(25):
    pos = rng.randrange(8000, len(newer) - 64)
    newer[pos:pos + rng.randrange(1, 24)] = bytes(rng.randrange(256) for _ in range(rng.randrange(1, 24)))
newer = bytes(newer[:19 * 4096])
key = Ed25519PrivateKey.from_private_bytes(bytes([1]) * 32)
manifest, blocks = mcota.make_package(new, newer, key, "sim_board", "1.0.2", 3)
mcota.write_package(os.path.join(out, "update2.mcota"), manifest, blocks)
open(os.path.join(out, "newer.bin"), "wb").write(newer)
other_words = [bytes(rng.randrange(256) for _ in range(rng.randrange(4, 40))) for _ in range(300)]
other = bytearray()
while len(other) < 64000:
    other += rng.choice(other_words)
other = bytes(other[:64000])
manifest, blocks = mcota.make_package(old, other, key, "sim_board", "2.0.0", 4, auto_apply=True)
mcota.write_package(os.path.join(out, "switch.mcota"), manifest, blocks)
open(os.path.join(out, "switch.bin"), "wb").write(other)
manifest, blocks = mcota.make_package(old, other, key, "sim_board", "2.0.0", 4, auto_apply=True, no_rollback=True)
mcota.write_package(os.path.join(out, "switch-norollback.mcota"), manifest, blocks)
print("Replacement package", sum(len(b) for b in blocks), "bytes")
import zlib
with open(os.path.join(out, "inflate.txt"), "w") as cases:
    for i in range(400):
        kind = i % 4
        size = rng.randrange(0, 4800)
        if kind == 0:
            data = bytes(rng.randrange(256) for _ in range(size))
        elif kind == 1:
            data = bytes(rng.randrange(256) if rng.random() < 0.3 else 0 for _ in range(size))
        elif kind == 2:
            data = bytes(rng.choice(b"abcdefgh") for _ in range(size))
        else:
            data = bytes(rng.randrange(4) for _ in range(rng.randrange(0, 40)))
        packer = zlib.compressobj(rng.choice((0, 1, 6, 9)), zlib.DEFLATED, -15, 9, rng.choice((zlib.Z_DEFAULT_STRATEGY, zlib.Z_FIXED)))
        packed = packer.compress(data) + packer.flush()
        assert mcota.unpack(mcota.pack(data)) == data
        cases.write(packed.hex() + " " + (data.hex() or "-") + "\n")
open(os.path.join(out, "old.bin"), "wb").write(old)
open(os.path.join(out, "new.bin"), "wb").write(new)

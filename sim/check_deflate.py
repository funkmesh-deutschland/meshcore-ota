import struct
import sys
import zlib

image = open(sys.argv[1], "rb").read()
packed = open(sys.argv[2], "rb").read()
pos, at, failures = 0, 0, 0
while pos < len(packed):
    size, length = struct.unpack("<HH", packed[pos:pos + 4])
    if zlib.decompress(packed[pos + 4:pos + 4 + size], -15) != image[at:at + length]:
        failures += 1
    pos += 4 + size
    at += length
print(f"Cross-check with zlib: {at} bytes, {failures} errors")
sys.exit(1 if failures or at != len(image) else 0)

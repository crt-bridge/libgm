"""Extracts a GMFX container as concatenated Annex B.
Usage: python -I extract_annexb.py <input.bin> <output.h264>
Format (see load_stream in the codec client test):
  'GMFX' + n u32 LE, then n x (size u32 LE, key flag u8), then the data.
"""
import struct
import sys

src, dst = sys.argv[1], sys.argv[2]
b = open(src, "rb").read()
assert b[:4] == b"GMFX", "not GMFX"
n = struct.unpack_from("<I", b, 4)[0]
recs = [struct.unpack_from("<IB", b, 8 + 5 * i) for i in range(n)]
pos = 8 + 5 * n
out = bytearray()
for size, key in recs:
    out += b[pos:pos + size]
    pos += size
assert pos == len(b), "extra or missing bytes: %d vs %d" % (pos, len(b))
open(dst, "wb").write(out)
print("%s: %d packets, %d keys, %d bytes, starts with %s" % (
    src, n, sum(1 for _, k in recs if k), len(out), bytes(out[:6]).hex()))

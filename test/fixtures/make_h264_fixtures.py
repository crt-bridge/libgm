#!/usr/bin/env python3
"""Regenerates the H.264 4:4:4 test fixtures of the codec tests from a SYNTHETIC source (the ffmpeg
`testsrc2` test pattern: no game, no rights). One script, parameterised, replaces the files that
were made from captured pictures.

    python -I make_h264_fixtures.py [--ffmpeg FFMPEG] [--outdir DIR] [--only NAME] [--list]
    python -I make_h264_fixtures.py --custom WIDTH HEIGHT UNITS KEYINT OUT.bin [--ffmpeg FFMPEG]

FFMPEG is an ffmpeg built with libx264 (High 4:4:4 Predictive profile); the default is `ffmpeg` on the
PATH. Each fixture is written as NAME.bin (the access units) and NAME.framemd5 (what an independent
decode of those units gives, one MD5 per picture, three planes, no conversion).

The stream, so that every test sees the same shape:
  - libx264, yuv444p, no B-frames, one thread (the output does not depend on the number of cores),
    no scene-cut key frames: a key frame only every KEYINT pictures, the first included;
  - aud=1 to delimit the units, then every delimiter is REMOVED: unit 0 is SPS PPS SEI IDR, every
    other unit is SEI + slice, none has an access-unit delimiter, all start codes are 4 bytes;
  - GMFX container: 'GMFX', n u32 LE, n x (size u32 LE, key flag u8), then the data.

The bytes depend on the x264 version: regenerating with another build gives other units and other
checksums, both files change together. Generate the pair, do not mix files from two builds.
"""
import argparse
import os
import struct
import subprocess
import sys

# name: (width, height, units, key interval, rate control)
FIXTURES = {
    "synthetic-h264-444-368x480-60":       (368, 480, 60, 1000, ("bitrate", 5000)),
    "synthetic-h264-444-368x480-60-key10": (368, 480, 60, 10,   ("bitrate", 5000)),
    "synthetic-h264-444-320x240-30":       (320, 240, 30, 1000, ("crf", 23)),
}

AUD = b"\x00\x00\x00\x01\x09"


def encode(ffmpeg, width, height, units, keyint, rate):
    kind, value = rate
    ctl = ["-b:v", "%dk" % value] if kind == "bitrate" else ["-crf", str(value)]
    cmd = [ffmpeg, "-hide_banner", "-loglevel", "error", "-f", "lavfi", "-i",
           "testsrc2=size=%dx%d:rate=60" % (width, height), "-frames:v", str(units),
           "-vf", "format=yuv444p", "-c:v", "libx264", "-preset", "medium"] + ctl + [
           "-x264-params", "bframes=0:aud=1:keyint=%d:min-keyint=%d:scenecut=0:threads=1" % (keyint, keyint),
           "-f", "h264", "-"]
    return subprocess.run(cmd, check=True, stdout=subprocess.PIPE).stdout


def split_units(raw, expected):
    pos = [i for i in range(len(raw)) if raw.startswith(AUD, i)]
    assert pos and pos[0] == 0, "the stream does not start with a delimiter"
    pos.append(len(raw))
    units = []
    for a, b in zip(pos, pos[1:]):
        body = raw[a:b][6:]            # the delimiter: start code (4) + NAL header (1) + 1 byte
        if body.startswith(b"\x00\x00\x01"):
            body = b"\x00" + body      # 4-byte start codes everywhere
        assert body.startswith(b"\x00\x00\x00\x01"), body[:8].hex()
        units.append(body)
    assert len(units) == expected, "%d units, %d asked" % (len(units), expected)
    return units


def is_key(unit):
    i = 0
    while True:
        i = unit.find(b"\x00\x00\x01", i)
        if i < 0:
            return False
        if unit[i + 3] & 0x1F == 5:
            return True
        i += 3


def write_gmfx(path, units, keyint):
    keys = [1 if is_key(u) else 0 for u in units]
    assert keys == [1 if i % keyint == 0 else 0 for i in range(len(units))], keys
    assert units[0][4] & 0x1F == 7, "unit 0 does not start with an SPS"
    out = bytearray(b"GMFX" + struct.pack("<I", len(units)))
    for u, k in zip(units, keys):
        out += struct.pack("<IB", len(u), k)
    for u in units:
        out += u
    with open(path, "wb") as f:
        f.write(out)
    return len(out), sum(keys)


def framemd5(ffmpeg, units, path):
    """The reference checksums: ffmpeg decodes the concatenated units, one MD5 per picture."""
    p = subprocess.run([ffmpeg, "-hide_banner", "-loglevel", "error", "-framerate", "60", "-f", "h264", "-i", "-",
                        "-f", "framemd5", "-"], input=b"".join(units), check=True, stdout=subprocess.PIPE)
    text = p.stdout.decode()
    lines = [l for l in text.splitlines() if l and not l.startswith("#")]
    assert len(lines) == len(units), "%d checksums for %d units" % (len(lines), len(units))
    with open(path, "w", newline="\n") as f:
        f.write(text if text.endswith("\n") else text + "\n")


def write_rgb(ffmpeg, spec, units, outdir, name):
    """For the macOS decoder test (client test-vt): the source pictures and the stream decoded by libavcodec, both RGB24, raw."""
    width, height, units_n, keyint, rate = spec
    src = subprocess.run([ffmpeg, "-hide_banner", "-loglevel", "error", "-f", "lavfi", "-i",
                          "testsrc2=size=%dx%d:rate=60" % (width, height), "-frames:v", str(units_n),
                          "-pix_fmt", "rgb24", "-f", "rawvideo", "-"], check=True, stdout=subprocess.PIPE).stdout
    sw = subprocess.run([ffmpeg, "-hide_banner", "-loglevel", "error", "-framerate", "60", "-f", "h264", "-i", "-",
                         "-pix_fmt", "rgb24", "-f", "rawvideo", "-"], input=b"".join(units), check=True,
                        stdout=subprocess.PIPE).stdout
    open(os.path.join(outdir, name + ".originals.rgb"), "wb").write(src)
    open(os.path.join(outdir, name + ".software.rgb"), "wb").write(sw)


def make(ffmpeg, name, spec, outdir, rgb_dir=None):
    width, height, units_n, keyint, rate = spec
    units = split_units(encode(ffmpeg, width, height, units_n, keyint, rate), units_n)
    base = os.path.join(outdir, name)
    size, keys = write_gmfx(base + ".bin", units, keyint)
    framemd5(ffmpeg, units, base + ".framemd5")
    if rgb_dir:
        write_rgb(ffmpeg, spec, units, rgb_dir, name)
    print("%s: %dx%d, %d units, %d key frame(s), %d bytes" % (name, width, height, units_n, keys, size))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--ffmpeg", default="ffmpeg")
    ap.add_argument("--outdir", default=os.path.dirname(os.path.abspath(__file__)))
    ap.add_argument("--only", help="one fixture by name")
    ap.add_argument("--list", action="store_true")
    ap.add_argument("--rgb-dir", help="also write NAME.originals.rgb and NAME.software.rgb here (about 32 MB each for 368x480): "
                                      "the inputs of the macOS decoder test, `make test-vt ORIG=... REF=...`")
    ap.add_argument("--custom", nargs=5, metavar=("WIDTH", "HEIGHT", "UNITS", "KEYINT", "OUT"),
                    help="one stream of your own: written to OUT (.bin) and OUT with .framemd5")
    a = ap.parse_args()
    if a.list:
        for n, s in FIXTURES.items():
            print("%s: %dx%d, %d units, key every %d, %s %d" % (n, s[0], s[1], s[2], s[3], s[4][0], s[4][1]))
        return 0
    if a.custom:
        w, h, n, k, out = int(a.custom[0]), int(a.custom[1]), int(a.custom[2]), int(a.custom[3]), a.custom[4]
        d, base = os.path.dirname(os.path.abspath(out)), os.path.splitext(os.path.basename(out))[0]
        make(a.ffmpeg, base, (w, h, n, k, ("crf", 23)), d)
        return 0
    for n, s in FIXTURES.items():
        if a.only and a.only != n:
            continue
        make(a.ffmpeg, n, s, a.outdir, a.rgb_dir)
    return 0


if __name__ == "__main__":
    sys.exit(main())

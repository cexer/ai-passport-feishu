"""Exercise real JPEG decode, bounded previews and failures without Pillow at runtime."""
import json
import struct
import subprocess
import sys
from pathlib import Path

executable = str(Path(sys.argv[1]).resolve())
directory = Path(sys.argv[2]).resolve()
fixtures = Path(__file__).resolve().parent / "fixtures/jpeg"

def run(name, source, zoom=0, x=-1, y=-1, expected=0, mode=0):
    output = directory / (name + ".rgb565")
    process = subprocess.run([executable, str(source), str(output), str(zoom), str(x), str(y), str(expected), str(mode)],
                             capture_output=True, text=True, check=True)
    info = json.loads(process.stdout)
    if not expected or mode:
        assert output.stat().st_size == info["w"] * info["h"] * 2 == info["bytes"]
        assert 0 < info["w"] <= 216 and 0 < info["h"] <= 226 and info["workspace"] < 14000
        if mode:
            assert info["recovery"] == 0
    return info, output.read_bytes()

def pixel(info, raw, x, y):
    value = struct.unpack_from("<H", raw, (y * info["w"] + x) * 2)[0]
    return ((value >> 11) * 8, ((value >> 5) & 63) * 4, (value & 31) * 8)

base = fixtures / "quadrants.jpg"
info, raw = run("overview", base)
for fx, fy, primary in ((1, 1, 0), (3, 1, 1), (1, 3, 2)):
    rgb = pixel(info, raw, info["w"] * fx // 4, info["h"] * fy // 4)
    assert rgb[primary] > 200 and all(value < 45 for channel, value in enumerate(rgb) if channel != primary), rgb
top, pixels = run("top-left", base, 3, 0, 0)
assert top["left"] == top["top"] == 0
assert pixel(top, pixels, top["w"] // 2, top["h"] // 2)[0] > 200
center, pixels = run("center-2x", base, 1, 512, 384)
assert pixel(center, pixels, 54, 132)[2] > 200
up, pixels = run("pan-up-2x", base, 1, 512, 267)
assert up["top"] == 0 and pixel(up, pixels, 54, 132)[0] > 200
bottom, pixels = run("bottom-right", base, 99, 99999, 99999)
assert bottom["left"] + bottom["crop_w"] == 1024 and bottom["top"] + bottom["crop_h"] == 768
rgb = pixel(bottom, pixels, bottom["w"] // 2, bottom["h"] // 2)
assert rgb[0] > 200 and rgb[1] > 200 and rgb[2] < 45
small, _ = run("small", fixtures / "small.jpg")
assert (small["w"], small["h"]) == (64, 48)
gray, pixels = run("gray", fixtures / "gray.jpg")
rgb = pixel(gray, pixels, gray["w"] // 2, gray["h"] // 2)
assert max(rgb) - min(rgb) <= 8
run("odd-dimensions", fixtures / "odd.jpg", 1, 50, 200)

data = base.read_bytes()
# Valid oversized APP metadata models a >300 KiB original without needing a
# large binary fixture or retaining its contents in the device implementation.
padded = directory / "large-metadata.jpg"
app = b"\xff\xef" + struct.pack(">H", 65535) + bytes(65533)
padded.write_bytes(data[:2] + app * 6 + data[2:])
large, pixels = run("large-original", padded)
assert padded.stat().st_size > 300 * 1024 and large["workspace"] == info["workspace"]
assert pixels == raw
truncated = directory / "truncated.jpg"
truncated.write_bytes(data[:-2])
run("missing-eoi", truncated, expected=1)
run("progressive", fixtures / "progressive.jpg", expected=2)
huge = directory / "huge-dimensions.jpg"
declared = bytearray(data)
at = declared.index(b"\xff\xc0")
struct.pack_into(">HH", declared, at + 5, 5000, 5000)
huge.write_bytes(declared)
run("pixel-cap", huge, expected=3)
for name, mode, expected in (("io-fault", 1, 4), ("storage-full", 2, 7), ("cancel", 3, 6)):
    run(name, base, expected=expected, mode=mode)
print("JPEG viewport: normal/oversized/gray/odd, zoom/pan, unsupported/truncated/pixel limits and recovery PASS")

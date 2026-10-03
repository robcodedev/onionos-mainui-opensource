# SPDX-License-Identifier: GPL-3.0-only
"""Exercise allocation limits without allocating oversized decoded images in Python."""
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
import zlib
from PIL import Image
from env import BUILD

def chunk(kind, data):
    return struct.pack(">I", len(data)) + kind + data + struct.pack(
        ">I", zlib.crc32(kind + data) & 0xffffffff)

def png(path, width, height):
    header = struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)
    compressor = zlib.compressobj()
    # Stream rows so even the 12000-square bomb takes only kilobytes here.
    row = b"\0" * (1 + width * 3)
    with path.open("wb") as output:
        output.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", header))
        for _ in range(height):
            data = compressor.compress(row)
            if data:
                output.write(chunk(b"IDAT", data))
        output.write(chunk(b"IDAT", compressor.flush()) + chunk(b"IEND", b""))

def main():
    with tempfile.TemporaryDirectory(prefix="allocation-bounds-", dir=BUILD) as directory:
        root = Path(directory)
        png(root / "theme-big.png", 1000, 1000)
        png(root / "icon-big.png", 2000, 2000)
        png(root / "icon-mid.png", 1200, 1200)
        png(root / "icon-other.png", 1200, 1200)
        png(root / "small.png", 1, 1)
        png(root / "edge.png", 2000, 1)
        png(root / "wide.png", 2001, 1)
        png(root / "tall.png", 1, 2001)
        png(root / "zero.png", 0, 1)
        png(root / "bomb.png", 12000, 12000)
        Image.new("RGB", (2, 2)).save(root / "jpeg.png", format="JPEG")
        Image.new("RGB", (2, 2)).save(root / "bmp.png", format="BMP")
        Image.new("P", (2, 2)).save(root / "palette.png", format="PNG")
        jpeg = bytearray((root / "jpeg.png").read_bytes())
        sof = jpeg.index(b"\xff\xc0")
        jpeg[sof + 5:sof + 9] = struct.pack(">HH", 12000, 12000)
        (root / "jpeg-large.png").write_bytes(jpeg)
        (root / "jpeg-bad.png").write_bytes(b"\xff\xd8\xff\xe0\x00\x01")
        bmp = bytearray((root / "bmp.png").read_bytes())
        bmp[18:26] = struct.pack("<ii", 12000, 12000)
        (root / "bmp-large.png").write_bytes(bmp)
        (root / "truncated.png").write_bytes((root / "small.png").read_bytes()[:23])
        (root / "random.png").write_bytes(b"garbage" * 100)
        (root / "empty.png").write_bytes(b"")
        Image.new("RGB", (2, 2)).save(root / "unknown.png", format="GIF")
        with (root / "encoded-large.png").open("wb") as output:
            output.write((root / "small.png").read_bytes())
            output.seek(16 * 1024 * 1024)
            output.write(b"x")
        (root / "skin").mkdir()
        shutil.copyfile(root / "bomb.png", root / "skin/thumb-default.png")
        for name, text in (
            ("MapBytes", 'alpha "oversized"\n' + "x" * (8 * 1024 * 1024)),
            ("MapLines", 'alpha "oversized"\n' + "\n" * 131072),
        ):
            target = root / name / "BIOS/arcade_lists/arcade-rom-names.txt"
            target.parent.mkdir(parents=True)
            target.write_text(text)
        subprocess.run([str(BUILD / "fixture-allocation_bounds"), str(root)],
                       check=True, timeout=15)

if __name__ == "__main__":
    main()

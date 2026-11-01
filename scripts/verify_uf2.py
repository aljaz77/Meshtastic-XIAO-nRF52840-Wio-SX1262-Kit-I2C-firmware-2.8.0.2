#!/usr/bin/env python3
"""Offline self-test for a Meshtastic nRF52840 UF2 image.

Checks, without any hardware:
  * the file is a well-formed UF2 (magic numbers, block numbering, payload size)
  * every block targets the nRF52840 family (0xADA52840)
  * the image sits entirely inside the application flash region, so it can't
    overwrite the SoftDevice, the LittleFS config partition or the bootloader
  * the expected firmware version string is embedded in the image
  * the SX1262 radio driver and the I2C sensor code are linked in

Usage:
  verify_uf2.py FIRMWARE.uf2 [--version 2.8.1.8e6a88d] [--ld path/to/linker.ld]
                [--require-string TEXT ...]

Exit code 0 = all checks passed, 1 = at least one check failed.
"""

from __future__ import annotations

import argparse
import re
import struct
import sys
from pathlib import Path

UF2_MAGIC_START0 = 0x0A324655
UF2_MAGIC_START1 = 0x9E5D5157
UF2_MAGIC_END = 0x0AB16F30
UF2_FLAG_FAMILY_ID = 0x00002000
UF2_FLAG_NOT_MAIN_FLASH = 0x00000001
NRF52840_FAMILY = 0xADA52840

# Fallback app region if no linker script is given: after S140 v7 SoftDevice,
# before the Adafruit bootloader. The linker script is stricter (it also
# reserves the LittleFS partition) and is preferred.
DEFAULT_FLASH_START = 0x27000
DEFAULT_FLASH_END = 0xF4000

# Strings that must be present in any working build of this variant.
REQUIRED_STRINGS = [
    b"SX1262",           # radio driver
    b"I2C devices found",  # I2C scanner, proves the Wire bus is compiled in
]


class Result:
    def __init__(self) -> None:
        self.failed = 0

    def ok(self, msg: str) -> None:
        print(f"  PASS  {msg}")

    def fail(self, msg: str) -> None:
        self.failed += 1
        print(f"  FAIL  {msg}")
        # GitHub Actions annotation, ignored elsewhere
        print(f"::error title=UF2 self-test::{msg}")

    def check(self, cond: bool, msg: str) -> bool:
        (self.ok if cond else self.fail)(msg)
        return cond


def flash_region_from_ld(ld_path: Path) -> tuple[int, int]:
    """Return (start, end) of the FLASH memory region in a GNU ld script."""
    text = ld_path.read_text()
    m = re.search(
        r"FLASH\s*\([^)]*\)\s*:\s*ORIGIN\s*=\s*([0-9a-fA-Fx]+)\s*,\s*LENGTH\s*=\s*([0-9a-fA-Fx+\-\s]+)",
        text,
    )
    if not m:
        raise ValueError(f"no FLASH region found in {ld_path}")
    origin = int(m.group(1), 0)
    # LENGTH is sometimes written as an expression such as "0xEA000 - 0x27000"
    expr = m.group(2).strip()
    if not re.fullmatch(r"[0-9a-fA-Fx+\-\s]+", expr):
        raise ValueError(f"unsupported LENGTH expression: {expr!r}")
    length = 0
    for sign, num in re.findall(r"([+\-]?)\s*(0x[0-9a-fA-F]+|\d+)", expr):
        length += -int(num, 0) if sign == "-" else int(num, 0)
    return origin, origin + length


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("uf2", type=Path)
    ap.add_argument("--version", help="firmware version string that must be embedded, e.g. 2.8.1.8e6a88d")
    ap.add_argument("--ld", type=Path, help="linker script to read the allowed FLASH region from")
    ap.add_argument("--require-string", action="append", default=[], metavar="TEXT",
                    help="extra text that must be present in the image (repeatable), e.g. to prove a driver is linked in")
    args = ap.parse_args()

    r = Result()
    data = args.uf2.read_bytes()
    print(f"Verifying {args.uf2} ({len(data)} bytes)")

    # ---- UF2 container -------------------------------------------------
    if not r.check(len(data) > 0 and len(data) % 512 == 0, "file size is a non-zero multiple of 512"):
        return 1

    nblocks = len(data) // 512
    payload: dict[int, bytes] = {}
    bad_magic = bad_family = bad_seq = bad_total = bad_size = 0
    for i in range(nblocks):
        blk = data[i * 512 : (i + 1) * 512]
        m0, m1, flags, addr, size, blockno, total, family = struct.unpack_from("<8I", blk, 0)
        (mend,) = struct.unpack_from("<I", blk, 508)
        if (m0, m1, mend) != (UF2_MAGIC_START0, UF2_MAGIC_START1, UF2_MAGIC_END):
            bad_magic += 1
            continue
        if not (flags & UF2_FLAG_FAMILY_ID) or family != NRF52840_FAMILY:
            bad_family += 1
        if blockno != i:
            bad_seq += 1
        if total != nblocks:
            bad_total += 1
        if size == 0 or size > 476:
            bad_size += 1
            continue
        if flags & UF2_FLAG_NOT_MAIN_FLASH:
            continue
        payload[addr] = blk[32 : 32 + size]

    r.check(bad_magic == 0, f"all {nblocks} blocks have valid UF2 magic numbers")
    r.check(bad_family == 0, "all blocks target the nRF52840 family (0xADA52840)")
    r.check(bad_seq == 0 and bad_total == 0, "block numbering is sequential and complete")
    r.check(bad_size == 0, "payload sizes are valid")
    if not payload:
        r.fail("image contains no flash payload")
        return 1

    # ---- Flash layout --------------------------------------------------
    lo = min(payload)
    hi = max(a + len(b) for a, b in payload.items())
    if args.ld:
        fstart, fend = flash_region_from_ld(args.ld)
        src = args.ld.name
    else:
        fstart, fend = DEFAULT_FLASH_START, DEFAULT_FLASH_END
        src = "built-in defaults"
    print(f"  info  image spans 0x{lo:05X}-0x{hi:05X}; allowed region 0x{fstart:05X}-0x{fend:05X} ({src})")
    r.check(lo >= fstart, f"image starts at or after app start 0x{fstart:05X} (SoftDevice is safe)")
    r.check(hi <= fend, f"image ends before 0x{fend:05X} (config storage / bootloader are safe)")
    used = hi - lo
    room = fend - fstart
    print(f"  info  flash used {used / 1024:.1f} KiB of {room / 1024:.1f} KiB ({100 * used / room:.1f}%)")

    addrs = sorted(payload)
    overlaps = sum(1 for a, b in zip(addrs, addrs[1:]) if a + len(payload[a]) > b)
    r.check(overlaps == 0, "no overlapping blocks")

    # ---- Content -------------------------------------------------------
    image = bytearray(b"\xff" * (hi - lo))
    for a, b in payload.items():
        image[a - lo : a - lo + len(b)] = b

    if args.version:
        r.check(args.version.encode() in image, f"version string '{args.version}' is embedded")
    for s in REQUIRED_STRINGS + [t.encode() for t in args.require_string]:
        r.check(s in image, f"contains '{s.decode()}'")

    print()
    if r.failed:
        print(f"UF2 self-test FAILED ({r.failed} check(s))")
        return 1
    print("UF2 self-test passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())

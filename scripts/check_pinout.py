#!/usr/bin/env python3
"""Verify that upstream's I2C variant still uses the pinout this repo documents.

Upstream Meshtastic can change variant.h at any time. If the I2C pins, the GPS
pins or the LoRa pins ever move, a board wired by following our README would
stop working after an update. This check catches that before a release is
published.

How it works:
  1. Ask PlatformIO for the fully resolved build_flags of the env (so the
     -D / -U chain of `extends = ...` is reproduced exactly).
  2. Compile a tiny host program that includes the real variant.h with those
     flags and static_asserts every pin we care about.

Usage (run from the root of a meshtastic/firmware checkout):
  check_pinout.py [--env seeed_xiao_nrf52840_kit_i2c]
"""

from __future__ import annotations

import argparse
import json
import shlex
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

VARIANT_DIR = "variants/nrf52840/seeed_xiao_nrf52840_kit"

# The wiring this repo promises. Arduino pin numbers from the XIAO variant:
# D0..D10 = 0..10, NFC1 = 30 (P0.09), NFC2 = 31 (P0.10).
EXPECTED = {
    "PIN_WIRE_SDA": 6,   # D6  / P1.11
    "PIN_WIRE_SCL": 7,   # D7  / P1.12
    "GPS_TX_PIN": 30,    # NFC1 / P0.09
    "GPS_RX_PIN": 31,    # NFC2 / P0.10
    "BUTTON_PIN": 0,     # D0  / P0.02
    "SX126X_CS": 4,      # D4
    "SX126X_DIO1": 1,    # D1
    "SX126X_BUSY": 3,    # D3
    "SX126X_RESET": 2,   # D2
    "SX126X_RXEN": 5,    # D5
    "PIN_SPI_SCK": 8,    # D8
    "PIN_SPI_MISO": 9,   # D9
    "PIN_SPI_MOSI": 10,  # D10
}
MUST_BE_DEFINED = ["SEEED_XIAO_NRF_KIT_I2C", "USE_SX1262", "HAS_GPS"]
MUST_NOT_BE_DEFINED = ["SEEED_XIAO_NRF_KIT_DEFAULT", "PIN_GPS_STANDBY", "XIAO_BLE_LEGACY_PINOUT"]

# variant.h includes the Arduino core's WVariant.h; a host build only needs stdint
STUB_WVARIANT = "#pragma once\n#include <stdint.h>\n"


def resolved_build_flags(env: str) -> list[str]:
    out = subprocess.check_output(["pio", "project", "config", "--json-output"], text=True)
    for section, options in json.loads(out):
        if section == f"env:{env}":
            for key, value in options:
                if key == "build_flags":
                    flags = value if isinstance(value, list) else [value]
                    return [tok for item in flags for tok in shlex.split(item)]
            return []
    sys.exit(f"::error title=Pinout check::env '{env}' not found in platformio config "
             f"(upstream may have renamed or removed it)")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--env", default="seeed_xiao_nrf52840_kit_i2c")
    args = ap.parse_args()

    if not Path(VARIANT_DIR, "variant.h").exists():
        sys.exit(f"::error title=Pinout check::{VARIANT_DIR}/variant.h not found; run from a meshtastic/firmware checkout")

    # Keep only macro definitions/undefinitions, in order, so "-U" after "-D" works
    flags = []
    toks = resolved_build_flags(args.env)
    i = 0
    while i < len(toks):
        tok = toks[i]
        if tok in ("-D", "-U") and i + 1 < len(toks):  # "-D NAME" written with a space
            flags.append(tok + toks[i + 1])
            i += 2
            continue
        if tok.startswith(("-D", "-U")) and len(tok) > 2:
            flags.append(tok)
        i += 1
    print("Resolved macro flags:", " ".join(flags))

    lines = ['#include "variant.h"', ""]
    for name, value in EXPECTED.items():
        lines += [
            f"#ifndef {name}",
            f'#error "{name} is not defined"',
            "#endif",
            f'_Static_assert(({name}) == {value}, "{name} changed upstream (expected {value})");',
        ]
    for name in MUST_BE_DEFINED:
        lines += [f"#ifndef {name}", f'#error "{name} should be defined"', "#endif"]
    for name in MUST_NOT_BE_DEFINED:
        lines += [f"#ifdef {name}", f'#error "{name} should NOT be defined in the I2C variant"', "#endif"]
    lines.append("int main(void) { return 0; }")

    cc = shutil.which("cc") or shutil.which("gcc") or shutil.which("clang")
    if not cc:
        sys.exit("::error title=Pinout check::no host C compiler found")

    with tempfile.TemporaryDirectory() as tmp:
        Path(tmp, "WVariant.h").write_text(STUB_WVARIANT)
        src = Path(tmp, "check.c")
        src.write_text("\n".join(lines) + "\n")
        cmd = [cc, "-std=c11", "-fsyntax-only", "-I", tmp, "-I", VARIANT_DIR, *flags, str(src)]
        proc = subprocess.run(cmd, capture_output=True, text=True)

    if proc.returncode != 0:
        print(proc.stderr)
        for line in proc.stderr.splitlines():
            if "error:" in line:
                print(f"::error title=Pinout check::{line.strip()}")
        print("Pinout check FAILED: upstream changed the I2C variant wiring.")
        return 1

    width = max(map(len, EXPECTED))
    for name, value in EXPECTED.items():
        print(f"  PASS  {name:<{width}} = {value}")
    for name in MUST_BE_DEFINED:
        print(f"  PASS  {name} is defined")
    for name in MUST_NOT_BE_DEFINED:
        print(f"  PASS  {name} is not defined")
    print("\nPinout check passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())

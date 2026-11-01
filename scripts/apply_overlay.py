#!/usr/bin/env python3
"""Apply this repository's additions (the BQ25798 solar charger driver) to a
meshtastic/firmware checkout.

1. Copies everything under overlay/ into the firmware tree (new files only, so
   upstream code is never overwritten).
2. Makes small, anchored insertions in two upstream files:
     src/Power.cpp
     - #include "power/BQ25798BatteryLevel.h" just before Power::setup()
     - `else if (bq25798Init())` in the battery detection chain, just before
       the analog-ADC fallback
     src/modules/Telemetry/PowerTelemetry.cpp
     - #include "power/BQ25798Telemetry.h"
     - keep the module enabled when the charger is present (no INA sensor needed)
     - add the charger's readings to every power-metrics packet
   Anchors are matched on text, not line numbers, so unrelated upstream edits
   don't matter. If an anchor disappears the script fails loudly, the
   automatic build stops and an issue is opened, rather than silently
   building firmware without the driver.

Safe to run twice (already-applied edits are detected and skipped).

Usage:  apply_overlay.py path/to/meshtastic-firmware
"""

from __future__ import annotations

import re
import shutil
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
OVERLAY = REPO_ROOT / "overlay"

INCLUDE_LINE = '#include "power/BQ25798BatteryLevel.h" // added by apply_overlay.py (XIAO I2C kit overlay)\n'
SETUP_ANCHOR = re.compile(r"^bool Power::setup\(\)\s*$", re.M)
CHAIN_ANCHOR = re.compile(r"^(?P<indent>[ \t]*)\} else if \(analogInit\(\)\) \{\s*$", re.M)


def fail(msg: str) -> None:
    print(f"::error title=apply_overlay::{msg}")
    sys.exit(1)


def copy_overlay(fw: Path) -> None:
    for src in sorted(OVERLAY.rglob("*")):
        if src.is_dir():
            continue
        rel = src.relative_to(OVERLAY)
        dst = fw / rel
        if dst.exists() and dst.read_bytes() != src.read_bytes():
            fail(f"{rel} already exists upstream with different content; the overlay would overwrite upstream code")
        dst.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(src, dst)
        print(f"  copied   {rel}")


def patch_power_cpp(fw: Path) -> None:
    path = fw / "src" / "Power.cpp"
    if not path.exists():
        fail("src/Power.cpp not found (upstream restructured power handling)")
    text = path.read_text()

    if "power/BQ25798BatteryLevel.h" in text:
        print("  skipped  src/Power.cpp include (already applied)")
    else:
        matches = list(SETUP_ANCHOR.finditer(text))
        if len(matches) != 1:
            fail(f"expected exactly one 'bool Power::setup()' in src/Power.cpp, found {len(matches)}")
        pos = matches[0].start()
        # Keep Power::setup()'s doc comment attached to it: insert above the comment block
        before = text[:pos]
        if before.rstrip().endswith("*/"):
            start = before.rfind("/**")
            if start != -1 and before[start:].count("*/") == 1:
                pos = start
        text = text[:pos] + INCLUDE_LINE + "\n" + text[pos:]
        print("  patched  src/Power.cpp: include before Power::setup()")

    if "bq25798Init()" in text.split("bool Power::setup()", 1)[1].split("\n}\n", 1)[0]:
        print("  skipped  src/Power.cpp detection chain (already applied)")
    else:
        setup_start = SETUP_ANCHOR.search(text).start()
        matches = [m for m in CHAIN_ANCHOR.finditer(text) if m.start() > setup_start]
        if not matches:
            fail("could not find '} else if (analogInit()) {' inside Power::setup()")
        m = matches[0]
        ind = m.group("indent")
        insert = f"{ind}}} else if (bq25798Init()) {{ // added by apply_overlay.py\n{ind}    found = true;\n"
        text = text[: m.start()] + insert + text[m.start():]
        print("  patched  src/Power.cpp: bq25798Init() in battery detection chain")

    path.write_text(text)


TELEMETRY_INCLUDE = '#include "power/BQ25798Telemetry.h" // added by apply_overlay.py\n'
TELEMETRY_INCLUDE_ANCHOR = re.compile(r'^#include "PowerTelemetry\.h"[^\n]*\n', re.M)
TELEMETRY_INIT_ANCHOR = re.compile(r"^(?P<indent>[ \t]*)return result == UINT32_MAX \? disable\(\) : setStartDelay\(\);", re.M)
TELEMETRY_GET_FUNC = re.compile(r"^bool PowerTelemetryModule::getPowerTelemetry\(meshtastic_Telemetry \*\w+\)\s*\{", re.M)
TELEMETRY_GET_RETURN = re.compile(r"^(?P<indent>[ \t]*)return valid;", re.M)


def patch_power_telemetry_cpp(fw: Path) -> None:
    path = fw / "src" / "modules" / "Telemetry" / "PowerTelemetry.cpp"
    if not path.exists():
        fail("src/modules/Telemetry/PowerTelemetry.cpp not found (upstream restructured power telemetry)")
    text = path.read_text()

    if "power/BQ25798Telemetry.h" in text:
        print("  skipped  PowerTelemetry.cpp (already applied)")
        return

    m = TELEMETRY_INCLUDE_ANCHOR.search(text)
    if not m:
        fail('could not find \'#include "PowerTelemetry.h"\' in PowerTelemetry.cpp')
    text = text[: m.end()] + TELEMETRY_INCLUDE + text[m.end():]

    matches = list(TELEMETRY_INIT_ANCHOR.finditer(text))
    if len(matches) != 1:
        fail(f"expected one 'return result == UINT32_MAX ? disable() : setStartDelay();' in PowerTelemetry.cpp, found {len(matches)}")
    m = matches[0]
    ind = m.group("indent")
    insert = f"{ind}if (bq25798HasTelemetry()) // added by apply_overlay.py\n{ind}    result = 0;\n"
    text = text[: m.start()] + insert + text[m.start():]

    f = TELEMETRY_GET_FUNC.search(text)
    if not f:
        fail("could not find PowerTelemetryModule::getPowerTelemetry() in PowerTelemetry.cpp")
    m = TELEMETRY_GET_RETURN.search(text, f.end())
    if not m or "\n}\n" in text[f.end(): m.start()]:
        fail("could not find 'return valid;' inside getPowerTelemetry()")
    ind = m.group("indent")
    arg = re.search(r"\*(\w+)\)", f.group(0)).group(1)
    insert = f"{ind}if (bq25798GetPowerMetrics({arg})) // added by apply_overlay.py\n{ind}    valid = true;\n"
    text = text[: m.start()] + insert + text[m.start():]

    path.write_text(text)
    print("  patched  PowerTelemetry.cpp: include, keep-enabled check, readings in getPowerTelemetry()")


def main() -> int:
    if len(sys.argv) != 2:
        print(__doc__)
        return 2
    fw = Path(sys.argv[1]).resolve()
    if not (fw / "platformio.ini").exists():
        fail(f"{fw} does not look like a meshtastic/firmware checkout")
    print(f"Applying overlay to {fw}")
    copy_overlay(fw)
    patch_power_cpp(fw)
    patch_power_telemetry_cpp(fw)
    print("Overlay applied")
    return 0


if __name__ == "__main__":
    sys.exit(main())

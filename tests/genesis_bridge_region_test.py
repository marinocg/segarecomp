#!/usr/bin/env python3
"""SEG-055: the bridge's console-region selection (`--region`) -- pure-Python, hermetic, no ROM and no generation.

Covers: header region-support hints (old letter style and the new hexadecimal bitfield style), the documented preference order
(overseas NTSC, else Japan, else Europe), `auto` vs explicit selection, the default console, Europe/PAL refusal, the environment
variable handed to the generated program, and the new typed diagnostics accepted by the stop-pair table.
"""
import os
import pathlib
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import genesis_startup_bridge as bridge  # noqa: E402


def rom_with_region(field: bytes) -> bytes:
  image = bytearray(0x200)
  image[0x1F0:0x1F0 + 16] = field.ljust(16, b" ")
  return bytes(image)


def check(condition, message):
  if not condition:
    raise AssertionError(message)


def main():
  hint = bridge.header_region_hint
  check(hint(rom_with_region(b"JUE"))[0] == "usa", "JUE prefers overseas NTSC (the historical default)")
  check(hint(rom_with_region(b"JU"))[0] == "usa", "JU declares overseas NTSC")
  check(hint(rom_with_region(b"J"))[0] == "japan", "J alone (domestic only) selects Japan NTSC")
  check(hint(rom_with_region(b"E"))[0] == "europe", "E alone selects Europe (later refused as PAL)")
  check(hint(rom_with_region(b"U"))[0] == "usa", "U alone selects USA NTSC")
  # New style: one hexadecimal digit, bit 0 = Japan 60 Hz, bit 2 = Americas 60 Hz, bit 3 = Europe 50 Hz.
  check(hint(rom_with_region(b"1"))[0] == "japan", "new style 1 = Japan only")
  check(hint(rom_with_region(b"4"))[0] == "usa", "new style 4 = Americas only")
  check(hint(rom_with_region(b"5"))[0] == "usa", "new style 5 = Japan + Americas prefers overseas NTSC")
  check(hint(rom_with_region(b"8"))[0] == "europe", "new style 8 = Europe only")
  check(hint(rom_with_region(b"0"))[0] == "usa", "an empty declaration falls back to the default")
  check(hint(b"\x00" * 0x100)[0] == "usa", "an image too short for the header field falls back to the default")
  check("J" in hint(rom_with_region(b"J"))[1], "the hint describes what the header declares")

  saved = os.environ.get("SEGARECOMP_GENESIS_REGION")
  try:
    os.environ.pop("SEGARECOMP_GENESIS_REGION", None)
    profile, summary = bridge.configure_region("usa", rom_with_region(b"J"))
    check(profile == "usa" and os.environ["SEGARECOMP_GENESIS_REGION"] == "usa" and summary["version_register"] == "0xA0",
          "explicit usa is exported and never overridden by the header")
    profile, summary = bridge.configure_region("japan", rom_with_region(b"JUE"))
    check(profile == "japan" and os.environ["SEGARECOMP_GENESIS_REGION"] == "japan" and summary["version_register"] == "0x20" and
          summary["source"] == "explicit", "explicit japan is honored regardless of the header")
    profile, summary = bridge.configure_region("auto", rom_with_region(b"J"))
    check(profile == "japan" and summary["source"] == "header_hint" and "J" in summary["note"], "auto derives a Japan hint from a domestic-only header")
    os.environ["SEGARECOMP_GENESIS_REGION"] = "stale-value"
    profile, _ = bridge.configure_region("usa", rom_with_region(b"JUE"))
    check(os.environ["SEGARECOMP_GENESIS_REGION"] == "usa", "the default (usa) replaces an inherited value, so the run is deterministic")
    os.environ["SEGARECOMP_GENESIS_REGION"] = "japan"
    refused, summary = bridge.configure_region("europe", rom_with_region(b"JUE"))
    check(refused is None and summary["error"] == "europe_pal_timing_not_implemented" and summary["version_register"] == "0xE0",
          "Europe PAL is representable (0xE0) but refused honestly")
    check(os.environ["SEGARECOMP_GENESIS_REGION"] == "japan", "a refused profile leaves the environment untouched")
    refused_auto, _ = bridge.configure_region("auto", rom_with_region(b"E"))
    check(refused_auto is None, "auto resolving to Europe is refused as well")
  finally:
    if saved is None:
      os.environ.pop("SEGARECOMP_GENESIS_REGION", None)
    else:
      os.environ["SEGARECOMP_GENESIS_REGION"] = saved

  pairs = bridge.STOP_DIAGNOSTIC_PAIRS["unsupported_interrupt_or_scheduling_event"]
  check({"unsupported_region_timing", "hint_handler_not_installed", "stopped_without_wake_source"} <= pairs,
        "the new typed interrupt/region diagnostics are accepted by the stop-pair table")
  check(bridge.REGION_CHOICES == ("usa", "japan", "europe", "auto"), "documented region choices")
  print("genesis bridge region: ok")


if __name__ == "__main__":
  main()

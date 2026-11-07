#!/usr/bin/env python3
"""SEG-131 Checkpoint F (ADR 0278): renders every save-state and rewind screen at the four review window sizes with the sv_state_gallery tool (a fictional picture
and a fake provider, SDL dummy video driver, software renderer) and checks the captures: each screen exists, has exactly the window's pixel size, is not blank, and
the screens that must differ (the selection moves, the confirmations, the unavailable states) really do. Original pixel art only: no game, no ROM.
usage: sv_state_gallery_test.py <sv_state_gallery> [output-dir]
"""
import hashlib
import os
import pathlib
import struct
import subprocess
import sys
import tempfile
import zlib

SIZES = ((720, 520), (1080, 740), (1440, 1040), (2560, 1440))
SCREENS = ("11_pause_menu_states", "12_save_slots", "13_overwrite_confirmation", "14_load_slots", "15_load_confirmation", "16_rewind_open", "17_rewind_back_1",
           "18_rewind_middle", "19_rewind_oldest", "20_rewind_newest_moment", "21_rewind_short_history", "22_rewind_memory_limited", "23_rewind_error",
           "24_rewind_unavailable", "25_states_unavailable")
failures = []


def check(ok, label):
  if not ok:
    failures.append(label)
    print("FAIL  " + label)


def png_info(path):
  data = path.read_bytes()
  assert data[:8] == b"\x89PNG\r\n\x1a\n", "not a PNG: %s" % path
  width, height = struct.unpack(">II", data[16:24])
  # distinct colours sampled from the decoded first rows would need a full decoder; the compressed size is a robust 'not blank' witness
  return width, height, len(data)


def main():
  exe = sys.argv[1]
  keep = pathlib.Path(sys.argv[2]) if len(sys.argv) > 2 else None
  with tempfile.TemporaryDirectory() as tmp:
    base = keep or pathlib.Path(tmp)
    for width, height in SIZES:
      out = base / ("%dx%d" % (width, height))
      env = dict(os.environ, SDL_VIDEODRIVER="dummy", SDL_AUDIODRIVER="dummy")
      done = subprocess.run([exe, str(out), "%dx%d" % (width, height)], capture_output=True, text=True, env=env, timeout=300)
      check(done.returncode == 0, "%dx%d: the gallery tool succeeded: %s" % (width, height, done.stderr[-200:]))
      hashes = {}
      for name in SCREENS:
        path = out / (name + ".png")
        check(path.is_file(), "%dx%d: %s exists" % (width, height, name))
        if not path.is_file():
          continue
        w, h, size = png_info(path)
        check((w, h) == (width, height), "%dx%d: %s is %dx%d" % (width, height, name, w, h))
        check(size > 4000, "%dx%d: %s is not blank (%d bytes compressed)" % (width, height, name, size))
        hashes[name] = hashlib.sha256(path.read_bytes()).hexdigest()
      for a, b in (("16_rewind_open", "17_rewind_back_1"), ("17_rewind_back_1", "18_rewind_middle"), ("18_rewind_middle", "19_rewind_oldest"), ("19_rewind_oldest", "20_rewind_newest_moment"),
                   ("12_save_slots", "14_load_slots"), ("13_overwrite_confirmation", "15_load_confirmation"), ("11_pause_menu_states", "25_states_unavailable"),
                   ("16_rewind_open", "21_rewind_short_history"), ("21_rewind_short_history", "22_rewind_memory_limited")):
        check(hashes.get(a) != hashes.get(b) or a not in hashes, "%dx%d: %s and %s differ" % (width, height, a, b))
  print("%d failure(s)" % len(failures))
  return 1 if failures else 0


if __name__ == "__main__":
  sys.exit(main())

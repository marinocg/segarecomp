#!/usr/bin/env python3
"""SEG-055: generated-native proof of the VDP H-interrupt (level-4 autovector, vector 28).

Project-authored synthetic ROM images only. For each image the real `segarecomp emit-general-startup-bridge-c` emitter produces
the bridge C, which is compiled as strict C11 together with the production runtime and executed:

1. A ROM-resident vector-28 handler is resolved and rooted at build time: the generated `main` carries
   `software_exception_handler_entry[28]` / `..._present[28]`, the handler executes under the real scheduler (counted in D7),
   the mainline resumes through the handler's RTE and completes, and two runs are byte-identical. The H-interrupt count
   equals the independently derived schedule (register #10 = 9 -> 22 requests per 224-line active display).
2. The same image with the H-interrupt DISABLED executes no handler (D7 stays 0) -- the interrupt is gated by register #0 bit 4.
3. A work-RAM vector slot (RAM jump-table convention) builds without a handler. By default an enabled but undeliverable
   H-interrupt is counted and dropped (what every earlier build did, now observable through RUNTIME_NOTES-style counters) and the
   program continues; with SEGARECOMP_GENESIS_HINT=strict the run stops fail-closed with the typed `hint_handler_not_installed`
   diagnostic instead.
4. A vector slot that is zero (never installed) behaves the same way.
"""
import pathlib
import re
import struct
import subprocess
import sys
import tempfile
import hashlib

HANDLER = 0x180


def build_rom(vector28, handler_body=b"\x52\x87\x4e\x73"):
  """Entry 0x100: MOVE.W #$FFFF,D0 ; LOOP: DBRA D0,LOOP ; RESET (a ~655 k-cycle countdown, several frames).
  Handler at 0x180: ADDQ.L #1,D7 ; RTE (D7 counts accepted H-interrupts)."""
  image = bytearray(HANDLER + 0x300)
  struct.pack_into(">I", image, 0x0, 0x00FF8000)
  struct.pack_into(">I", image, 0x4, 0x100)
  struct.pack_into(">I", image, 0x70, vector28)
  image[0x100:0x104] = b"\x30\x3c\xff\xff"            # MOVE.W #$FFFF,D0
  image[0x104:0x106] = b"\x51\xc8"                    # DBRA D0,LOOP
  image[0x106:0x108] = struct.pack(">h", -2)          # displacement back to 0x104
  image[0x108:0x10A] = b"\x4e\x70"                    # RESET (post-loop terminal)
  if handler_body and vector28 == HANDLER:
    image[HANDLER:HANDLER + len(handler_body)] = handler_body
  return bytes(image)


def emit(executable, rom, temp):
  path = pathlib.Path(temp) / "rom.bin"
  path.write_bytes(rom)
  out = pathlib.Path(temp) / "generated.c"
  digest = hashlib.sha256(rom).hexdigest()
  run = subprocess.run([executable, "emit-general-startup-bridge-c", "--rom", str(path), "--reset-entry",
                        "--rom-sha256", digest, "--generated-c-output", str(out)], text=True, capture_output=True)
  assert run.returncode == 0, run.stderr
  return out.read_text()


def seed(generated_c, enable, reg10):
  """Test-only patch of the generated main: supervisor mode with the mask open, H-interrupt enabled, counter reload set; plus a
  stderr print of D7 (the handler's execution witness) and of the accepted-interrupt bookkeeping after the run."""
  anchor = "runtime.sr = UINT16_C(0x2700); "
  assert anchor in generated_c, generated_c[:300]
  seeded = generated_c.replace(
      anchor,
      "runtime.sr = UINT16_C(0x2000); runtime.devices.vdp.registers[0] = %s; runtime.devices.vdp.registers[10] = %d; "
      % ("UINT16_C(0x0010)" if enable else "UINT16_C(0)", reg10), 1)
  drive = re.search(r"result = genesis_runtime_run\([^;]*\);", seeded)
  assert drive is not None, seeded
  instrumentation = (' fprintf(stderr, "d7=%u frames=%llu\\n", (unsigned)runtime.d[7], '
                     '(unsigned long long)runtime.virtual_frame_count);')
  return seeded[:drive.end()] + instrumentation + seeded[drive.end():]


def build_and_run(compiler, runtime_dir, source, name, budget="2000000", env=None):
  with tempfile.TemporaryDirectory() as temp:
    path = pathlib.Path(temp)
    (path / "generated.c").write_text(source)
    prog = path / name
    build = subprocess.run([compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic", "-I", str(runtime_dir),
                            str(path / "generated.c"), str(runtime_dir / "runtime.c"), "-o", str(prog)],
                           text=True, capture_output=True)
    assert build.returncode == 0, build.stderr
    import os
    run_env = dict(os.environ)
    run_env.pop("SEGARECOMP_GENESIS_HINT", None)
    run_env.update(env or {})
    first = subprocess.run([str(prog), "--instruction-budget", budget], text=True, capture_output=True, env=run_env)
    second = subprocess.run([str(prog), "--instruction-budget", budget], text=True, capture_output=True, env=run_env)
    assert (first.returncode, first.stdout, first.stderr) == (second.returncode, second.stdout, second.stderr), \
        "generated program must be deterministic"
    return first


def d7_of(run):
  match = re.search(r"d7=(\d+) frames=(\d+)", run.stderr)
  assert match is not None, run.stderr
  return int(match.group(1)), int(match.group(2))


def main():
  executable, compiler, root = sys.argv[1:]
  runtime_dir = pathlib.Path(root) / "platforms/genesis/runtime"

  with tempfile.TemporaryDirectory() as temp:
    resolved = emit(executable, build_rom(HANDLER), temp)
  assert "runtime.software_exception_handler_entry[28] = UINT32_C(0x00000180);" in resolved, resolved[:600]
  assert "runtime.software_exception_handler_present[28] = 1;" in resolved
  with tempfile.TemporaryDirectory() as temp:
    assert resolved == emit(executable, build_rom(HANDLER), temp), "generated bridge C must be deterministic"

  # 1. Enabled: the handler runs under the real scheduler, resumes the mainline, and the count follows the line schedule.
  enabled = build_and_run(compiler, runtime_dir, seed(resolved, True, 9), "hint-enabled")
  count, frames = d7_of(enabled)
  # The mainline's own post-loop RESET is the recognizable terminal (an unsupported-form stop): it can only be reached by
  # a mainline that was correctly resumed by every handler RTE.
  assert '"family":"reset"' in enabled.stdout, enabled.stdout
  assert frames >= 3, (count, frames)
  # 22 requests per 224-line active display (reg #10 = 9); the loop covers `frames` onsets plus a partial frame, and each
  # request is accepted at its own line. Bound the count by the independently derived schedule.
  assert 22 * frames <= count <= 22 * (frames + 1) + 1, (count, frames)

  # 2. Disabled in register #0: the very same program accepts no H-interrupt.
  disabled = build_and_run(compiler, runtime_dir, seed(resolved, False, 9), "hint-disabled")
  assert d7_of(disabled)[0] == 0, disabled.stderr

  # 3. A work-RAM vector slot: the build succeeds without a handler. Default policy: the enabled interrupt is dropped (counted)
  # and the mainline completes; strict policy: typed fail-closed stop.
  with tempfile.TemporaryDirectory() as temp:
    ram_vector = emit(executable, build_rom(0x00FFFF90), temp)
  assert "software_exception_handler_entry[28]" not in ram_vector
  skipped = build_and_run(compiler, runtime_dir, seed(ram_vector, True, 9), "hint-ram-vector")
  assert d7_of(skipped)[0] == 0 and '"family":"reset"' in skipped.stdout, (skipped.stdout, skipped.stderr)
  strict = build_and_run(compiler, runtime_dir, seed(ram_vector, True, 9), "hint-ram-vector-strict",
                         env={"SEGARECOMP_GENESIS_HINT": "strict"})
  assert '"diagnostic_category":"hint_handler_not_installed"' in strict.stdout, (strict.stdout, strict.stderr)
  assert '"stop_class":"unsupported_interrupt_or_scheduling_event"' in strict.stdout, strict.stdout

  # 4. A never-installed (zero) slot behaves the same way.
  with tempfile.TemporaryDirectory() as temp:
    absent = emit(executable, build_rom(0), temp)
  assert "software_exception_handler_entry[28]" not in absent
  dropped = build_and_run(compiler, runtime_dir, seed(absent, True, 9), "hint-absent")
  assert d7_of(dropped)[0] == 0 and '"family":"reset"' in dropped.stdout, dropped.stdout
  strict_absent = build_and_run(compiler, runtime_dir, seed(absent, True, 9), "hint-absent-strict",
                                env={"SEGARECOMP_GENESIS_HINT": "strict"})
  assert '"diagnostic_category":"hint_handler_not_installed"' in strict_absent.stdout, strict_absent.stdout
  quiet = build_and_run(compiler, runtime_dir, seed(absent, False, 9), "hint-absent-disabled", env={"SEGARECOMP_GENESIS_HINT": "strict"})
  assert d7_of(quiet)[0] == 0 and "hint_handler_not_installed" not in quiet.stdout, quiet.stdout

  print("genesis hint generated: ok")


if __name__ == "__main__":
  main()

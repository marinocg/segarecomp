import pathlib
import subprocess
import sys
import tempfile

def main():
    compiler, runtime_dir, runtime_c = sys.argv[1:]
    source = r'''#include "runtime.h"
static GenesisControlTransfer block(GenesisRuntime *r) {
  GenesisControlTransfer t;
  t = genesis_runtime_retire_m68k_instruction(r, 4U, 0x102U);
  if (t.kind != GENESIS_CONTINUE_AT_PC || t.next_pc != r->pc) return t;
  t = genesis_runtime_retire_m68k_instruction(r, 4U, 0x104U);
  if (t.kind != GENESIS_CONTINUE_AT_PC || t.next_pc != r->pc) return t;
  return t;
}
int main(void) {
  GenesisRuntime r = {0}; GenesisControlTransfer t;
  r.pc = 0x100U; r.sr = 0x2000U; r.devices.vdp.registers[1] = 0x20U;
  r.scheduler.master_ticks = GENESIS_NTSC_VBLANK_ONSET_TICK - 28U;
  t = block(&r);
  return t.kind == GENESIS_CONTINUE_AT_PC && r.scheduler.master_ticks == GENESIS_NTSC_VBLANK_ONSET_TICK + 28U && r.devices.interrupt.vblank_transition_count == 1U ? 0 : 1;
}'''
    with tempfile.TemporaryDirectory() as directory:
        path = pathlib.Path(directory)
        generated = path / "generated.c"
        executable = path / "generated"
        generated.write_text(source + "\n")  # ISO C requires a final newline (clang -Wnewline-eof)
        build = subprocess.run([compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic", "-I", runtime_dir, str(generated), runtime_c, "-o", str(executable)], text=True, capture_output=True)
        assert build.returncode == 0, build.stderr
        run = subprocess.run([str(executable)], text=True, capture_output=True)
        assert run.returncode == 0, run.stderr
if __name__ == "__main__": main()

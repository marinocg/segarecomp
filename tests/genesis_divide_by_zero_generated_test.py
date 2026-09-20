#!/usr/bin/env python3
"""Strict-C11 end-to-end synthetic vector-5 C4 dispatcher regression."""
import pathlib
import re
import subprocess
import sys
import tempfile
import os
import shutil


def main():
    executable, compiler, root = sys.argv[1:]
    generated = subprocess.run([executable, "--emit-general-startup-bridge-divide-by-zero-rte"],
                               text=True, capture_output=True, check=True).stdout
    assert "runtime.divide_by_zero_handler_present = 1;" in generated
    # Test-only observation after the production drive call: D3 must retain its
    # seed, D7 proves handler execution, PC proves RTE continuation, and IRQ6
    # scheduler/device state remained untouched by the synchronous transfer.
    observe = (' fprintf(stderr, "d3=%u d7=%u pc=%u sr=%u tick=%llu pending=%u\\n", '
               '(unsigned)runtime.d[3], (unsigned)runtime.d[7], (unsigned)runtime.pc, '
                '(unsigned)runtime.sr, (unsigned long long)runtime.scheduler.master_ticks, '
               '(unsigned)runtime.devices.interrupt.vblank_pending);')
    generated = generated.replace("runtime.divide_by_zero_handler_present = 1; ",
                                  "runtime.divide_by_zero_handler_present = 1; runtime.sr = 0xA300; runtime.d[3] = 0x12345678; ", 1)
    anchor = re.search(r"result = genesis_runtime_run\([^;]*\);", generated)
    assert anchor is not None
    generated = generated[:anchor.end()] + observe + generated[anchor.end():]
    with tempfile.TemporaryDirectory() as temp:
        temp = pathlib.Path(temp)
        source = temp / "generated.c"
        source.write_text(generated)
        binary = temp / "generated"
        env = os.environ.copy()
        if not env.get("SDKROOT") and shutil.which("xcrun"):
            env["SDKROOT"] = subprocess.run(["xcrun", "--show-sdk-path"], text=True,
                                              capture_output=True, check=True).stdout.strip()
        build = subprocess.run([compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic",
                                "-I", str(pathlib.Path(root) / "platforms/genesis/runtime"), str(source),
                                str(pathlib.Path(root) / "platforms/genesis/runtime/runtime.c"), "-o", str(binary)],
                               text=True, capture_output=True, env=env)
        assert build.returncode == 0, build.stderr
        run = subprocess.run([binary], text=True, capture_output=True)
        assert run.returncode == 0, run.stderr
        # Synchronous divide-by-zero does not advance virtual video time;
        # vector-5 itself neither consumes pending state nor resets it.
        assert "d3=305419896 d7=85 pc=258 sr=41728 tick=238 pending=0" in run.stderr, run.stderr

        # This separate synthetic dispatcher has no vector-5 table entry. It
        # proves emitted DIV lowering reaches the runtime helper: its typed
        # fail-closed divide-by-zero result must leave the generated dispatcher
        # rather than becoming a zero-initialized stop.
        no_handler_generated = subprocess.run(
            [executable, "--emit-operation-c4-divs-word"], text=True,
            capture_output=True, check=True).stdout
        no_handler_harness = r'''
int main(void) {
  GenesisRuntime runtime = {0};
  runtime.a[7] = UINT32_C(0x00FF8000);
  runtime.pc = UINT32_C(0x00000B00);
  runtime.sr = UINT16_C(0x2000);
  const GenesisControlTransfer result =
      genesis_runtime_run(&runtime, genesis_bridge_dispatch, UINT32_C(4));
  return result.kind == GENESIS_STOP &&
                 result.stop.stop_class == GENESIS_STOP_UNSUPPORTED_CPU_FORM &&
                 result.stop.diagnostic_category == GENESIS_DIAG_UNSUPPORTED_DIVIDE_BY_ZERO_EXCEPTION
             ? 0
             : 1;
}
'''
        no_handler_source = temp / "no_handler_generated.c"
        no_handler_source.write_text(no_handler_generated + no_handler_harness)
        no_handler_binary = temp / "no_handler_generated"
        no_handler_build = subprocess.run(
            [compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic",
             "-I", str(pathlib.Path(root) / "platforms/genesis/runtime"), str(no_handler_source),
             str(pathlib.Path(root) / "platforms/genesis/runtime/runtime.c"), "-o", str(no_handler_binary)],
            text=True, capture_output=True, env=env)
        assert no_handler_build.returncode == 0, no_handler_build.stderr
        no_handler_run = subprocess.run([no_handler_binary], text=True, capture_output=True)
        assert no_handler_run.returncode == 0, no_handler_run.stderr
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

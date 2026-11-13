#!/usr/bin/env python3
"""SEG-138-T003/T004 (ADR 0298): every rejected hook-site class has a negative BUILD test, and the supported classes build.

Each case is a project-authored synthetic image (vector table + code at $200) hooked at one site; the generator must refuse the build with the stable class and the failing site index, and a
supported site must build. Classes (mod-execution-phases.md 5.2): E_HOOK_MID_INSTRUCTION, E_HOOK_NOT_FUNCTION_ENTRY, E_HOOK_UNCLASSIFIED_RETURN, E_HOOK_COMPUTED_TARGET, E_HOOK_OVERLAP,
E_HOOK_MUTABLE_CODE, E_HOOK_NOT_COMPILED, E_HOOK_INTERRUPT_CONTEXT, E_HOOK_REPLACE_CONTRACT, E_HOOK_CPU_UNSUPPORTED (+ E_HOOK_ID, E_HOOK_COUNT). E_HOOK_DIGEST and E_HOOK_OVERLAY_UNRESOLVED are
declaration/identity-level classes tested through the CLI in mod_hooks_test.py.
usage: genesis_function_hook_reject_test.py <m68k_pipeline_tests>"""
import subprocess
import sys

emitter = sys.argv[1]


def build(parts, base=0x200):
    img = bytearray()
    for addr, hexs in parts:
        off = addr - base
        assert off >= len(img), hex(addr)
        img += b"\x4e\x71" * ((off - len(img)) // 2)
        img += bytes.fromhex(hexs)
    return img.hex().upper()


def attempt(parts, hooks, vectors="-"):
    run = subprocess.run([emitter, "--emit-synthetic-hooked-bridge", hooks, vectors, build(parts)], text=True, capture_output=True)
    if run.returncode == 0:
        return "OK", None
    assert run.returncode == 3, (hooks, run.returncode, run.stdout[:200], run.stderr[:200])
    word, cls, site = run.stdout.split()
    assert word == "REJECTED"
    return cls, int(site)


JSR_A, JSR_B = "4EB900000220", "4EB900000240"
MAIN = [(0x200, JSR_A + JSR_B + "66F2" + "4E70"), (0x220, "5280" + "4E75"), (0x240, JSR_A + "5281" + "4E75")]
TAIL = [(0x200, JSR_A + "66F6" + "4E70"), (0x220, "5280" + "4EF900000240"), (0x240, "4E75")]            # A ends in JMP $240 (a tail jump)
RTE = [(0x200, JSR_A + "66F6" + "4E70"), (0x220, "5280" + "4E73")]                                      # A returns with RTE
TABLE = [(0x200, JSR_A + "66F6" + "4E70"), (0x220, "5280" + "4EFB0002" + "4E75")]                       # A ends in JMP (d8,PC,Xn): a jump table
PUSH = [(0x200, JSR_A + "66F6" + "4E70"), (0x220, "487A0004" + "4E75" + "4E75")]                        # A pushes a code address (PEA d16(PC)) then RTS: a computed return
STACK = [(0x200, JSR_A + "66F6" + "4E70"), (0x220, "2E7C00FF8000" + "4E75")]                            # A replaces the stack pointer (MOVEA.L #imm,A7)
COMPUTED = [(0x200, JSR_A + "2050" + "4E90" + "4E70"), (0x220, "5280" + "4E75"), (0x260, "5281" + "4E75")]   # $260 is never called directly; the program has JSR (A0)
IRQ = [(0x200, JSR_A + "66F6" + "4E70"), (0x220, "5280" + "4E75"), (0x280, JSR_A + "4E73")]            # the level-6 handler ($280) also calls A
BRANCH = [(0x200, JSR_A + "6600" + "0018" + "4E70"), (0x220, "5280" + "4E75")]                          # A is also the target of a conditional branch
SELF = [(0x200, JSR_A + "66F6" + "4E70"), (0x220, "4E71" + "60FE")]                                            # A is BRA.S to itself
CLOSED = [(0x200, JSR_A + "66F6" + "4E70"), (0x220, "4E71" + "5280" + "66FC" + "60FA")]                      # a closed conditional graph, no RTS
LOOPRTS = [(0x200, JSR_A + "66F6" + "4E70"), (0x220, "4E71" + "5280" + "66FC" + "4E75")]                    # a loop with a reachable RTS
BSR = [(0x200, "6100001E" + "66FA" + "4E70"), (0x220, "5280" + "4E75")]

CASES = [
    ("function branching to itself, no RTS", SELF, "a:220:before", "-", "E_HOOK_UNCLASSIFIED_RETURN", 0),
    ("closed conditional graph, no RTS", CLOSED, "a:220:before", "-", "E_HOOK_UNCLASSIFIED_RETURN", 0),
    ("supported: loop with a reachable RTS", LOOPRTS, "a:220:before", "-", "OK", None),
    # (what, parts, hooks, vectors, expected class, expected site)
    ("supported: JSR-called subroutine, before", MAIN, "a:220:before", "-", "OK", None),
    ("supported: nested callee, after", MAIN, "b:240:after:3", "-", "OK", None),
    ("supported: two sites, before-after and a replacement of the leaf", MAIN, "a:220:replace:1:10001:24:1;b:240:both:3", "-", "OK", None),   # B (hooked) calls the replaced leaf A: allowed, A is not B's hook-owner
    ("supported: BSR-called subroutine", BSR, "a:220:before", "-", "OK", None),
    ("mid-instruction (inside the JSR extension word)", MAIN, "a:202:before", "-", "E_HOOK_MID_INSTRUCTION", 0),
    ("not compiled (outside the image)", MAIN, "a:2000:before", "-", "E_HOOK_NOT_COMPILED", 0),
    ("the reset entry is not a call entry", MAIN, "a:200:before", "-", "E_HOOK_NOT_FUNCTION_ENTRY", 0),
    ("the instruction after a call is not a function entry", MAIN, "a:206:before", "-", "E_HOOK_NOT_FUNCTION_ENTRY", 0),
    ("a branch target is not only a call entry", BRANCH, "a:220:before", "-", "E_HOOK_NOT_FUNCTION_ENTRY", 0),
    ("function with a tail jump", TAIL, "a:220:before", "-", "E_HOOK_UNCLASSIFIED_RETURN", 0),
    ("function returning with RTE", RTE, "a:220:before", "-", "E_HOOK_UNCLASSIFIED_RETURN", 0),
    ("function ending in a jump table", TABLE, "a:220:before", "-", "E_HOOK_UNCLASSIFIED_RETURN", 0),
    ("function pushing a code address (computed return)", PUSH, "a:220:before", "-", "E_HOOK_UNCLASSIFIED_RETURN", 0),
    ("function replacing the stack pointer", STACK, "a:220:before", "-", "E_HOOK_UNCLASSIFIED_RETURN", 0),
    ("computed target: only reachable through JSR (An)", COMPUTED, "c:260:before", "-", "E_HOOK_COMPUTED_TARGET", 0),
    ("work RAM is mutable code", MAIN, "a:ff0400:before", "-", "E_HOOK_MUTABLE_CODE", 0),
    ("a function called from an interrupt handler", IRQ, "a:220:before", "78:280", "E_HOOK_INTERRUPT_CONTEXT", 0),
    ("an interrupt vector target", IRQ, "h:280:before", "78:280", "E_HOOK_INTERRUPT_CONTEXT", 0),
    ("one site claimed twice", MAIN, "a:220:before;b:220:after", "-", "E_HOOK_OVERLAP", 1),
    ("a site reachable only through a replaced function", MAIN, "b:240:replace:1:10001:48:1;a:220:before", "-", "E_HOOK_OVERLAP", 1),
    ("replace without a contract", MAIN, "a:220:replace:1:1:24:0:0", "-", "E_HOOK_REPLACE_CONTRACT", 0),
    ("replace with a cost of zero", MAIN, "a:220:replace:1:1:0:0", "-", "E_HOOK_REPLACE_CONTRACT", 0),
    ("replace flags effect disagrees with the writes", MAIN, "a:220:replace:1:1:24:1", "-", "E_HOOK_REPLACE_CONTRACT", 0),
    ("replace writing A7", MAIN, "a:220:replace:1:8000:24:0", "-", "E_HOOK_REPLACE_CONTRACT", 0),
    ("a non-replace hook with writes", MAIN, "a:220:before:1:1", "-", "E_HOOK_REPLACE_CONTRACT", 0),
    ("a register-view slot that does not exist", MAIN, "a:220:before:40000", "-", "E_HOOK_REPLACE_CONTRACT", 0),
    ("the Z80 has no succeeded spike", MAIN, "a:220:before:1:0:0:0:1:z80", "-", "E_HOOK_CPU_UNSUPPORTED", 0),
    ("a bad id", MAIN, "A!:220:before", "-", "E_HOOK_ID", 0),
]
many = ";".join(f"h{i:02d}:{0x220 + 2 * i:x}:before" for i in range(65))
CASES.append(("more than 64 sites", MAIN, many, "-", "E_HOOK_COUNT", 0))

failures = []
for what, parts, hooks, vectors, want_class, want_site in CASES:
    got = attempt(parts, hooks, vectors)
    if got != (want_class, want_site):
        failures.append(f"{what}: expected {(want_class, want_site)}, got {got}")
# deterministic classification: the same input refuses the same way
assert attempt(TAIL, "a:220:before") == attempt(TAIL, "a:220:before")
# a rejected build produces no program (the emitter wrote nothing but the rejection)
run = subprocess.run([emitter, "--emit-synthetic-hooked-bridge", "a:220:before", "-", build(TAIL)], text=True, capture_output=True)
assert run.returncode == 3 and "genesis_dispatch" not in run.stdout
if failures:
    raise SystemExit("\n".join(failures))
print(f"genesis_function_hook_reject_test: OK ({len(CASES)} cases)")

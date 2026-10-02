#!/usr/bin/env python3
"""SEG-032-T002 (ADR 0073): the activation-signature proof on the REAL Genesis runtime router.

A raw 68K access script (rendered from the same backend-neutral operations as the `multi_epoch_dirty` fixture ROM) is
replayed through genesis_route_access with the generic Z80 image-epoch observer installed. Five epochs occur:
  1 X over a clean mailbox   2 X again, mailbox dirty (A)   3 Y (different code), mailbox dirty (A)
  4 X again, mailbox dirty (B)   5 X' (one code byte changed), mailbox dirty (B)
  6 a plain Z80 restart (reset pulse, no upload): the hold window is empty, so the epoch re-binds the image bound before it
Proved here:
  * the activation signature S1* (the 68K-written extents of the hold window) selects X for epochs 1, 2 and 4 (identical
    despite different carry-over data and different power-on fills), Y for 3 and X' for 5 (three classes);
  * the content hash differs for every epoch with different carry-over data and for every fill (it names images, it is
    not selection authority): a whole-RAM identity would build five images instead of three;
  * the runtime's hold-window bitmap equals an independently written model of the observer state machine;
  * negative controls: the discarded definitions (whole RAM; writes since the last /RESET assertion) are detected as
    insufficient by the same assertions (collisions / wrong class counts).
usage: genesis_z80_epoch_signature_test.py <cc> <source-root>
"""
import pathlib
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(sys.argv[2]) if len(sys.argv) > 2 else pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))
import genesis_z80_fixture_rom as fx  # noqa: E402
import genesis_z80_materialization_probe as probe  # noqa: E402


def model(script_lines, definition="hold_window"):
    """Independent model of the observer (contract section 5): returns [(written set)] per epoch."""
    reset, busreq, pristine, written, epochs = True, False, True, set(), []
    for line in script_lines:
        kind, address, value = line.split()
        address, value = int(address, 16), int(value, 16)
        if kind == "w8":
            written.add(address - fx.Z80_RAM)
            continue
        was_runnable = not reset and not busreq
        if address == fx.Z80_BUSREQ:
            busreq = bool(value & 0x100)
        else:
            asserted = not (value & 0x100)
            if definition == "since_assertion" and asserted and not reset:
                written = set()
            if not asserted and reset:
                pristine = True
            reset = asserted
        runnable = not reset and not busreq
        if runnable and not was_runnable:
            epoch_fired = False
            if pristine:
                pristine = False
                epoch_fired = True
                epochs.append(set(written))
            if definition == "hold_window":
                written = set()
            elif definition == "epoch_only" and epoch_fired:
                written = set()
    return epochs


def bitmap_set(bitmap):
    return {o for o in range(probe.RAM_BYTES) if (bitmap[o >> 3] >> (o & 7)) & 1}


def run_harness(binary, script, fill):
    out = subprocess.run([str(binary), str(script), "%02X" % fill], text=True, capture_output=True, check=True).stdout
    epochs = []
    for line in out.splitlines():
        if line.startswith("EPOCH "):
            _, ordinal, ram, written = line.split()
            epochs.append((bytes.fromhex(ram), bytes.fromhex(written)))
        elif line.startswith("END "):
            assert int(line.split()[1]) == len(epochs)
    return epochs


def classes(items):
    seen, out = {}, []
    for item in items:
        out.append(seen.setdefault(item, len(seen)))
    return out


def main():
    cc = sys.argv[1] if len(sys.argv) > 1 else "cc"
    programs = dict(zip(("x", "x2", "y"), fx.multi_epoch_programs()))
    script_text = fx.access_script(fx.multi_epoch_ops(), programs)
    lines = script_text.splitlines()
    failures = []

    def check(ok, label):
        print("%-5s %s" % ("ok" if ok else "FAIL", label))
        if not ok:
            failures.append(label)

    with tempfile.TemporaryDirectory() as tmp:
        tmp = pathlib.Path(tmp)
        binary = tmp / "harness"
        subprocess.run([cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic", "-I", str(ROOT / "platforms/genesis/runtime"),
                        str(ROOT / "tests/tools/genesis_z80_epoch_script_harness.c"),
                        str(ROOT / "platforms/genesis/runtime/runtime.c"), "-o", str(binary)], check=True)
        script = tmp / "script.txt"
        script.write_text(script_text)
        runs = {fill: run_harness(binary, script, fill) for fill in (0x00, 0xFF, 0xA5)}
        base = runs[0x00]
        check(all(len(r) == 6 for r in runs.values()), "six epochs under every power-on fill")
        expected = [bitmap for bitmap in model(lines)]
        check(len(expected) == 6 and all(bitmap_set(base[i][1]) == expected[i] for i in range(6)),
              "runtime hold-window bitmap equals the independent model for every epoch")
        sigs = {fill: probe.effective_signatures(eps) for fill, eps in runs.items()}
        check(classes(sigs[0x00]) == [0, 0, 1, 0, 2, 2], "S1* classes X X Y X X' X' (three images; the empty hold window re-binds the previous image)")
        check(not probe.extents(base[5][1]), "epoch 6 has an empty hold window")
        check(all(sigs[f] == sigs[0x00] for f in sigs), "S1* is identical under every power-on fill")
        content = {fill: [probe.content_hash(r) for r, _ in eps] for fill, eps in runs.items()}
        check(classes(content[0x00]) == [0, 1, 2, 3, 4, 4], "content hash differs for every epoch that changed RAM (carry-over data, X' code); a plain restart changes nothing")
        check(all(content[f][0] != content[0x00][0] for f in content if f != 0x00), "content hash differs per power-on fill")
        check(len({r[:96] for r, _ in base[:1] + base[1:2] + base[3:4]}) == 1, "epochs 1, 2, 4 upload identical code bytes")
        # carry-over is outside the signature: mailbox bytes differ while the signature does not
        check(base[0][0][fx.DIRTY_BASE:fx.DIRTY_BASE + 4] != base[1][0][fx.DIRTY_BASE:fx.DIRTY_BASE + 4]
              and sigs[0x00][0] == sigs[0x00][1], "dirty mailbox differs between epochs 1 and 2, signature does not")
        # sensitivity: one changed code byte (X') and different code (Y) change the signature
        check(sigs[0x00][4] != sigs[0x00][3] and sigs[0x00][2] != sigs[0x00][1],
              "a one-byte code change and different code change the signature")
        # negative controls: the discarded definitions fail the same assertions
        whole = [probe.content_hash(r) for r, _ in base]
        check(classes(whole) != [0, 0, 1, 0, 2, 2], "control: whole-RAM identity is NOT insensitive to carry-over data (rejected)")
        since = model(lines, "since_assertion")
        collide = classes([frozenset(s) for s in since])
        check(since and all(not s for s in since) and len(set(collide)) == 1,
              "control: 'writes since the last /RESET assertion' is empty for every epoch, so X, Y and X' collide (rejected)")
        # control: clearing the window only at an epoch (not at a plain resume) leaks earlier holds' command writes
        def sig_from_sets(sets):
            out = []
            for (ram, _), positions in zip(base, sets):
                bm = bytearray(probe.RAM_BYTES // 8)
                for o in positions:
                    bm[o >> 3] |= 1 << (o & 7)
                out.append(probe.signature(ram, bytes(bm)))
            return out
        leaked = sig_from_sets(model(lines, "epoch_only"))
        check(classes(leaked) != classes(sigs[0x00]),
              "control: clearing only at an epoch (not at a resume) leaks command writes into later signatures (rejected)")
    print("genesis z80 epoch signature: %s" % ("FAILED (%d)" % len(failures) if failures else "ok"))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())

"""SEG-009-T008: shared helpers of the end-to-end Master System machine fixture tests (sms_machine_e2e_test.py,
sms_machine_e2e_oracle_test.py).

Everything here that predicts a result is written from the machine contract and the published Z80 T-states (and the
independent models tests/sms_vdp_model.py, sms_render_model.py, sms_psg_model.py), never from the C runtime:
  * the guest-visible results of the `machine_e2e` fixture (tools/sms_fixture_rom.py documents the RAM layout);
  * the interrupt trace (flag/counter/enable model driven only by the register writes and status reads of the VDP trace);
  * the mapper trace, the VDP register-write sequence, the VRAM/CRAM image, the per-frame framebuffers (render model);
  * the PSG write timeline (acknowledge T-state + the statically known T-states of the handler path) and its PCM.
"""
import hashlib
import json
import pathlib
import re
import struct
import sys

HERE = pathlib.Path(__file__).resolve().parent
ROOT = HERE.parent
sys.path.insert(0, str(ROOT / "tools"))
sys.path.insert(0, str(HERE))
import sms_fixture_rom as builder  # noqa: E402
import sms_psg_model as psg_model  # noqa: E402
import sms_render_model as render_model  # noqa: E402
import sms_vdp_model as vdp_model  # noqa: E402
from sms_vdp_native import Native  # noqa: E402,F401

LINE, FRAME = 228, 59736
FIXTURE = "machine_e2e"
MANIFEST = ROOT / "tests" / "fixtures" / "sms-e2e-validation.json"
RESET_REGS = [0x36, 0x80, 0xFF, 0xFF, 0xFF, 0xFF, 0xFB, 0, 0, 0, 0xFF]
FIRST_DISPLAY_FRAME_MIN = 4  # the uploads precede the display enable, which lands in frame 4 at the earliest


def sha256(data):
    return hashlib.sha256(data).hexdigest()


# ---- scripted input ------------------------------------------------------------------------------------------------

def script_events():
    events = []
    for line in builder.E2E_SCRIPT.splitlines():
        if not line.strip() or line.startswith("#"):
            continue
        frame, p1, p2, pause = line.split()
        mask = lambda t: sum(1 << i for i, (c, l) in enumerate(zip(t, "UDLR12")) if c == l)  # noqa: E731
        events.append((int(frame), mask(p1), mask(p2), pause == "P"))
    return events


def input_state(frame):
    state = (0, 0, False)
    for f, p1, p2, pause in script_events():
        if f <= frame:
            state = (p1, p2, pause)
    return state


def pause_edges():
    edges, prev = [], False
    for f, _, _, pause in script_events():
        if pause and not prev:
            edges.append(f)
        prev = pause
    return edges


def ref_ports(p1, p2, ctrl):
    """($DC, $DD) from named pins (contract section 11); p1/p2 are pressed masks over U D L R 1 2."""
    def released(mask, bit):
        return not (mask >> bit) & 1
    a = {"up": released(p1, 0), "down": released(p1, 1), "left": released(p1, 2), "right": released(p1, 3),
         "tl": released(p1, 4), "tr": released(p1, 5), "th": True}
    b = {"up": released(p2, 0), "down": released(p2, 1), "left": released(p2, 2), "right": released(p2, 3),
         "tl": released(p2, 4), "tr": released(p2, 5), "th": True}
    for pin_dir_bit, level_bit, port, pin in ((0, 4, a, "tr"), (1, 5, a, "th"), (2, 6, b, "tr"), (3, 7, b, "th")):
        if not (ctrl >> pin_dir_bit) & 1:
            port[pin] = bool((ctrl >> level_bit) & 1)
    dc = [a["up"], a["down"], a["left"], a["right"], a["tl"], a["tr"], b["up"], b["down"]]
    dd = [b["left"], b["right"], b["tl"], b["tr"], True, True, a["th"], b["th"]]
    return (sum(1 << i for i, x in enumerate(dc) if x), sum(1 << i for i, x in enumerate(dd) if x))


# ---- native build and run --------------------------------------------------------------------------------------------

def build_native(emitter, cc, root, tmp):
    tmp = pathlib.Path(tmp)
    roms = tmp / "roms"
    builder.main(["--out", str(roms), "--fixture", FIXTURE])
    (tmp / "input.txt").write_text(builder.E2E_SCRIPT, encoding="utf-8")
    native = Native(emitter, cc, root, tmp)
    exe = native.build(roms, FIXTURE)
    return native, exe


def run_native(native, exe, tag, *extra):
    res = native.execute(exe, tag, "--frames", str(builder.E2E_FRAMES), "--input", str(native.tmp / "input.txt"), *extra)
    if res["code"] != 0:
        return res
    d = res["dir"]
    res["irq"] = [(int(a[0]), a[1], a[2]) for a in res["irq"]]
    res["mapper"] = [(int(a[0]), int(a[1], 16), int(a[2], 16)) for a in (l.split() for l in (d / "mapper.trace").read_text().splitlines())]
    res["vdp_trace"] = [(int(a[0]), a[1], int(a[2]), int(a[3], 16), int(a[4], 16)) for a in res["vdp_trace"]]
    res["frames"] = [(int(a[0]), int(a[1]), int(a[2]), a[3]) for a in (l.split() for l in (d / "frames.txt").read_text().splitlines())]
    res["pcm"] = (d / "audio.pcm").read_bytes()
    res["audio_lines"] = (d / "audio.sha256").read_text().splitlines()
    res["fb_last"] = (d / "framebuffer.bin").read_bytes()
    return res


ARTIFACTS = ("status.json", "state.sha256", "irq.trace", "mapper.trace", "ram.bin", "vdp.trace", "vram.bin", "cram.bin", "vdp.json",
             "frames.txt", "framebuffer.bin", "frame.json", "audio.pcm", "audio.sha256")


def artifact_digests(res):
    """SHA-256 of every artifact file plus the headline numbers: the content of the committed validation manifest."""
    d = res["dir"]
    out = {name: sha256((d / name).read_bytes()) for name in ARTIFACTS}
    frame_hashes = [f[3] for f in res["frames"]]
    return {
        "artifact_sha256": out,
        "state_digest": res["digest"],
        "cycles": res["status"]["cycles"],
        "frames_completed": res["status"]["frames_completed"],
        "irq_trace_entries": len(res["irq"]),
        "mapper_trace_entries": len(res["mapper"]),
        "vdp_trace_entries": len(res["vdp_trace"]),
        "frame_record_hashes_sha256": sha256("\n".join(frame_hashes).encode()),
        "pcm_samples": len(res["pcm"]) // 2,
        "pcm_sha256": sha256(res["pcm"]),
    }


# ---- guest-visible results -------------------------------------------------------------------------------------------

INIT_BLOCK = {0x100: 0x5A, 0x101: 0x3C, 0x102: 0xA7, 0x103: 0xB1, 0x104: 0xB2, 0x105: 0xB3, 0x106: 0xB5, 0x107: 0xB2, 0x108: 0xB5,
              0x109: 0x11, 0x10A: 0x33, 0x10B: 0x66, 0x10C: 0x11, 0x10D: 0x01, 0x10E: 0xFF, 0x10F: 0xFF, 0x110: 0xC0, 0x111: 0x00}
# bank-switch, mirror and port expectations: SMS Power "Sega mapper" (power-on banks 1 and 2, value masked to the bank count),
# MacDonald (RAM mirrored at $E000, reads of ports $00-$3F return $FF, I/O control readback of the export console).


def e2e_data():
    return builder.e2e_data()


def expected_vram():
    data = e2e_data()
    vram = bytearray(0x4000)
    vram[0:len(data["tiles"])] = data["tiles"]
    vram[0x3800:0x3800 + len(data["nt"])] = data["nt"]
    vram[0x3F00:0x3F00 + len(data["sat"])] = data["sat"]
    return bytes(vram)


def expected_cram():
    return bytes(b & 0x3F for b in e2e_data()["cram"])


def expected_readback():
    vram = expected_vram()[:0x800]
    return sum(vram) & 0xFF, _xor(vram)


def _xor(data):
    out = 0
    for b in data:
        out ^= b
    return out


def accepted_frame_times(irq):
    return [t for t, src, ev in irq if src == "frame" and ev == "accepted"]


def guest_frames(ram_or_log):
    """Split the frame log of a RAM image ($C200 + 16 n) into records while the counter field continues 0, 1, 2 ..."""
    records, n = [], 0
    while 0x200 + 16 * (n + 1) <= len(ram_or_log):
        rec = ram_or_log[0x200 + 16 * n:0x210 + 16 * n]
        if rec[0] != n & 0xFF or (n > 0 and rec[1] != 0xC1):
            break
        records.append(rec)
        n += 1
    return records


def expected_record(n, absolute_frame, shift=0):
    """Expected frame log record n observed at absolute frame `absolute_frame` (input shifted by `shift` frames)."""
    p1, p2, _ = input_state(absolute_frame - shift)
    dc, dd = ref_ports(p1, p2, 0xFF)
    pauses = sum(1 for f in pause_edges() if f <= absolute_frame - shift)
    lines = vdp_model.line_interrupt_lines(builder.E2E_R10)
    rec = [n & 0xFF, 0xC1, 0xE0, dc, dd, len(lines)] + lines + [0] * (4 - len(lines))
    rec += [pauses, 0xB0 + 4 + (n & 3), 0xB0 + 2 + (n & 1)]
    return rec


# ---- PSG timeline ----------------------------------------------------------------------------------------------------

T_TABLE = [(r"^push ", 11), (r"^pop ", 10), (r"^in a,\(0x", 11), (r"^ld [abcdehl],[abcdehl]$", 4), (r"^ld [abcdehl],(0x[0-9a-f]+|\d+)$", 7),
           (r"^ld [abcdehl],\(hl\)$", 7), (r"^ld \(hl\),[abcdehl]$", 7), (r"^and 0x", 7), (r"^or a$", 4), (r"^jp ", 10),
           (r"^ld a,\(0x", 13), (r"^add a,a$", 4), (r"^ld hl,", 10), (r"^add hl,de$", 11), (r"^inc hl$", 6), (r"^out \(0x", 11),
           (r"^jr z,", None)]


def _t(text, taken):
    text = text.strip().lower()
    for pattern, t in T_TABLE:
        if re.match(pattern, text):
            return (12 if taken else 7) if t is None else t
    raise ValueError("no T-state for %r" % text)


def _strip_label(line):
    m = re.match(r"^[A-Za-z_][A-Za-z_0-9]*:\s*(.*)$", line)
    return m.group(1) if m else line


def psg_offsets():
    """{(first written, second written): [T-state of each `out` from the acknowledge]} for the frame handler path: each
    instruction's published T-states walked over the assembler source of the fixture from `irq:` to the PSG writes
    (`jp z,line_irq` not taken on the frame path; `jr z` taken when that byte is zero)."""
    lines = [l.split(";")[0].strip() for l in builder.e2e_source().splitlines()]
    body = [_strip_label(l).lower() for l in lines[lines.index("irq:    push af"):lines.index("psg_s2:")]]
    body = [l for l in body if l]
    offsets = {}
    for first_written, second_written in ((True, True), (True, False), (False, True)):
        t, outs, jr_seen = 13 + 10, [], 0   # IM1 response, then `jp irq` at $0038
        for text in body:
            if text.startswith("jr z,"):
                written = (first_written, second_written)[jr_seen]
                jr_seen += 1
                t += _t(text, not written)
            elif text.startswith("out (0x7f)"):
                outs.append(t)
                t += 11
            else:
                t += _t(text, False)
        offsets[(first_written, second_written)] = outs
    return offsets


def psg_writes(irq):
    """[(T, byte)] of every PSG write of the program: acknowledge time of the frame interrupt of guest frame n plus the
    static offsets, bytes from the fixture's table (latch/data per frame, zero = no write)."""
    offsets = psg_offsets()
    writes = []
    for n, acc in enumerate(accepted_frame_times(irq)):
        b1, b2 = builder.E2E_PSG.get(n, (0, 0))
        case = (b1 != 0, b2 != 0)
        if not any(case):
            continue
        outs = offsets[case]
        bytes_ = [b for b in (b1, b2) if b]
        for t, b in zip(outs, bytes_):
            writes.append((acc + t, b))
    return writes


def expected_pcm(irq, end):
    return psg_model.pcm_for(psg_writes(irq), end)


def pcm_bytes(samples):
    return b"".join(struct.pack("<h", s) for s in samples)


# ---- interrupt trace -------------------------------------------------------------------------------------------------

def expected_irq_edges(vdp_trace, frames_total):
    """Edges (T, source, event) of the frame and line interrupts from the contract flag/counter model, driven by the
    register writes and status reads the program performed (their times come from the VDP trace)."""
    accesses = []
    for t, kind, arg, byte, _ in vdp_trace:
        if kind == "reg" and arg in (0, 1, 10):
            accesses.append((t, "reg", arg, byte))
        elif kind == "status":
            accesses.append((t, "status", 0, 0))
    accesses.sort(key=lambda a: a[0])
    regs = list(RESET_REGS)
    counter, frame_pending, line_pending = 0xFF, 0, 0
    level = (0, 0)
    edges = []

    def sample(t):
        nonlocal level
        now = (int(bool(frame_pending and regs[1] & 0x20)), int(bool(line_pending and regs[0] & 0x10)))
        for index, source in enumerate(("frame", "line")):
            if now[index] != level[index]:
                edges.append((t, source, "asserted" if now[index] else "deasserted"))
        level = now

    pos = 0
    total_lines = frames_total * 262
    for abs_line in range(total_lines):
        t_line = abs_line * LINE
        while pos < len(accesses) and accesses[pos][0] < t_line:  # accesses strictly before the event are applied first
            t, kind, arg, value = accesses[pos]
            if kind == "reg":
                regs[arg] = value
            else:
                frame_pending = line_pending = 0
            sample(t)
            pos += 1
        line = abs_line % 262
        if line <= 192:
            if counter == 0:
                counter, line_pending = regs[10], 1
            else:
                counter -= 1
        else:
            counter = regs[10]
        if line == 193:
            frame_pending = 1
        sample(t_line)
    while pos < len(accesses):
        t, kind, arg, value = accesses[pos]
        if kind == "reg":
            regs[arg] = value
        else:
            frame_pending = line_pending = 0
        sample(t)
        pos += 1
    return edges


def pause_edges_trace():
    return [(f * FRAME, "pause", "asserted") for f in pause_edges()]


# ---- mapper trace / VDP register sequence --------------------------------------------------------------------------------

def expected_mapper(frames_serviced):
    seq = [(0xFFFE, 3), (0xFFFF, 5), (0xFFFF, 10), (0xFFFE, 13), (0xFFFE, 1), (0xFFFE, 3), (0xFFFE, 6), (0xFFFE, 1),
           (0xFFFE, 2), (0xFFFE, 3), (0xFFFF, 4), (0xFFFF, 4)]
    for n in range(frames_serviced):
        seq += [(0xFFFF, 4 + (n & 3)), (0xFFFE, 2 + (n & 1))]
    return seq


def expected_reg_sequence(frames_serviced):
    seq = [(0, 0x16), (2, 0x0E), (5, 0xFF), (6, 0xFB), (7, 0x03), (8, 0), (9, 0), (10, builder.E2E_R10), (1, 0xE0)]
    lines = vdp_model.line_interrupt_lines(builder.E2E_R10)
    for n in range(frames_serviced):
        seq += [(8, 9 * (k + 1) & 0xFF) for k in range(len(lines))]
        seq += [(8, 3 * (n + 1) & 0xFF), (9, 2 * (n + 1) & 0xFF)]
    return seq


# ---- framebuffers ----------------------------------------------------------------------------------------------------

def reg_writes(vdp_trace):
    return [(t, arg, byte) for t, kind, arg, byte, _ in vdp_trace if kind == "reg" and arg <= 10]


def regs_at_fn(writes, frame):
    def regs_at(line):
        t = frame * FRAME + line * LINE
        regs = list(RESET_REGS)
        for cycles, reg, value in writes:
            if cycles < t:
                regs[reg] = value
        return regs
    return regs_at


def model_frame(vram, cram, writes, frame, mutation=None):
    height, rows, overflow, collision = render_model.render(vram, cram, regs_at_fn(writes, frame), mutation)
    return height, bytes(v for row in rows for v in row), overflow, collision


def display_enable_time(writes):
    return next(c for c, r, v in writes if r == 1 and v & 0x40)


def gap_mask(writes, frame):
    """Pixels of the U8 fine-scroll gap: the machine contract keeps the backdrop there; the pinned references disagree with it
    (Gearsystem shows the wrapped background column, Genesis Plus GX a constant colour), so references are compared outside it."""
    regs_at = regs_at_fn(writes, frame)
    mask = set()
    for y in range(192):
        regs = regs_at(y)
        fine = 0 if (regs[0] & 0x40 and y < 16) else regs[8] & 7
        mask.update(256 * y + x for x in range(fine))
    return mask

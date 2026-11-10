#!/usr/bin/env python3
"""SEG-145-T002: declared audio assets and mappings in mod projects. Packaging validates the DECLARED format, size, rate, channels, frames, checksum and revision binding, applies the one
deterministic reference PCM conversion (`mod convert-audio`, target u8-mono) and refuses everything else. Synthetic generated audio only; no game format is interpreted.
usage: mod_audio_test.py <segarecomp-cli>"""
import hashlib
import os
import struct
import sys
import zipfile

from mod_test_support import *  # noqa: F401,F403

cli = Cli(sys.argv[1])
tmp = temp()
REV_B = sha(b"synthetic-source-image-B")


def wav(rate=8000, channels=1, frames=64, seed=1, header_patch=None):
    """Canonical 44-byte-header RIFF/WAVE PCM s16le of a deterministic synthetic signal."""
    samples = bytearray()
    x = seed
    for _ in range(frames * channels):
        x = (x * 1103515245 + 12345) & 0x7FFFFFFF
        samples += struct.pack("<h", (x >> 8) % 65536 - 32768)
    block = channels * 2
    head = b"RIFF" + struct.pack("<I", 36 + len(samples)) + b"WAVE" + b"fmt " + struct.pack("<IHHIIHH", 16, 1, channels, rate, rate * block, block, 16) + b"data" + struct.pack("<I", len(samples))
    out = bytearray(head + bytes(samples))
    if header_patch:
        header_patch(out)
    return bytes(out)


KICK, THEME, SEQ, SCRIPT = wav(8000, 1, 80, 1), wav(22050, 2, 100, 2), b"\x10\x20\x30seq", b"-- audio script"

PROJECT = f'''[mod]
id = "audio-mod"
version = "1.0.0"
name = "Audio"
author = "T"
license = "CC0-1.0"

[target]
platform = "genesis"

[[target.revision]]
label = "rev-a"
sha256 = "{SRC_SHA}"

[[target.revision]]
label = "rev-b"
sha256 = "{sha(b'synthetic-source-image-B')}"

[capabilities]
required = ["audio.event.read", "audio.command.send"]
optional = ["audio.host.play"]

[[source]]
kind = "luau-source"
path = "scripts/main.luau"

[[source]]
kind = "asset"
path = "assets/kick.wav"
asset_class = "audio"
audio_format = "wav-pcm-s16le"
sample_rate = 8000
channels = 1
frames = 80
loop_start = 10
loop_end = 70

[[source]]
kind = "asset"
path = "assets/theme.wav"
asset_class = "audio"
audio_format = "wav-pcm-s16le"
sample_rate = 22050
channels = 2
frames = 100

[[source]]
kind = "asset"
path = "assets/seq.bin"
asset_class = "data"
format = "seq-raw"

[[audio.mapping]]
id = "kick-bind"
kind = "asset-binding"
asset = "assets/kick.wav"
target = "dac-kick"
use = "pcm-sample"
revision = "rev-a"
offset = 4096
length = 80
bank = 1
track = 2

[[audio.mapping]]
id = "theme-bind"
kind = "asset-binding"
asset = "assets/theme.wav"
target = "ost-theme"
use = "enhanced-soundtrack"

[[audio.mapping]]
id = "seq-bind"
kind = "asset-binding"
asset = "assets/seq.bin"
target = "song-1"
use = "music-sequence"

[[audio.mapping]]
id = "cmd"
kind = "command"
space = "m68k.work_ram"
offset = 65280
width = 1

[[audio.mapping.value]]
name = "stop"
value = 0

[[audio.mapping.value]]
name = "play"
value = 1

[[audio.mapping]]
id = "evt"
kind = "event"
space = "m68k.work_ram"
offset = 65282
width = 2

[[audio.mapping.value]]
name = "jingle"
value = 300
'''
FILES = {"scripts/main.luau": SCRIPT, "assets/kick.wav": KICK, "assets/theme.wav": THEME, "assets/seq.bin": SEQ}


def project(name, toml=PROJECT, files=None):
    root = tmp / name
    write(root / "mod.toml", toml)
    for path, data in (FILES if files is None else files).items():
        write(root / path, data)
    return root


def check(what, root, code=None):
    rc, doc, p = cli.run("mod", "check", root)
    if code is None:
        cli.check(rc == 0 and doc and doc.get("ok") is True, f"{what}: expected ok, got {doc}")
    else:
        cli.check(rc == 1 and doc and doc.get("class") == code, f"{what}: expected {code}, got {doc and (doc.get('class'), doc.get('error'))}")
    return doc


def edit(name, old, new, files=None):
    assert old in PROJECT, old
    return project(name, PROJECT.replace(old, new, 1), files)


# ------------------------------------------------------------------ the declaration maps onto the schema 3 manifest exactly
ok = check("a full audio declaration", project("full"))
out = tmp / "full.segapatch"
rc, doc, _ = cli.run("mod", "build", tmp / "full", "--out", out)
cli.check(rc == 0, f"build the audio project ({doc})")
expected = {
    "format": "segarecomp-patch", "schema_version": 3, "id": "audio-mod", "version": "1.0.0", "name": "Audio", "platform": "genesis", "author": "T", "license": "CC0-1.0",
    "source": {"kind": "cartridge-image", "revisions": [{"label": "rev-a", "sha256": SRC_SHA}, {"label": "rev-b", "sha256": REV_B}]},
    "capabilities": ["audio.command.send", "audio.event.read"], "capabilities_optional": ["audio.host.play"],
    "mappings": [
        {"id": "kick-bind", "kind": "asset-binding", "asset": "assets/kick.wav", "target": "dac-kick", "use": "pcm-sample", "revision": "rev-a", "offset": 4096, "length": 80, "bank": 1, "track": 2},
        {"id": "theme-bind", "kind": "asset-binding", "asset": "assets/theme.wav", "target": "ost-theme", "use": "enhanced-soundtrack"},
        {"id": "seq-bind", "kind": "asset-binding", "asset": "assets/seq.bin", "target": "song-1", "use": "music-sequence"},
        {"id": "cmd", "kind": "command", "space": "m68k.work_ram", "offset": 65280, "width": 1, "commands": [{"name": "stop", "value": 0}, {"name": "play", "value": 1}]},
        {"id": "evt", "kind": "event", "space": "m68k.work_ram", "offset": 65282, "width": 2, "events": [{"name": "jingle", "value": 300}]},
    ],
    "payloads": [
        payload("asset", "assets/kick.wav", KICK, asset_class="audio", audio_format="wav-pcm-s16le", sample_rate=8000, channels=1, frames=80, loop_start=10, loop_end=70),
        payload("asset", "assets/seq.bin", SEQ, asset_class="data", format="seq-raw"),
        payload("asset", "assets/theme.wav", THEME, asset_class="audio", audio_format="wav-pcm-s16le", sample_rate=22050, channels=2, frames=100),
        payload("luau-source", "scripts/main.luau", SCRIPT, role="script"),
    ],
}
with zipfile.ZipFile(out) as z:
    cli.check(z.read("manifest.json") == canon(expected), f"the manifest equals the independent expectation:\n{z.read('manifest.json')}\n{canon(expected)}")
rc, doc, _ = cli.run("mod", "inspect", out)
cli.check(rc == 0 and doc["trust_class"] == "luau" and doc["payloads"]["assets"] == 3, f"the built package re-reads: {doc}")

# ------------------------------------------------------------------ rejections
LICENSE = 'license = "CC0-1.0"\n'
check("no license statement with audio assets", project("nolic", PROJECT.replace(LICENSE, "")), "E_FIELD_MISSING")
check("wrong sample rate", edit("rate", "sample_rate = 8000", "sample_rate = 11025"), "E_FIELD_VALUE")
check("sample rate below the range", edit("rate-lo", "sample_rate = 8000", "sample_rate = 4000"), "E_FIELD_VALUE")
check("sample rate above the range", edit("rate-hi", "sample_rate = 22050", "sample_rate = 96000"), "E_FIELD_VALUE")
check("wrong channel count", edit("chan", "channels = 1\nframes = 80", "channels = 2\nframes = 80"), "E_FIELD_VALUE")
check("three channels", edit("chan3", "channels = 1\nframes = 80", "channels = 3\nframes = 80"), "E_FIELD_VALUE")
check("wrong frame count", edit("frames", "frames = 80\nloop_start", "frames = 81\nloop_start"), "E_FIELD_VALUE")
check("zero frames", edit("frames0", "frames = 100", "frames = 0"), "E_FIELD_VALUE")
check("loop outside the frames", edit("loop", "loop_end = 70", "loop_end = 81"), "E_FIELD_VALUE")
check("loop start after end", edit("loop2", "loop_start = 10", "loop_start = 70"), "E_FIELD_VALUE")
check("loop_start without loop_end", edit("loop3", "loop_end = 70\n", ""), "E_FIELD_MISSING")
check("undeclared audio format", edit("fmt", 'audio_format = "wav-pcm-s16le"\nsample_rate = 8000', 'audio_format = "mp3"\nsample_rate = 8000'), "E_FIELD_VALUE")
check("unknown asset class", edit("class", 'asset_class = "data"\nformat = "seq-raw"', 'asset_class = "midi"\nformat = "seq-raw"'), "E_FIELD_VALUE")
check("data asset with an invalid format token", edit("token", 'format = "seq-raw"', 'format = "Seq Raw"'), "E_FIELD_VALUE")
check("a duration above the limit", edit("long", "frames = 100", "frames = 13230001"), "E_PAYLOAD_CAP")
check("a file that is not a canonical WAV", project("notwav", files={**FILES, "assets/kick.wav": b"RIFF" + b"\x00" * 60}), "E_FIELD_VALUE")
check("a WAV that is not 16-bit PCM", project("pcm8", files={**FILES, "assets/kick.wav": wav(8000, 1, 80, header_patch=lambda b: b.__setitem__(slice(34, 36), struct.pack("<H", 8)))}), "E_FIELD_VALUE")
check("a WAV whose data size lies", project("lie", files={**FILES, "assets/kick.wav": wav(8000, 1, 80, header_patch=lambda b: b.__setitem__(slice(40, 44), struct.pack("<I", 9999)))}), "E_FIELD_VALUE")
check("mismatched revision", edit("rev", 'revision = "rev-a"', 'revision = "rev-z"'), "E_SOURCE_DIGEST")
check("binding to an undeclared asset", edit("undeclared", 'asset = "assets/seq.bin"', 'asset = "assets/other.bin"'), "E_FIELD_VALUE")
check("a PCM use on a data asset", edit("useclass", 'asset = "assets/seq.bin"\ntarget = "song-1"\nuse = "music-sequence"', 'asset = "assets/seq.bin"\ntarget = "song-1"\nuse = "pcm-sample"'), "E_FIELD_VALUE")
check("an unknown use", edit("use", 'use = "music-sequence"', 'use = "karaoke"'), "E_FIELD_VALUE")
check("enhanced soundtrack without audio.host.play", edit("host", 'optional = ["audio.host.play"]', 'optional = []'), "E_CAPABILITY_INCONSISTENT")
check("a command mapping without audio.command.send", edit("nocmd", '"audio.event.read", "audio.command.send"', '"audio.event.read"'), "E_CAPABILITY_INCONSISTENT")
check("an event mapping without audio.event.read", edit("noevt", '"audio.event.read", "audio.command.send"', '"audio.command.send"'), "E_CAPABILITY_INCONSISTENT")
check("a command into a space that is not mod-writable", edit("ro", 'space = "m68k.work_ram"\noffset = 65280', 'space = "z80.ram"\noffset = 16'), "E_SPACE_READONLY")
check("an event space that does not exist on the platform", edit("sp", 'space = "m68k.work_ram"\noffset = 65282', 'space = "sub.prg_ram"\noffset = 65282'), "E_SPACE_UNKNOWN")
check("an unknown space name", edit("sp2", 'space = "m68k.work_ram"\noffset = 65280', 'space = "vram"\noffset = 65280'), "E_FIELD_VALUE")
check("a mailbox outside its space", edit("range", "offset = 65280", "offset = 65536"), "E_ACCESS_RANGE")
check("a misaligned 16-bit event on a big-endian space", edit("align", "offset = 65282", "offset = 65283"), "E_ACCESS_ALIGNMENT")
check("a value wider than the mailbox", edit("wide", 'name = "play"\nvalue = 1', 'name = "play"\nvalue = 256'), "E_FIELD_VALUE")
check("a width that is not 1, 2 or 4", edit("w3", "width = 1", "width = 3"), "E_FIELD_VALUE")
check("a repeated value name", edit("dupv", 'name = "play"', 'name = "stop"'), "E_FIELD_VALUE")
check("a repeated mapping id", edit("dupid", 'id = "evt"', 'id = "cmd"'), "E_FIELD_VALUE")
check("an unknown mapping kind", edit("kind", 'kind = "event"', 'kind = "poke"'), "E_FIELD_VALUE")
check("an unknown mapping key", edit("key", 'kind = "event"', 'kind = "event"\nshell = "x"'), "E_FIELD_UNKNOWN")
check("asset-binding fields on a command", edit("mix", 'kind = "command"', 'kind = "command"\ntarget = "x"'), "E_FIELD_UNKNOWN")
check("a site without a length", project("site", PROJECT.replace("length = 80\n", "")), "E_FIELD_MISSING")

# platform adapters: a Master System project has no PCM adapter; Luau-bound audio capabilities are not yet supported there
SMS = PROJECT.replace('platform = "genesis"', 'platform = "master-system"').replace(f'sha256 = "{SRC_SHA}"', f'sha256 = "{SRC_SHA}"\nmapper = "sega"').replace(f'sha256 = "{sha(b"synthetic-source-image-B")}"', f'sha256 = "{sha(b"synthetic-source-image-B")}"\nmapper = "rom_only"')
check("master-system declares audio capabilities it lacks", project("sms", SMS), "E_CAPABILITY_UNSUPPORTED")
sms_pcm = SMS.replace('required = ["audio.event.read", "audio.command.send"]', 'required = ["host.log"]').split("[[audio.mapping]]")[0] + '[[audio.mapping]]\nid = "k"\nkind = "asset-binding"\nasset = "assets/kick.wav"\ntarget = "t"\nuse = "pcm-sample"\n'
sms_pcm = sms_pcm.replace('optional = ["audio.host.play"]\n', '')
check("master-system has no PCM adapter for a pcm-sample binding", project("sms-pcm", sms_pcm), "E_CAPABILITY_UNSUPPORTED")

# limits: count, per-asset size and the package-size limit cannot be bypassed by assets
many = {f"assets/a{i:03d}.bin": bytes([i % 251]) * 5 for i in range(129)}
manyp = PROJECT.split("[[source]]")[0] + "".join(f'[[source]]\nkind = "asset"\npath = "{p}"\nasset_class = "data"\nformat = "raw"\n\n' for p in many)
manyp = manyp.replace('required = ["audio.event.read", "audio.command.send"]', 'required = []').replace('optional = ["audio.host.play"]\n', "")
check("129 assets", project("many", manyp, many), "E_PAYLOAD_CAP")
big = os.urandom(8 << 20) + b"x"
bigp = PROJECT.split("[[source]]")[0].replace("audio.event.read", "host.log").replace('"audio.command.send"', '"audio.command.send"') + '[[source]]\nkind = "asset"\npath = "assets/big.bin"\nasset_class = "data"\nformat = "raw"\n'
check("an asset over 8 MiB", project("big", bigp.replace('required = ["host.log", "audio.command.send"]', "required = []").replace('optional = ["audio.host.play"]\n', ""), {"assets/big.bin": big}), "E_PAYLOAD_CAP")
chunks = {f"assets/p{i}.bin": os.urandom(8 << 20) for i in range(5)}   # 5 x 8 MiB incompressible: each asset is within its cap, the archive is not
pk = PROJECT.split("[[source]]")[0].replace('required = ["audio.event.read", "audio.command.send"]', "required = []").replace('optional = ["audio.host.play"]\n', "") + "".join(
    f'[[source]]\nkind = "asset"\npath = "{p}"\nasset_class = "data"\nformat = "raw"\n\n' for p in chunks)
check("assets cannot exceed the package-size limit", project("heavy", pk, chunks), "E_ARCHIVE_SIZE")

# integrity: a built package whose audio bytes were altered (CRC re-fixed) is refused by the manifest digest
with zipfile.ZipFile(out) as z:
    entries = [(i.filename, z.read(i.filename)) for i in z.infolist()]
tampered = [(n, (bytes([d[0] ^ 1]) + d[1:]) if n == "assets/kick.wav" else d) for n, d in entries]
write(tmp / "tampered.segapatch", make_zip(tampered))
rc, doc, _ = cli.run("mod", "inspect", tmp / "tampered.segapatch")
cli.check(rc == 1 and doc["class"] == "E_PAYLOAD_DIGEST", f"tampered audio bytes are refused by the payload digest ({doc})")

# ------------------------------------------------------------------ deterministic conversion (u8-mono), three runs, independent reference
def reference_u8_mono(data, channels):
    samples = struct.unpack("<%dh" % (len(data) // 2), data)
    out = bytearray()
    for f in range(len(samples) // channels):
        s = sum(samples[f * channels:(f + 1) * channels])
        mono = s // 2 if channels == 2 else s   # Python // floors, as specified
        out.append((mono + 32768) >> 8)
    return bytes(out)


for name, source, channels in (("kick", KICK, 1), ("theme", THEME, 2)):
    src = write(tmp / f"{name}.wav", source)
    digests = set()
    for run in range(3):
        o = tmp / f"{name}.{run}.u8"
        rc, doc, _ = cli.run("mod", "convert-audio", src, "--out", o)
        cli.check(rc == 0 and doc["target"] == "u8-mono", f"convert {name} run {run}: {doc}")
        digests.add((o.read_bytes(), doc["output_sha256"]))
    cli.check(len(digests) == 1, f"{name}: three runs produce identical bytes and digest")
    produced = next(iter(digests))[0]
    cli.check(produced == reference_u8_mono(source[44:], channels), f"{name}: the conversion equals the independent reference")
    cli.check(len(produced) == len(source[44:]) // (2 * channels), f"{name}: one output byte per frame")
extreme = struct.pack("<hhhh", -32768, -32768, 32767, 32767)   # stereo frames at the extremes (floor of the negative sum)
ew = bytearray(wav(8000, 2, 2)[:44]) + bytearray(extreme)
ew[4:8] = struct.pack("<I", 36 + len(extreme)); ew[40:44] = struct.pack("<I", len(extreme))
write(tmp / "extreme.wav", bytes(ew))
rc, doc, _ = cli.run("mod", "convert-audio", tmp / "extreme.wav", "--out", tmp / "extreme.u8")
cli.check(rc == 0 and (tmp / "extreme.u8").read_bytes() == reference_u8_mono(extreme, 2), "conversion at sample extremes matches the reference")
rc, doc, _ = cli.run("mod", "convert-audio", write(tmp / "bad.wav", b"not a wav"), "--out", tmp / "bad.u8")
cli.check(rc == 1 and doc["class"] == "E_FIELD_VALUE" and not (tmp / "bad.u8").exists(), "an invalid file converts to nothing")

cli.done("mod_audio_test")

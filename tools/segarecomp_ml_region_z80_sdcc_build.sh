#!/usr/bin/env bash
# SEG-056 (ADR 0117): reproducible build of two SDCC-built SMS homebrew reference images with relocated listings.
# Private research tooling. Usage: sdcc_build.sh SRC_DIR WORK_DIR
#   SRC_DIR holds checkouts at the pinned commits: devkitSMS 533ae573, duckslayer-adventures 7ddffba6, stalactites-sms e3064b0b.
# Needs sdcc 4.2.0 (sdasz80/sdldz80/sdar), gcc, python3. Output: WORK_DIR/{duckslayer,stalactites}.{sms,truth.json}.
set -euo pipefail
SRC=$(realpath "$1"); W=$(realpath -m "$2"); T=$(realpath "$(dirname "$0")")
rm -rf "$W"; mkdir -p "$W/bin"; cd "$W"
gcc -O2 -w -o bin/folder2c "$SRC/devkitSMS/tools/legacy/folder2c/src/folder2c.c"
gcc -O2 -w -o bin/ihx2sms "$SRC/devkitSMS/tools/legacy/ihx2sms/src/ihx2sms.c"
export PATH="$W/bin:$PATH"
# shared libraries from source (masked from truth: shared across SDCC lineages)
mkdir -p dsms && cp -r "$SRC/devkitSMS/SMSlib/src" dsms/SMSlib && cp -r "$SRC/devkitSMS/PSGlib/src" dsms/PSGlib && cp -r "$SRC/devkitSMS/crt0/src" dsms/crt0
(cd dsms/SMSlib && make SMSlib.lib -j8 >/dev/null) ; (cd dsms/PSGlib && make PSGlib.lib -j8 >/dev/null)
(cd dsms/crt0 && sdasz80 -plosgff -o crt0_sms.s)
D="$W/dsms"
relink() {  # dir name : relink with explicit library objects so that ASxxxx -u can relocate their listings
python3 - "$1" "$2" <<'PY'
import os, re, sys
d, name = sys.argv[1:3]
os.chdir(d)
members = re.findall(r'^(\S+\.lib)\s+\[ (\S+\.rel) \]', open(f'{name}.map').read(), re.M)
res = []
for l in [l for l in open(f'{name}.lk').read().split('\n') if l.strip()]:
    if l.startswith('-mjwx'):
        l = '-mjwxu'
    if l.startswith('-l ') and not l.startswith('-l z80'):
        continue
    if l == '-e':
        res += [os.path.join(os.path.dirname(lib), rel) for lib, rel in members if 'sdcc/lib' not in lib]
    res.append(l)
open(f'{name}_x.lk', 'w').write('\n'.join(res) + '\n')
PY
(cd "$1" && sdldz80 -nf "$2_x.lk" >/dev/null)
}
# --- Duckslayer Adventures
cp -r "$SRC/duckslayer-adventures" duck && cd duck && rm -rf SMSlib PSGlib && mkdir -p SMSlib/src PSGlib/src
cp "$D/SMSlib/SMSlib.h" SMSlib/src/ && cp "$D/PSGlib/PSGlib.h" PSGlib/src/
folder2c gfx gfx >/dev/null
sdcc -mz80 -c --std-sdcc99 duckslayer-adventures.c 2>/dev/null; sdcc -mz80 -c --std-sdcc99 gfx.c
sdcc -mz80 --no-std-crt0 --data-loc 0xC000 -o duck.ihx "$D/crt0/crt0_sms.rel" duckslayer-adventures.rel gfx.rel "$D/PSGlib/PSGlib.lib" "$D/SMSlib/SMSlib.lib" >/dev/null
relink . duck; ihx2sms duck.ihx ../duckslayer.sms >/dev/null
python3 "$T/segarecomp_ml_region_z80_sdcc_truth.py" ../duckslayer.sms ../duckslayer.truth.json sdcc:duckslayer-adventures.rst sdcc:gfx.rst
cd "$W"
# --- Stalactites
cp -r "$SRC/stalactites-sms" stal && cd stal && cp "$D/SMSlib/SMSlib.h" "$D/PSGlib/PSGlib.h" lib/
folder2c data data >/dev/null
M="data actor shot shots map score progress_bar sound stalactites"
for f in $M; do sdcc -c -mz80 --peep-file lib/peep-rules.txt $f.c; done
OBJS=$(for f in $M; do printf '%s.rel ' $f; done)
sdcc -o stal.ihx -mz80 --no-std-crt0 --data-loc 0xC000 "$D/crt0/crt0_sms.rel" $OBJS "$D/SMSlib/SMSlib.lib" "$D/PSGlib/PSGlib.lib" >/dev/null
relink . stal; ihx2sms stal.ihx ../stalactites.sms >/dev/null
python3 "$T/segarecomp_ml_region_z80_sdcc_truth.py" ../stalactites.sms ../stalactites.truth.json $(for f in $M; do printf 'sdcc:%s.rst ' $f; done)
cd "$W"; sha256sum duckslayer.sms stalactites.sms

#!/usr/bin/env python3
"""SEG-150-T010: the launcher's asset manifest, the shipped asset tree and the font licences agree (hermetic, source tree only).

  1. every file under apps/segarecomp-launcher/assets is described by apps/segarecomp-launcher/assets/manifest.json `runtime_assets`
     (documentation files and the font licences excepted), and every manifest entry matches at least one file (a `count`, when stated, is exact);
  2. every asset the launcher code names literally (`asset("...")`, `asset_path("...")`, `load_asset_surface("...")`, `icon("name")` -> ui/icon-name.png) exists;
  3. every bundled font file has its licence next to it, and packaging/install.cmake installs each licence plus the font directory (so a package carries both);
  4. the install rules exclude only documentation (README.md, manifest.json) and the licences that are installed separately into licenses/.

usage: packaging_launcher_assets_test.py <source-root>
"""
import fnmatch
import json
import pathlib
import re
import sys

ROOT = pathlib.Path(sys.argv[1]).resolve()
ASSETS = ROOT / "apps" / "segarecomp-launcher" / "assets"
failures = []


def check(ok, label):
    print(("ok    " if ok else "FAIL  ") + label)
    if not ok:
        failures.append(label)


manifest = json.loads((ASSETS / "manifest.json").read_text(encoding="utf-8"))
runtime = manifest["runtime_assets"]
patterns = {}
for key, value in runtime.items():
    glob = re.sub(r"placeholder-1\.\.4", "placeholder-?", key)   # the manifest names the four covers as a range
    patterns[glob] = value
excluded = {"README.md", "manifest.json", "fonts/Inter-OFL.txt", "fonts/Silkscreen-OFL.txt"}
files = sorted(str(p.relative_to(ASSETS)).replace("\\", "/") for p in ASSETS.rglob("*") if p.is_file())
shipped = [f for f in files if f not in excluded]

for f in shipped:
    check(any(fnmatch.fnmatchcase(f, g) for g in patterns), f"the manifest describes {f}")
for glob, value in patterns.items():
    matched = [f for f in shipped if fnmatch.fnmatchcase(f, glob)]
    check(bool(matched), f"the manifest entry {glob} matches a shipped file")
    if "count" in value:
        check(len(matched) == value["count"], f"{glob}: count {value['count']} == {len(matched)}")

sources = "\n".join(p.read_text(encoding="utf-8", errors="replace") for p in (ROOT / "apps" / "segarecomp-launcher").glob("*.cpp") if not p.name.endswith("_test.cpp"))
named = set(re.findall(r'(?:asset|asset_path|load_asset_surface)\("([^"]+)"\)', sources))
named |= {f"ui/icon-{n}.png" for n in re.findall(r'\bicon\("([a-z-]+)"\)', sources)}
for n in sorted(named):
    check(n in files, f"the launcher names {n} and it exists")

for font in [f for f in files if f.startswith("fonts/") and f.endswith(".ttf")]:
    family = font.split("/")[1].split("-")[0]
    check(f"fonts/{family}-OFL.txt" in files, f"{font} has its licence fonts/{family}-OFL.txt")
install = (ROOT / "packaging" / "install.cmake").read_text(encoding="utf-8")
for licence in ("Inter-OFL.txt", "Silkscreen-OFL.txt"):
    check(re.search(r"install\(FILES [^)]*fonts/" + re.escape(licence) + r"\s+DESTINATION \$\{_res\}/licenses", install) is not None, f"install.cmake installs {licence} into licenses/")
check("apps/segarecomp-launcher/assets/ DESTINATION ${_res}/assets" in install, "install.cmake installs the assets directory (fonts included)")
exclude = set(re.findall(r'PATTERN "([^"]+)" EXCLUDE', install[install.index("assets/ DESTINATION"):install.index("MPL-2.0.txt")]))
check(exclude == {"README.md", "manifest.json", "Silkscreen-OFL.txt", "Inter-OFL.txt"}, f"only documentation and the separately installed licences are excluded: {sorted(exclude)}")
check("THIRD-PARTY-NOTICES.txt" in install, "install.cmake installs THIRD-PARTY-NOTICES.txt")
notices = (ROOT / "packaging" / "THIRD-PARTY-NOTICES.txt").read_text(encoding="utf-8", errors="replace")
check("Inter" in notices and "Silkscreen" in notices, "THIRD-PARTY-NOTICES names Inter and Silkscreen")

if failures:
    print(f"{len(failures)} failure(s)")
    sys.exit(1)
print("packaging_launcher_assets_test: ok")

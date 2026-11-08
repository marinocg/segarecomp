#!/usr/bin/env python3
"""Public documentation gate: validates the compatibility ledger and a few cross-document facts.

usage: check_product_truth.py [--root <repository root>] [--render-matrix]

Standard library only. It reads public files of this repository: no network, no external repository, no commercial
image. The ledger (docs/product/compatibility-ledger.json) is a REPORTING document; nothing here changes how the
launcher detects capabilities or admits builds. Format and rules: docs/product/compatibility-ledger.md.

Checks:
  1. ledger structure, enumerations, unique ids, contradictory outcomes and broken local evidence references;
  2. local Markdown links of the authoritative status documents;
  3. a few facts that the status documents and the ledger must agree on;
  4. known contradictory legacy assertions that must not return to the current-status pages;
  5. the mod support matrix (docs/product/mod-support-matrix.json): structure, one row per platform x capability,
     a `supported-*` level only with cited test evidence, an owning test for every row, and the rendered docs page.
Exit status 0 when clean, 1 with one line per problem.
"""
import argparse
import json
import pathlib
import re
import struct
import sys

LEDGER_PATH = "docs/product/compatibility-ledger.json"
MATRIX_PATH = "docs/product/mod-support-matrix.json"
MATRIX_DOC = "docs/product/mod-support-matrix.md"
MATRIX_BEGIN = "<!-- matrix:begin -->"
MATRIX_END = "<!-- matrix:end -->"
MATRIX_LEVELS = ("supported-and-tested", "supported-with-limitations", "experimental", "unsupported")
MATRIX_GATED = "spike-gated"
MATRIX_PLANNED = re.compile(r"^SEG-\d{3}-T\d{3}$")

PLATFORM_STATUS = ("implemented", "experimental", "planned")
EXECUTION_LEVELS = ("unknown", "boot", "title", "gameplay", "playable", "completed")
# Evidence quality is a separate dimension from the playability outcome.
EVIDENCE_CATEGORIES = (
    "synthetic_implementation_tests",
    "reference_comparison",
    "automated_gameplay_route",
    "maintainer_reported_playthrough",
    "independently_reproduced_completion",
    "untested",
)
# The highest execution level a category can support on its own. A human report is the only thing that supports
# "playable" or "completed" until an independent reproduction exists; automated checkpoints stop at "gameplay".
MAX_LEVEL = {
    "synthetic_implementation_tests": "unknown",
    "reference_comparison": "gameplay",
    "automated_gameplay_route": "gameplay",
    "maintainer_reported_playthrough": "completed",
    "independently_reproduced_completion": "completed",
    "untested": "unknown",
}
FEATURE_STATUS = ("implemented", "partial", "experimental")
BUILD_MODES = ("compatibility", "optimized", "file_first_structural", "unknown")
INPUT_KINDS = ("original", "modified", "unknown")
DISTRIBUTION = ("release", "maintained_outside_repository")
PATCH_SCHEMAS = (1, 2)
REF_PATTERN = re.compile(r"^(ADR (\d{4})|PR #\d+)$")

# Pages whose statements describe the CURRENT product. Historical ADRs are deliberately not listed.
STATUS_DOCS = (
    "README.md",
    "platforms/README.md",
    "platforms/genesis/README.md",
    "platforms/master-system/README.md",
    "platforms/game-gear/README.md",
    "platforms/32x/README.md",
    "platforms/saturn/README.md",
    "platforms/sega-cd/README.md",
    "docs/product/sega-cd-play.md",
    "docs/product/viewer-ui.md",
    "docs/product/launcher-library.md",
    "docs/product/patch-packages.md",
    "docs/product/native-patch-sdk.md",
    "docs/product/widescreen-and-patches.md",
    "docs/product/release-readiness.md",
    "packaging/README.md",
    "packaging/README.txt",
)
LINK_DOCS = STATUS_DOCS + ("docs/product/compatibility-ledger.md", "docs/product/mod-support-matrix.md", "docs/testing/commercial-games.md", "CHANGELOG.md", "docs/assets/showcase/README.md")

# Contradictory or superseded assertions that must not reappear in a current-status page.
FORBIDDEN = (
    (r"no key remapping", "viewer remapping exists (Controls page); do not claim there is none"),
    (r"Generated-native Z80 execution is \*\*not\*\* implemented", "generated-native Z80 is implemented"),
    (r"there are no FM or PSG sounds", "Sega CD FM/PSG from the main-side Z80 is implemented"),
    (r"\(the Z80 does not execute\)", "the Sega CD main-side Z80 executes generated-native"),
    (r"No sound is available for Sega CD", "Sega CD plays CD-DA, PCM, FM and PSG"),
    (r"\*\*Saving\*\* \(the launcher passes", "Sega CD internal backup RAM persists since v0.7.0"),
    (r"saving \(each run starts from a new console's empty", "Sega CD internal backup RAM persists since v0.7.0"),
    (r"a second title has only \*\*experimental observations\*\*", "the second Sega CD title is covered by the ledger"),
    (r"\*\*A second title\*\* \| Experimental observations only", "the second Sega CD title is covered by the ledger"),
    (r"\|\s*\[Master System\]\([^)]*\)\s*\|\s*no\s*\|", "Master System is implemented"),
    (r"A verified game-specific widescreen mode \(none exists yet\)", "game-specific providers exist (maintained outside this repository)"),
    (r"independently (reproduced|validated|replayed)[^.\n]{0,40}(playthrough|completion)", "human completion must not be described as independently validated"),
    (r"segarecomp-harness|/root/dev/|/home/[a-z]+/", "no private or local path in public documentation"),
)


def parse_args(argv):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--root", default=str(pathlib.Path(__file__).resolve().parents[1]))
    parser.add_argument("--render-matrix", action="store_true", help="print the beta matrix table that docs/product/mod-support-matrix.md must contain")
    return parser.parse_args(argv)


def level_index(level):
    return EXECUTION_LEVELS.index(level)


def ref_exists(ref, root):
    """A reference is 'ADR NNNN' (the ADR file exists), 'PR #N' (not verifiable offline: shape only) or a repository path (optional #anchor)."""
    match = REF_PATTERN.match(ref)
    if match:
        if match.group(2):
            return any((root / "docs" / "decisions").glob(match.group(2) + "-*.md"))
        return True
    path = ref.split("#", 1)[0]
    return bool(path) and not path.startswith(("/", "..")) and (root / path).exists()


def check_refs(owner, refs, root, problems):
    if not isinstance(refs, list) or not refs:
        problems.append(f"{owner}: refs must be a non-empty list")
        return
    for ref in refs:
        if not isinstance(ref, str) or not ref_exists(ref, root):
            problems.append(f"{owner}: broken evidence reference {ref!r}")


def expect_str(owner, entry, key, problems, allow_null=False):
    value = entry.get(key)
    if value is None and allow_null and key in entry:
        return
    if not isinstance(value, str) or not value.strip():
        problems.append(f"{owner}: '{key}' must be a non-empty string")


def expect_str_list(owner, entry, key, problems):
    value = entry.get(key)
    if not isinstance(value, list) or any(not isinstance(item, str) or not item.strip() for item in value):
        problems.append(f"{owner}: '{key}' must be a list of non-empty strings")


def expect_keys(owner, entry, required, optional, problems):
    if not isinstance(entry, dict):
        problems.append(f"{owner}: must be an object")
        return False
    for key in required:
        if key not in entry:
            problems.append(f"{owner}: missing '{key}'")
    for key in entry:
        if key not in required and key not in optional:
            problems.append(f"{owner}: unknown key '{key}'")
    return True


def check_evidence(owner, evidence, root, problems):
    if not isinstance(evidence, list) or not evidence:
        problems.append(f"{owner}: evidence must be a non-empty list")
        return []
    categories = []
    for index, item in enumerate(evidence):
        where = f"{owner}.evidence[{index}]"
        if not expect_keys(where, item, ("category", "scope", "refs"), (), problems):
            continue
        if item.get("category") not in EVIDENCE_CATEGORIES:
            problems.append(f"{where}: unsupported evidence category {item.get('category')!r}")
            continue
        categories.append(item["category"])
        expect_str(where, item, "scope", problems)
        check_refs(where, item.get("refs"), root, problems)
    return categories


def validate_ledger(ledger, root):
    problems = []
    if not expect_keys("ledger", ledger, ("schema", "schema_version", "as_of", "latest_release", "note", "platforms", "games", "patch_packages", "features"), (), problems):
        return problems
    if ledger.get("schema") != "segarecomp-compatibility-ledger":
        problems.append("ledger: schema must be 'segarecomp-compatibility-ledger'")
    if ledger.get("schema_version") != 1:
        problems.append("ledger: schema_version must be 1")
    if not re.fullmatch(r"\d{4}-\d{2}-\d{2}", str(ledger.get("as_of"))):
        problems.append("ledger: as_of must be YYYY-MM-DD")
    if not re.fullmatch(r"v\d+\.\d+\.\d+", str(ledger.get("latest_release"))):
        problems.append("ledger: latest_release must look like v0.7.0")

    platform_ids = []
    for index, entry in enumerate(ledger.get("platforms", [])):
        owner = f"platforms[{index}]"
        if not expect_keys(owner, entry, ("id", "name", "status", "cpu_mode", "capabilities", "limitations", "evidence"), (), problems):
            continue
        owner = f"platform {entry.get('id')!r}"
        platform_ids.append(entry.get("id"))
        expect_str(owner, entry, "id", problems)
        expect_str(owner, entry, "name", problems)
        expect_str(owner, entry, "cpu_mode", problems)
        if entry.get("status") not in PLATFORM_STATUS:
            problems.append(f"{owner}: unsupported status {entry.get('status')!r}")
        expect_str_list(owner, entry, "capabilities", problems)
        expect_str_list(owner, entry, "limitations", problems)
        if entry.get("status") == "planned":
            if entry.get("capabilities"):
                problems.append(f"{owner}: a planned platform must not list capabilities")
            if entry.get("cpu_mode") != "none":
                problems.append(f"{owner}: a planned platform must have cpu_mode 'none'")
        elif not entry.get("capabilities"):
            problems.append(f"{owner}: an implemented/experimental platform needs capabilities")
        if not entry.get("limitations"):
            problems.append(f"{owner}: limitations must not be empty")
        check_refs(owner, entry.get("evidence"), root, problems)
    if len(platform_ids) != len(set(platform_ids)):
        problems.append("platforms: duplicate id")
    status_by_platform = {p.get("id"): p.get("status") for p in ledger.get("platforms", []) if isinstance(p, dict)}

    game_ids = []
    for index, entry in enumerate(ledger.get("games", [])):
        owner = f"games[{index}]"
        required = ("id", "platform", "title", "edition", "execution_level", "evidence", "build_mode", "input", "blockers", "untested")
        if not expect_keys(owner, entry, required, ("unknown_conditions",), problems):
            continue
        owner = f"game {entry.get('id')!r}"
        game_ids.append(entry.get("id"))
        expect_str(owner, entry, "id", problems)
        expect_str(owner, entry, "title", problems)
        expect_str(owner, entry, "edition", problems, allow_null=True)
        if entry.get("platform") not in platform_ids:
            problems.append(f"{owner}: unknown platform {entry.get('platform')!r}")
        elif status_by_platform.get(entry.get("platform")) == "planned":
            problems.append(f"{owner}: a game cannot be tested on a planned platform")
        if entry.get("execution_level") not in EXECUTION_LEVELS:
            problems.append(f"{owner}: unsupported execution_level {entry.get('execution_level')!r}")
        if entry.get("build_mode") not in BUILD_MODES:
            problems.append(f"{owner}: unsupported build_mode {entry.get('build_mode')!r}")
        if entry.get("input") not in INPUT_KINDS:
            problems.append(f"{owner}: unsupported input {entry.get('input')!r}")
        for key in ("blockers", "untested"):
            expect_str_list(owner, entry, key, problems)
        if "unknown_conditions" in entry:
            expect_str_list(owner, entry, "unknown_conditions", problems)
        categories = check_evidence(owner, entry.get("evidence"), root, problems)
        level = entry.get("execution_level")
        if level in EXECUTION_LEVELS and categories:
            best = max(level_index(MAX_LEVEL[c]) for c in categories)
            if level_index(level) > best:
                problems.append(f"{owner}: execution_level {level!r} is not supported by its evidence categories {sorted(set(categories))} (needs a human report or independent reproduction)")
            if level != "unknown" and set(categories) == {"untested"}:
                problems.append(f"{owner}: untested evidence cannot support execution_level {level!r}")
            if level == "unknown" and any(c != "untested" for c in categories):
                problems.append(f"{owner}: execution_level 'unknown' contradicts non-empty evidence")
        if level == "completed" and "independently_reproduced_completion" not in categories and "maintainer_reported_playthrough" not in categories:
            problems.append(f"{owner}: completed needs a maintainer report or an independent reproduction")
        if entry.get("input") == "modified":
            problems.append(f"{owner}: a game entry must use original input; patched runs belong to patch_packages")
        if "maintainer_reported_playthrough" in categories and entry.get("build_mode") == "unknown" and level == "completed" and not entry.get("unknown_conditions"):
            problems.append(f"{owner}: a human completion with an unknown build mode must list unknown_conditions")
    if len(game_ids) != len(set(game_ids)):
        problems.append("games: duplicate id")

    package_ids = []
    for index, entry in enumerate(ledger.get("patch_packages", [])):
        owner = f"patch_packages[{index}]"
        required = ("id", "game", "revision", "schema", "kind", "distribution", "provides", "gameplay_changes", "validated_scope", "untested", "evidence")
        if not expect_keys(owner, entry, required, (), problems):
            continue
        owner = f"patch package {entry.get('id')!r}"
        package_ids.append(entry.get("id"))
        for key in ("id", "game", "revision", "kind", "gameplay_changes", "validated_scope"):
            expect_str(owner, entry, key, problems)
        if entry.get("schema") not in PATCH_SCHEMAS:
            problems.append(f"{owner}: unsupported schema {entry.get('schema')!r}")
        if entry.get("distribution") not in DISTRIBUTION:
            problems.append(f"{owner}: unsupported distribution {entry.get('distribution')!r}")
        elif entry.get("distribution") == "release":
            problems.append(f"{owner}: 'release' distribution requires an actual release asset; none is recorded (use maintained_outside_repository)")
        expect_str_list(owner, entry, "provides", problems)
        expect_str_list(owner, entry, "untested", problems)
        if not entry.get("untested"):
            problems.append(f"{owner}: untested must not be empty (state what was not validated)")
        check_refs(owner, entry.get("evidence"), root, problems)
    if len(package_ids) != len(set(package_ids)):
        problems.append("patch_packages: duplicate id")

    feature_ids = []
    for index, entry in enumerate(ledger.get("features", [])):
        owner = f"features[{index}]"
        if not expect_keys(owner, entry, ("id", "name", "platforms", "status", "released_in", "available_on_main", "evidence", "limitations"), (), problems):
            continue
        owner = f"feature {entry.get('id')!r}"
        feature_ids.append(entry.get("id"))
        expect_str(owner, entry, "id", problems)
        expect_str(owner, entry, "name", problems)
        if entry.get("status") not in FEATURE_STATUS:
            problems.append(f"{owner}: unsupported status {entry.get('status')!r}")
        platforms = entry.get("platforms")
        if not isinstance(platforms, list) or not platforms or any(p not in platform_ids for p in platforms):
            problems.append(f"{owner}: platforms must be a non-empty list of known platform ids")
        released = entry.get("released_in")
        if released is not None and released != ledger.get("latest_release"):
            problems.append(f"{owner}: released_in must be null or the ledger's latest_release ({ledger.get('latest_release')})")
        if not isinstance(entry.get("available_on_main"), bool):
            problems.append(f"{owner}: available_on_main must be true or false")
        elif released is not None and not entry.get("available_on_main"):
            problems.append(f"{owner}: a released feature that is gone from main needs a note, not a silent flag")
        expect_str_list(owner, entry, "limitations", problems)
        if not entry.get("limitations"):
            problems.append(f"{owner}: limitations must not be empty")
        check_evidence(owner, entry.get("evidence"), root, problems)
    if len(feature_ids) != len(set(feature_ids)):
        problems.append("features: duplicate id")
    return problems


def is_test_ref(ref):
    """A test reference is an existing repository file under tests/ or an app test directory whose name says it is a test."""
    path = ref.split("#", 1)[0]
    return path.startswith("tests/") or (path.startswith("apps/") and "_test" in path.rsplit("/", 1)[-1])


def validate_mod_matrix(matrix, root):
    """The support-level matrix: a row may claim `supported-*` only with cited, existing test evidence."""
    problems = []
    if not expect_keys("matrix", matrix, ("schema", "schema_version", "as_of", "note", "levels", "beta_targets", "platforms", "capabilities", "rows"), (), problems):
        return problems
    if matrix.get("schema") != "segarecomp-mod-support-matrix":
        problems.append("matrix: schema must be 'segarecomp-mod-support-matrix'")
    if matrix.get("schema_version") != 1:
        problems.append("matrix: schema_version must be 1")
    if not re.fullmatch(r"\d{4}-\d{2}-\d{2}", str(matrix.get("as_of"))):
        problems.append("matrix: as_of must be YYYY-MM-DD")
    if tuple(matrix.get("levels", ())) != MATRIX_LEVELS:
        problems.append(f"matrix: levels must be exactly {list(MATRIX_LEVELS)}")
    if tuple(matrix.get("beta_targets", ())) != MATRIX_LEVELS + (MATRIX_GATED,):
        problems.append(f"matrix: beta_targets must be the levels plus {MATRIX_GATED!r}")
    platform_ids = [p.get("id") for p in matrix.get("platforms", []) if isinstance(p, dict)]
    capability_ids = [c.get("id") for c in matrix.get("capabilities", []) if isinstance(c, dict)]
    if not platform_ids or len(platform_ids) != len(set(platform_ids)):
        problems.append("matrix: platforms must be a non-empty list of unique ids")
    if not capability_ids or len(capability_ids) != len(set(capability_ids)):
        problems.append("matrix: capabilities must be a non-empty list of unique ids")
    seen = set()
    for index, row in enumerate(matrix.get("rows", [])):
        owner = f"matrix.rows[{index}]"
        if not expect_keys(owner, row, ("platform", "capability", "level", "beta_target", "evidence", "existing_tests", "planned_owner"), ("gate",), problems):
            continue
        key = (row.get("platform"), row.get("capability"))
        owner = f"matrix row {key[0]}/{key[1]}"
        if key[0] not in platform_ids or key[1] not in capability_ids:
            problems.append(f"{owner}: unknown platform or capability")
        if key in seen:
            problems.append(f"{owner}: duplicate row")
        seen.add(key)
        level = row.get("level")
        if level not in MATRIX_LEVELS:
            problems.append(f"{owner}: unsupported level {level!r}")
        target = row.get("beta_target")
        if target not in MATRIX_LEVELS + (MATRIX_GATED,):
            problems.append(f"{owner}: unsupported beta_target {target!r}")
        if target == MATRIX_GATED and not (isinstance(row.get("gate"), str) and MATRIX_PLANNED.match(row["gate"])):
            problems.append(f"{owner}: a spike-gated target must name its gate task (SEG-NNN-TNNN)")
        if target != MATRIX_GATED and "gate" in row:
            problems.append(f"{owner}: 'gate' is only for a spike-gated target")
        if not isinstance(row.get("planned_owner"), str) or not MATRIX_PLANNED.match(row["planned_owner"]):
            problems.append(f"{owner}: planned_owner must be a task id (SEG-NNN-TNNN)")
        check_refs(f"{owner}.evidence", row.get("evidence"), root, problems)
        tests = row.get("existing_tests")
        if not isinstance(tests, list) or any(not isinstance(t, str) for t in tests):
            problems.append(f"{owner}: existing_tests must be a list of paths")
            continue
        for ref in tests:
            if not is_test_ref(ref) or not ref_exists(ref, root):
                problems.append(f"{owner}: existing_tests entry {ref!r} is not an existing test file")
        if isinstance(level, str) and level.startswith("supported") and not tests:
            problems.append(f"{owner}: level {level!r} requires cited test evidence in existing_tests")
        if level == "unsupported" and tests:
            problems.append(f"{owner}: an unsupported row must not cite passing tests")
    for platform in platform_ids:
        for capability in capability_ids:
            if (platform, capability) not in seen:
                problems.append(f"matrix: missing row {platform}/{capability}")
    return problems


def render_mod_matrix(matrix):
    """The beta test matrix as Markdown: every platform x capability with its owning test."""
    names = {p["id"]: p["name"] for p in matrix["platforms"]}
    caps = {c["id"]: c["name"] for c in matrix["capabilities"]}
    lines = ["| Capability | Platform | Current level | Public-beta target | Owning test |", "| --- | --- | --- | --- | --- |"]
    for capability in matrix["capabilities"]:
        for row in (r for r in matrix["rows"] if r["capability"] == capability["id"]):
            target = row["beta_target"] if row["beta_target"] != MATRIX_GATED else f"spike-gated ({row['gate']})"
            test = ", ".join(f"`{t}`" for t in row["existing_tests"]) if row["existing_tests"] else f"planned: {row['planned_owner']}"
            lines.append(f"| {caps[row['capability']]} | {names[row['platform']]} | {row['level']} | {target} | {test} |")
    return "\n".join(lines)


def check_mod_matrix_doc(root, matrix):
    path = root / MATRIX_DOC
    if not path.exists():
        return [f"{MATRIX_DOC}: missing (it renders the support matrix)"]
    text = path.read_text(encoding="utf-8")
    if MATRIX_BEGIN not in text or MATRIX_END not in text:
        return [f"{MATRIX_DOC}: must contain the {MATRIX_BEGIN} / {MATRIX_END} markers around the rendered table"]
    body = text.split(MATRIX_BEGIN, 1)[1].split(MATRIX_END, 1)[0].strip()
    if body != render_mod_matrix(matrix):
        return [f"{MATRIX_DOC}: the table differs from the matrix; regenerate it with tools/check_product_truth.py --render-matrix"]
    return []


LINK_PATTERN = re.compile(r"!?\[[^\]]*\]\(([^)\s]+)(?:\s+\"[^\"]*\")?\)")


def slug(heading):
    text = re.sub(r"[`*_]", "", heading.strip().lower())
    text = re.sub(r"[^\w\- ]", "", text)
    return text.replace(" ", "-")


def anchors(path):
    found = set()
    in_fence = False
    for line in path.read_text(encoding="utf-8").splitlines():
        if line.lstrip().startswith("```"):
            in_fence = not in_fence
        elif not in_fence and line.startswith("#"):
            found.add(slug(line.lstrip("#")))
    return found


def check_links(root, files):
    problems = []
    for name in files:
        path = root / name
        if not path.exists():
            continue
        text = path.read_text(encoding="utf-8")
        in_fence = False
        for number, line in enumerate(text.splitlines(), 1):
            if line.lstrip().startswith("```"):
                in_fence = not in_fence
            if in_fence:
                continue
            for target in LINK_PATTERN.findall(line):
                if re.match(r"^[a-z][a-z0-9+.-]*:", target) or re.match(r"^(\.\./)+(releases|issues)\b", target):
                    continue  # external URL or a GitHub-relative page such as ../../releases
                local, _, fragment = target.partition("#")
                target_path = path if not local else path.parent / local
                if not target_path.exists():
                    problems.append(f"{name}:{number}: broken local link {target!r}")
                elif fragment and target_path.suffix == ".md" and slug(fragment) not in anchors(target_path):
                    problems.append(f"{name}:{number}: broken anchor {target!r}")
    return problems


def check_forbidden(texts):
    problems = []
    for name, text in texts.items():
        for pattern, why in FORBIDDEN:
            match = re.search(pattern, text, re.IGNORECASE)
            if match:
                line = text.count("\n", 0, match.start()) + 1
                problems.append(f"{name}:{line}: legacy or private assertion {match.group(0)[:60]!r}: {why}")
    return problems


def check_consistency(ledger, texts):
    """A few facts the public status pages and the ledger must agree on."""
    problems = []
    readme = texts.get("README.md", "")
    platforms_readme = texts.get("platforms/README.md", "")
    release = ledger.get("latest_release", "")
    if "docs/product/compatibility-ledger.json" not in readme and "docs/product/compatibility-ledger.md" not in readme:
        problems.append("README.md: must link the compatibility ledger")
    if release and release not in readme:
        problems.append(f"README.md: must name the latest published release {release} to separate it from main")
    for game in ledger.get("games", []):
        if game.get("execution_level") in ("playable", "completed") and game.get("title", "") not in readme:
            problems.append(f"README.md: ledger game {game.get('title')!r} ({game.get('execution_level')}) is not mentioned")
    if any(g.get("execution_level") == "completed" and any(e.get("category") == "maintainer_reported_playthrough" for e in g.get("evidence", [])) for g in ledger.get("games", [])):
        if "maintainer-reported" not in readme:
            problems.append("README.md: must call human completions 'maintainer-reported'")
    rows = {}
    for line in platforms_readme.splitlines():
        cells = [c.strip() for c in line.strip().strip("|").split("|")]
        if line.startswith("|") and len(cells) >= 3:
            rows[re.sub(r"[\[\]]|\(.*?\)", "", cells[0]).strip().lower()] = cells[1].lower()
    names = {"genesis": "genesis / mega drive", "master_system": "master system", "sega_cd": "sega cd", "game_gear": "game gear", "sega_32x": "32x", "saturn": "saturn"}
    for platform in ledger.get("platforms", []):
        key = names.get(platform.get("id"))
        if key is None or key not in rows:
            problems.append(f"platforms/README.md: no table row for ledger platform {platform.get('id')!r}")
            continue
        code = rows[key]
        planned = platform.get("status") == "planned"
        if planned and code != "no":
            problems.append(f"platforms/README.md: {key} is planned in the ledger but the table says code {code!r}")
        if not planned and code != "yes":
            problems.append(f"platforms/README.md: {key} is {platform.get('status')} in the ledger but the table says code {code!r}")
    return problems


SHOWCASE_DIR = "docs/assets/showcase"
SHOWCASE_MAX_FILES = 7
SHOWCASE_MAX_BYTES = 512 * 1024
SHOWCASE_SIZES = ((1200, 675), (900, 675))  # 16:9 widescreen captures and 4:3 originals


def check_showcase(root):
    """The promotional screenshot exception (docs/testing/commercial-games.md) is closed: only listed, small PNG/WebP files."""
    problems = []
    directory = root / SHOWCASE_DIR
    if not directory.is_dir():
        return problems
    listing = (directory / "README.md").read_text(encoding="utf-8") if (directory / "README.md").exists() else ""
    for entry in directory.iterdir():
        if entry.is_dir():
            problems.append(f"{SHOWCASE_DIR}/{entry.name}: subdirectories are not allowed")
    images = sorted(p for p in directory.iterdir() if p.is_file() and p.name != "README.md")
    if len(images) > SHOWCASE_MAX_FILES:
        problems.append(f"{SHOWCASE_DIR}: more than {SHOWCASE_MAX_FILES} files")
    if images and not listing:
        problems.append(f"{SHOWCASE_DIR}: images require the README that records provenance and attribution")
    for image in images:
        if image.suffix.lower() not in (".png", ".webp"):
            problems.append(f"{SHOWCASE_DIR}/{image.name}: only PNG or WebP is allowed")
        if image.stat().st_size > SHOWCASE_MAX_BYTES:
            problems.append(f"{SHOWCASE_DIR}/{image.name}: larger than {SHOWCASE_MAX_BYTES // 1024} KiB")
        if image.suffix.lower() == ".png" and image.stat().st_size >= 24:
            width, height = struct.unpack(">II", image.read_bytes()[16:24])
            if (width, height) not in SHOWCASE_SIZES:
                problems.append(f"{SHOWCASE_DIR}/{image.name}: must be one of {', '.join(f'{w}x{h}' for w, h in SHOWCASE_SIZES)}, is {width}x{height}")
        if f"`{image.name}`" not in listing:
            problems.append(f"{SHOWCASE_DIR}/{image.name}: not listed in the showcase README")
    return problems


def run(root):
    ledger_file = root / LEDGER_PATH
    try:
        ledger = json.loads(ledger_file.read_text(encoding="utf-8"))
    except (OSError, ValueError) as error:
        return [f"{LEDGER_PATH}: cannot read ledger: {error}"]
    problems = validate_ledger(ledger, root)
    try:
        matrix = json.loads((root / MATRIX_PATH).read_text(encoding="utf-8"))
    except (OSError, ValueError) as error:
        problems.append(f"{MATRIX_PATH}: cannot read matrix: {error}")
    else:
        matrix_problems = validate_mod_matrix(matrix, root)
        problems += matrix_problems
        if not matrix_problems:
            problems += check_mod_matrix_doc(root, matrix)
    texts = {name: (root / name).read_text(encoding="utf-8") for name in STATUS_DOCS + ("CHANGELOG.md", LEDGER_PATH) if (root / name).exists()}
    problems += check_links(root, LINK_DOCS)
    problems += check_forbidden(texts)
    problems += check_consistency(ledger, texts)
    problems += check_showcase(root)
    return problems


def main(argv=None):
    args = parse_args(sys.argv[1:] if argv is None else argv)
    if args.render_matrix:
        root = pathlib.Path(args.root).resolve()
        print(render_mod_matrix(json.loads((root / MATRIX_PATH).read_text(encoding="utf-8"))))
        return 0
    problems = run(pathlib.Path(args.root).resolve())
    for problem in problems:
        print("FAIL:", problem, file=sys.stderr)
    if problems:
        return 1
    print("product truth gate: ledger, links and status documents are consistent")
    return 0


if __name__ == "__main__":
    sys.exit(main())

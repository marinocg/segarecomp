#!/usr/bin/env python3
"""SEG-073 (ADR 0155): offline diagnosis of the SEG-072 M68K ML v3 failures from the preserved private handoff bundle (OFFLINE research only).

  smoke    relocation smoke test: resolve every registry image through the bundle manifest (bundled SGDK builds -> bundle payload; operator
           images / Sonic CD modules -> explicit manifest paths, optionally rebased, each verified against the manifest SHA-256), reproduce
           stored production-M3 scores bit for bit and a stored development table pair from the bundle alone, and fail if any file of the
           original SEG-072/SEG-049 private working roots is opened (Python audit hook)
  infer    regenerate the not-preserved sealed per-cell scores by deterministic INFERENCE with the preserved final weights (frozen M3, M3-R,
           M4-Dense three-seed logit ensemble at 128/64-byte cells) and check them against the stored frozen regions and post-hoc aggregates
  analyze  label credibility of the sealed misses, model complementarity, training exposure, metric reinterpretation and the three cheap
           recovery experiments (A rank fusion, B bounded structural expansion, C sampling/objective feasibility); writes a private result
           and a public aggregate-only record

Nothing here trains a model, changes a SEG-072 artifact, reruns the one-shot sealed evaluation or touches production behaviour. The core
(ranking, budgets, fusion, resolution conversion, structural expansion, block evidence, exposure accounting) is standard-library only;
numpy/torch are imported lazily by the private commands. Private outputs must live in ignored storage; committed output is aggregates only.
"""
from __future__ import annotations

import argparse
import bisect
import hashlib
import json
import math
import os
import pathlib
import re
import subprocess
import sys

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
SCHEMA = "segarecomp.ml_v3_diagnosis.v1"
CELL = 128
BUDGETS = {"B35": 35, "B45": 45, "B50": 50}
RRF_K = 60                      # reciprocal-rank-fusion constant (fixed before any measurement; the conventional value)
SIGNATURE_THRESHOLD = 0.5       # flank-data high-byte histogram intersection at or above which a block shares the adjacent data's signature
FLANK_BYTES = 512               # data window on each side of a labelled code block for the signature
WALK_LIMIT_BYTES = 4096         # bound on one direct-flow continuation walk
HARD_POSITIVE_WEIGHT = 3.0      # hypothetical schedule: extra weight of a segment holding a rare-fragment positive cell (not trained here)
SGDK_LINEAGES = ("D05", "D06", "D07", "D08", "D09", "D10", "D11", "D12")
CONTROL_TERMINAL = (2, 3, 4, 5, 6, 7)  # token control: jumps, calls, return, trap - only proven fall-through (no callee-return assumption)
TOKEN_FIELDS = 8                 # segarecomp.m68k_decode_tokens.v1: status, length, family, control, ea_src, ea_dst, width, flags
SEALED_CLASSES = {"E01": "e01.1.cls", "E03": "e03.1.cls", "E05": "e05.1.cls", "E06": "e06.cls", "E07": "e07.cls"}
SEALED_RAW = {"E05": "e05.1.raw.cls"}
SEALED_BUNDLED_IMAGES = {"E06": "seg072/sealed/hb/e06/inst/out/rom.bin", "E07": "seg072/sealed/hb/e07/inst/out/rom.bin"}


class ArtifactError(Exception):
    """Typed private-artifact failure: kind in {missing, digest_mismatch, manifest_mismatch, unknown_image, original_root_access}."""

    def __init__(self, kind: str, detail: str):
        super().__init__(f"{kind}: {detail}")
        self.kind, self.detail = kind, detail


def sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def sha256_file(path) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


# ------------------------------------------------------------------------------------------------------------------ the bundle
class Bundle:
    """Manifest-bound, read-only view of the SEG-072 handoff bundle. Archived files (including `meta.json` with its absolute paths) are never
    rewritten: image paths are resolved through the manifest instead."""

    def __init__(self, root, rebase: dict | None = None, verify_files: bool = True):
        self.root = pathlib.Path(root).resolve()
        self.payload = self.root / "payload"
        mf, ms = self.root / "MANIFEST.json", self.root / "MANIFEST.sha256"
        for p in (mf, ms):
            if not p.is_file():
                raise ArtifactError("missing", p.name)
        expect = ms.read_text().split()[0] if ms.read_text().split() else ""
        self.manifest_sha256 = sha256_file(mf)
        if self.manifest_sha256 != expect:
            raise ArtifactError("manifest_mismatch", "MANIFEST.json does not match MANIFEST.sha256")
        self.manifest = json.loads(mf.read_text())
        self.files = self.manifest.get("files", {})
        self.external = self.manifest.get("external_inputs", {})
        self.rebase = dict(rebase or {})
        self.verify_files = verify_files
        self._checked: dict[str, str] = {}

    def file(self, rel: str) -> pathlib.Path:
        """A bundled payload file, verified against its manifest digest (once per process)."""
        rec = self.files.get(rel)
        if rec is None:
            raise ArtifactError("missing", f"not in manifest: {rel}")
        p = self.payload / rel
        if not p.is_file():
            raise ArtifactError("missing", rel)
        if self.verify_files and rel not in self._checked:
            got = sha256_file(p)
            if got != rec["sha256"]:
                raise ArtifactError("digest_mismatch", rel)
            self._checked[rel] = got
        return p

    def _rebased(self, path: str) -> pathlib.Path:
        for old, new in sorted(self.rebase.items(), key=lambda kv: -len(kv[0])):
            if path == old or path.startswith(old.rstrip("/") + "/"):
                return pathlib.Path(new.rstrip("/") + path[len(old.rstrip("/")):])
        return pathlib.Path(path)

    def external_input(self, key: str) -> pathlib.Path:
        rec = self.external.get(key)
        if rec is None:
            raise ArtifactError("unknown_image", key)
        p = self._rebased(rec["path"])
        if not p.is_file():
            raise ArtifactError("missing", f"external input {key}")
        if sha256_file(p) != rec["sha256"]:
            raise ArtifactError("digest_mismatch", f"external input {key}")
        return p

    def resolve_image(self, meta: dict) -> pathlib.Path:
        """Registry image -> path: project-authored SGDK builds come from the bundle payload (the archived absolute path is only parsed for its
        relative tail); everything else must be an external input recorded by id with its SHA-256. The result always matches meta['sha256']."""
        orig = str(meta.get("image", ""))
        m = re.search(r"/(seg072/(?:sealed/)?hb/.+)$", orig)
        if m:
            p = self.file(m.group(1))
        else:
            p = self.external_input(meta["id"])
        if meta.get("sha256") and sha256_file(p) != meta["sha256"]:
            raise ArtifactError("digest_mismatch", f"image {meta.get('id')}")
        return p

    def sealed_image(self, lineage: str) -> pathlib.Path:
        if lineage in SEALED_BUNDLED_IMAGES:
            return self.file(SEALED_BUNDLED_IMAGES[lineage])
        return self.external_input("sealed/" + lineage)

    def original_roots(self) -> list[str]:
        """The original private working roots the bundle was copied from (derived from archived image paths; used only to prove non-use)."""
        roots = set()
        for rel in self.files:
            if rel.endswith("meta.json") and rel.startswith("seg072/data/"):
                try:
                    orig = json.loads((self.payload / rel).read_text()).get("image", "")
                except (OSError, ValueError):
                    continue
                m = re.match(r"^(.*)/seg072/(?:sealed/)?hb/", orig)
                if m:
                    roots.add(m.group(1) + "/seg072")
                    roots.add(m.group(1) + "/seg049")
                    roots.add(m.group(1) + "/seg048")
        return sorted(roots)


def _under(path: str, root: str) -> bool:
    return path == root.rstrip("/") or path.startswith(root.rstrip("/") + "/")


def forbidden_opens(paths, roots, allow=()) -> list[str]:
    """Opened paths that fall inside any forbidden root (directory-prefix semantics), except those under an allowed root (the running
    interpreter's own environment, which is software, not a research artifact)."""
    out = []
    for p in paths:
        s = os.path.abspath(str(p))
        if any(_under(s, r) for r in roots) and not any(_under(s, a) for a in allow):
            out.append(s)
    return sorted(set(out))


def parse_rebase(items) -> dict:
    out = {}
    for it in items or []:
        if "=" not in it:
            raise SystemExit("--rebase expects OLD=NEW")
        old, new = it.split("=", 1)
        out[old] = new
    return out


# ----------------------------------------------------------------------------------------------------------------------- labels
def cell_count(size: int, cell: int = CELL) -> int:
    if size <= 0 or cell <= 0:
        raise ValueError("invalid size")
    return (size + cell - 1) // cell


def cell_labels(classes: bytes, cell: int = CELL) -> list[int]:
    """1 positive (holds an I), 0 negative (all D), -1 masked - the SEG-072 rule (ADR 0151) restated for stdlib tests."""
    out = []
    for c in range(cell_count(len(classes), cell)):
        chunk = classes[c * cell:(c + 1) * cell]
        out.append(1 if b"I" in chunk else (0 if chunk.count(b"D") == len(chunk) else -1))
    return out


def occupancy(classes: bytes, c: int, cell: int = CELL) -> float:
    chunk = classes[c * cell:(c + 1) * cell]
    return (chunk.count(b"I") + chunk.count(b"c")) / max(1, len(chunk))


def boundary_distance(labels, c: int) -> int | None:
    """Cells to the nearest non-positive cell (negative or masked); None when the image is entirely positive."""
    n = len(labels)
    for d in range(1, n):
        if (c - d >= 0 and labels[c - d] != 1) or (c + d < n and labels[c + d] != 1):
            return d
    return None


# ---------------------------------------------------------------------------------------------------------------------- ranking
def rank_order(scores) -> list[int]:
    """Score descending, cell ordinal ascending (the SEG-072 order)."""
    s = list(scores)
    for v in s:
        if isinstance(v, bool) or not isinstance(v, (int, float)) or math.isnan(v):
            raise ValueError("rank: score is not a number")
    return sorted(range(len(s)), key=lambda c: (-s[c], c))


def rank_positions(scores) -> list[int]:
    pos = [0] * len(scores)
    for r, c in enumerate(rank_order(scores)):
        pos[c] = r
    return pos


def budget_cells(n: int, percent: int) -> int:
    if n <= 0 or not 0 < percent <= 100:
        raise ValueError("invalid budget")
    return (n * percent + 99) // 100


def fixed_region(scores, seeds, percent: int) -> set[int]:
    """ceil(n * percent / 100) ranked cells UNION seed cells (the SEG-072 fixed policy)."""
    order = rank_order(scores)
    return set(order[:budget_cells(len(order), percent)]) | {c for c in seeds if 0 <= c < len(order)}


def region_bytes(region, cell: int, size: int) -> int:
    return sum(min((c + 1) * cell, size) - c * cell for c in region if 0 <= c * cell < size)


def missed_starts(starts, region, cell: int = CELL) -> int:
    return sum(1 for a in starts if a // cell not in region)


def normalized_ranks(scores) -> list[float]:
    n = len(scores)
    return [r / (n - 1) if n > 1 else 0.0 for r in rank_positions(scores)]


def fuse_mean_rank(a, b) -> list[float]:
    """Experiment A1: average of per-image normalized ranks (0 = best); returned as a score (higher = better) for the common rank order."""
    if len(a) != len(b):
        raise ValueError("fusion: different cell counts (convert to one unit first)")
    ra, rb = normalized_ranks(a), normalized_ranks(b)
    return [-(x + y) / 2.0 for x, y in zip(ra, rb)]


def fuse_rrf(a, b, k: int = RRF_K) -> list[float]:
    """Experiment A2: reciprocal-rank fusion, sum of 1 / (k + rank + 1) with ranks starting at 0."""
    if len(a) != len(b):
        raise ValueError("fusion: different cell counts (convert to one unit first)")
    ra, rb = rank_positions(a), rank_positions(b)
    return [1.0 / (k + x + 1) + 1.0 / (k + y + 1) for x, y in zip(ra, rb)]


def to_coarser_cells(scores, factor: int) -> list[float]:
    """Deterministic resolution conversion: a coarse cell's score is the max of its `factor` fine cells (an odd tail stays alone). For
    M4-Dense, whose fine score is itself a max of word logits, max-of-64 equals its native 128-byte aggregation exactly."""
    if factor < 1:
        raise ValueError("factor")
    s = list(scores)
    return [max(s[i:i + factor]) for i in range(0, len(s), factor)]


def ranking_metrics(scores, labels, seeds) -> dict:
    """Diagnostic-only metrics separating ranking from budget/seed artifacts (never replace the SEG-072 measurements).

    min_required_fraction  |seeds UNION positive| / cells: the retained fraction a perfect ranking would still need
    required_fraction      |seeds UNION top-k| / cells for the smallest k whose ranked prefix plus the seeds holds every positive cell
    ranking_excess         required - minimum: retention attributable to imperfect ranking
    last_positive_rank     rank percentile (0 = best) of the worst-ranked non-seed positive cell (None if every positive is a seed)
    """
    n = len(scores)
    seeds = {c for c in seeds if 0 <= c < n}
    pos = {c for c, y in enumerate(labels) if y == 1}
    rest = pos - seeds
    rank = rank_positions(scores)
    order = rank_order(scores)
    k = (max(rank[c] for c in rest) + 1) if rest else 0
    required = len(seeds | set(order[:k])) / n
    minimum = len(seeds | pos) / n
    return {"cells": n, "positive_cells": len(pos), "seed_cells": len(seeds), "seed_covered_positive": len(pos & seeds),
            "min_required_fraction": minimum, "required_fraction": required, "ranking_excess": required - minimum,
            "last_positive_rank": (max(rank[c] for c in rest) / n) if rest else None, "cells_needed": k,
            "all_positive": len(pos) == n and n > 0}


def budget_diagnosis(scores, labels, seeds, percent: int) -> dict:
    """At one fixed budget: is the nominal capacity sufficient for the non-seed positives, and if cells are missed, is it ranking?"""
    n = len(scores)
    seeds = {c for c in seeds if 0 <= c < n}
    rest = {c for c, y in enumerate(labels) if y == 1} - seeds
    k = budget_cells(n, percent)
    region = fixed_region(scores, seeds, percent)
    missed = rest - region
    return {"budget_cells": k, "non_seed_positive": len(rest), "capacity_sufficient": k >= len(rest), "missed_positive_cells": len(missed),
            "class": "complete" if not missed else ("ranking_failure" if k >= len(rest) else "capacity_failure"),
            "extra_cells_for_full_recall": max(0, ranking_metrics(scores, labels, seeds)["cells_needed"] - k)}


# ---------------------------------------------------------------------------------------------------------- structural expansion
def gap_fill(region, n: int) -> set[int]:
    """Experiment B1: add every unselected cell whose two neighbours are both selected (single-cell holes only)."""
    r = set(region)
    return {c for c in range(1, n - 1) if c not in r and c - 1 in r and c + 1 in r}


def token_at(tokens: bytes, word: int) -> tuple | None:
    off = word * TOKEN_FIELDS
    if word < 0 or off + TOKEN_FIELDS > len(tokens):
        return None
    return tuple(tokens[off:off + TOKEN_FIELDS])


def direct_flow_cells(tokens: bytes, size: int, roots, cell: int = CELL, limit: int = WALK_LIMIT_BYTES) -> set[int]:
    """Experiment B2: cells covered by linear fall-through from ESTABLISHED roots (production direct-control discovery addresses) using only
    the CPU-owned decode tokens: continue while the instruction decodes ordinarily with a known length (1..4 words) and is neither a
    jump, a call (the callee's return is not proven), a return nor a trap; only conditional branches and ordinary instructions continue. Branch/call targets are not followed (the tokens carry none) and nothing is invented."""
    out = set()
    for a0 in sorted(set(roots)):
        a = a0
        if a < 0 or a >= size or a & 1:
            continue
        while a < size and a - a0 < limit:
            t = token_at(tokens, a // 2)
            if t is None or t[0] != 2 or not 1 <= t[1] <= 4:
                break
            end = min(size, a + 2 * t[1])
            out.update(range(a // cell, (end - 1) // cell + 1))
            if t[3] in CONTROL_TERMINAL:
                break
            a = end
    return out


# --------------------------------------------------------------------------------------------------------------- label evidence
def code_blocks(classes: bytes) -> list[tuple[int, int, int]]:
    """Maximal runs of labelled instruction bytes (I/c): (begin, end, starts)."""
    out, i, n = [], 0, len(classes)
    while i < n:
        if classes[i] in b"Ic":
            j = i
            while j < n and classes[j] in b"Ic":
                j += 1
            out.append((i, j, classes[i:j].count(b"I")))
            i = j
        else:
            i += 1
    return out


def any_in(sorted_addrs, lo: int, hi: int) -> bool:
    k = bisect.bisect_left(sorted_addrs, lo)
    return k < len(sorted_addrs) and sorted_addrs[k] < hi


def high_byte_signature(rom: bytes, classes: bytes, data_classes: bytes, block, flank: int = FLANK_BYTES) -> float | None:
    """Histogram intersection of the high (first) byte of the block's labelled instruction-start words and of the adjacent labelled-data words
    (both bytes D) within `flank` bytes on each side. Data mis-read as code keeps the neighbouring table's byte signature; real code does not.
    None when fewer than 16 adjacent data words exist (no evidence)."""
    lo, hi = block[0], block[1]
    code = [a for a in range(lo, hi) if classes[a] == ord("I") and a + 1 < len(rom)]
    begin = max(0, lo - flank) & ~1
    data = [a for a in range(begin, min(len(rom) - 1, hi + flank), 2)
            if (a < lo or a >= hi) and data_classes[a] == ord("D") and data_classes[a + 1] == ord("D")]
    if len(data) < 16 or not code:
        return None

    def hist(addrs):
        h = {}
        for a in addrs:
            h[rom[a]] = h.get(rom[a], 0) + 1
        return {k: v / len(addrs) for k, v in h.items()}
    p, q = hist(code), hist(data)
    return sum(min(p.get(k, 0.0), q.get(k, 0.0)) for k in set(p) | set(q))


IDA_LINE = re.compile(r"^ROM:([0-9A-Fa-f]{6,8})\b")
IDA_LABEL = re.compile(r"^ROM:([0-9A-Fa-f]{6,8})\s+([A-Za-z_.@$?][\w.@$?]*):")
IDA_XREF = re.compile(r"CODE XREF:\s*([^\s\u2191\u2193]+)")
IDA_AUTO = re.compile(r"^(?:sub|loc|locret|nullsub)_([0-9A-Fa-f]+)$")


def _ida_symbol(name: str, symbols: dict) -> int | None:
    if name in symbols:
        return symbols[name]
    m = IDA_AUTO.match(name)
    return int(m.group(1), 16) if m else None


def _ida_origin(token: str, symbols: dict) -> int | None:
    """Resolve one IDA xref origin: `ROM:00012345`, `ROM:loc_123`, `name`, `name+1A`, `func:local` (+offset)."""
    base, _, off = token.partition("+")
    try:
        delta = int(off, 16) if off else 0
    except ValueError:
        return None
    if base.startswith("ROM:"):
        rest = base[4:]
        if re.fullmatch(r"[0-9A-Fa-f]{6,8}", rest):
            return int(rest, 16) + delta
        base = rest
    for name in reversed(base.split(":")):
        v = _ida_symbol(name, symbols)
        if v is not None:
            return v + delta
    return None


def parse_ida_code_xrefs(text: str, stats: dict | None = None) -> list[tuple[int, int]]:
    """(listing-line address, referencing address) for every `CODE XREF` comment of an IDA text listing (private input; offline only).
    Origins resolve through the listing's own label definitions (named routines, local `func:label` forms) and IDA's auto-name
    convention; unresolvable origins are counted in `stats` instead of being dropped silently."""
    symbols = {}
    for line in text.splitlines():
        m = IDA_LABEL.match(line)
        if m:
            symbols.setdefault(m.group(2), int(m.group(1), 16))
    out, unresolved = [], 0
    for line in text.splitlines():
        m = IDA_LINE.match(line)
        if not m:
            continue
        for x in IDA_XREF.finditer(line):
            src = _ida_origin(x.group(1), symbols)
            if src is None:
                unresolved += 1
            else:
                out.append((int(m.group(1), 16), src))
    if stats is not None:
        stats.update({"code_xrefs": len(out) + unresolved, "unresolved": unresolved, "symbols": len(symbols)})
    return out


def evidenced_origin(blocks, block_starts, support, src: int) -> bool:
    """A code reference corroborates only when its origin lies in a labelled code block that is itself supported (production root or
    independent-disassembler candidate); references from other unsupported blocks (e.g. branch-like words inside the same table) do not."""
    k = bisect.bisect_right(block_starts, src) - 1
    return k >= 0 and blocks[k][0] <= src < blocks[k][1] and support[k]


def classify_block(seed_inside: bool, independent_inside: bool, external_xrefs: int, signature: float | None,
                   threshold: float = SIGNATURE_THRESHOLD) -> str:
    """Evidence class of one labelled code block (never derived from any model score):
    CORROBORATED   a production direct-control-discovery root lies inside, or an independent disassembler's candidate lies inside AND an
                   external code reference reaches it
    PLAUSIBLE      independent-disassembler or external-reference support without a data signature
    DATA_EVIDENCE  no root, no independent candidate, no external code reference, and the block carries the adjacent data's byte signature
    UNRESOLVED     otherwise"""
    if seed_inside or (independent_inside and external_xrefs > 0):
        return "CORROBORATED"
    data_sig = signature is not None and signature >= threshold
    if (independent_inside or external_xrefs > 0) and not data_sig:
        return "PLAUSIBLE"
    if not independent_inside and external_xrefs == 0 and data_sig:
        return "DATA_EVIDENCE"
    return "UNRESOLVED"


def cell_evidence_class(block_classes) -> str:
    """A missed cell takes the weakest class of the labelled blocks holding its starts (DATA_EVIDENCE < UNRESOLVED < PLAUSIBLE < CORROBORATED)."""
    order = ["DATA_EVIDENCE", "UNRESOLVED", "PLAUSIBLE", "CORROBORATED"]
    cls = list(block_classes)
    return min(cls, key=order.index) if cls else "UNRESOLVED"


# ------------------------------------------------------------------------------------------------------------------- exposure
def m3r_epoch_segments(cells_per_image, offsets, seg: int = 32, per_batch: int = 8, order=None) -> list[tuple[int, int]]:
    """The SEG-072 M3-R epoch: per image a segment offset, segments [off, off+seg) up to the last full one, shuffled by `order`, batches of
    `per_batch` with the remainder dropped. Returns the (image, start) segments actually trained. `order` is a permutation of the segment list
    (numpy RandomState(49).permutation in the real sampler; injected here so the accounting is testable without numpy)."""
    segments = []
    for k, (n, off) in enumerate(zip(cells_per_image, offsets)):
        segments += [(k, s) for s in range(off, n - seg + 1, seg)]
    order = list(range(len(segments))) if order is None else list(order)
    if sorted(order) != list(range(len(segments))):
        raise ValueError("order is not a permutation of the segment list")
    kept = (len(order) // per_batch) * per_batch
    return [segments[i] for i in order[:kept]]


def cell_classes(classes: bytes, lineage: str, cell: int = CELL) -> list[str]:
    """Exposure class per cell: positive split into dense / mixed (some D/U, occupancy < 0.75) / sparse (occupancy <= 0.25); negative;
    masked split into sdk_runtime (U-dominated cells of SGDK homebrew) and other_masked."""
    out = []
    for c, y in enumerate(cell_labels(classes, cell)):
        chunk = classes[c * cell:(c + 1) * cell]
        occ = (chunk.count(b"I") + chunk.count(b"c")) / len(chunk)
        if y == 1:
            out.append("positive_sparse" if occ <= 0.25 else ("positive_mixed" if occ < 0.75 else "positive_dense"))
        elif y == 0:
            out.append("negative")
        else:
            sdk = lineage in SGDK_LINEAGES and chunk.count(b"U") * 2 >= len(chunk)
            out.append("masked_sdk_runtime" if sdk else "masked_other")
    return out


def rare_fragment_cells(labels, classes: bytes, cell: int = CELL) -> set[int]:
    """Positive cells that are sparse/mixed (occupancy < 0.75) or adjacent to a non-positive cell (code/data boundary)."""
    out = set()
    n = len(labels)
    for c, y in enumerate(labels):
        if y != 1:
            continue
        if occupancy(classes, c, cell) < 0.75 or (c > 0 and labels[c - 1] != 1) or (c + 1 < n and labels[c + 1] != 1):
            out.add(c)
    return out


def balanced_schedule_exposure(images, total_segments: int, seg: int = 32, weight: float = HARD_POSITIVE_WEIGHT) -> list[list[float]]:
    """Hypothetical (NOT trained) M3 schedule: lineage chosen uniformly, then a segment start uniformly over every valid start of the
    lineage's images (images by size) with segments holding a rare-fragment positive weighted `weight`; the same number of segments per
    epoch as the actual sampler. Returns the EXPECTED per-cell visits per epoch for each image. images: [(lineage, n_cells, rare_set)]."""
    lineages = sorted({im[0] for im in images})
    out = [[0.0] * im[1] for im in images]
    for L in lineages:
        members = [k for k, im in enumerate(images) if im[0] == L]
        cand = []
        for k in members:
            n, rare = images[k][1], images[k][2]
            pref = [0] * (n + 1)
            for c in range(n):
                pref[c + 1] = pref[c] + (1 if c in rare else 0)
            for s in range(0, n - seg + 1):
                cand.append((k, s, weight if pref[s + seg] - pref[s] > 0 else 1.0))
        total_w = sum(w for _, _, w in cand)
        if not total_w:
            continue
        share = total_segments / len(lineages)
        diff = {k: [0.0] * (images[k][1] + 1) for k in members}
        for k, s, w in cand:
            v = share * w / total_w
            diff[k][s] += v
            diff[k][s + seg] -= v
        for k in members:
            acc = 0.0
            for c in range(images[k][1]):
                acc += diff[k][c]
                out[k][c] = acc
    return out


# ------------------------------------------------------------------------------------------------------------------- reporting
def public(obj):
    """Strip private keys (leading underscore) before anything is written to a committed record."""
    if isinstance(obj, dict):
        return {k: public(v) for k, v in obj.items() if not str(k).startswith("_")}
    if isinstance(obj, list):
        return [public(v) for v in obj]
    if isinstance(obj, float):
        return round(obj, 6)
    return obj


def require_private_dir(path: pathlib.Path) -> None:
    """Private outputs must be git-ignored when they sit inside a repository (commercial privacy guard)."""
    path.mkdir(parents=True, exist_ok=True)
    top = subprocess.run(["git", "-C", str(path), "rev-parse", "--show-toplevel"], capture_output=True, text=True)
    if top.returncode == 0 and subprocess.run(["git", "-C", str(path), "check-ignore", "-q", "--", str(path)]).returncode != 0:
        raise SystemExit(f"private output directory is tracked by git: {path}")


# ======================================================================================================== private commands (numpy/torch)
def _env(bundle: Bundle) -> None:
    os.environ["SEG072_ROOT"] = str(bundle.payload / "seg072")
    os.environ["SEG049_ROOT"] = str(bundle.payload / "seg049")


def _np():
    import numpy
    return numpy


def registry(bundle: Bundle) -> dict:
    out = {}
    for rel in sorted(r for r in bundle.files if r.startswith("seg072/data/") and r.endswith("/meta.json")):
        m = json.loads(bundle.file(rel).read_text())
        if "audit" not in m:
            continue
        m["dir"] = str(bundle.payload / rel[:-len("/meta.json")])
        out[m["id"]] = m
    return out


def load_dev_image(bundle: Bundle, m: dict):
    import segarecomp_ml_v3_train as tr
    d = pathlib.Path(m["dir"])
    for name in ("classes.bin", "tokens.bin", "seeds.json"):
        bundle.file(str((d / name).relative_to(bundle.payload)))
    return tr.ExternalImage(bundle.resolve_image(m), d / "tokens.bin", d / "classes.bin", json.loads((d / "seeds.json").read_text()), m["id"], m["lineage"])


def seed_addresses(m: dict) -> list[int]:
    d = pathlib.Path(m["dir"])
    p = d / ("d.txt" if m["kind"] == "genesis_rom" else "seed_starts.txt")
    return [int(t, 16) for t in p.read_text().split()] if p.exists() else []


def cmd_smoke(a) -> int:
    bundle = Bundle(a.bundle, parse_rebase(a.rebase))
    _env(bundle)
    roots = bundle.original_roots()
    opened: list[str] = []

    def hook(event, args):
        if event == "open" and args and isinstance(args[0], (str, bytes, os.PathLike)):
            opened.append(os.fsdecode(args[0]))
    sys.addaudithook(hook)
    np = _np()
    import segarecomp_ml_v3 as v3
    import segarecomp_ml_v3_eval as ev
    import segarecomp_ml_v3_train as tr
    v3.verify_plan()
    if pathlib.Path(ev.ROOT).resolve() != (bundle.payload / "seg072").resolve() or pathlib.Path(tr.FINAL).resolve() != (bundle.payload / "seg072/final").resolve():
        raise ArtifactError("original_root_access", "research modules did not bind to the bundle payload")
    reg = registry(bundle)
    resolved = {}
    for iid, m in reg.items():
        p = bundle.resolve_image(m)
        resolved[iid] = "bundled" if str(p).startswith(str(bundle.payload)) else "external"
    stored = np.load(bundle.file("seg072/scores/M3_FROZEN.production.npz"))
    scorer = tr.final_scorer("M3_FROZEN")
    repro = {}
    for iid in a.images:
        m = reg[iid]
        im = load_dev_image(bundle, m)
        s, cell = scorer(im)
        got = sha256_bytes(np.asarray(s, dtype=np.float64).tobytes())
        want = sha256_bytes(np.asarray(stored[iid.replace("/", "@@")], dtype=np.float64).tobytes())
        repro[iid] = {"source": resolved[iid], "kind": m["kind"], "cells": int(len(s)), "cell_bytes": cell, "classes": len(im.classes),
                      "starts": len(im.starts), "seed_cells": len(im.seeds), "bitwise_equal_stored_production_scores": got == want}
    folds = v3.lolo_folds(tr.qualified_lineages())
    res = ev.evaluate_config("M3_R", {k: dict(v) for k, v in reg.items()}, folds)
    summary = json.loads(bundle.file("seg072/results/table.summary.json").read_text())["M3_R"]["pairs"]
    table_ok = {pol: res["pairs"][pol] == summary[pol] for pol in summary}
    allow = sorted({os.path.abspath(sys.prefix), os.path.abspath(sys.base_prefix)})
    bad = forbidden_opens(opened, roots, allow)
    rep = {"schema": SCHEMA, "command": "smoke", "manifest_sha256": bundle.manifest_sha256, "bundled_files": len(bundle.files),
           "external_inputs": len(bundle.external), "registry_images": len(reg),
           "resolution": {"bundled": sum(1 for v in resolved.values() if v == "bundled"), "external": sum(1 for v in resolved.values() if v == "external")},
           "reproduced_production_m3": repro, "m3r_table_pairs_reproduced": table_ok, "original_roots_checked": len(roots),
           "interpreter_environment_under_original_root": any(_under(x, r) for x in allow for r in roots),
           "original_root_opens": len(bad), "_original_root_open_dirs": sorted({str(pathlib.Path(b).parent) for b in bad})[:20]}
    ok = all(r["bitwise_equal_stored_production_scores"] for r in repro.values()) and all(table_ok.values()) and not bad and len(reg) == 50
    rep["verdict"] = "PASS" if ok else "FAIL"
    if a.out:
        out = pathlib.Path(a.out)
        require_private_dir(out.parent)
        out.write_text(json.dumps(rep, indent=1, sort_keys=True))
    print(json.dumps(public(rep), indent=1, sort_keys=True))
    return 0 if ok else 1


def sealed_image(bundle: Bundle, L: str):
    import segarecomp_ml_v3_train as tr
    data = bundle.payload / "seg072/sealed/data" / L.lower()
    bundle.file(f"seg072/sealed/data/{L.lower()}/tokens.bin")
    cls = bundle.file("seg072/sealed/truth/" + SEALED_CLASSES[L])
    img = bundle.sealed_image(L)
    seeds = [int(t, 16) for t in (data / "d.txt").read_text().split()] if (data / "d.txt").exists() else []
    im = tr.ExternalImage(img, data / "tokens.bin", cls, [x // CELL for x in seeds if x < img.stat().st_size], L, L)
    return im, seeds


def cmd_infer(a) -> int:
    """Sealed per-cell scores by inference with the preserved weights (no training, no label read for scoring)."""
    bundle = Bundle(a.bundle, parse_rebase(a.rebase))
    _env(bundle)
    np = _np()
    import torch
    import segarecomp_ml_v3_train as tr
    out = pathlib.Path(a.out)
    require_private_dir(out)
    torch.use_deterministic_algorithms(True)
    scorers = {"M3_FROZEN": tr.final_scorer("M3_FROZEN"), "M3_R": tr.final_scorer("M3_R")}
    m4 = []
    for s in tr.M4["seeds"]:
        m = tr.build_m4(torch)
        m.load_state_dict(torch.load(bundle.file(f"seg072/final/M4.s{s}.pt")))
        m.eval()
        m4.append(m)
    bundle.file("seg072/final/M3R.pt")
    arrays, ident = {}, {}
    for L in SEALED_CLASSES:
        im, _ = sealed_image(bundle, L)
        for name, f in scorers.items():
            s, cell = f(im)
            arrays[f"{L}.{name}"] = np.asarray(s, dtype=np.float64)
        torch.set_num_threads(tr.M4["threads"])
        wl = np.mean([tr.infer_m4(m, im) for m in m4], axis=0).astype(np.float32)
        arrays[f"{L}.M4_DENSE@128"] = tr.cell_max(wl, im.size, 128)
        arrays[f"{L}.M4_DENSE@64"] = tr.cell_max(wl, im.size, 64)
        print("inferred", L, flush=True)
    for k, v in arrays.items():
        ident[k] = {"cells": int(len(v)), "cell_bytes": 64 if k.endswith("@64") else 128, "precision": "float32 network, float64 cell scores",
                    "sha256": sha256_bytes(v.tobytes())}
    np.savez(out / "sealed_scores.npz", **{k.replace("@", "_at_"): v for k, v in arrays.items()})
    weights = {"M3R": json.loads(bundle.file("seg072/final/M3R.digests.json").read_text()),
               "M4": json.loads(bundle.file("seg072/final/M4.digests.json").read_text()),
               "M3_FROZEN_model_sha256": sha256_file(HERE / "segarecomp_ml_region_gen2.model.json")}
    (out / "sealed_scores.json").write_text(json.dumps({"schema": SCHEMA, "lineage": "inference with preserved final weights (bundle "
                                                        + bundle.manifest_sha256[:16] + "); no training", "weights": weights, "arrays": ident},
                                                       indent=1, sort_keys=True))
    print(json.dumps({"arrays": len(arrays)}))
    return 0


def load_sealed_scores(out: pathlib.Path) -> dict:
    np = _np()
    with np.load(out / "sealed_scores.npz") as z:
        return {k.replace("_at_", "@"): z[k].astype(np.float64) for k in z.files}


# ------------------------------------------------------------------------------------------------------------------- analyses
DEV_MODELS = ("M3_FROZEN", "M1_HGB", "M3_R", "M4_DENSE@128", "M4_DENSE@64")
SEALED_MODELS = ("M3_FROZEN", "M3_R", "M4_DENSE@128", "M4_DENSE@64")


class Case:
    """One image for analysis: labels at 128 (and 64) bytes, seeds, starts, per-model scores; private."""

    def __init__(self, iid, lineage, kind, size, classes, seeds_addr, scores: dict, tokens: bytes | None, universe=None, raw_classes=None, rom=None):
        self.id, self.lineage, self.kind, self.size, self.classes = iid, lineage, kind, size, classes
        self.seeds_addr = [x for x in seeds_addr if 0 <= x < size]
        self.scores = {k: [float(x) for x in v] for k, v in scores.items()}
        self.tokens, self.universe, self.raw, self.rom = tokens, universe, raw_classes, rom
        self.labels = {128: cell_labels(classes, 128), 64: cell_labels(classes, 64)}
        self.starts = [i for i, b in enumerate(classes) if b == ord("I")]

    def universe_cells(self) -> dict | None:
        if self.universe is None:
            return None
        if not hasattr(self, "_ucells"):
            self._ucells = {}
            for x in self.universe:
                self._ucells[x // CELL] = self._ucells.get(x // CELL, 0) + 1
        return self._ucells

    def seeds(self, cell=CELL):
        return {x // cell for x in self.seeds_addr}

    def cell_of(self, model: str) -> int:
        return 64 if model.endswith("@64") else 128


def dev_cases(bundle: Bundle) -> list:
    np = _np()
    import segarecomp_ml_v3 as v3
    import segarecomp_ml_v3_eval as ev
    import segarecomp_ml_v3_train as tr
    reg = registry(bundle)
    folds = v3.lolo_folds(tr.qualified_lineages())
    sc = {c: ev.config_scores(c, {k: dict(v) for k, v in reg.items()}, folds) for c in DEV_MODELS}
    cases = []
    for iid, m in reg.items():
        d = pathlib.Path(m["dir"])
        cls = (d / "classes.bin").read_bytes()
        uni = [int(x, 16) for x in (d / "u.txt").read_text().split()] if (d / "u.txt").exists() else None
        cases.append(Case(iid, m["lineage"], m["kind"], m["size"], cls, seed_addresses(m), {c: sc[c][iid][0] for c in DEV_MODELS if iid in sc[c]},
                          (d / "tokens.bin").read_bytes(), uni))
    del np
    return cases


def sealed_cases(bundle: Bundle, out: pathlib.Path) -> list:
    scores = load_sealed_scores(out)
    cases = []
    for L in SEALED_CLASSES:
        data = bundle.payload / "seg072/sealed/data" / L.lower()
        img = bundle.sealed_image(L)
        rom = img.read_bytes()
        cls = bundle.file("seg072/sealed/truth/" + SEALED_CLASSES[L]).read_bytes()
        raw = bundle.file("seg072/sealed/truth/" + SEALED_RAW[L]).read_bytes() if L in SEALED_RAW else None
        seeds = [int(t, 16) for t in (data / "d.txt").read_text().split()] if (data / "d.txt").exists() else []
        uni = [int(x, 16) for x in (data / "u.txt").read_text().split()] if (data / "u.txt").exists() else None
        cases.append(Case(L, L, "genesis_rom", len(rom), cls, seeds, {m: scores[f"{L}.{m}"] for m in SEALED_MODELS},
                          bundle.file(f"seg072/sealed/data/{L.lower()}/tokens.bin").read_bytes(), uni, raw, rom))
    return cases


def check_sealed_consistency(bundle: Bundle, cases) -> dict:
    """The regenerated sealed scores must reproduce the stored frozen M3-R regions (all five lineages) and the stored post-hoc M4 aggregates."""
    priv = json.loads(bundle.file("seg072/results/sealed.private.json").read_text())["lineages"]
    post = json.loads(bundle.file("seg072/results/posthoc.m4_sealed.json").read_text())
    out = {"m3r_b50_regions_equal": {}, "m4_posthoc_misses_equal": {}}
    for c in cases:
        r = fixed_region(c.scores["M3_R"], c.seeds(), 50)
        out["m3r_b50_regions_equal"][c.id] = sorted(r) == priv[c.id]["_region"]
        for cfg in ("M4_DENSE@128", "M4_DENSE@64"):
            cell = c.cell_of(cfg)
            for pol, pct in (("B35", 35), ("B45", 45), ("B50", 50)):
                reg = fixed_region(c.scores[cfg], c.seeds(cell), pct)
                out["m4_posthoc_misses_equal"][f"{cfg}+{pol}+{c.id}"] = missed_starts(c.starts, reg, cell) == post[f"{cfg}+{pol}+{c.id}"]["starts_missed"]
    out["all_equal"] = all(out["m3r_b50_regions_equal"].values()) and all(out["m4_posthoc_misses_equal"].values())
    return out


def analysis_label_credibility(sealed, ghidra: dict, ida: dict) -> dict:
    """Section 4: evidence class of every labelled block of the sealed lineages with an independent-disassembler input, and of every cell
    that any model misses at B35/B45/B50 (evidence never uses a model score); diagnostic credibility-adjusted miss counts (SEG-072 labels and
    results are unchanged)."""
    res = {}
    for c in sealed:
        if c.id not in ghidra:
            continue
        g = sorted(ghidra[c.id])
        xrefs = ida.get(c.id, [])
        seeds = sorted(c.seeds_addr)
        data_cls = c.raw if c.raw is not None else c.classes
        blocks = []
        raw_blocks = code_blocks(c.classes)
        starts_of_blocks = [b[0] for b in raw_blocks]
        support = [any_in(seeds, b[0], b[1]) or any_in(g, b[0], b[1]) for b in raw_blocks]
        for i, b in enumerate(raw_blocks):
            refs = [src for tgt, src in xrefs if b[0] <= tgt < b[1] and not b[0] <= src < b[1]]
            ext = sum(1 for src in refs if evidenced_origin(raw_blocks, starts_of_blocks, support, src))
            sig = high_byte_signature(c.rom, c.classes, data_cls, b)
            cls = classify_block(any_in(seeds, b[0], b[1]), any_in(g, b[0], b[1]), ext, sig)
            blocks.append({"_range": (b[0], b[1]), "starts": b[2], "class": cls, "signature": sig, "external_code_xrefs": ext,
                           "unevidenced_code_xrefs": len(refs) - ext, "seed": any_in(seeds, b[0], b[1]),
                           "independent": any_in(g, b[0], b[1])})
        supported = [x["signature"] for x in blocks if (x["seed"] or x["independent"]) and x["signature"] is not None]
        by_class = {}
        for x in blocks:
            k = by_class.setdefault(x["class"], {"blocks": 0, "starts": 0})
            k["blocks"] += 1
            k["starts"] += x["starts"]
        data_starts = {a for x in blocks if x["class"] == "DATA_EVIDENCE" for a in range(x["_range"][0], x["_range"][1]) if c.classes[a] == ord("I")}
        missed, adjusted = {}, {}
        for model in SEALED_MODELS:
            cell = c.cell_of(model)
            for pol, pct in BUDGETS.items():
                region = fixed_region(c.scores[model], c.seeds(cell), pct)
                miss = [a for a in c.starts if a // cell not in region]
                adjusted[f"{model}+{pol}"] = {"misses": len(miss), "misses_excluding_data_evidence": sum(1 for a in miss if a not in data_starts)}
                for a in miss:
                    missed.setdefault(a // CELL, set()).add(f"{model}+{pol}")
        cells = []
        for cc, models in sorted(missed.items()):
            lo, hi = cc * CELL, (cc + 1) * CELL
            hit = [x for x in blocks if x["_range"][0] < hi and x["_range"][1] > lo and any(lo <= s < hi for s in c.starts if x["_range"][0] <= s < x["_range"][1])]
            by_cls = {}
            for x in hit:
                n_in = sum(1 for s in c.starts if lo <= s < hi and x["_range"][0] <= s < x["_range"][1])
                by_cls[x["class"]] = by_cls.get(x["class"], 0) + n_in
            cells.append({"_cell": cc, "missed_by": sorted(models), "starts": sum(1 for s in c.starts if lo <= s < hi),
                          "occupancy": occupancy(c.classes, cc), "class": cell_evidence_class(x["class"] for x in hit), "starts_by_class": by_cls,
                          "block_signatures": sorted(round(x["signature"], 3) for x in hit if x["signature"] is not None),
                          "block_external_code_xrefs": sum(x["external_code_xrefs"] for x in hit),
                          "block_unevidenced_code_xrefs": sum(x["unevidenced_code_xrefs"] for x in hit), "block_roots": any(x["seed"] for x in hit),
                          "block_independent_candidates": any(x["independent"] for x in hit)})
        res[c.id] = {"blocks": len(blocks), "labelled_starts": len(c.starts), "by_class": by_class,
                     "independent_candidates": len(g), "supported_block_signature_max": max(supported) if supported else None,
                     "signature_threshold": SIGNATURE_THRESHOLD,
                     "unsupported_blocks_at_or_above_threshold": sum(1 for x in blocks if not (x["seed"] or x["independent"])
                                                                     and x["signature"] is not None and x["signature"] >= SIGNATURE_THRESHOLD),
                     "data_evidence_starts": len(data_starts), "missed_cells": cells, "credibility_adjusted_misses": adjusted, "_blocks": blocks}
    return res


def analysis_signature_control(bundle: Bundle, cases) -> dict:
    """Negative control of the flank-data signature on development truth extracted from source listings (no IDA rows): how many labelled
    code blocks with adjacent known data reach the threshold, and how many of those hold a production direct-control root."""
    out = {"threshold": SIGNATURE_THRESHOLD, "images": 0, "blocks_with_signature": 0, "blocks_at_or_above": 0, "starts_at_or_above": 0,
           "rooted_blocks_at_or_above": 0, "labelled_starts": 0, "max_rooted_signature": None}
    reg = registry(bundle)
    rooted_max = None
    for c in cases:
        if c.kind != "genesis_rom" or b"D" not in c.classes:
            continue
        rom = bundle.resolve_image(reg[c.id]).read_bytes()
        seeds = sorted(c.seeds_addr)
        out["images"] += 1
        out["labelled_starts"] += len(c.starts)
        for b in code_blocks(c.classes):
            sig = high_byte_signature(rom, c.classes, c.classes, b)
            if sig is None:
                continue
            rooted = any_in(seeds, b[0], b[1])
            out["blocks_with_signature"] += 1
            if rooted:
                rooted_max = sig if rooted_max is None else max(rooted_max, sig)
            if sig >= SIGNATURE_THRESHOLD:
                out["blocks_at_or_above"] += 1
                out["starts_at_or_above"] += b[2]
                out["rooted_blocks_at_or_above"] += 1 if rooted else 0
    out["max_rooted_signature"] = rooted_max
    return out


def model_view(c: Case, model: str):
    """(scores at 128-byte cells, native cell bytes): M4@64 is converted with the documented max-of-two rule."""
    s = c.scores[model]
    return (to_coarser_cells(s, 2) if model.endswith("@64") else s), (64 if model.endswith("@64") else 128)


def analysis_complementarity(cases, models) -> dict:
    """Section 5: positive cells outside any model's B45/B50 ranked selection ("hard" cells); per hard cell the rank percentile under every
    model at the common 128-byte unit, occupancy, boundary distance, seed coverage, whether another model ranks it inside; per model the extra
    cells needed for full recall."""
    out = {"models": list(models), "budgets": {}}
    for pol in ("B35", "B45", "B50"):
        pct = BUDGETS[pol]
        hard = []
        extra = {m: [] for m in models}
        for c in cases:
            labels = c.labels[128]
            seeds = c.seeds()
            n = len(labels)
            k = budget_cells(n, pct)
            ranks = {}
            for m in models:
                if m not in c.scores:
                    continue
                s, _ = model_view(c, m)
                ranks[m] = rank_positions(s)
                extra[m].append(budget_diagnosis(s, labels, seeds, pct)["extra_cells_for_full_recall"])
            for cc, y in enumerate(labels):
                if y != 1 or cc in seeds:
                    continue
                inside = {m: ranks[m][cc] < k for m in ranks}
                if all(inside.values()):
                    continue
                hard.append({"_cell": cc, "lineage": c.lineage, "image": c.id, "inside": inside, "rank_pct": {m: ranks[m][cc] / n for m in ranks},
                             "occupancy": occupancy(c.classes, cc), "boundary_distance": boundary_distance(labels, cc)})
        rows = {}
        for m in models:
            mine = [h for h in hard if m in h["inside"] and not h["inside"][m]]
            rows[m] = {"hard_cells": len(mine), "rescued_by_another_model": sum(1 for h in mine if any(v for k2, v in h["inside"].items() if k2 != m)),
                       "images_needing_extra_cells": sum(1 for e in extra[m] if e), "extra_cells_total": sum(extra[m])}
        systematic = [h for h in hard if not any(h["inside"].values())]
        out["budgets"][pol] = {
            "hard_cells_any_model": len(hard), "systematic_cells_missed_by_every_model": len(systematic),
            "systematic_by_lineage": _count(h["lineage"] for h in systematic),
            "complementary_cells": len(hard) - len(systematic), "per_model": rows,
            "hard_cell_mean_occupancy": _mean(h["occupancy"] for h in hard),
            "systematic_mean_occupancy": _mean(h["occupancy"] for h in systematic),
            "hard_cell_boundary_distance_le_1": sum(1 for h in hard if h["boundary_distance"] is not None and h["boundary_distance"] <= 1),
            "pairwise_overlap": {f"{x}|{y}": sum(1 for h in hard if x in h["inside"] and y in h["inside"] and not h["inside"][x] and not h["inside"][y])
                                 for i, x in enumerate(models) for y in models[i + 1:]},
            "_hard": hard}
    return out


def _count(items) -> dict:
    d = {}
    for i in items:
        d[i] = d.get(i, 0) + 1
    return dict(sorted(d.items()))


def _mean(xs):
    xs = list(xs)
    return sum(xs) / len(xs) if xs else None


def analysis_metrics(bundle: Bundle, cases) -> dict:
    """Section 7: diagnostic-only metrics per lineage and the D07 / degenerate-FRF explanations; original SEG-072 measurements reproduced
    unchanged next to them."""
    sel = json.loads(bundle.file("seg072/results/selection.json").read_text())["table"]
    original = {k: {"misses": v["misses"], "mean_retained": v["mean_retained"], "worst_frf": v["worst_frf"], "complete": v["complete"]}
                for k, v in sel.items() if k.split("+")[1] in ("B35", "B45", "B50")}
    per = {}
    d07 = {}
    for m in DEV_MODELS:
        rows = []
        for c in cases:
            if m not in c.scores:
                continue
            cell = c.cell_of(m)
            labels = c.labels[cell]
            rm = ranking_metrics(c.scores[m], labels, c.seeds(cell))
            rm.update({"lineage": c.lineage, "id": c.id})
            rows.append(rm)
            if c.id == "d07":
                d07[m] = {"positive_cells": rm["positive_cells"], "cells": rm["cells"], "seed_cells": rm["seed_cells"],
                          "seed_covered_positive": rm["seed_covered_positive"], "min_required_fraction": rm["min_required_fraction"],
                          "required_fraction": rm["required_fraction"], "last_positive_rank": rm["last_positive_rank"],
                          "budgets": {pol: budget_diagnosis(c.scores[m], labels, c.seeds(cell), pct) for pol, pct in BUDGETS.items()}}
        degenerate = [r for r in rows if r["all_positive"]]
        by_l = {}
        for r in rows:
            L = by_l.setdefault(r["lineage"], {"images": 0, "max_min_required": 0.0, "max_required": 0.0, "max_ranking_excess": 0.0,
                                               "max_last_positive_rank": 0.0})
            L["images"] += 1
            L["max_min_required"] = max(L["max_min_required"], r["min_required_fraction"])
            L["max_required"] = max(L["max_required"], r["required_fraction"])
            L["max_ranking_excess"] = max(L["max_ranking_excess"], r["ranking_excess"])
            L["max_last_positive_rank"] = max(L["max_last_positive_rank"], r["last_positive_rank"] or 0.0)
        per[m] = {"lineages": by_l, "all_positive_images": len(degenerate),
                  "all_positive_images_fully_seed_covered": sum(1 for r in degenerate if r["seed_covered_positive"] == r["positive_cells"]),
                  "worst_lineage_last_positive_rank": max((r["last_positive_rank"] or 0.0) for r in rows),
                  "worst_lineage_ranking_excess": max(r["ranking_excess"] for r in rows)}
    overhead = {}
    for name in ("kstage.M3_FROZEN.B35.json", "kstage.M3_R.B50.json", "kstage.M4_DENSE@64.B50.json"):
        rel = "seg072/results/" + name
        if rel not in bundle.files:
            continue
        ks = json.loads(bundle.file(rel).read_text())["images"]
        rows = []
        for c in cases:
            k = ks.get(c.id)
            if not k or k.get("validator") != "accepted":
                continue
            in_k = len(c.starts) - k["c_outside_k"]
            rows.append({"k_over_u": k["k_over_u"], "non_labelled_entry_share": (k["k"] - in_k) / k["k"], "entries_per_labelled_start": k["k"] / max(1, len(c.starts))})
        overhead[name[len("kstage."):-len(".json")]] = {"images": len(rows), "mean_k_over_u": _mean(r["k_over_u"] for r in rows),
                                                        "mean_non_labelled_entry_share": _mean(r["non_labelled_entry_share"] for r in rows),
                                                        "mean_entries_per_labelled_start": _mean(r["entries_per_labelled_start"] for r in rows)}
    return {"original_seg072_pairs_unchanged": original, "diagnostic": per, "d07": d07, "post_validator_overhead": overhead}


def experiment_fusion(cases) -> dict:
    """Experiment A: M3-R and M4-Dense (128-byte unit; M4@64 converted by max-of-two = its native @128 aggregation) and their two fixed
    fusions, at identical cell budgets (hence identical ranked-byte budgets) with the same seeds."""
    methods = {"M3_R": lambda c: c.scores["M3_R"], "M4_DENSE@128": lambda c: c.scores["M4_DENSE@128"],
               "FUSE_MEAN_RANK": lambda c: fuse_mean_rank(c.scores["M3_R"], c.scores["M4_DENSE@128"]),
               "FUSE_RRF60": lambda c: fuse_rrf(c.scores["M3_R"], c.scores["M4_DENSE@128"])}
    conv_equal = sum(1 for c in cases if "M4_DENSE@64" in c.scores and to_coarser_cells(c.scores["M4_DENSE@64"], 2) == c.scores["M4_DENSE@128"])
    out = {"m4_64_to_128_equals_native_128": f"{conv_equal}/{sum(1 for c in cases if 'M4_DENSE@64' in c.scores)}", "rrf_k": RRF_K, "methods": {}}
    for name, f in methods.items():
        res = {}
        for pol, pct in BUDGETS.items():
            per_l, misses, imgs_missing, rb = {}, 0, 0, []
            for c in cases:
                s = f(c)
                region = fixed_region(s, c.seeds(), pct)
                ms = missed_starts(c.starts, region)
                misses += ms
                imgs_missing += 1 if ms else 0
                per_l.setdefault(c.lineage, []).append(region_bytes(region, CELL, c.size) / c.size)
                rb.append((c.id, ms))
            res[pol] = {"complete": misses == 0, "misses": misses, "images_missing": imgs_missing,
                        "mean_retained": _mean(_mean(v) for v in per_l.values()), "_per_image": dict(rb)}
        out["methods"][name] = res
    for pol in BUDGETS:
        base = {m: out["methods"][m][pol]["_per_image"] for m in ("M3_R", "M4_DENSE@128")}
        for fname in ("FUSE_MEAN_RANK", "FUSE_RRF60"):
            fz = out["methods"][fname][pol]["_per_image"]
            out["methods"][fname][pol]["images_worse_than_best_standalone"] = sum(1 for i in fz if fz[i] > min(base["M3_R"][i], base["M4_DENSE@128"][i]))
            out["methods"][fname][pol]["images_worse_than_m3r"] = sum(1 for i in fz if fz[i] > base["M3_R"][i])
            out["methods"][fname][pol]["images_better_than_m3r"] = sum(1 for i in fz if fz[i] < base["M3_R"][i])
    return out


def experiment_expansion(cases, model: str = "M3_R") -> dict:
    """Experiment B: bounded deterministic expansion of the model's fixed-budget region: B1 single-cell gap fill, B2 direct-flow continuation
    from production direct-control roots (decode tokens only), B1+B2. Offline only: never execution authority, validator unchanged."""
    out = {"model": model, "walk_limit_bytes": WALK_LIMIT_BYTES, "policies": {}}
    for pol, pct in BUDGETS.items():
        rows = {"base": [], "B1_gap_fill": [], "B2_direct_flow": [], "B1_B2": []}
        for c in cases:
            n = len(c.labels[128])
            base = fixed_region(c.scores[model], c.seeds(), pct)
            flow = direct_flow_cells(c.tokens, c.size, c.seeds_addr) if c.tokens else set()
            variants = {"base": base, "B1_gap_fill": base | gap_fill(base, n), "B2_direct_flow": base | flow}
            variants["B1_B2"] = variants["B2_direct_flow"] | gap_fill(variants["B2_direct_flow"], n)
            uni = c.universe_cells()
            for k, r in variants.items():
                added = r - base
                rows[k].append({"lineage": c.lineage, "misses": missed_starts(c.starts, r), "retained": region_bytes(r, CELL, c.size) / c.size,
                                "added_bytes": region_bytes(added, CELL, c.size),
                                "added_universe_entries": (sum(uni.get(x, 0) for x in added) if uni is not None else None),
                                "universe": len(c.universe) if uni is not None else None})
        res = {}
        for k, rs in rows.items():
            by_l = {}
            for r in rs:
                by_l.setdefault(r["lineage"], []).append(r["retained"])
            ue = [r for r in rs if r["added_universe_entries"] is not None]
            res[k] = {"misses": sum(r["misses"] for r in rs), "images_missing": sum(1 for r in rs if r["misses"]),
                      "mean_retained": _mean(_mean(v) for v in by_l.values()), "added_bytes_total": sum(r["added_bytes"] for r in rs),
                      "added_universe_entries_total": sum(r["added_universe_entries"] for r in ue) if ue else None,
                      "added_universe_fraction_mean": _mean(r["added_universe_entries"] / r["universe"] for r in ue if r["universe"]) if ue else None}
        out["policies"][pol] = res
    return out


def m3r_actual_exposure(cases, epochs: int = 30, seed: int = 49) -> list[list[int]]:
    """Replay of the SEG-072 M3-R final-fit sampler (numpy RandomState(seed); the exact call order of `train_m3r`); per-cell visit counts."""
    np = _np()
    rng = np.random.RandomState(seed)
    visits = [[0] * len(c.labels[128]) for c in cases]
    for _ in range(epochs):
        offs = [int(rng.randint(0, 32)) for _ in cases]
        segs = []
        for k, (c, off) in enumerate(zip(cases, offs)):
            segs += [(k, s) for s in range(off, len(c.labels[128]) - 32 + 1, 32)]
        order = rng.permutation(len(segs))
        for k, s in m3r_epoch_segments([len(c.labels[128]) for c in cases], offs, order=[int(i) for i in order]):
            for cc in range(s, s + 32):
                visits[k][cc] += 1
    return visits


def m4_actual_exposure(cases, seeds=(1, 2, 3), steps: int = 4000, batch: int = 8, central: int = 2048, quantum: int = 16) -> list[list[float]]:
    """Replay of the SEG-072 M4-Dense final-fit window sampler (numpy default_rng(seed), the call order of `fit_m4`); per-cell labelled-word
    visits (central words only), summed over the three seeds."""
    np = _np()
    lineages = sorted({c.lineage for c in cases})
    by_lin = {L: [k for k, c in enumerate(cases) if c.lineage == L] for L in lineages}
    sizes = {L: np.array([cases[k].size // 2 for k in by_lin[L]], dtype=np.float64) for L in lineages}
    visits = [np.zeros(len(c.labels[128]), dtype=np.float64) for c in cases]
    for sd in seeds:
        rng = np.random.default_rng(sd)
        for _ in range(steps):
            for _ in range(batch):
                L = lineages[int(rng.integers(0, len(lineages)))]
                k = by_lin[L][int(rng.choice(len(by_lin[L]), p=sizes[L] / sizes[L].sum()))]
                nw = cases[k].size // 2
                lo_q, hi_q = -(central // 2) // quantum, (nw - central // 2) // quantum
                start = int(rng.integers(lo_q, max(lo_q, hi_q) + 1)) * quantum
                a, b = max(0, start), min(nw, start + central)
                if b > a:
                    c0, c1 = (2 * a) // CELL, (2 * b - 1) // CELL
                    visits[k][c0:c1 + 1] += 1
    return [v.tolist() for v in visits]


def analysis_exposure(cases, model_cases_hard=None) -> dict:
    """Section 6: training exposure of the actual M3-R and M4-Dense final fits by lineage and cell class, and of the hypothetical
    lineage-balanced, hard-positive-aware M3 schedule (expected visits; NOT trained)."""
    m3 = m3r_actual_exposure(cases)
    m4 = m4_actual_exposure(cases)
    classes = [cell_classes(c.classes, c.lineage) for c in cases]
    rare = [rare_fragment_cells(c.labels[128], c.classes) for c in cases]
    total_segments = sum(sum(v) for v in m3) / 32 / 30
    hyp = balanced_schedule_exposure([(c.lineage, len(c.labels[128]), rare[k]) for k, c in enumerate(cases)], total_segments)

    def summarize(visits, scale):
        tot = sum(sum(v) for v in visits) or 1.0
        by_l, by_c, rare_v = {}, {}, 0.0
        for k, c in enumerate(cases):
            s = sum(visits[k])
            by_l[c.lineage] = by_l.get(c.lineage, 0.0) + s
            for cc, cl in enumerate(classes[k]):
                by_c[cl] = by_c.get(cl, 0.0) + visits[k][cc]
            rare_v += sum(visits[k][cc] for cc in rare[k])
        pos = sum(v for k, v in by_c.items() if k.startswith("positive"))
        return {"lineage_share": {L: v / tot for L, v in sorted(by_l.items())}, "class_share": {k: v / tot for k, v in sorted(by_c.items())},
                "rare_fragment_share_of_positive": rare_v / pos if pos else None,
                "unexposed_images": sum(1 for v in visits if sum(v) == 0), "visits_total": tot * scale}
    cells = {"images": len(cases), "cells_by_lineage": {}, "class_cells": {}, "rare_fragment_cells": sum(len(r) for r in rare),
             "images_below_32_cells": sum(1 for c in cases if len(c.labels[128]) < 32)}
    for k, c in enumerate(cases):
        cells["cells_by_lineage"][c.lineage] = cells["cells_by_lineage"].get(c.lineage, 0) + len(classes[k])
        for cl in classes[k]:
            cells["class_cells"][cl] = cells["class_cells"].get(cl, 0) + 1
    out = {"corpus": cells, "m3r_actual": summarize(m3, 1), "m4_actual": summarize(m4, 1),
           "hypothetical_balanced_hard_positive": summarize(hyp, 30), "hypothetical_definition": {
               "lineage": "uniform over training lineages", "segment": "uniform over every valid 32-cell start of the lineage's images",
               "hard_positive_weight": HARD_POSITIVE_WEIGHT, "rare_fragment": "positive cell with occupancy < 0.75 or adjacent to a non-positive cell",
               "segments_per_epoch": round(total_segments), "epochs": 30, "status": "expected exposure only; NOT trained"}}
    if model_cases_hard:
        # visits of the observed hard cells (development B50 complementarity) under the hypothetical schedule vs the actual M3-R final fit
        idx = {c.id: k for k, c in enumerate(cases)}
        act = hypv = 0.0
        unexposed = 0
        for h in model_cases_hard:
            k = idx.get(h["image"])
            if k is None:
                continue
            a_v, h_v = m3[k][h["_cell"]], hyp[k][h["_cell"]] * 30
            act += a_v
            hypv += h_v
            unexposed += 1 if a_v == 0 else 0
        out["hard_cells_b50"] = {"cells": len(model_cases_hard), "unexposed_under_actual_m3r": unexposed, "actual_m3r_visits": act,
                                 "hypothetical_expected_visits": hypv, "multiplier": (hypv / act) if act else None}
    return out


def experiment_feasibility(bundle: Bundle) -> dict:
    """Experiment C: training/build cost estimates from SEG-072's recorded CPU measurements (no training)."""
    secs = {"M3R": [], "M4": [], "M1": []}
    for rel in bundle.files:
        if rel.startswith("seg072/scores/") and rel.endswith(".json"):
            r = json.loads(bundle.file(rel).read_text())
            secs.setdefault(r["model"], []).append(r["seconds"])
    hw = json.loads(bundle.file("seg072/results/hardware.json").read_text())
    final_m3r = 574.0  # ADR 0153: final M3-R refit seconds (seed 49, 30 epochs, 4 threads)
    m4_fit = 4000 * hw["cpu_farm"]["ms_per_step_median"] / 1000.0
    return {"recorded": {"m3r_fold_seconds_sum": sum(secs["M3R"]), "m3r_folds": len(secs["M3R"]), "m3r_fold_seconds_max": max(secs["M3R"]),
                         "m4_fold_seconds_sum": sum(secs["M4"]), "m4_fits": len(secs["M4"]), "m1_fold_seconds_sum": sum(secs["M1"]),
                         "m3r_final_seconds": final_m3r, "m4_seconds_per_fit_estimate": m4_fit, "cpu_farm_concurrency": hw["cpu_farm"]["processes"]},
            "estimates_cpu_seconds": {
                "H1_lineage_balanced_m3_cv_plus_final": sum(secs["M3R"]) + final_m3r,
                "H2_multitask_m4_cv_plus_final_3_seeds": sum(secs["M4"]) * 1.25 + 3 * m4_fit * 1.25,
                "H3_fusion_inference_only": 0.0, "H4_expansion_offline_only": 0.0},
            "notes": {"H1": "same recipe/steps as M3-R: cost equals the recorded M3-R cross-validation plus one final refit",
                      "H2": "M4-Dense-sized network with an added per-word start head: recorded M4 cost x1.25 (assumed head/loss overhead; untested)",
                      "wall": "the 8-process CPU farm runs folds concurrently; ROCm was 8.9% faster end-to-end (ADR 0153), below the 25% rule"}}


def cmd_analyze(a) -> int:
    bundle = Bundle(a.bundle, parse_rebase(a.rebase))
    _env(bundle)
    import segarecomp_ml_v3 as v3
    v3.verify_plan()
    out = pathlib.Path(a.out)
    require_private_dir(out)
    ghidra = {}
    for item in a.independent or []:
        L, p = item.split("=", 1)
        recs = json.loads(pathlib.Path(p).read_text())
        ghidra[L] = sorted(r["address"] for r in recs if r.get("kind") == "code_entry_candidate")
    ida, ida_stats = {}, {}
    for item in a.ida_listing or []:
        L, p = item.split("=", 1)
        st = {}
        ida[L] = parse_ida_code_xrefs(pathlib.Path(p).read_text(errors="replace"), st)
        ida_stats[L] = st
    dev = dev_cases(bundle)
    sealed = sealed_cases(bundle, out)
    consistency = check_sealed_consistency(bundle, sealed)
    if not consistency["all_equal"]:
        raise SystemExit("regenerated sealed scores do not reproduce the stored frozen regions / post-hoc aggregates")
    cred = analysis_label_credibility(sealed, ghidra, ida)
    cred["development_signature_control"] = analysis_signature_control(bundle, dev)
    for L, st in ida_stats.items():
        if L in cred:
            cred[L]["ida_code_xref_parse"] = st
    comp_dev = analysis_complementarity(dev, DEV_MODELS)
    comp_sealed = analysis_complementarity(sealed, SEALED_MODELS)
    metrics = analysis_metrics(bundle, dev)
    fusion = {"development_oof": experiment_fusion(dev), "sealed_exploratory": experiment_fusion(sealed)}
    expansion = {"development_oof": experiment_expansion(dev), "sealed_exploratory": experiment_expansion(sealed)}
    exposure = analysis_exposure(dev, comp_dev["budgets"]["B50"]["_hard"])
    feas = experiment_feasibility(bundle)
    rec = {"schema": SCHEMA, "bundle_manifest_sha256": bundle.manifest_sha256, "plan_sha256": v3.DIGEST_PATH.read_text().strip(),
           "sealed_scores": json.loads((out / "sealed_scores.json").read_text())["arrays"], "sealed_consistency": consistency,
           "label_credibility": cred, "complementarity": {"development_oof": comp_dev, "sealed_exploratory": comp_sealed},
           "metrics": metrics, "experiment_a_fusion": fusion, "experiment_b_expansion": expansion, "exposure": exposure,
           "experiment_c_feasibility": feas}
    (out / "diagnosis.private.json").write_text(json.dumps(rec, default=list))
    pub = public(json.loads(json.dumps(rec, default=list)))
    for v in pub["label_credibility"].values():
        for cell in v.get("missed_cells", []):
            cell.pop("block_signatures", None)
    if a.public:
        pathlib.Path(a.public).write_text(json.dumps(pub, indent=1, sort_keys=True) + "\n")
    print(json.dumps({"written": str(out / "diagnosis.private.json"), "public": a.public}))
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    for name, fn in (("smoke", cmd_smoke), ("infer", cmd_infer), ("analyze", cmd_analyze)):
        p = sub.add_parser(name)
        p.add_argument("--bundle", required=True)
        p.add_argument("--rebase", action="append", help="OLD=NEW prefix rebase for external input paths (operator images, Sonic CD modules)")
        p.set_defaults(func=fn)
        if name == "smoke":
            p.add_argument("--images", nargs="+", default=["d09", "s1", "scd/TITLES.BIN"])
            p.add_argument("--out")
        else:
            p.add_argument("--out", required=True, help="ignored private output directory")
        if name == "analyze":
            p.add_argument("--independent", action="append", help="LINEAGE=path of an independent disassembler's code_entry_candidate artifact")
            p.add_argument("--ida-listing", action="append", help="LINEAGE=path of the private IDA text listing (CODE XREF comments)")
            p.add_argument("--public", help="aggregate-only public record")
    a = ap.parse_args()
    try:
        return a.func(a)
    except ArtifactError as e:
        print(json.dumps({"verdict": "BLOCKED", "error": e.kind, "detail": e.detail}))
        return 2


if __name__ == "__main__":
    raise SystemExit(main())

#!/usr/bin/env python3
"""SEG-086 (ADR 0195): sample-role enforcement for restored research samples (OFFLINE research code; nothing here is a production route).

A restored sample directory carries `meta.json`. The private harness (`corpus_snapshot_sb.py restore-sb`) writes `role`, `role_class` and `evaluation_only` into it.
This module is the PRODUCT-side consumer that makes those fields binding:

  * `require_trainable(meta)`      - every training / fitting / calibration entry point calls it BEFORE it reads a class map, a ROM or a token file. An
                                     evaluation-only sample, an exploratory diagnostic-only sample, an inconsistent role record, an unknown role and an identity that appears
                                     in a frozen evaluation manifest are all refused. A LEGACY sample (no role field at all: restored before SEG-086) stays usable, but only
                                     if it is not a frozen evaluation identity (backward compatibility for qualified development samples).
  * `EvaluationContext`            - the only way to read an evaluation-only sample. It is built from a frozen manifest (identities, not directory discovery) and
                                     `admit(meta)` refuses reserved identities FIRST, then requires an exact (image sha256, lineage) match with a manifest entry.
                                     `discover(root)` rejects any sample directory that is not in the manifest and any manifest entry that is absent or duplicated.

Pure, deterministic, no ROM bytes. Failures raise `RoleError`; nothing is silently skipped.
"""
from __future__ import annotations

import hashlib
import json
import pathlib

HERE = pathlib.Path(__file__).resolve().parent
SCHEMA = "segarecomp.sample_roles.v1"
DEVELOPMENT, EVALUATION_ONLY, EXPLORATORY, LEGACY = "development", "evaluation-only", "exploratory", "legacy-development"
KNOWN_CLASSES = (DEVELOPMENT, EVALUATION_ONLY, EXPLORATORY)
# frozen evaluation manifests whose identities may never be trained, fitted or calibrated on (extended by each evaluation milestone that freezes a cohort)
EVALUATION_MANIFESTS = (HERE / "segarecomp_seg085_cohort.manifest.json",)


class RoleError(Exception):
    pass


def classify_role_string(role: str) -> str:
    """Canonical class of a manifest role string (same grammar as the private harness exporter); anything else is refused."""
    r = str(role)
    if r.startswith("evaluation-only"):
        return EVALUATION_ONLY
    if r.startswith("exploratory diagnostic-only"):
        return EXPLORATORY
    if r.startswith("development"):
        return DEVELOPMENT
    raise RoleError(f"role {r[:40]!r} is not a known sample role")


def role_of(meta: dict) -> str:
    """The sample's role class, or LEGACY when the sample predates role preservation. Contradictory records are an error (fail closed)."""
    has_class, has_role, has_flag = "role_class" in meta, "role" in meta, "evaluation_only" in meta
    if not (has_class or has_role or has_flag):
        return LEGACY
    if not (has_class or has_role):
        raise RoleError("evaluation_only flag without a role record")
    cls = None
    if has_class:
        if meta["role_class"] not in KNOWN_CLASSES:
            raise RoleError(f"unknown role_class {str(meta['role_class'])[:40]!r}")
        cls = meta["role_class"]
    if has_role:
        c2 = classify_role_string(meta["role"])
        if cls is not None and cls != c2:
            raise RoleError("role and role_class disagree")
        cls = c2
    if has_flag:
        if not isinstance(meta["evaluation_only"], bool) or meta["evaluation_only"] != (cls == EVALUATION_ONLY):
            raise RoleError("evaluation_only flag disagrees with the role")
    return cls


def evaluation_identities(manifests=None) -> tuple:
    """(image sha256 set, lineage set) of every frozen evaluation manifest. Fails closed when a listed manifest is unreadable."""
    shas, lins = set(), set()
    for p in (EVALUATION_MANIFESTS if manifests is None else manifests):
        try:
            m = json.loads(pathlib.Path(p).read_text())
            for e in m["entries"]:
                shas.add(str(e["image_sha256"]).lower())
                lins.add(str(e["lineage"]))
        except (OSError, ValueError, KeyError) as ex:
            raise RoleError(f"frozen evaluation manifest {pathlib.Path(p).name} unreadable: {type(ex).__name__}") from ex
    return shas, lins


def require_trainable(meta: dict, purpose: str = "training", manifests=None) -> str:
    """Raise unless the sample may be used to train, fit or calibrate. Returns its role class. Call BEFORE reading any label, ROM or token file."""
    role = role_of(meta)
    if role == EVALUATION_ONLY:
        raise RoleError(f"{meta.get('id', '?')}: evaluation-only sample refused for {purpose}")
    if role == EXPLORATORY:
        raise RoleError(f"{meta.get('id', '?')}: exploratory diagnostic-only sample refused for {purpose}")
    shas, lins = evaluation_identities(manifests)
    if str(meta.get("sha256", "")).lower() in shas or str(meta.get("lineage", "")) in lins:
        raise RoleError(f"{meta.get('id', '?')}: identity or lineage of a frozen evaluation manifest refused for {purpose}")
    return role


def require_trainable_lineages(lineages, purpose: str = "calibration", manifests=None) -> None:
    """Lineage-level guard for fits that only receive lineage names (e.g. cross-validation fold definitions)."""
    _, lins = evaluation_identities(manifests)
    bad = sorted(set(map(str, lineages)) & lins)
    if bad:
        raise RoleError(f"evaluation lineage(s) {bad} refused for {purpose}")


def manifest_digest(manifest: dict) -> str:
    return hashlib.sha256(json.dumps(manifest, sort_keys=True).encode()).hexdigest()


class EvaluationContext:
    """Explicit permission to read evaluation-only samples that belong to ONE frozen manifest.

    `reserved_check(sha256)` must raise for a reserved / sealed / development identity; it is injected so tests can use synthetic identities, and defaults to the
    SEG-085 guard (`segarecomp_seg085_cohort.refuse_reserved`, which fails closed when its reserved list cannot be read).
    """

    def __init__(self, manifest: dict, purpose: str, reserved_check=None, expected_digest: str | None = None):
        if not purpose:
            raise RoleError("an evaluation context needs a stated purpose")
        digest = manifest_digest(manifest)
        if expected_digest is not None and digest != expected_digest:
            raise RoleError("frozen manifest digest mismatch")
        ents = manifest.get("entries")
        if not isinstance(ents, list) or not ents:
            raise RoleError("frozen manifest has no entries")
        self.members = {}
        for e in ents:
            key = (str(e["image_sha256"]).lower(), str(e["lineage"]))
            if key in self.members:
                raise RoleError("duplicate manifest entry")
            self.members[key] = e
        self.purpose, self.digest = purpose, digest
        if reserved_check is None:
            import segarecomp_seg085_cohort as co
            reserved_check = co.refuse_reserved
        self._reserved = reserved_check

    @classmethod
    def from_file(cls, path, purpose: str, reserved_check=None, expected_digest: str | None = None) -> "EvaluationContext":
        return cls(json.loads(pathlib.Path(path).read_text()), purpose, reserved_check, expected_digest)

    def admit(self, meta: dict) -> tuple:
        """Reserved identity check FIRST (before the role or any label is looked at), then exact frozen-manifest membership, then the role record."""
        sha = str(meta.get("sha256", "")).lower()
        if not sha:
            raise RoleError("sample has no image identity")
        try:
            self._reserved(sha)
        except RoleError:
            raise
        except Exception as ex:                                                   # the reserved guard's own error type: refuse, do not read on
            raise RoleError(f"reserved identity refused: {ex}") from ex
        key = (sha, str(meta.get("lineage", "")))
        if key not in self.members:
            raise RoleError(f"{meta.get('id', '?')}: not a member of the frozen evaluation manifest")
        role = role_of(meta)
        if role not in (EVALUATION_ONLY, LEGACY):
            raise RoleError(f"{meta.get('id', '?')}: a {role} sample is not evaluation material")
        return key

    def discover(self, root, require_complete: bool = True) -> list:
        """Sample directories under `root` whose meta.json is a frozen-manifest member. A non-member sample, a duplicate member or (when required) a missing
        member is an error: membership comes from the manifest, never from whatever the directory contains."""
        seen, dirs = {}, []
        for m in sorted(pathlib.Path(root).rglob("meta.json")):
            meta = json.loads(m.read_text())
            if "audit" not in meta:
                continue                                                         # not a registry sample (same convention as the training registry)
            key = self.admit(meta)
            if key in seen:
                raise RoleError(f"duplicate sample for manifest entry {key[1]}")
            seen[key] = m.parent
            dirs.append(m.parent)
        if require_complete and set(self.members) != set(seen):
            raise RoleError(f"{len(set(self.members) - set(seen))} frozen manifest entries have no sample")
        return dirs


# ----------------------------------------------------------------------------------------------------------------------------- guarded loaders
# The frozen SEG-072/076/084/085 tools bind `segarecomp_ml_v3_train.py` by digest, so the existing loaders cannot be edited without invalidating those frozen
# results. SEG-086 code therefore reaches the registry ONLY through these wrappers; the wrappers add the role checks and call the unchanged loaders.
def trainable_dirs(registry_dirs, purpose: str = "training", manifests=None) -> list:
    """Filter-free check of an enumerated registry: every directory's meta.json must be trainable (raises on the first violation; nothing is dropped silently)."""
    out = []
    for d in registry_dirs:
        require_trainable(json.loads((pathlib.Path(d) / "meta.json").read_text()), purpose, manifests)
        out.append(pathlib.Path(d))
    return out


def load_trainable_images(tr, lineages, purpose: str = "training", manifests=None) -> list:
    """`tr.load_images(lineages)` with the role check applied to metadata BEFORE any sample file is read."""
    require_trainable_lineages(lineages, purpose, manifests)
    dirs = trainable_dirs(tr.registry(set(lineages)), purpose, manifests)
    return [tr.Image(d) for d in dirs]


def load_evaluation_images(tr, context: EvaluationContext, root, require_complete: bool = True) -> list:
    """The only reader of evaluation-only samples: manifest membership and reserved-identity checks happen on metadata before `tr.Image` reads labels or features."""
    return [tr.Image(d) for d in context.discover(root, require_complete)]

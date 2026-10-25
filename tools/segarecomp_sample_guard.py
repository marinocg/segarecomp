#!/usr/bin/env python3
"""SEG-086 post-freeze correctness amendment (ADR 0198): a non-fitting, runtime protection layer for the registry loaders.

The frozen SEG-086 plan binds `segarecomp_seg086_cal.py`, `segarecomp_seg086_diag.py` and `segarecomp_sample_roles.py` by SHA-256, and many older plans bind
`segarecomp_ml_v3_train.py`; none of them may be edited. Every sample those tools load is constructed through `segarecomp_ml_v3_train.Image` (directly or through
`xplat_transfer.load`), so the per-sample boundary is enforced HERE, at the single construction point, using the existing `segarecomp_sample_roles` guards unchanged:

  * `Image(meta_dir)`            -> `require_trainable(meta)` on metadata only (evaluation-only, exploratory, contradictory/unknown roles, frozen-evaluation identities
                                   and lineages are refused) after a reserved/sealed identity check; nothing but meta.json is read before the check passes;
  * `Image(meta_dir, evaluation=ctx)` -> `ctx.admit(meta)` (reserved identities first, exact frozen-manifest membership, consistent role);
  * `registry(ids)`              -> every selected sample is checked before its directory is returned.

`install()` is idempotent and returns an `uninstall` callable (used by tests). Nothing here fits, scores or selects anything. Entry points that must be protected go through
`tools/segarecomp_seg086_guarded.py`; calling the frozen tools directly still works exactly as before and is NOT protected.
"""
from __future__ import annotations

import json
import pathlib
import sys

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import segarecomp_sample_roles as sr  # noqa: E402


def default_reserved_identities() -> set:
    """Image digests that no training or calibration may read: SEG-077 reserved titles and SEG-072 sealed images (development images are legitimate training data).
    Fail-closed: an unreadable reserved list raises."""
    import segarecomp_seg085_cohort as co
    return {sha for sha, why in co.reserved_sha256().items() if why != "SEG-072 development image"}


class Guard:
    def __init__(self, tr, reserved=None, manifests=None):
        self.tr = tr
        self.reserved = reserved                  # callable returning a set of digests, or a set; None -> default (resolved lazily, once)
        self.manifests = manifests
        self._orig_init = None
        self._orig_registry = None
        self._reserved_set = None

    def reserved_set(self) -> set:
        if self._reserved_set is None:
            r = self.reserved() if callable(self.reserved) else (self.reserved if self.reserved is not None else default_reserved_identities())
            self._reserved_set = {str(x).lower() for x in r}
        return self._reserved_set

    def check_meta(self, meta: dict, evaluation=None, purpose: str = "training") -> None:
        """Reserved identity first, then the role rules. Raises RoleError."""
        if str(meta.get("sha256", "")).lower() in self.reserved_set():
            raise sr.RoleError(f"{meta.get('id', '?')}: reserved or sealed identity refused")
        if evaluation is None:
            sr.require_trainable(meta, purpose, self.manifests)
        else:
            evaluation.admit(meta)

    def install(self):
        tr, guard = self.tr, self
        self._orig_init, self._orig_registry = tr.Image.__init__, tr.registry
        orig_init, orig_registry = self._orig_init, self._orig_registry

        def init(self_, meta_dir, evaluation=None):
            meta = json.loads((pathlib.Path(meta_dir) / "meta.json").read_text())          # metadata only: no ROM, class map or token file yet
            guard.check_meta(meta, evaluation, "Image() load")
            orig_init(self_, meta_dir)

        def registry(ids=None):
            dirs = orig_registry(ids)
            for d in dirs:
                guard.check_meta(json.loads((pathlib.Path(d) / "meta.json").read_text()), None, "training registry")
            return dirs

        tr.Image.__init__, tr.registry = init, registry
        return self.uninstall

    def uninstall(self):
        if self._orig_init is not None:
            self.tr.Image.__init__, self.tr.registry = self._orig_init, self._orig_registry
            self._orig_init = self._orig_registry = None


_ACTIVE = []


def install(tr=None, reserved=None, manifests=None):
    """Install on `segarecomp_ml_v3_train` (or a stand-in module for tests). Idempotent for the real module."""
    if tr is None:
        import segarecomp_ml_v3_train as tr
    for g in _ACTIVE:
        if g.tr is tr and g._orig_init is not None:
            return g.uninstall
    g = Guard(tr, reserved, manifests)
    g.install()
    _ACTIVE.append(g)
    return g.uninstall

#!/usr/bin/env python3
"""SEG-075 (ADR 0160): private research-artifact audit, content-addressed snapshot and physical verification (OFFLINE tooling).

The T001 `segarecomp_xplat_qualify.py verify` compares *recorded* manifest evidence. This tool checks the *physical* private files.

  audit     enumerate the artifacts the SEG-075 successors need (pinned source checkouts, both reproducibility runs of every recipe,
            listings/objects/maps, logs, records, toolchain inputs, Dockerfiles, embedded helpers, Docker images, SEG-072 bundle), rehash
            every physical file, compare it with the strongest available independent evidence (T001 record digest, cross-run identity, plan
            SHA-256, git blob identity, product Dockerfile/helper text, recorded image id) and classify each required artifact:
              PRESENT_AND_VERIFIED, PRESENT_BUT_CHANGED, REPRODUCIBLE_BUT_NOT_PRESERVED, MISSING_REBUILD_REQUIRED,
              MISSING_UNRECOVERABLE, NOT_REQUIRED_FOR_SUCCESSOR
            then store an independent read-only copy of every preserved file in a content-addressed store and write a private manifest plus a sanitized receipt
  add       snapshot further private research trees (T002 corpus, T003 snapshot, T004/T005 models, ...) into the same store
  verify    rehash every manifest entry's content-addressed object (physical bytes, not saved JSON), check sizes, image-archive identities
            and the absence of any dependency on a task worktree; run it from a fresh process (`--fresh-process` re-executes itself
            isolated: `python3 -I`, cwd `/`, minimal environment)
  restore   materialize a manifest's logical tree from the store (independent copies; --hardlink for read-only inspection) into an empty directory

Durable (committed) output is the sanitized receipt only: counts, bytes, classes, verification bases, manifest identity and the verified
storage path. Third-party sources, binaries, listings, labels and weights stay in the private store (git-ignored harness `.cache/`).
"""
from __future__ import annotations

import argparse
import datetime
import fnmatch
import hashlib
import json
import os
import pathlib
import subprocess
import sys
import tarfile

HERE = pathlib.Path(__file__).resolve().parent
SCHEMA = "segarecomp.xplat_handoff.manifest.v1"
RECEIPT_SCHEMA = "segarecomp.xplat_handoff.receipt.v1"
STATUSES = ("PRESENT_AND_VERIFIED", "PRESENT_BUT_CHANGED", "REPRODUCIBLE_BUT_NOT_PRESERVED", "MISSING_REBUILD_REQUIRED",
            "MISSING_UNRECOVERABLE", "NOT_REQUIRED_FOR_SUCCESSOR")
# How a PRESENT file was verified, strongest first. `baseline` = no earlier independent digest exists: the snapshot digest is the first
# integrity record (future drift is detected, past correctness is not claimed).
BASES = ("t001_record", "t001_record_code", "plan_sha256", "plan_commit", "git_blob", "product_text", "cross_run_identical", "image_id", "external_manifest",
         "baseline", "cross_run_differs")
# A required entry with one of these statuses is a problem for both `audit` and `verify` (one list, one verdict).
PROBLEM_STATUSES = ("PRESENT_BUT_CHANGED", "MISSING_REBUILD_REQUIRED", "MISSING_UNRECOVERABLE")
# Candidates whose artifacts the SEG-075 successors consume (T001 ADR 0159 "usable without review" + the toolchain demos used only to
# validate extraction). Everything else is preserved but NOT_REQUIRED_FOR_SUCCESSOR.
REQUIRED_CANDIDATES = ("BattleIsleEditor", "amiga-realtime3d", "HelloAmi", "toslibc", "emutos", "space-invaders", "neogeosdk",
                       "atarist-toolkit-docker")
# Build intermediates T002 needs beyond the declared outputs (glob patterns relative to a run directory). A pattern that matches nothing
# is a missing required artifact. `not_preserved` lists truth artifacts T001 never generated (listings of compiler-generated assembly):
# they are reproducible from the pinned source and preserved toolchain image, and T002's truth recipes create them.
SUCCESSOR_NEEDS = {
    "BattleIsleEditor": {"editors": ["out/*.lst", "out/*.o"]},
    "amiga-realtime3d": {"programs": ["out/P*.lst"]},
    "HelloAmi": {"units": ["out/*.lst"]},
    "space-invaders": {"objects-vbcc": ["out/*.o", "out/*.lst", "src/*.asm"]},
    "toslibc": {"lib+examples": ["example/*.elf", "test/*.elf", "lib/*.o", "example/*.o", "test/*.o"]},
    "emutos": {r: ["obj/*.o", "emutos.map"] for r in ("rom-192", "rom-256", "cart", "amiga-rom")},
    "neogeosdk": {"demo-compile-link": ["out/*.o", "out/game"]},
}
NOT_PRESERVED = {
    "toslibc": "GNU as listings of the GCC 13.3 assembly (T001 kept objects and relocatable ELF only)",
    "emutos": "GNU as listings of the GCC 4.6.4 assembly and .S sources (T001 kept objects and the link map only)",
    "neogeosdk": "GNU as listings of the GCC 13.3 assembly (T001 kept objects and the linked ELF only)",
    "space-invaders": "vasm listings of the vbcc-emitted assembly (T001 kept the .asm and objects only)",
}
WORKTREE_MARKERS = ("/.worktrees/", "/.worktrees-harness/")


def under_worktree(path) -> bool:
    """True when a path (any platform's separators) lies inside a task worktree (pure; unit-tested)."""
    s = str(path).replace("\\", "/") + "/"
    return any(m in s for m in WORKTREE_MARKERS)


class HandoffError(Exception):
    pass


# ------------------------------------------------------------------------------------------------------------------------- hashing
def sha256_file(path: pathlib.Path) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def git_blob_sha1(path: pathlib.Path) -> str:
    data = path.read_bytes()
    return hashlib.sha1(b"blob %d\0" % len(data) + data).hexdigest()


def canonical(obj) -> bytes:
    return (json.dumps(obj, indent=1, sort_keys=True) + "\n").encode()


def iter_files(root: pathlib.Path, skip_git: bool = True):
    """Every regular file under root (symlinks are reported separately, never followed), deterministic order."""
    for dp, dns, fns in os.walk(root):
        dns[:] = sorted(d for d in dns if not (skip_git and d == ".git"))
        for fn in sorted(fns):
            p = pathlib.Path(dp) / fn
            if p.is_symlink() or not p.is_file():
                continue
            yield p


def symlinks_escaping(root: pathlib.Path) -> list[str]:
    """Symlinks under root whose target leaves root or names a task worktree (a dependency on transient state)."""
    bad = []
    rr = root.resolve()
    for dp, dns, fns in os.walk(root):
        dns[:] = [d for d in dns if d != ".git"]
        for n in list(dns) + fns:
            p = pathlib.Path(dp) / n
            if not p.is_symlink():
                continue
            target = os.readlink(p)
            resolved = (p.parent / target).resolve() if not os.path.isabs(target) else pathlib.Path(target).resolve()
            if under_worktree(resolved):
                bad.append(f"{p.relative_to(root).as_posix()} -> worktree")
            else:
                try:
                    resolved.relative_to(rr)
                except ValueError:
                    if not target.startswith("/xplat") and not target.startswith("/opt") and not target.startswith("/usr"):
                        bad.append(f"{p.relative_to(root).as_posix()} -> outside")
    return sorted(bad)


# ---------------------------------------------------------------------------------------------------------------- classification
def classify_present(actual: str, expected: str | None, basis: str) -> tuple[str, str]:
    """(status, basis) of a present file given an independent expected digest (pure; unit-tested)."""
    if expected is None:
        return "PRESENT_AND_VERIFIED", "baseline"
    return ("PRESENT_AND_VERIFIED", basis) if actual == expected else ("PRESENT_BUT_CHANGED", basis)


def classify_missing(recoverable: bool) -> str:
    return "MISSING_REBUILD_REQUIRED" if recoverable else "MISSING_UNRECOVERABLE"


def entry(logical: str, group: str, required: bool, status: str, basis: str | None, path: pathlib.Path | None = None,
          sha256: str | None = None, size: int | None = None, note: str | None = None, verified: str | None = None) -> dict:
    """`status` is the successor-facing class; `verified` keeps the underlying verification result of a not-required file."""
    e = {"logical": logical, "group": group, "required": required, "status": status}
    if basis:
        e["basis"] = basis
    if verified is not None and verified != status:
        e["verified"] = verified
    if sha256 is not None:
        e["sha256"], e["size"] = sha256, size
    if path is not None:
        e["origin"] = str(path)
    if note:
        e["note"] = note
    return e


# --------------------------------------------------------------------------------------------------------------------- T001 audit
def _qualify():
    sys.path.insert(0, str(HERE))
    import segarecomp_xplat_qualify as q  # noqa: E402
    return q


def audit_sources(q, plan: dict, cache: pathlib.Path, cache_parent: pathlib.Path) -> list[dict]:
    """Pinned checkouts: HEAD equals the pin, the tree is clean, and every working file's git blob id equals the committed index entry."""
    out = []
    for c in plan["candidates"]:
        cid = c["id"]
        required = cid in REQUIRED_CANDIDATES
        src = cache / "src" / cid
        group = "t001.source"
        if not (src / ".git").exists():
            status = classify_missing(True) if required else "NOT_REQUIRED_FOR_SUCCESSOR"
            out.append(entry(f"seg075/q/src/{cid}", group, required, status, None, note="checkout missing (re-fetch the pinned commit)"))
            continue
        head = subprocess.run(["git", "-C", str(src), "rev-parse", "HEAD"], capture_output=True, text=True).stdout.strip()
        dirty = subprocess.run(["git", "-C", str(src), "status", "--porcelain", "--untracked-files=all"], capture_output=True,
                               text=True).stdout.strip()
        index = {}
        for ln in subprocess.run(["git", "-C", str(src), "ls-files", "-s", "-z"], capture_output=True, text=True).stdout.split("\0"):
            if ln:
                meta, path = ln.split("\t", 1)
                mode, blob, _stage = meta.split()
                index[path] = (mode, blob)
        # skip-worktree entries (an archive candidate's sparse pilot checkout) are intentionally absent from the working tree
        for ln in subprocess.run(["git", "-C", str(src), "ls-files", "-t", "-z"], capture_output=True, text=True).stdout.split("\0"):
            if ln[:2] == "S ":
                index.pop(ln[2:], None)
        checkout_ok = head == c["commit"] and not dirty
        working = list(iter_files(src))
        # `git hash-object --stdin-paths` applies the same clean filters (.gitattributes eol conversion) as the index, so a CRLF
        # working copy of an LF blob verifies against its committed blob id
        rels = [p.relative_to(src).as_posix() for p in working]
        hashed = subprocess.run(["git", "-C", str(src), "hash-object", "--stdin-paths"], input="".join(r + "\n" for r in rels),
                                capture_output=True, text=True).stdout.split() if rels else []
        if len(hashed) != len(rels):
            raise HandoffError(f"{cid}: git hash-object returned {len(hashed)} ids for {len(rels)} files")
        blob_of = dict(zip(rels, hashed))
        files = 0
        for p in working:
            rel = p.relative_to(src).as_posix()
            expect = index.get(rel)
            actual_blob = blob_of[rel]
            ok = expect is not None and expect[1] == actual_blob and checkout_ok
            sha = sha256_file(p)
            status = "PRESENT_AND_VERIFIED" if ok else "PRESENT_BUT_CHANGED"
            out.append(entry(p.relative_to(cache_parent).as_posix(), group, required, status if required else "NOT_REQUIRED_FOR_SUCCESSOR",
                             "git_blob", p, sha, p.stat().st_size, verified=status))
            files += 1
        missing = [p for p, (mode, _b) in index.items() if mode != "120000" and mode != "160000" and not (src / p).is_file()]
        for p in missing:
            out.append(entry(f"seg075/q/src/{cid}/{p}", group, required, classify_missing(True) if required else "NOT_REQUIRED_FOR_SUCCESSOR",
                             "git_blob", note="tracked file missing from the checkout"))
        ident_status = "PRESENT_AND_VERIFIED" if checkout_ok else "PRESENT_BUT_CHANGED"
        out.append(entry(f"seg075/q/src/{cid}/.git#HEAD", "t001.source_identity", required,
                         ident_status if required else "NOT_REQUIRED_FOR_SUCCESSOR", "plan_commit", verified=ident_status, note=f"head {'== pin' if head == c['commit'] else '!= pin'}; {'clean' if not dirty else 'modified'}; "
                         f"{files} files"))
    return out


def audit_builds(q, plan: dict, cache: pathlib.Path, cache_parent: pathlib.Path) -> list[dict]:
    """Both runs of every recipe: declared outputs against the T001 record digest; other files against the other run (cross-run
    identity) when both runs exist, else a baseline digest. Logs are baseline (volatile by nature)."""
    out = []
    for c in plan["candidates"]:
        cid = c["id"]
        required = cid in REQUIRED_CANDIDATES
        rec_path = cache / "records" / f"{cid}.json"
        rec = json.loads(rec_path.read_text()) if rec_path.exists() else {"recipes": []}
        by_name = {r["name"]: r for r in rec["recipes"]}
        for r in c["recipes"]:
            rr = by_name.get(r["name"], {})
            recorded = {o["path"]: o["sha256"] for o in rr.get("outputs", [])}
            recorded_code = {o["path"]: o["code_sha256"] for o in rr.get("outputs", []) if "code_sha256" in o}
            identical_runs = rr.get("byte_identical_runs", False)
            volatile = set(rr.get("volatile_outputs", {}))
            base = cache / "build" / cid / r["name"]
            runs = ["a", "b"] if r.get("expect", "success") == "success" else ["a"]
            hashes: dict[str, dict[str, str]] = {}
            for run in runs:
                d = base / run
                hashes[run] = {p.relative_to(d).as_posix(): sha256_file(p) for p in iter_files(d)} if d.is_dir() else {}
            for run in runs:
                d = base / run
                group = f"t001.build.{run}"
                if not d.is_dir():
                    st = classify_missing(True) if required else "NOT_REQUIRED_FOR_SUCCESSOR"
                    out.append(entry(f"seg075/q/build/{cid}/{r['name']}/{run}", group, required, st, None, note="run directory missing"))
                    continue
                other = hashes.get("b" if run == "a" else "a", {})
                needs = SUCCESSOR_NEEDS.get(cid, {}).get(r["name"], [])
                for rel, sha in sorted(hashes[run].items()):
                    p = d / rel
                    # A build file is required only when it is a declared output or a successor intermediate (listing/object/map);
                    # other build by-products are preserved but not required.
                    file_required = required and (rel in recorded or any(fnmatch.fnmatch(rel, pat) for pat in needs))
                    if rel in recorded_code and rel not in volatile and run == "b" and not identical_runs:
                        # T001 declared these runs deterministic by the loadable-code digest (a PRG's symbol table may differ)
                        status, basis = classify_present(q.code_digest(p.read_bytes()), recorded_code[rel], "t001_record_code")
                    else:
                        status, basis = verify_build_file(rel, sha, recorded, volatile, identical_runs, run, other, len(runs) == 2,
                                                          file_required)
                    out.append(entry(p.relative_to(cache_parent).as_posix(), group, file_required,
                                     status if file_required else "NOT_REQUIRED_FOR_SUCCESSOR", basis, p, sha, p.stat().st_size,
                                     verified=status))
                for rel in sorted(set(recorded) - set(hashes[run])):
                    if run == "a" or identical_runs:
                        st = classify_missing(True) if required else "NOT_REQUIRED_FOR_SUCCESSOR"
                        out.append(entry(f"seg075/q/build/{cid}/{r['name']}/{run}/{rel}", group, required, st, "t001_record",
                                         note="declared output missing"))
                for pat in SUCCESSOR_NEEDS.get(cid, {}).get(r["name"], []) if run == "a" else []:
                    if not list(d.glob(pat)):
                        out.append(entry(f"seg075/q/build/{cid}/{r['name']}/a/{pat}", "t002.intermediate", True,
                                         classify_missing(True), None, note="successor intermediate missing"))
                log = base / f"{run}.log"
                if log.is_file():
                    out.append(entry(log.relative_to(cache_parent).as_posix(), "t001.log", required,
                                     "PRESENT_AND_VERIFIED" if required else "NOT_REQUIRED_FOR_SUCCESSOR", "baseline", log, sha256_file(log),
                                     log.stat().st_size, verified="PRESENT_AND_VERIFIED"))
        if cid in NOT_PRESERVED:
            out.append(entry(f"seg075/q/build/{cid}#assembler-listings", "t002.truth_listing", True, "REPRODUCIBLE_BUT_NOT_PRESERVED", None,
                             note=NOT_PRESERVED[cid]))
    return out


def verify_build_file(rel: str, sha: str, recorded: dict, volatile: set, identical_runs: bool, run: str, other: dict, two_runs: bool,
                      needed: bool) -> tuple[str, str]:
    """(status, basis) of one build file (pure; unit-tested). A declared output is checked against the T001 record (run b only when
    T001 recorded the runs byte-identical); any other file against the other run. A run-to-run difference gets its own basis
    `cross_run_differs`: it is PRESENT_BUT_CHANGED for a file a successor needs (a needed listing/object/map must be reproducible),
    and only recorded (never counted as verified) for other by-products."""
    if rel in recorded and rel not in volatile and (run == "a" or identical_runs):
        return classify_present(sha, recorded[rel], "t001_record")
    if rel in other and two_runs:
        if other[rel] == sha:
            return "PRESENT_AND_VERIFIED", "cross_run_identical"
        return ("PRESENT_BUT_CHANGED" if needed else "PRESENT_UNVERIFIED"), "cross_run_differs"
    return "PRESENT_AND_VERIFIED", "baseline"


def audit_control(q, plan: dict, cache: pathlib.Path, cache_parent: pathlib.Path, product_root: pathlib.Path) -> list[dict]:
    """Records, toolchain ids, pilot list, toolchain inputs (plan SHA-256), staged Dockerfiles (product text), embedded helpers."""
    out = []
    for p in sorted((cache / "records").glob("*.json")) + [cache / "toolchains.json"] + sorted((cache / "pilot").glob("*.tsv")):
        if p.is_file():
            out.append(entry(p.relative_to(cache_parent).as_posix(), "t001.record", True, "PRESENT_AND_VERIFIED", "baseline", p, sha256_file(p),
                             p.stat().st_size))
    for tid, tc in plan["toolchains"].items():
        for item in tc.get("inputs", []):
            p = cache / "toolchains" / tid / item["name"]
            if p.is_file():
                st, basis = classify_present(sha256_file(p), item["sha256"], "plan_sha256")
                out.append(entry(p.relative_to(cache_parent).as_posix(), "t001.toolchain_input", True, st, basis, p, sha256_file(p), p.stat().st_size))
            else:
                # vasm/vlink/vbcc URLs serve "latest"; a lost pinned archive cannot be re-downloaded by digest.
                out.append(entry(f"seg075/q/toolchains/{tid}/{item['name']}", "t001.toolchain_input", True, classify_missing(False), "plan_sha256"))
        if "dockerfile" in tc:
            p = cache / "toolchains" / tid / "Dockerfile"
            want = hashlib.sha256((product_root / tc["dockerfile"]).read_bytes()).hexdigest()
            if p.is_file():
                st, basis = classify_present(sha256_file(p), want, "product_text")
                out.append(entry(p.relative_to(cache_parent).as_posix(), "t001.dockerfile", True, st, basis, p, sha256_file(p), p.stat().st_size))
            else:
                out.append(entry(f"seg075/q/toolchains/{tid}/Dockerfile", "t001.dockerfile", True, "REPRODUCIBLE_BUT_NOT_PRESERVED",
                                 "product_text", note="staged copy of the committed Dockerfile"))
    helpers = {"ci_alias.py": q.CI_ALIAS, "asts_pilot.py": q.ASTS_PILOT, "bin/date": q.DATE_SHIM}
    for rel, text in helpers.items():
        p = cache / "xplat" / rel
        want = hashlib.sha256(text.encode()).hexdigest()
        if p.is_file():
            st, basis = classify_present(sha256_file(p), want, "product_text")
            out.append(entry(p.relative_to(cache_parent).as_posix(), "t001.helper", True, st, basis, p, sha256_file(p), p.stat().st_size))
        else:
            out.append(entry(f"seg075/q/xplat/{rel}", "t001.helper", True, "REPRODUCIBLE_BUT_NOT_PRESERVED", "product_text"))
    return out


def docker_image_id(ref: str) -> str | None:
    try:
        r = subprocess.run(["docker", "image", "inspect", ref, "--format", "{{.Id}}"], capture_output=True, text=True, timeout=60)
    except (OSError, subprocess.TimeoutExpired):
        return None
    return r.stdout.strip() if r.returncode == 0 else None


def image_archive_ids(path: pathlib.Path) -> dict[str, str]:
    """Image id per repo tag, read from the archive itself (docker-save manifest.json `Config` = the image config digest)."""
    out = {}
    with tarfile.open(path) as t:
        man = json.load(t.extractfile("manifest.json"))
        for m in man:
            cfg = m["Config"].rsplit("/", 1)[-1].removesuffix(".json")
            for tag in m.get("RepoTags") or []:
                out[tag] = "sha256:" + cfg
    return out


def audit_images(q, plan: dict, cache: pathlib.Path, store: pathlib.Path, save: bool) -> tuple[list[dict], pathlib.Path | None]:
    tcs = json.loads((cache / "toolchains.json").read_text()) if (cache / "toolchains.json").exists() else {}
    out = []
    refs = {tid: q.image(tid) for tid in plan["toolchains"]}
    live = {tid: docker_image_id(ref) for tid, ref in refs.items()}
    archive = store / "images" / "segarecomp-xplat-toolchains.tar"
    if save and all(live.values()) and all(live[t] == tcs.get(t, {}).get("image_id") for t in refs):
        archive.parent.mkdir(parents=True, exist_ok=True)
        tmp = archive.with_suffix(".tmp")
        subprocess.run(["docker", "save", "-o", str(tmp)] + [refs[t] for t in sorted(refs)], check=True)
        tmp.replace(archive)
    archived = image_archive_ids(archive) if archive.is_file() else {}
    for tid in sorted(refs):
        want = tcs.get(tid, {}).get("image_id")
        got = live[tid]
        in_archive = archived.get(refs[tid])
        if want is None:
            st, note = classify_missing(True), "no recorded image id"
        elif in_archive == want:
            st, note = "PRESENT_AND_VERIFIED", f"archived; daemon {'same id' if got == want else 'missing/different'}"
        elif got == want:
            st, note = "PRESENT_AND_VERIFIED", "daemon only (not archived)"
        elif got is not None:
            st, note = "PRESENT_BUT_CHANGED", "daemon tag points to a different image"
        else:
            st, note = classify_missing(tid != "tc-mint"), "rebuild from the committed Dockerfile and preserved inputs"
        out.append(entry(f"docker:{refs[tid]}", "t001.toolchain_image", True, st, "image_id", note=note))
    return out, (archive if archive.is_file() else None)


def audit_external(harness_cache: pathlib.Path) -> list[dict]:
    """SEG-072 handoff bundle (consumed read-only by T004+): its archive digest against its own checksum file."""
    out = []
    base = harness_cache / "seg072-handoff"
    tar = base / "seg072-handoff-bundle-v1.tar"
    chk = base / "seg072-handoff-bundle-v1.tar.sha256"
    if tar.is_file() and chk.is_file():
        want = chk.read_text().split()[0]
        st, basis = classify_present(sha256_file(tar), want, "external_manifest")
        out.append(entry("seg072-handoff/seg072-handoff-bundle-v1.tar", "external.seg072_bundle", True, st, basis, note="external, not copied"))
    else:
        out.append(entry("seg072-handoff/seg072-handoff-bundle-v1.tar", "external.seg072_bundle", True, classify_missing(False), None))
    man = base / "bundle-v1" / "MANIFEST.json"
    msum = base / "bundle-v1" / "MANIFEST.sha256"
    if man.is_file() and msum.is_file():
        st, basis = classify_present(sha256_file(man), msum.read_text().split()[0], "external_manifest")
        out.append(entry("seg072-handoff/bundle-v1/MANIFEST.json", "external.seg072_bundle", True, st, basis, note="external, not copied"))
    return out


def audit_exploratory(seg075: pathlib.Path) -> list[dict]:
    """Pre-tool exploratory trees of the T001 session (superseded by q/): counted, not required, not preserved."""
    out = []
    for name in ("src", "build", "tools", "logs", "scripts"):
        d = seg075 / name
        if d.is_dir():
            n = sum(1 for _ in iter_files(d, skip_git=False))
            out.append(entry(f"seg075/{name}/", "t001.exploratory", False, "NOT_REQUIRED_FOR_SUCCESSOR", None,
                             note=f"{n} files; superseded by the qualified cache q/"))
    return out


# ------------------------------------------------------------------------------------------------------------------- the store
def cas_path(store: pathlib.Path, sha: str) -> pathlib.Path:
    return store / "cas" / "sha256" / sha[:2] / sha


def put(store: pathlib.Path, src: pathlib.Path, sha: str) -> str:
    """Store an independent read-only COPY of a verified file (content-addressed; returns how it was stored).

    A hardlink would share the live file's inode, so any in-place rewrite of the live cache (the T001 tool rewrites records, helpers and
    logs with write_text) would silently destroy the preserved evidence. An existing object is re-hashed before it is reused: a corrupted
    object is replaced from a verified source, never kept."""
    import shutil
    dst = cas_path(store, sha)
    if dst.exists():
        st = dst.stat()
        shares_inode = st.st_nlink > 1 or (src.exists() and src.stat().st_ino == st.st_ino and src.stat().st_dev == st.st_dev)
        if dst.is_file() and not shares_inode and sha256_file(dst) == sha:
            return "dedup"
        os.chmod(dst, 0o644)
        dst.unlink()
        how = "replaced"
    else:
        how = "copy"
    dst.parent.mkdir(parents=True, exist_ok=True)
    tmp = dst.with_suffix(".tmp")
    if tmp.exists():
        os.chmod(tmp, 0o644)
        tmp.unlink()
    shutil.copyfile(src, tmp)
    if sha256_file(tmp) != sha:
        tmp.unlink()
        raise HandoffError(f"{src}: changed while being stored")
    os.chmod(tmp, 0o444)
    tmp.replace(dst)
    return how


def snapshot(entries: list[dict], store: pathlib.Path) -> dict:
    stats = {"copy": 0, "dedup": 0, "replaced": 0}
    for e in entries:
        if "sha256" in e and "origin" in e:
            stats[put(store, pathlib.Path(e["origin"]), e["sha256"])] += 1
    return stats


def write_manifest(store: pathlib.Path, label: str, body: dict) -> tuple[pathlib.Path, str]:
    mdir = store / "manifests"
    mdir.mkdir(parents=True, exist_ok=True)
    path = mdir / f"{label}.json"
    data = canonical(body)
    path.write_bytes(data)
    digest = hashlib.sha256(data).hexdigest()
    (mdir / f"{label}.sha256").write_text(f"{digest}  {label}.json\n")
    return path, digest


def required_problems(entries: list[dict]) -> list[str]:
    return sorted(e["logical"] for e in entries if e["required"] and e["status"] in PROBLEM_STATUSES)


def summarize(entries: list[dict]) -> dict:
    by_status: dict[str, int] = {}
    by_group: dict[str, dict[str, int]] = {}
    by_basis: dict[str, int] = {}
    unrequired_verified: dict[str, int] = {}
    files = size = 0
    for e in entries:
        by_status[e["status"]] = by_status.get(e["status"], 0) + 1
        g = by_group.setdefault(e["group"], {})
        g[e["status"]] = g.get(e["status"], 0) + 1
        if not e["required"] and "sha256" in e:
            v = e.get("verified", e["status"])
            unrequired_verified[v] = unrequired_verified.get(v, 0) + 1
        if "sha256" in e:
            files += 1
            size += e["size"]
            by_basis[e.get("basis", "baseline")] = by_basis.get(e.get("basis", "baseline"), 0) + 1
    return {"entries": len(entries), "physical_files": files, "physical_bytes": size, "by_status": dict(sorted(by_status.items())),
            "by_basis": dict(sorted(by_basis.items())), "by_group": {k: dict(sorted(v.items())) for k, v in sorted(by_group.items())},
            "not_required_underlying": dict(sorted(unrequired_verified.items())), "required_problems": required_problems(entries)}


# --------------------------------------------------------------------------------------------------------------------- verify
def verify_manifest(manifest_path: pathlib.Path, check_origin: bool = False, expect_sha256: str | None = None) -> dict:
    """Physical verification: every entry's content-addressed object exists, has the recorded size and rehashes to the recorded digest.
    The manifest itself must match its .sha256 file (a missing file fails) and, when given, the independently recorded digest (the
    committed receipt's `manifest_sha256`), so a manifest rewritten together with its digest file is caught."""
    raw = manifest_path.read_bytes()
    sums = manifest_path.with_suffix(".sha256")
    identity = hashlib.sha256(raw).hexdigest()
    problems = []
    if not sums.is_file():
        problems.append("manifest .sha256 file missing")
    elif sums.read_text().split()[0] != identity:
        problems.append("manifest digest differs from its .sha256 file")
    if expect_sha256 is not None and expect_sha256 != identity:
        problems.append("manifest digest differs from the expected (receipt) digest")
    man = json.loads(raw)
    if man.get("schema") != SCHEMA:
        raise HandoffError("unexpected manifest schema")
    store = manifest_path.resolve().parent.parent
    files = size = 0
    drift = 0
    for e in man["entries"]:
        if under_worktree(e.get("origin", "")):
            problems.append(f"{e['logical']}: origin is inside a task worktree")
        if "sha256" not in e:
            continue
        p = cas_path(store, e["sha256"])
        if not p.is_file():
            problems.append(f"{e['logical']}: object missing")
            continue
        if p.stat().st_size != e["size"]:
            problems.append(f"{e['logical']}: object size {p.stat().st_size} != {e['size']}")
            continue
        if sha256_file(p) != e["sha256"]:
            problems.append(f"{e['logical']}: object bytes changed")
            continue
        files += 1
        size += e["size"]
        if check_origin and "origin" in e:
            o = pathlib.Path(e["origin"])
            if not o.is_file() or sha256_file(o) != e["sha256"]:
                drift += 1
    for img in man.get("image_archives", []):
        p = cas_path(store, img["sha256"])
        if p.is_file():
            ids = image_archive_ids(p)
            for tag, want in img["images"].items():
                if ids.get(tag) != want:
                    problems.append(f"image archive: {tag} id {ids.get(tag)} != {want}")
        else:
            problems.append("image archive object missing")
    for logical in required_problems(man["entries"]):
        problems.append(f"{logical}: recorded required problem")
    res = {"manifest": str(manifest_path), "manifest_sha256": identity, "verified_files": files, "verified_bytes": size,
           "problems": problems, "verdict": "PASS" if not problems else "FAIL"}
    if check_origin:
        res["origin_drift"] = drift
    return res


def verify_fresh(manifest_path: pathlib.Path, check_origin: bool = False, expect_sha256: str | None = None) -> dict:
    """Re-run `verify` in an isolated fresh interpreter (no inherited state, cwd `/`, minimal environment)."""
    env = {"PATH": "/usr/bin:/bin:/usr/local/bin", "HOME": os.environ.get("HOME", "/root"), "LANG": "C.UTF-8"}
    cmd = [sys.executable, "-I", str(pathlib.Path(__file__).resolve()), "verify", "--manifest", str(manifest_path.resolve())]
    if check_origin:
        cmd.append("--check-origin")
    if expect_sha256 is not None:
        cmd += ["--expect-sha256", expect_sha256]
    r = subprocess.run(cmd, cwd="/", env=env, capture_output=True, text=True)
    try:
        res = json.loads(r.stdout)
    except json.JSONDecodeError as exc:
        raise HandoffError(f"fresh verify produced no JSON (rc={r.returncode}): {r.stderr[-400:]}") from exc
    res["fresh_process"] = {"interpreter": "isolated (-I)", "cwd": "/", "rc": r.returncode}
    return res


def restore(manifest_path: pathlib.Path, dest: pathlib.Path, hardlink: bool = False) -> int:
    """Materialize the logical tree. Default: independent writable copies (a restored cache may be rebuilt without touching the store);
    `hardlink=True` only for read-only inspection."""
    import shutil
    man = json.loads(manifest_path.read_bytes())
    store = manifest_path.resolve().parent.parent
    if dest.exists() and any(dest.iterdir()):
        raise HandoffError("restore target must be empty")
    n = 0
    for e in man["entries"]:
        if "sha256" not in e:
            continue
        target = dest / e["logical"]
        target.parent.mkdir(parents=True, exist_ok=True)
        if hardlink:
            os.link(cas_path(store, e["sha256"]), target)
        else:
            shutil.copyfile(cas_path(store, e["sha256"]), target)
        n += 1
    return n


# ------------------------------------------------------------------------------------------------------------------------- CLI
def receipt_of(label: str, summary: dict, manifest_digest: str, store: pathlib.Path, verification: dict, extra: dict) -> dict:
    harness = store.parent.parent
    return {"schema": RECEIPT_SCHEMA, "label": label, "manifest_sha256": manifest_digest,
            "storage": {"store": f"<HARNESS_ROOT>/{store.relative_to(harness).as_posix()}", "physical_path_verified": store.is_dir(),
                        "layout": "cas/sha256/<2>/<sha256> + manifests/<label>.json (git-ignored)"},
            "summary": {k: v for k, v in summary.items() if k != "required_problems"},
            "required_problems": len(summary["required_problems"]),
            "verification": {k: verification[k] for k in ("verdict", "verified_files", "verified_bytes") if k in verification}
            | {"problems": len(verification.get("problems", [])), "fresh_process": verification.get("fresh_process")},
            **extra}


def cmd_audit(a) -> int:
    q = _qualify()
    plan = q.load_plan()
    cache = pathlib.Path(a.cache).resolve()
    seg075 = cache.parent
    harness_cache = seg075.parent
    store = pathlib.Path(a.store).resolve()
    product_root = HERE.parent
    entries = []
    entries += audit_sources(q, plan, cache, harness_cache)
    entries += audit_builds(q, plan, cache, harness_cache)
    entries += audit_control(q, plan, cache, harness_cache, product_root)
    imgs, archive = audit_images(q, plan, cache, store, a.save_images)
    entries += imgs
    entries += audit_external(harness_cache)
    entries += audit_exploratory(seg075)
    escapes = symlinks_escaping(cache)
    t001 = subprocess.run([sys.executable, str(HERE / "segarecomp_xplat_qualify.py"), "verify", "--cache", str(cache)],
                          capture_output=True, text=True)
    stats = snapshot(entries, store)
    image_archives = []
    if archive is not None:
        sha = sha256_file(archive)
        put(store, archive, sha)
        if archive.stat().st_ino != cas_path(store, sha).stat().st_ino:
            # the convenience path inside the store shares the read-only CAS object (both are store-owned; no live file involved)
            archive.unlink()
            os.link(cas_path(store, sha), archive)
        image_archives.append({"sha256": sha, "size": archive.stat().st_size, "images": image_archive_ids(archive)})
        entries.append(entry("handoff/images/segarecomp-xplat-toolchains.tar", "t001.toolchain_image_archive", True, "PRESENT_AND_VERIFIED",
                             "image_id", archive, sha, archive.stat().st_size))
    body = {"schema": SCHEMA, "label": a.label, "created": datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
            "cache_parent": str(harness_cache), "entries": entries, "image_archives": image_archives,
            "symlinks_escaping": escapes, "t001_recorded_verify": (t001.stdout.strip().splitlines() or ["?"])[-1],
            "toolchain_reproducibility": TOOLCHAIN_REPRODUCIBILITY, "store_stats": stats}
    mpath, digest = write_manifest(store, a.label, body)
    summary = summarize(entries)
    ver = verify_fresh(mpath)
    print(json.dumps({"manifest": str(mpath), "manifest_sha256": digest, "summary": summary, "symlinks_escaping": escapes,
                      "t001_recorded_verify": body["t001_recorded_verify"], "store_stats": stats, "verify": ver}, indent=1))
    if a.receipt:
        rec = receipt_of(a.label, summary, digest, store, ver, {
            "t001_recorded_verify": body["t001_recorded_verify"], "symlinks_escaping": len(escapes),
            "toolchain_reproducibility": TOOLCHAIN_REPRODUCIBILITY,
            "image_archive": {"images": len(image_archives[0]["images"]) if image_archives else 0,
                              "bytes": image_archives[0]["size"] if image_archives else 0}})
        pathlib.Path(a.receipt).write_bytes(canonical(rec))
    return 0 if ver["verdict"] == "PASS" and not summary["required_problems"] else 1


def resolve_truth_listings(entries: list[dict]) -> None:
    """A C1 `REPRODUCIBLE_BUT_NOT_PRESERVED` truth-listing entry becomes PRESENT_AND_VERIFIED only when the snapshot really holds that
    program's listings (>= 1 `*.lst` under seg075/t002/truth/<program>/); the count and an aggregate digest of the listing files are
    recorded (pure over the entry list; unit-tested)."""
    for e in entries:
        if e["group"] != "t002.truth_listing" or e["status"] != "REPRODUCIBLE_BUT_NOT_PRESERVED":
            continue
        prog = e["logical"].split("/build/", 1)[1].split("#", 1)[0]
        prefix = f"seg075/t002/truth/{prog}/"
        allst = sorted((x["logical"], x["sha256"]) for x in entries
                       if x.get("group") == "t002.truth" and x["logical"].startswith(prefix) and x["logical"].endswith(".lst") and "sha256" in x)
        lst = [x for x in allst if not x[0].endswith(".relist.lst")]
        if lst:
            e["status"] = "PRESENT_AND_VERIFIED"
            e["note"] = (f"regenerated by the T002 truth run: {len(lst)} assembler listings (+ {len(allst) - len(lst)} scrubbed re-assembly "
                         "listings) preserved under group t002.truth")
            e["listings"] = len(lst)
            e["relist_listings"] = len(allst) - len(lst)
            e["listings_sha256"] = hashlib.sha256("".join(f"{l} {s}\n" for l, s in lst).encode()).hexdigest()


def cmd_add(a) -> int:
    """Snapshot further private trees into the store; every file gets a baseline digest (the producing tool's own manifests carry the
    stronger evidence), unless an existing manifest is extended (`--base`)."""
    store = pathlib.Path(a.store).resolve()
    harness_cache = pathlib.Path(a.cache_parent).resolve()
    entries = []
    if a.base:
        entries = json.loads(pathlib.Path(a.base).read_bytes())["entries"]
    seen = {e["logical"] for e in entries}
    for spec in a.tree:
        group, _, root = spec.partition("=")
        root_p = pathlib.Path(root).resolve()
        if under_worktree(root_p):
            raise HandoffError(f"{root}: refusing a task-worktree path")
        for p in iter_files(root_p, skip_git=False):
            logical = p.relative_to(harness_cache).as_posix()
            if logical in seen:
                entries = [e for e in entries if e["logical"] != logical]
            entries.append(entry(logical, group, True, "PRESENT_AND_VERIFIED", "baseline", p, sha256_file(p), p.stat().st_size))
    resolve_truth_listings(entries)
    stats = snapshot(entries, store)
    base_body = json.loads(pathlib.Path(a.base).read_bytes()) if a.base else {}
    body = {**{k: v for k, v in base_body.items() if k not in ("entries", "label", "created", "store_stats")},
            "schema": SCHEMA, "label": a.label, "created": datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
            "cache_parent": str(harness_cache), "entries": entries, "store_stats": stats}
    body.setdefault("image_archives", [])
    if a.extra:
        body["extra"] = json.loads(pathlib.Path(a.extra).read_text())
    mpath, digest = write_manifest(store, a.label, body)
    ver = verify_fresh(mpath)
    summary = summarize(entries)
    print(json.dumps({"manifest": str(mpath), "manifest_sha256": digest, "summary": summary, "verify": ver}, indent=1))
    if a.receipt:
        pathlib.Path(a.receipt).write_bytes(canonical(receipt_of(a.label, summary, digest, store, ver, json.loads(a.receipt_extra or "{}"))))
    return 0 if ver["verdict"] == "PASS" else 1


TOOLCHAIN_REPRODUCIBILITY = {
    "classes": {"existing_machine": "the recorded image id is present on this machine and in the preserved image archive",
                "pinned_tool_inputs": "the tool itself is built from SHA-256-pinned preserved sources; its host build environment (base image "
                                      "apt packages) floats",
                "environment_dependent": "the image installs floating package versions (apt / PPA / 'latest' downloads) that no pin controls"},
    "tc-gnu": ["existing_machine", "environment_dependent"],
    "tc-elf": ["existing_machine", "pinned_tool_inputs"],
    "tc-vbcc": ["existing_machine", "pinned_tool_inputs"],
    "tc-a68k": ["existing_machine", "pinned_tool_inputs"],
    "tc-mint": ["existing_machine", "environment_dependent"],
    "note": "two successful runs prove run-to-run determinism on this machine, not long-term rebuildability; the durable recovery path is "
            "the preserved image archive (`docker load` restores the recorded image ids)",
}


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    a = sub.add_parser("audit")
    a.add_argument("--cache", required=True, help="the T001 qualification cache (…/.cache/seg075/q)")
    a.add_argument("--store", required=True, help="content-addressed handoff store (…/.cache/seg075-handoff)")
    a.add_argument("--label", default="c1-t001-audit")
    a.add_argument("--receipt", help="write the sanitized receipt here")
    a.add_argument("--save-images", action="store_true", help="docker-save the recorded toolchain images into the store")
    a.set_defaults(func=cmd_audit)
    d = sub.add_parser("add")
    d.add_argument("--store", required=True)
    d.add_argument("--cache-parent", required=True, help="logical paths are relative to this directory (the harness .cache)")
    d.add_argument("--label", required=True)
    d.add_argument("--base", help="extend this manifest")
    d.add_argument("--tree", action="append", default=[], help="GROUP=DIR (repeatable)")
    d.add_argument("--extra", help="JSON file merged into the manifest under `extra`")
    d.add_argument("--receipt")
    d.add_argument("--receipt-extra")
    d.set_defaults(func=cmd_add)
    v = sub.add_parser("verify")
    v.add_argument("--manifest", required=True)
    v.add_argument("--fresh-process", action="store_true")
    v.add_argument("--check-origin", action="store_true")
    v.add_argument("--expect-sha256", help="the independently recorded manifest digest (e.g. a committed receipt's manifest_sha256)")
    v.add_argument("--receipt", help="read the expected manifest digest from this committed receipt")
    v.set_defaults(func=lambda x: _verify_cli(x))
    r = sub.add_parser("restore")
    r.add_argument("--manifest", required=True)
    r.add_argument("--to", required=True)
    r.add_argument("--hardlink", action="store_true", help="read-only inspection only: share inodes with the store")
    r.set_defaults(func=lambda x: print(json.dumps({"restored_files": restore(pathlib.Path(x.manifest), pathlib.Path(x.to), x.hardlink)})) or 0)
    args = ap.parse_args(argv)
    try:
        return args.func(args)
    except HandoffError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2


def _verify_cli(x) -> int:
    m = pathlib.Path(x.manifest)
    exp = x.expect_sha256
    if x.receipt:
        exp = json.loads(pathlib.Path(x.receipt).read_text()).get("manifest_sha256")
        if not isinstance(exp, str) or len(exp) != 64:
            raise HandoffError(f"{x.receipt}: no manifest_sha256")
    res = verify_fresh(m, x.check_origin, exp) if x.fresh_process else verify_manifest(m, x.check_origin, exp)
    print(json.dumps(res, indent=1))
    return 0 if res["verdict"] == "PASS" else 1


if __name__ == "__main__":
    sys.exit(main())

"""SEG-032-T001 (ADR 0072/0074): pinned Genesis Z80/audio reference checkouts shared by the oracle smoke tests.

The references live in an ignored directory named by SEGARECOMP_GENESIS_ORACLE_CHECKOUT (conventionally the product's
`.tools/genesis-oracles/`), cloned at exactly these commits. Genesis Plus GX and ares are shared with the Master System
oracles (`.tools/sms-oracles/`); the checkout directory may contain symlinks to them. Nothing is vendored or committed;
every build happens in a temporary copy. A missing variable or directory is a clean skip; a wrong HEAD or a modified
tracked file is a hard failure (Musashi/Z80/SMS oracle convention).
"""
import os
import pathlib
import shutil
import subprocess

CHECKOUT_ENV = "SEGARECOMP_GENESIS_ORACLE_CHECKOUT"

PINS = {
    # behavioural Genesis machine reference: bus/reset/bank/interrupt/YM2612 scheduling (licence: non-commercial; test-only)
    "ekeeke_Genesis-Plus-GX": "939ce4f045f981f89965f24780cef045cc5e52d7",
    # second, independent behavioural reference: ares MegaDrive (ISC); also the independent YM2612/PSG components
    "ares-emulator_ares": "4cb8d92b441557cb6bcaf133c4cbc7f6819b1122",
    # independent YM2612 oracle: die-shot derived cycle-accurate OPN2 (LGPL-2.1; test-only, never linked or distributed)
    "nukeykt_Nuked-OPN2": "335747d78cb0abbc3b55b004e62dad9763140115",
    # production YM2612 candidate (BSD-3-Clause); pinned here so the toolchain measurement is reproducible
    "aaronsgiles_ymfm": "81aec25ccbb98f4873a255f7551ac4dadac59b4a",
}


def checkout(names):
    """Return the checkout root, or None (skip) when unset or missing. Raises on a wrong or dirty pin."""
    configured = os.environ.get(CHECKOUT_ENV)
    if not configured:
        return None
    root = pathlib.Path(configured)
    if not all((root / name).is_dir() for name in names):
        return None
    for name in names:
        head = subprocess.run(["git", "-C", str(root / name), "rev-parse", "HEAD"], text=True, capture_output=True)
        if head.returncode != 0 or head.stdout.strip() != PINS[name]:
            raise AssertionError("%s checkout is not the pinned revision %s" % (name, PINS[name]))
        dirty = subprocess.run(["git", "-C", str(root / name), "diff", "--quiet", "HEAD", "--"])
        if dirty.returncode != 0:
            raise AssertionError("%s pinned checkout has local modifications" % name)
    return root


def skip_reason(names):
    configured = os.environ.get(CHECKOUT_ENV)
    if not configured:
        return "pinned Genesis reference checkout unavailable (%s unset)" % CHECKOUT_ENV
    return "pinned Genesis reference checkout unavailable (%s=%s lacks %s)" % (CHECKOUT_ENV, configured, ", ".join(names))


def private_copy(root, name, destination):
    """Copy a pinned tree without its .git so builds never touch the checkout."""
    target = pathlib.Path(destination) / name
    shutil.copytree(root / name, target, ignore=shutil.ignore_patterns(".git"), symlinks=True)
    return target

"""SEG-009-T001 (ADR 0062): pinned Master System reference checkouts shared by the oracle smoke tests.

The references live in an ignored directory named by SEGARECOMP_SMS_ORACLE_CHECKOUT (conventionally the product's
`.tools/sms-oracles/`), cloned at exactly these commits. Nothing is vendored or committed; every build happens in a
temporary copy so the pinned trees stay clean. A missing variable or directory is a clean skip; a wrong HEAD or a
modified tracked file is a hard failure (Musashi/Z80 oracle convention).
"""
import os
import pathlib
import shutil
import subprocess

CHECKOUT_ENV = "SEGARECOMP_SMS_ORACLE_CHECKOUT"

PINS = {
    # primary machine/VDP/mapper reference (GPL-3.0); also carries Blargg's Sms_Apu 0.1.4 (LGPL-2.1+)
    "drhelius_Gearsystem": "704a92ebb702febc4c9c1dafc056339c09c4d5c4",
    # secondary machine/VDP/mapper reference (Genesis Plus GX licence: non-commercial; local test-only use)
    "ekeeke_Genesis-Plus-GX": "939ce4f045f981f89965f24780cef045cc5e52d7",
    # primary PSG chip reference: ares SN76489 component + nall (ISC)
    "ares-emulator_ares": "4cb8d92b441557cb6bcaf133c4cbc7f6819b1122",
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
        return "pinned SMS reference checkout unavailable (%s unset)" % CHECKOUT_ENV
    return "pinned SMS reference checkout unavailable (%s=%s lacks %s)" % (CHECKOUT_ENV, configured, ", ".join(names))


def private_copy(root, name, destination):
    """Copy a pinned tree without its .git so builds never touch the checkout."""
    target = pathlib.Path(destination) / name
    shutil.copytree(root / name, target, ignore=shutil.ignore_patterns(".git"), symlinks=True)
    return target

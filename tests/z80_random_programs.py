"""Seeded random Z80 programs as conformance scenario documents (SEG-033-T003/T009).

Shared by the grouped-owner differential gate and its mutation test. Random bytes are almost all legal Z80 code (many prefixes,
HALT, interrupts, block instructions); the scenarios start at random PCs with random state, run bounded T-state steps with
random INT/NMI edges and, on the SMS-shaped map, random remap steps. Nothing here is an oracle: the programs are only compared
between two emissions.
"""
import pathlib
import random
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent.parent / "tools"))
import z80_conformance as z  # noqa: E402


def random_documents():
    """Seeded random-program scenarios: invariant and SMS-shaped images, random state, INT/NMI and remap steps."""
    rng = random.Random(33033)
    images = {
        "rnd_invariant": {"images": [{"identity": 1, "kind": "invariant", "windows": [["0000", "0000", "10000"]], "fill": "00",
                                      "size": 0x10000, "patch": [["0000", bytes(rng.randrange(256) for _ in range(0x10000)).hex()]]}]},
        "rnd_sms": {"images": [
            {"identity": 1, "kind": "invariant", "windows": [["0000", "0000", "0400"]], "fill": "00", "size": 0x400,
             "patch": [["0000", bytes(rng.randrange(256) for _ in range(0x400)).hex()]]},
            {"identity": 2, "kind": "banked", "windows": [["0000", "0400", "3C00"], ["4000", "0000", "4000"], ["8000", "0000", "4000"]],
             "fill": "00", "size": 0x4000, "patch": [["0000", bytes(rng.randrange(256) for _ in range(0x4000)).hex()]]},
            {"identity": 3, "kind": "banked", "windows": [["0000", "0400", "3C00"], ["4000", "0000", "4000"], ["8000", "0000", "4000"]],
             "fill": "00", "size": 0x4000, "patch": [["0000", bytes(rng.randrange(256) for _ in range(0x4000)).hex()]]}],
            "entry_chunk": 4096},
    }
    sms_maps = {"0": [["0", "400", 1, "0"], ["400", "4000", 2, "0"], ["4000", "8000", 2, "4000"], ["8000", "C000", 2, "8000"]],
                "1": [["0", "400", 1, "0"], ["400", "4000", 3, "0"], ["4000", "8000", 3, "4000"], ["8000", "C000", 2, "8000"]],
                "2": [["0", "400", 1, "0"], ["400", "4000", 2, "0"], ["4000", "8000", 3, "4000"], ["8000", "C000", 3, "8000"]]}
    scenarios = []
    for n in range(48):
        invariant = n % 2 == 0
        name = "rnd_%02d_%s" % (n, "inv" if invariant else "sms")
        steps = []
        for _ in range(60):
            steps.append({"mode": "t", "budget": rng.choice((1, 4, 11, 23, 57, 200)), "int": int(rng.random() < 0.15),
                          "nmi": int(rng.random() < 0.05), "map": 0 if invariant else rng.randrange(3)})
        state = {"pc": "%04X" % (rng.randrange(0x10000) if invariant else rng.randrange(0xC000)), "sp": "%04X" % (0xC000 + rng.randrange(0x3000) if not invariant else rng.randrange(0x10000)),
                 "im": rng.randrange(3), "iff1": rng.randrange(2), "iff2": rng.randrange(2)}
        sc = {"name": name, "image": "rnd_invariant" if invariant else "rnd_sms", "profile": rng.choice(("zero", "ones", "edge", "mixed")),
              "steps": steps, "state": state, "oracle": False, "ack": bytes(rng.randrange(256) for _ in range(4)).hex(),
              "in": bytes(rng.randrange(256) for _ in range(16)).hex()}
        sc["load"] = [[1, "0000"]] if invariant else [[1, "0000"], [2, "0000"], [2, "4000"], [2, "8000"], [3, "0000"], [3, "4000"], [3, "8000"]]
        if not invariant:
            sc["maps"] = sms_maps
        scenarios.append(sc)
    return {"schema": z.SCHEMA, "images": images, "scenarios": scenarios}

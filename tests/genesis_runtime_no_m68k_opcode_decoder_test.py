#!/usr/bin/env python3
"""Architecture regression: the shipped Genesis runtime never decodes MC68000 instructions from mutable memory.

ADR 0077 / the executable-image contract: guest code enters only through build-time authority (CPU-owned decode, lift and timing)
and a runtime byte-identity guard. This scans every Genesis runtime source for the shapes of a runtime M68K opcode decoder
(the experiment that preceded ADR 0097's build-time RAM-thunk producer was exactly such a two-opcode JMP decoder):
  * identifiers naming a runtime thunk/opcode decoder;
  * JMP/JSR opcode literals (0x4EB8/0x4EB9/0x4EF8/0x4EF9) anywhere in the runtime.
usage: genesis_runtime_no_m68k_opcode_decoder_test.py <source-root>
"""
import pathlib
import re
import sys

FORBIDDEN = [
    (re.compile(r"(?i)\bgenesis_\w*(thunk|opcode)\w*decode\w*|\b\w*decode_?(thunk|opcode)\w*"), "runtime thunk/opcode decoder identifier"),
    (re.compile(r"(?i)\b0x4E[BF][89]U?\b|\b0x4E[BF][89]\)"), "MC68000 JMP/JSR opcode literal"),
]


def main() -> None:
  root = pathlib.Path(sys.argv[1]) / "platforms" / "genesis" / "runtime"
  sources = sorted(path for path in root.iterdir() if path.suffix in (".c", ".h"))
  assert sources, root
  failures = []
  for path in sources:
    for number, text in enumerate(path.read_text(errors="replace").splitlines(), 1):
      code = text.split("//")[0]
      for pattern, what in FORBIDDEN:
        if pattern.search(code):
          failures.append(f"{path.name}:{number}: {what}: {text.strip()[:100]}")
  assert not failures, "runtime M68K opcode decoding reintroduced:\n" + "\n".join(failures)


if __name__ == "__main__":
  main()

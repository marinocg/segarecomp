#!/usr/bin/env python3
"""Hermeticity predicate of the release smoke test: only the bundled compiler and `$ (link)` are accepted commands."""
import importlib.util
import pathlib
import sys

root = pathlib.Path(sys.argv[1])
spec = importlib.util.spec_from_file_location("smoke_test", root / "packaging" / "smoke_test.py")
smoke = importlib.util.module_from_spec(spec)
spec.loader.exec_module(smoke)

cc = "/bundle/bin/cc"
ok = f"$ {cc} -c a.c\n# link target=game\n$ (link)\n# link target=game\nlinker noise\n"
assert smoke.hermetic_compile_lines(ok, cc), "bundled compiler, $ (link) and # comment lines are accepted"
assert not smoke.hermetic_compile_lines(ok + "$ foo --bar\n", cc), "negative control: an arbitrary command fails"
assert not smoke.hermetic_compile_lines(ok + "$ (link game)\n", cc), "negative control: no other pseudo-link form"
assert not smoke.hermetic_compile_lines("# link target=game\n", cc), "no command lines at all fails"
print("packaging smoke hermeticity: ok")

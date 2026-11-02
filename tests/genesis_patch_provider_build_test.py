#!/usr/bin/env python3
"""SEG-124 (ADR 0267): a trusted native-source patch package is compiled into a generated-native viewer program by the UNMODIFIED product: `segarecomp build --patch-package P --patch-identity ID`
re-verifies the package, extracts provider.c into the build's private area, re-checks its SHA-256, compiles it against the public SDK only and links it. Synthetic Genesis image and the
project-authored sample provider (docs/product/sdk-sample/provider.c); no game, no registry. Also proves the failure paths (changed package, identity mismatch, wrong image, missing ABI symbol,
compile error, private-header access) and that a Vanilla build contains no provider.
usage: genesis_patch_provider_build_test.py <segarecomp-cli> <C compiler> <source root> <SDL3 prefix>"""
import hashlib
import importlib.util
import json
import pathlib
import subprocess
import sys
import tempfile

IMAGE = bytes((0x00, 0xFF, 0x00, 0x04, 0x00, 0x00, 0x00, 0x08, 0x70, 0x00, 0x4E, 0x70))   # reset SSP/PC, MOVEQ #0,D0, RESET
failures = []


def check(cond, what):
    if not cond:
        failures.append(what)
        print("FAIL:", what)


def main():
    cli, compiler, root, sdl = sys.argv[1], sys.argv[2], pathlib.Path(sys.argv[3]).resolve(), pathlib.Path(sys.argv[4])
    spec = importlib.util.spec_from_file_location("segapatch", root / "tools" / "segapatch.py")
    sp = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(sp)
    provider = (root / "docs" / "product" / "sdk-sample" / "provider.c").read_bytes()
    caps = ["frame.work_ram", "frame.vdp", "scenery.words", "overlay.rgba"]
    with tempfile.TemporaryDirectory(prefix="segarecomp-provider-build-") as directory:
        tmp = pathlib.Path(directory)
        # every platform: the sample provider compiles as strict C11 against the public SDK header ALONE (no runtime/viewer header on the include path)
        for flags in (["-std=c11", "-Wall", "-Wextra", "-pedantic", "-Werror"],):
            r = subprocess.run([compiler, *flags, "-I", str(root / "platforms" / "genesis" / "sdk"), "-c", str(root / "docs" / "product" / "sdk-sample" / "provider.c"), "-o", str(tmp / "sample.o")], text=True, capture_output=True)
            check(r.returncode == 0, "the sample provider compiles as strict C11 against the SDK header only: " + r.stderr[-400:])
        if sys.platform != "linux":   # the full generated-native viewer build is exercised on the Linux runners (the build driver's SDL3 link is validated there)
            if failures:
                sys.exit(1)
            print("genesis_patch_provider_build_test: ok (SDK compile only on this platform)")
            return
        rom = tmp / "synthetic.bin"
        rom.write_bytes(IMAGE)

        def package(name, source=provider, **kw):
            out = tmp / name
            sp.create(IMAGE, None, out, id_="sample-provider", name="Sample", version="1.0.0", native_source=source, native_caps=kw.pop("caps", caps), author="Test", **kw)
            return out

        def identity(pkg):
            return json.loads(subprocess.run([cli, "patch", "inspect", str(pkg)], text=True, capture_output=True).stdout)["package_identity"]

        def build(out, pkg=None, ident=None, rom_path=rom):
            extra = ["--patch-package", str(pkg), "--patch-identity", ident or identity(pkg)] if pkg is not None else []
            return subprocess.run([cli, "build", "--rom", str(rom_path), "--output", str(out), "--cc", compiler, "--runtime-dir", str(root / "platforms" / "genesis"),
                                   "--optimize", "0", "--runtime-optimize", "1", "--sdl3-include", str(sdl / "include"), "--sdl3-lib", str(sdl / "lib"), *extra],
                                  text=True, capture_output=True, timeout=900)

        def exe(out):
            return next((p for p in out.iterdir() if p.stem == "game"), None)

        pkg = package("p.segapatch")
        out = tmp / "patched"
        r = build(out, pkg)
        check(r.returncode == 0, "the package's provider builds into a generated-native program: " + r.stdout[-600:] + r.stderr[-600:])
        st = json.loads((out / "status.json").read_text()) if (out / "status.json").exists() else {}
        n = st.get("native", {})
        check(n.get("provider_sha256") == hashlib.sha256(provider).hexdigest() and n.get("package_identity") == identity(pkg) and n.get("abi") == 1 and n.get("capabilities") == 15, "status records the package identity, provider digest, ABI and capabilities: " + str(n))
        check((out / "native" / "provider.c").read_bytes() == provider, "the verified source was extracted into the variant's own build folder")
        e = exe(out)
        check(e is not None and b"sample.synthetic" in e.read_bytes(), "the executable contains the package's provider (compiled in, no registry)")

        van = tmp / "vanilla"
        r = build(van)
        check(r.returncode == 0 and exe(van) is not None and b"sample.synthetic" not in exe(van).read_bytes(), "a Vanilla build contains no provider code")
        check("native" not in json.loads((van / "status.json").read_text()), "a Vanilla status carries no native member")

        # a changed package after approval: refused, nothing compiled
        other = package("changed.segapatch", source=provider + b"\n/* edited */\n")
        r = build(tmp / "changed", other, identity(pkg))
        check(r.returncode == 1 and "PATCH_IDENTITY_CHANGED" in r.stdout + (tmp / "changed" / "status.json").read_text() and not (tmp / "changed" / "native").exists(), "a package changed after approval is refused before extraction: " + r.stdout[-300:])
        r = build(tmp / "badid", pkg, "0" * 64)
        check(r.returncode == 1, "an identity the package does not have is refused")
        # the image being built must be the one the package declares
        wrong = tmp / "wrong.bin"
        wrong.write_bytes(IMAGE + b"\0\0")
        r = build(tmp / "wrongimg", pkg, rom_path=wrong)
        check(r.returncode == 1 and "PATCH_IMAGE_MISMATCH" in (tmp / "wrongimg" / "status.json").read_text(), "another image than the declared one is refused")
        # a BPS-only package carries no provider
        bps = tmp / "bps.segapatch"
        t = bytearray(IMAGE); t[8] = 0x72
        sp.create(IMAGE, bytes(t), bps, id_="b", name="B", version="1")
        r = build(tmp / "bpsbuild", bps)
        check(r.returncode == 1, "a package without a provider cannot be used as a provider reference")

        def broken(name, source, expect_fragment):
            p = package(name, source=source)
            o = tmp / ("out-" + name)
            rr = build(o, p)
            check(rr.returncode != 0 and not (exe(o) if o.exists() else None), f"{name}: the build fails closed ({rr.returncode}) {rr.stdout[-200:]}")
            check(expect_fragment in rr.stdout + ((o / "build.log").read_text() if (o / "build.log").exists() else ""), f"{name}: the diagnostic names the cause")
        broken("nosymbol.segapatch", b'#include "segarecomp_genesis_patch.h"\nint unrelated(void) { return 1; }\n', "segarecomp_patch_provider")
        broken("syntax.segapatch", b'#include "segarecomp_genesis_patch.h"\nthis is not C\n', "error")
        broken("private.segapatch", b'#include "runtime.h"\nconst int x = 1;\n', "runtime.h")
        # a provider that reports another ABI version at run time still builds; the viewer then refuses to attach it and presents generically (genesis_native_compat_viewer_sdl_test)
        # an injected shell string in the package cannot reach a command line: ids/authors are data, the compiler argv is fixed
        evil = tmp / "evil.segapatch"
        sp.create(IMAGE, None, evil, id_="x-touch-pwned", name="`touch pwned` $(touch pwned)", version="1", native_source=provider, native_caps=caps, author="a; touch pwned")
        o = tmp / "evil-out"
        r = build(o, evil)
        check(r.returncode == 0 and not (pathlib.Path.cwd() / "pwned").exists() and not (tmp / "pwned").exists(), "package strings never reach a command line")
    if failures:
        print(f"genesis_patch_provider_build_test: {len(failures)} failure(s)")
        sys.exit(1)
    print("genesis_patch_provider_build_test: ok")


if __name__ == "__main__":
    main()

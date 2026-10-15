#!/usr/bin/env python3
"""Pure-Python checks of the packaged SDL3 build (CI glue, not shipped; no third-party modules, no host tools).

usage:
  sdl_check.py driver  <sdl3 library> [--dynamic-libusb <file name SDL must load, e.g. libusb-1.0.dll>]
  sdl_check.py windows <SDL3.dll> <libusb-1.0.dll> <SDL3.lib> <libSDL3.dll.a>
  sdl_check.py mkdef   <SDL3.dll> <out.def>
  sdl_check.py exports <dll>

`driver` answers one narrow question: was SDL's Nintendo Switch 2 USB driver COMPILED into this binary? It looks for several independent
literals the driver (and only the driver) carries. That is evidence of compilation only: it says nothing about the libusb runtime, the USB
interface binding of a controller, or a physical controller (see docs/testing/input-diagnostics.md).

`windows` checks the Windows package contract: SDL3.dll and libusb-1.0.dll are x86-64 DLLs, SDL3.dll loads libusb lazily by name (it is not an
import, so SDL3.dll still loads without it), every libusb function SDL3.dll looks up is exported by the packaged libusb DLL, neither DLL needs a
redistributable runtime, and both import libraries (MSVC SDL3.lib for the launcher, MinGW libSDL3.dll.a for the bundled Zig) cover exactly the
exports of the SDL3.dll that ships.
"""
import re
import struct
import sys

# Literals that exist in an SDL3 binary only when SDL_JOYSTICK_HIDAPI_SWITCH2 (the libusb driver) was compiled in.
DRIVER_LITERALS = (
    b"SDL_JOYSTICK_HIDAPI_SWITCH2",
    b"Nintendo Switch2 controllers not supported over Bluetooth",
    b"Couldn't find bulk endpoints",
    b"SEGARECOMP_HIDAPI_SETUP_%04x_%04x",  # packaging/sdl-switch2-diagnostics.patch (setup trace for the input diagnostics) is applied
)
FORBIDDEN_RUNTIME = re.compile(r"^(vcruntime|msvcp|msvcr\d|concrt|vccorlib|ucrtbase|libgcc|libstdc|libwinpthread|libusb)", re.I)
REQUIRED_SDL_EXPORTS = ("SDL_Init", "SDL_OpenGamepad", "SDL_GetJoysticks", "SDL_hid_enumerate", "SDL_GetGamepadConnectionState", "SDL_CreateWindow")


class PE:
    def __init__(self, path):
        self.path = str(path)
        self.data = data = open(path, "rb").read()
        if data[:2] != b"MZ":
            raise SystemExit(f"{path}: not a PE file")
        pe = struct.unpack_from("<I", data, 0x3C)[0]
        if data[pe:pe + 4] != b"PE\0\0":
            raise SystemExit(f"{path}: not a PE file")
        self.machine, n_sections, _, _, _, opt_size, self.characteristics = struct.unpack_from("<HHIIIHH", data, pe + 4)
        opt = pe + 24
        magic = struct.unpack_from("<H", data, opt)[0]
        self.pe32_plus = magic == 0x20B
        self.dirs = opt + (112 if self.pe32_plus else 96)
        self.sections = [struct.unpack_from("<IIII", data, opt + opt_size + 40 * i + 8) for i in range(n_sections)]  # vsize, vaddr, rawsize, rawptr

    def off(self, rva):
        for vsize, vaddr, rawsize, rawptr in self.sections:
            if vaddr <= rva < vaddr + max(vsize, rawsize):
                return rva - vaddr + rawptr
        raise SystemExit(f"{self.path}: bad RVA {rva:#x}")

    def cstr(self, rva):
        at = self.off(rva)
        return self.data[at:self.data.index(b"\0", at)].decode("ascii", "replace")

    def directory(self, index):
        return struct.unpack_from("<II", self.data, self.dirs + 8 * index)

    @property
    def is_x64_dll(self):
        return self.machine == 0x8664 and self.pe32_plus and bool(self.characteristics & 0x2000)

    def exports(self):
        rva, size = self.directory(0)
        if not rva:
            return []
        at = self.off(rva)
        n_names, _, names_rva, _ = struct.unpack_from("<IIII", self.data, at + 24)
        return [self.cstr(struct.unpack_from("<I", self.data, self.off(names_rva) + 4 * i)[0]) for i in range(n_names)]

    def imports(self):
        rva, _ = self.directory(1)
        names = []
        if rva:
            at = self.off(rva)
            while True:
                fields = struct.unpack_from("<IIIII", self.data, at)
                if fields[3] == 0:
                    break
                names.append(self.cstr(fields[3]))
                at += 20
        return names


def archive_symbols(path):
    """Symbol names of the first linker member of a COFF/GNU archive (import libraries of both toolchains have one)."""
    data = open(path, "rb").read()
    if data[:8] != b"!<arch>\n":
        raise SystemExit(f"{path}: not an archive")
    header = data[8:68]
    name, size = header[:16].rstrip(), int(header[48:58])
    if name != b"/":
        raise SystemExit(f"{path}: no first linker member")
    body = data[68:68 + size]
    count = struct.unpack_from(">I", body, 0)[0]
    strings = body[4 + 4 * count:]
    return set(s.decode("ascii", "replace") for s in strings.split(b"\0") if s)


def check(condition, message, problems):
    if not condition:
        problems.append(message)


def driver(args):
    path = args[0]
    blob = open(path, "rb").read()
    missing = [lit.decode() for lit in DRIVER_LITERALS if lit not in blob]
    problems = []
    check(not missing, f"{path}: the Switch 2 USB driver is NOT compiled in (absent literals: {missing})", problems)
    if "--dynamic-libusb" in args:
        wanted = args[args.index("--dynamic-libusb") + 1].encode()
        check(wanted in blob, f"{path}: SDL does not carry the run-time libusb name {wanted.decode()} (libusb would be linked statically or not found)", problems)
    return problems


def windows(args):
    sdl_dll, usb_dll, msvc_lib, mingw_lib = args
    problems = []
    sdl, usb = PE(sdl_dll), PE(usb_dll)
    check(sdl.is_x64_dll, f"{sdl_dll}: not an x86-64 DLL", problems)
    check(usb.is_x64_dll, f"{usb_dll}: not an x86-64 DLL", problems)
    sdl_exports, usb_exports = set(sdl.exports()), set(usb.exports())
    for symbol in REQUIRED_SDL_EXPORTS:
        check(symbol in sdl_exports, f"{sdl_dll}: missing export {symbol}", problems)
    usb_name = usb_dll.replace("\\", "/").rsplit("/", 1)[-1]
    sdl_imports = [name.lower() for name in sdl.imports()]
    check(usb_name.lower() not in sdl_imports, f"{sdl_dll}: imports {usb_name} statically; SDL must load it lazily so SDL3.dll still loads without it", problems)
    for pe, label in ((sdl, sdl_dll), (usb, usb_dll)):
        for dll in pe.imports():
            check(not FORBIDDEN_RUNTIME.match(dll), f"{label}: depends on {dll}, which a clean machine may not have", problems)
    blob = sdl.data
    check(usb_name.encode() in blob, f"{sdl_dll}: does not carry the run-time libusb name {usb_name}", problems)
    wanted = sorted(set(m.decode() for m in re.findall(rb"libusb_[a-z0-9_]+", blob)))
    check(len(wanted) >= 20, f"{sdl_dll}: fewer libusb entry points than expected ({len(wanted)}); libusb support is not compiled in", problems)
    absent = [name for name in wanted if name not in usb_exports]
    check(not absent, f"{usb_dll} does not export what {sdl_dll} looks up: {absent}", problems)
    for lib, label in ((msvc_lib, "MSVC import library (launcher)"), (mingw_lib, "MinGW import library (bundled Zig)")):
        symbols = archive_symbols(lib)
        lacking = sorted(e for e in sdl_exports if e not in symbols or "__imp_" + e not in symbols)
        check(not lacking, f"{label} {lib} lacks {len(lacking)} of {len(sdl_exports)} SDL3.dll exports, for example {lacking[:5]}", problems)
    return problems, {"sdl_exports": len(sdl_exports), "libusb_entry_points_looked_up": len(wanted), "libusb_exports": len(usb_exports)}


def main():
    command, args = (sys.argv[1], sys.argv[2:]) if len(sys.argv) > 1 else ("", [])
    info = {}
    if command == "driver" and args:
        problems = driver(args)
    elif command == "windows" and len(args) == 4:
        problems, info = windows(args)
    elif command == "mkdef" and len(args) == 2:
        names = PE(args[0]).exports()
        open(args[1], "w", newline="\n").write("LIBRARY SDL3.dll\nEXPORTS\n" + "".join(f"  {n}\n" for n in sorted(names)))
        print(f"wrote {len(names)} exports to {args[1]}")
        return
    elif command == "exports" and len(args) == 1:
        print("\n".join(sorted(PE(args[0]).exports())))
        return
    else:
        raise SystemExit(__doc__)
    if problems:
        print("\n".join("sdl_check: " + p for p in problems), file=sys.stderr)
        sys.exit(1)
    print(f"sdl_check {command}: OK {info if info else ''}".rstrip())


if __name__ == "__main__":
    main()

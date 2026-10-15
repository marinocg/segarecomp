#!/usr/bin/env python3
"""packaging/sdl_check.py against synthetic PE/archive fixtures (no SDL, no libusb, no Windows needed).

Proves the CI gate logic, including its negative controls: a DLL without the Switch 2 driver literals is rejected; so are a libusb that is linked
statically instead of loaded lazily, a libusb DLL lacking a function SDL looks up, an import library that does not cover SDL3.dll's exports, a
redistributable runtime dependency and a 32-bit DLL. These fixtures prove the checker, not any real SDL build and not a physical controller.
"""
import importlib.util
import pathlib
import struct
import sys
import tempfile

root = pathlib.Path(sys.argv[1])
spec = importlib.util.spec_from_file_location("sdl_check", root / "packaging" / "sdl_check.py")
sdl_check = importlib.util.module_from_spec(spec)
spec.loader.exec_module(sdl_check)

SECTION_RVA, SECTION_RAW = 0x1000, 0x400


def make_dll(exports=(), imports=(), extra=b"", machine=0x8664):
    """A minimal PE32+ DLL with one section holding an export table, an import table and `extra` bytes."""
    body = bytearray()

    def here():
        return SECTION_RVA + len(body)

    def string(text):
        at = here()
        body.extend(text.encode() + b"\0")
        return at

    export_dir_at = len(body)
    body.extend(bytes(40))
    names = [string(n) for n in sorted(exports)]
    names_at = here()
    for rva in names:
        body.extend(struct.pack("<I", rva))
    dll_name = string("fixture.dll")
    struct.pack_into("<IIHHIIIIIII", body, export_dir_at, 0, 0, 0, 0, dll_name, 1, len(names), len(names), 0, names_at, 0)
    import_names = [string(n) for n in imports]
    import_dir_rva = here()
    for name_rva in import_names:
        body.extend(struct.pack("<IIIII", 0, 0, 0, name_rva, 0))
    body.extend(bytes(20))
    body.extend(extra)
    optional = bytearray(240)
    struct.pack_into("<H", optional, 0, 0x20B)
    struct.pack_into("<II", optional, 112, SECTION_RVA + export_dir_at, 40 if exports else 0)
    struct.pack_into("<II", optional, 120, import_dir_rva if imports else 0, 20 * (len(imports) + 1))
    coff = struct.pack("<HHIIIHH", machine, 1, 0, 0, 0, len(optional), 0x2022)
    section = struct.pack("<8sIIIIIIHHI", b".rdata", len(body), SECTION_RVA, len(body), SECTION_RAW, 0, 0, 0, 0, 0x40000040)
    header = bytearray(0x40) + b"PE\0\0" + coff + bytes(optional) + section
    header[:2] = b"MZ"
    struct.pack_into("<I", header, 0x3C, 0x40)
    return bytes(header) + bytes(SECTION_RAW - len(header)) + bytes(body)


def make_archive(symbols):
    names = b"".join(s.encode() + b"\0" for s in symbols)
    body = struct.pack(">I", len(symbols)) + bytes(4 * len(symbols)) + names
    header = b"/".ljust(16) + b"0".ljust(12) + b"0".ljust(6) + b"0".ljust(6) + b"0".ljust(8) + str(len(body)).encode().ljust(10) + b"`\n"
    return b"!<arch>\n" + header + body + (b"\n" if len(body) % 2 else b"")


SDL_EXPORTS = list(sdl_check.REQUIRED_SDL_EXPORTS) + ["SDL_Quit", "SDL_GetError"]
USB_FUNCTIONS = ["libusb_" + name for name in ("init", "exit", "open", "close", "claim_interface", "release_interface", "bulk_transfer", "get_device_list",
                                              "free_device_list", "get_device_descriptor", "get_config_descriptor", "free_config_descriptor", "get_bus_number",
                                              "get_port_numbers", "get_device_address", "handle_events", "interrupt_transfer", "reset_device", "set_debug", "strerror",
                                              "get_device", "kernel_driver_active", "detach_kernel_driver", "set_auto_detach_kernel_driver")]
LITERALS = b"".join(l + b"\0" for l in sdl_check.DRIVER_LITERALS)
LOOKUPS = b"".join(f.encode() + b"\0" for f in USB_FUNCTIONS)


def sdl_dll(imports=("KERNEL32.dll",), literals=LITERALS, lookups=LOOKUPS, name=b"libusb-1.0.dll\0", machine=0x8664, exports=SDL_EXPORTS):
    return make_dll(exports, imports, literals + lookups + name, machine)


def lib_for(exports):
    return make_archive([e for e in exports] + ["__imp_" + e for e in exports])


def run_windows(tmp, sdl=None, usb=None, msvc=None, mingw=None):
    files = {"SDL3.dll": sdl or sdl_dll(), "libusb-1.0.dll": usb or make_dll(USB_FUNCTIONS, ("KERNEL32.dll", "msvcrt.dll", "USER32.dll")),
             "SDL3.lib": msvc or lib_for(SDL_EXPORTS), "libSDL3.dll.a": mingw or lib_for(SDL_EXPORTS)}
    for name, data in files.items():
        (tmp / name).write_bytes(data)
    problems, info = sdl_check.windows([str(tmp / n) for n in ("SDL3.dll", "libusb-1.0.dll", "SDL3.lib", "libSDL3.dll.a")])
    return problems, info


def expect(problems, needle, what):
    assert any(needle in p for p in problems), f"negative control failed ({what}): {problems}"


def main():
    pe_file = make_dll(["b_export", "a_export"], ["KERNEL32.dll", "USER32.dll"])
    with tempfile.TemporaryDirectory() as d:
        tmp = pathlib.Path(d)
        (tmp / "p.dll").write_bytes(pe_file)
        pe = sdl_check.PE(tmp / "p.dll")
        assert pe.is_x64_dll and pe.exports() == ["a_export", "b_export"] and pe.imports() == ["KERNEL32.dll", "USER32.dll"], "PE parser round trip"
        (tmp / "x.lib").write_bytes(make_archive(["alpha", "__imp_alpha"]))
        assert sdl_check.archive_symbols(tmp / "x.lib") == {"alpha", "__imp_alpha"}, "archive symbol table"

        # driver literals: present passes; any absent literal is a failure naming it
        (tmp / "good.bin").write_bytes(LITERALS + b"libusb-1.0.dll\0")
        assert sdl_check.driver([str(tmp / "good.bin"), "--dynamic-libusb", "libusb-1.0.dll"]) == [], "driver present"
        (tmp / "bad.bin").write_bytes(b"SDL_JOYSTICK_HIDAPI_SWITCH2\0")  # the hint name alone is NOT accepted as proof
        expect(sdl_check.driver([str(tmp / "bad.bin")]), "NOT compiled in", "a single literal must not be enough")
        (tmp / "static.bin").write_bytes(LITERALS)
        expect(sdl_check.driver([str(tmp / "static.bin"), "--dynamic-libusb", "libusb-1.0.dll"]), "run-time libusb name", "no lazy libusb name")

        # the Windows package contract
        problems, info = run_windows(tmp)
        assert problems == [] and info["libusb_entry_points_looked_up"] == len(USB_FUNCTIONS), f"the consistent fixture set passes: {problems}"
        expect(run_windows(tmp, sdl=sdl_dll(imports=("KERNEL32.dll", "libusb-1.0.dll")))[0], "imports libusb-1.0.dll statically", "static libusb import")
        expect(run_windows(tmp, sdl=sdl_dll(imports=("KERNEL32.dll", "VCRUNTIME140.dll")))[0], "VCRUNTIME140.dll", "redistributable runtime")
        expect(run_windows(tmp, sdl=sdl_dll(machine=0x14C))[0], "not an x86-64 DLL", "32-bit DLL")
        expect(run_windows(tmp, sdl=sdl_dll(name=b"other.dll\0"))[0], "run-time libusb name", "SDL does not load the shipped libusb name")
        expect(run_windows(tmp, sdl=sdl_dll(lookups=b""))[0], "fewer libusb entry points", "no libusb support compiled in")
        expect(run_windows(tmp, usb=make_dll(USB_FUNCTIONS[1:], ("KERNEL32.dll",)))[0], "does not export what", "libusb lacks a function SDL looks up")
        expect(run_windows(tmp, usb=make_dll(USB_FUNCTIONS, ("KERNEL32.dll", "VCRUNTIME140.dll")))[0], "VCRUNTIME140.dll", "libusb needs a redistributable")
        expect(run_windows(tmp, msvc=lib_for(SDL_EXPORTS[:-1]))[0], "MSVC import library", "launcher import library lacks an export")
        expect(run_windows(tmp, mingw=make_archive(SDL_EXPORTS))[0], "MinGW import library", "Zig import library lacks __imp_ entries")
        expect(run_windows(tmp, sdl=sdl_dll(exports=SDL_EXPORTS[1:]))[0], "missing export", "SDL3.dll lacks a required export")
    print("packaging sdl_check: ok")


main()

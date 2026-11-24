"""Build and package the NXInputRelay Atmosphere sysmodule."""

import hashlib
import os
from pathlib import Path
import shutil
import struct
import subprocess


TITLE_ID = "4200000000006001"
ROOT = Path(__file__).resolve().parent
KIT = Path(os.environ.get("PLUME_DEVKITPRO", "C:/devkitPro" if os.name == "nt" else "/opt/devkitpro"))
EXE = ".exe" if os.name == "nt" else ""
GCC = KIT / "devkitA64/bin" / f"aarch64-none-elf-gcc{EXE}"
TOOLS = KIT / "tools/bin"
BUILD = ROOT / "build"
TARGET = ROOT / "nxinputrelay_sysmodule"
PACKAGE = ROOT / "atmosphere/contents" / TITLE_ID
ENV = os.environ.copy()
ENV["DEVKITPRO"] = KIT.as_posix()
ENV["PATH"] = os.pathsep.join(
    [str(GCC.parent), str(TOOLS), ENV.get("PATH", "")]
)


def run(*args: object) -> None:
    print(" ".join(map(str, args)), flush=True)
    subprocess.run(list(map(str, args)), cwd=ROOT, env=ENV, check=True)


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def load_image_memory(path: Path) -> int:
    """Return the page-aligned virtual extent of all ELF PT_LOAD segments."""
    data = path.read_bytes()
    if data[:5] != b"\x7fELF\x02":
        raise RuntimeError(f"Unexpected ELF format: {path}")
    phoff = struct.unpack_from("<Q", data, 0x20)[0]
    phentsize = struct.unpack_from("<H", data, 0x36)[0]
    phnum = struct.unpack_from("<H", data, 0x38)[0]
    extent = 0
    for index in range(phnum):
        offset = phoff + index * phentsize
        if struct.unpack_from("<I", data, offset)[0] != 1:  # PT_LOAD
            continue
        vaddr = struct.unpack_from("<Q", data, offset + 0x10)[0]
        memsz = struct.unpack_from("<Q", data, offset + 0x28)[0]
        extent = max(extent, vaddr + memsz)
    return (extent + 0xFFFF) & ~0xFFFF


def main() -> None:
    port_header = (ROOT / "include/network_input.h").read_text(encoding="utf-8")
    if "#define NETWORK_INPUT_PORT 6002" not in port_header:
        raise SystemExit(
            "Refusing sysmodule build: include/network_input.h must define port 6002"
        )
    BUILD.mkdir(exist_ok=True)
    flags = [
        "-std=gnu11", "-O2", "-g", "-Wall", "-Wextra", "-Wformat=2",
        "-Werror=implicit-function-declaration", "-Werror=incompatible-pointer-types",
        "-ffunction-sections", "-fdata-sections", "-march=armv8-a+crc+crypto",
        "-mtune=cortex-a57", "-mtp=soft", "-fPIE", "-D__SWITCH__",
        "-Iinclude", f"-I{KIT.as_posix()}/libnx/include",
        f"-fdebug-prefix-map={ROOT}=.", f"-fdebug-prefix-map={ROOT.as_posix()}=.",
    ]

    objects: list[Path] = []
    for source in sorted((ROOT / "source").glob("*.c")):
        obj = BUILD / f"{source.stem}.o"
        objects.append(obj)
        run(GCC, *flags, "-c", source, "-o", obj)

    elf = TARGET.with_suffix(".elf")
    nso = TARGET.with_suffix(".nso")
    npdm = TARGET.with_suffix(".npdm")
    nsp = TARGET.with_suffix(".nsp")
    map_file = TARGET.with_suffix(".map")

    run(
        GCC,
        f"-specs={KIT.as_posix()}/libnx/switch.specs",
        "-march=armv8-a+crc+crypto", "-mtune=cortex-a57", "-mtp=soft", "-fPIE",
        f"-Wl,-Map,{map_file.as_posix()}", *objects,
        f"-L{KIT.as_posix()}/libnx/lib", "-lnx", "-o", elf,
    )
    image_memory = load_image_memory(elf)
    if image_memory > 2 * 1024 * 1024:
        raise SystemExit(
            f"Refusing oversized sysmodule image: {image_memory / 1048576:.2f} MiB"
        )
    print(f"Static image memory: {image_memory / 1048576:.2f} MiB (limit 2.00 MiB)")
    run(TOOLS / f"elf2nso{EXE}", elf, nso)
    run(TOOLS / f"npdmtool{EXE}", ROOT / "nxinputrelay_sysmodule.json", npdm)

    exefs = BUILD / "exefs"
    if exefs.exists():
        shutil.rmtree(exefs)
    exefs.mkdir()
    shutil.copy2(nso, exefs / "main")
    shutil.copy2(npdm, exefs / "main.npdm")
    run(TOOLS / f"build_pfs0{EXE}", exefs, nsp)

    PACKAGE.mkdir(parents=True, exist_ok=True)
    shutil.copy2(nsp, PACKAGE / "exefs.nsp")
    (PACKAGE / "flags").mkdir(exist_ok=True)
    # Atmosphere treats this as a presence-only marker. Keep it nonempty so
    # Windows MTP/ZIP copy paths that discard zero-byte files still transfer it.
    (PACKAGE / "flags/boot2.flag").write_text("boot2\n", encoding="ascii")

    archive_base = ROOT / f"nxinputrelay_sysmodule_v1.3.6-touch_atmosphere_{TITLE_ID}"
    archive = Path(shutil.make_archive(str(archive_base), "zip", ROOT, "atmosphere"))

    print(f"Built: {nsp}")
    print(f"SHA-256: {digest(nsp)}")
    print(f"Install tree: {ROOT / 'atmosphere'}")
    print(f"Install ZIP: {archive}")
    print(f"ZIP SHA-256: {digest(archive)}")


if __name__ == "__main__":
    main()

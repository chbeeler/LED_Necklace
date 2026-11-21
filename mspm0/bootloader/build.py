"""Build the loader and package the current CCS Debug build for UniFlash."""
import argparse
import os
import struct
import subprocess
from pathlib import Path

src = Path(__file__).resolve().parent
root = src.parent
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--profile", choices=["full", "light"], default="full")
light = parser.parse_args().profile == "light"
app_start = 0x1000 if light else 0x1800
build_dir = root / ("Light" if light else "Debug")
out = build_dir / "boot-images"
out.mkdir(parents=True, exist_ok=True)
sdk = Path(os.environ.get("MSPM0_SDK_INSTALL_DIR", "C:/ti/mspm0_sdk_2_11_00_07"))
compiler = Path(os.environ.get("TICLANG_ARMCOMPILER", "C:/ti/ccs2101/ccs/tools/compiler/ti-cgt-armllvm_5.1.1.LTS"))
def run(tool, *args):
    subprocess.run([compiler / "bin" / (tool + ".exe"), *args], cwd=out, check=True)

flags = ["-mcpu=cortex-m0plus", "-mthumb", "-mfloat-abi=soft", "-Oz", "-g",
         "-D__MSPM0C1104__", "-I" + str(src), "-I" + str(sdk / "source"),
         "-I" + str(sdk / "source/third_party/CMSIS/Core/Include")]
if light:
    flags.append("-DAEQ_LIGHT=1")
objects = []
for file in sorted(src.glob("*.c")):
    obj = file.stem + ".obj"
    run("tiarmclang", *flags, "-c", file, "-o", obj)
    objects.append(obj)
run("tiarmclang", *flags, *objects, "-Wl,-u,_c_int00", "-Wl,--stack_size=256",
    *(["-Wl,--define=AEQ_LIGHT=1", "-Wl,--cinit_compression=off"] if light else []),
    src / "mspm0c1104.cmd", sdk / "source/ti/driverlib/lib/ticlang/m0p/mspm0c110x/driverlib.a",
    "-Wl,--rom_model", "-Wl,-m,loader.map", "-llibc.a", "-o", "loader.out")

def image(elf, start, limit):
    # First check every loadable byte in TI-TXT, then fill alignment holes.
    textfile = elf.stem + "-sections.txt"
    run("tiarmhex", "--ti_txt", "--memwidth=8", "--romwidth=8", "-o", textfile, elf)
    data = {}
    for token in (out / textfile).read_text().split():
        if token.startswith("@"):
            addr = int(token[1:], 16)
        elif token.lower() == "q":
            break
        else:
            if not start <= addr < limit or addr in data:
                raise ValueError(f"{elf}: unexpected byte at {addr:#x}")
            data[addr] = int(token, 16)
            addr += 1
    if min(data) != start: raise ValueError("Missing vector table")
    buf = bytes(data.get(i, 255) for i in range(start, (max(data)+8) & ~7))
    sp, pc = struct.unpack_from("<II", buf)
    if not (0x20000000 < sp <= 0x20000400 and sp % 8 == 0 and pc & 1 and start <= pc < start+len(buf)):
        raise ValueError(f"{elf}: invalid vectors: {sp:#x}, {pc:#x}")
    print(f"{elf.name}: {len(buf)} bytes at {start:#06x}; reset {pc:#06x}")
    return buf

def txt(start, buf):
    return f"@{start:04X}\n" + "".join(buf[i:i+16].hex(" ").upper() + "\n" for i in range(0,len(buf),16))
loader = txt(0, image(out / "loader.out", 0, app_start))
app = txt(app_start, image(build_dir / "AEQ.out", app_start, 0x4000))
(out / "AEQ-update.txt").write_text(app + "q\n")
(out / "AEQ-install.txt").write_text(loader + app + "q\n")

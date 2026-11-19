"""Build the same sources and SysConfig input used by the CCS project."""
import argparse
import os
from pathlib import Path
import subprocess
import xml.etree.ElementTree as ET

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--sdk", type=Path, default=os.environ.get("MSPM0_SDK_INSTALL_DIR", "C:/ti/mspm0_sdk_2_11_00_07"))
    parser.add_argument("--ccs", type=Path, default=os.environ.get("CCS_INSTALL_DIR", "C:/ti/ccs2101/ccs"))
    args = parser.parse_args()
    project = Path(__file__).resolve().parent
    build = project / "Debug"
    build.mkdir(exist_ok=True)
    compiler = args.ccs / "tools/compiler/ti-cgt-armllvm_5.1.1.LTS"
    sysconfig = args.ccs / "utils/sysconfig_1.28.1/sysconfig_cli.bat"
    subprocess.run([str(sysconfig), "--compiler", "ticlang", "--product",
                    str(args.sdk / ".metadata/product.json"), "--output", str(build),
                    str(project / "board.syscfg")], check=True)
    sources = sorted(project.glob("*.c")) + sorted(project.glob("*.cpp"))
    sources += sorted(build.glob("*.c"))
    for link in ET.parse(project / ".project").findall(".//link"):
        uri = link.findtext("locationURI")
        prefix, relative = uri.split("-PROJECT_LOC/", 1)
        levels = int(prefix.removeprefix("PARENT-"))
        sources.append(project.parents[levels - 1] / relative)
    flags = ["-mcpu=cortex-m0plus", "-march=thumbv6m", "-mthumb",
             "-mfloat-abi=soft", "-O2", "-g", "-Wall", "@device.opt",
             "-I" + str(project), "-I" + str(build),
             "-I" + str(project.parents[2] / "LED_Necklace/include"),
             "-I" + str(args.sdk / "source"),
             "-I" + str(args.sdk / "source/third_party/CMSIS/Core/Include")]
    objects = []
    for source in sources:
        obj = build / (source.stem + ".o")
        subprocess.run([str(compiler / "bin/tiarmclang.exe"), *flags,
                        "-c", str(source), "-o", str(obj)], cwd=build, check=True)
        objects.append(str(obj))
    subprocess.run([str(compiler / "bin/tiarmclang.exe"), *flags, *objects,
                    "device_linker.cmd", "-ldevice.cmd.genlibs",
                    "-L" + str(args.sdk / "source"), "-L" + str(compiler / "lib"),
                    "-Wl,--rom_model", "-Wl,--warn_sections",
                    "-Wl,-m,AEQ_L1306.map", "-o", "AEQ_L1306.out", "-llibc.a"],
                   cwd=build, check=True)
    print(build / "AEQ_L1306.out")

if __name__ == "__main__":
    main()

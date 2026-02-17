Import("env")

from pathlib import Path


def build_uf2(source, target, env):
    build_dir = Path(env.subst("$BUILD_DIR"))
    hex_path = build_dir / f"{env.subst('$PROGNAME')}.hex"
    uf2_path = build_dir / f"{env.subst('$PROGNAME')}.uf2"

    uf2conv = Path(r"C:\CBE\Adafruit_nRF52_Bootloader\lib\uf2\utils\uf2conv.py")
    if not uf2conv.exists():
        print(f"[UF2] Skipped: uf2conv.py not found at {uf2conv}")
        return

    cmd = (
        f'"{env.subst("$PYTHONEXE")}" "{uf2conv}" '
        f'"{hex_path}" -c -f NRF52840 -o "{uf2_path}"'
    )

    print(f"[UF2] Generating {uf2_path.name} from {hex_path.name}")
    if env.Execute(cmd) != 0:
        raise RuntimeError("[UF2] uf2 conversion failed")


env.AddPostAction("$BUILD_DIR/${PROGNAME}.hex", build_uf2)

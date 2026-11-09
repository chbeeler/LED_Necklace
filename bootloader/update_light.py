"""Update the AEQ Light configuration over UART (requires pyserial)."""
import argparse
from pathlib import Path
import struct
import serial
from uart_bsl import command, crc, ok

BASE = 0x1000


def check_loader(port):
    command(port, b"\x12", False)
    ident = command(port, b"\x19")
    if len(ident) != 25 or ident[0] != 0x31 or ident[21:25] != b"AQL1":
        raise IOError("Not the AEQ light v1 loader. No flash was erased.")


def application():
    path = Path(__file__).resolve().parent.parent / "Light/boot-images/AEQ-update.txt"
    tokens = path.read_text().split()
    if tokens[:1] != ["@1000"] or tokens[-1:] != ["q"]:
        raise ValueError("Expected a light application-only image at 0x1000")
    data = bytes.fromhex(" ".join(tokens[1:-1]))
    if not 1032 <= len(data) <= 0x4000-BASE or len(data) % 8:
        raise ValueError("Invalid application size for CRC verification")
    sp, pc = struct.unpack_from("<II", data)
    if not (0x20000000 < sp <= 0x20000400 and sp % 8 == 0 and pc & 1 and BASE+192 <= pc < BASE+len(data)):
        raise ValueError("Invalid light application vectors")
    return data


def verify(port, addr, data):
    reply = command(port, b"\x26" + struct.pack("<II", addr, len(data)))
    if reply != b"\x32" + struct.pack("<I", crc(data)):
        raise IOError(f"Application CRC mismatch at {addr:#x}; retry the update.")


def update(port, data):
    check_loader(port)
    ok(port, b"\x21" + b"\xff"*32)
    ok(port, b"\x23" + struct.pack("<II", BASE, 0x3fff))
    for off in range(8, len(data), 128):
        ok(port, b"\x20" + struct.pack("<I", BASE+off) + data[off:off+128])
    verify(port, BASE+8, data[8:])
    ok(port, b"\x20" + struct.pack("<I", BASE) + data[:8])
    verify(port, BASE, data)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("port")
    parser.add_argument("--probe", action="store_true", help="identify the light loader without writing")
    args = parser.parse_args()
    with serial.Serial(args.port, 9600, timeout=3, write_timeout=3) as port:
        if args.probe:
            check_loader(port)
            print("AEQ light v1 loader answered.")
        else:
            data = application()
            update(port, data)
            print(f"Verified CRC of all {len(data)} bytes. Tap RESET without S2 to start AEQ.")


if __name__ == "__main__":
    main()

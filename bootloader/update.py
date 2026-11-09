"""MSPM0C1104 UART updater for the experimental AEQ loader (requires pyserial)."""
import argparse
import struct
from pathlib import Path
import serial


def crc(data):
    c = 0xffff
    for byte in data:
        c ^= byte
        for _ in range(8):
            c = (c >> 1) ^ (0x8408 if c & 1 else 0)
    return c


def read(port, n):
    data = port.read(n)
    if len(data) != n:
        raise IOError("No complete UART reply. Hold S2, tap RESET, release S2, and retry.")
    return data


def command(port, payload, reply=True):
    port.write(b"\x80" + struct.pack("<H", len(payload)) + payload + struct.pack("<I", crc(payload)))
    ack = read(port, 1)
    if ack != b"\0": raise IOError("UART rejected frame: " + ack.hex())
    if reply:
        if read(port, 1) != b"\x08": raise IOError("Invalid reply header")
        size, = struct.unpack("<H", read(port, 2))
        if not 1 <= size <= 224: raise IOError("Invalid reply length")
        body = read(port, size)
        if read(port, 4) != struct.pack("<I", crc(body)): raise IOError("Bad reply CRC")
        return body


def ok(port, payload):
    reply = command(port, payload)
    if reply != b"\x3b\0": raise IOError("Loader rejected command: " + reply.hex())


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("port", help="XDS110 Application/User UART, e.g. COM42")
    parser.add_argument("--probe", action="store_true", help="check the connection without writing")
    args = parser.parse_args()
    folder = Path(__file__).resolve().parent.parent / "Debug/boot-images"
    tokens = (folder / "AEQ-update.txt").read_text().split()
    if tokens[0] != "@1800" or tokens[-1] != "q": raise ValueError("Expected application-only TI-TXT")
    app = bytes.fromhex(" ".join(tokens[1:-1]))
    if not (192 <= len(app) <= 0x2800 and len(app) % 8 == 0): raise ValueError("Invalid application size")
    with serial.Serial(args.port, 9600, timeout=3, write_timeout=3) as port:
        command(port, b"\x12", False)
        ok(port, b"\x21" + b"\xff"*32)
        # Identify this loader before writing: compare the resident reset vectors to the installation image.
        install = (folder / "AEQ-install.txt").read_text().split()
        def verify(addr, data):
            reply = command(port, b"\x29" + struct.pack("<II", addr, len(data)))
            if reply != b"\x30" + data: raise IOError(f"Readback mismatch at {addr:#06x}")
        verify(0, bytes.fromhex(" ".join(install[1:65])))
        print("AEQ loader answered and reset vectors matched.")
        if args.probe: return
        ok(port, b"\x23" + struct.pack("<II", 0x1800, 0x3fff))
        # Make the reset vector valid only after the rest of the application verifies.
        for off in [*range(8, len(app), 128), 0]:
            chunk = app[off:off+128] if off else app[:8]
            addr = 0x1800 + off
            ok(port, b"\x20" + struct.pack("<I", addr) + chunk)
            verify(addr, chunk)
        print(f"Verified all {len(app)} application bytes. Tap RESET without S2 to start AEQ.")


if __name__ == "__main__":
    main()

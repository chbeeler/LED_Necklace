"""Update an MSPM0L1306 via its factory ROM UART BSL (pyserial required).

Only MAIN 0..0xffff is erased/programmed. No factory reset or NONMAIN commands.
Uses the factory all-FF password; do not use after changing the BSL password.
Protocol: TI SLAU887, CRC32 with seed FFFFFFFF and no final XOR.
"""
import argparse
import binascii
from pathlib import Path
import struct
import time
import serial

TRACE = False


def crc(data):
    return binascii.crc32(data) ^ 0xffffffff


def read(port, count):
    data = port.read(count)
    if TRACE and data:
        print(f"RX ({len(data)}/{count} bytes): {data.hex(' ')}", flush=True)
    if len(data) != count:
        raise TimeoutError("Incomplete BSL reply. Check wiring; hold S1, tap RESET, release S1.")
    return data


def command(port, payload, reply=True):
    port.write(b"\x80" + struct.pack("<H", len(payload)) + payload
               + struct.pack("<I", crc(payload)))
    ack = read(port, 1)
    if ack != b"\0":
        raise IOError("BSL frame rejected: " + ack.hex())
    if not reply:
        return None
    if read(port, 1) != b"\x08":
        raise IOError("Invalid BSL response header")
    size, = struct.unpack("<H", read(port, 2))
    if not 1 <= size <= 256:
        raise IOError("Invalid BSL response size")
    body = read(port, size)
    if read(port, 4) != struct.pack("<I", crc(body)):
        raise IOError("BSL response CRC mismatch")
    return body


def ok(port, payload):
    response = command(port, payload)
    if response != b"\x3b\0":
        raise IOError("BSL command rejected: " + response.hex())


def identify(port, connected=False):
    if not connected:
        command(port, b"\x12", False)
    info = command(port, b"\x19")
    if len(info) != 25 or info[0] != 0x31:
        raise IOError("Unexpected BSL identity")
    buffer_size, = struct.unpack_from("<H", info, 11)
    bcr, bsl = struct.unpack_from("<II", info, 17)
    if buffer_size < 144 or (bcr, bsl) != (1, 1):
        raise IOError("Unexpected ROM BSL configuration; no erase performed")
    print("ROM BSL identity:", info.hex(), flush=True)
    return info


def application(path):
    data = path.read_bytes()
    if not 1032 <= len(data) <= 65528:
        raise ValueError("Expected MAIN-only L1306 binary at address zero")
    data += b"\xff" * (-len(data) % 8)
    sp, pc = struct.unpack_from("<II", data)
    if not (0x20000000 < sp <= 0x20001000 and sp % 8 == 0
            and pc & 1 and 192 <= (pc & ~1) < len(data)):
        raise ValueError("Invalid L1306 application vectors")
    return data


def verify(port, address, data):
    response = command(port, b"\x26" + struct.pack("<II", address, len(data)))
    if response != b"\x32" + struct.pack("<I", crc(data)):
        raise IOError(f"CRC verification failed at {address:#x}: {response.hex()}")


def update(port, data, connected=False, identified=False):
    if not identified:
        identify(port, connected)
    # One password attempt only. Abort on any error; never try factory reset.
    ok(port, b"\x21" + b"\xff" * 32)
    ok(port, b"\x23" + struct.pack("<II", 0, 0xffff))
    print("MAIN erased; programming application...", flush=True)
    # Keep reset vectors erased until the complete body has verified.
    for offset in range(8, len(data), 128):
        ok(port, b"\x20" + struct.pack("<I", offset) + data[offset:offset+128])
    verify(port, 8, data[8:])
    ok(port, b"\x20" + struct.pack("<I", 0) + data[:8])
    verify(port, 0, data)
    print(f"Verified {len(data)} bytes by ROM CRC. Starting application.", flush=True)
    command(port, b"\x40", False)


def wait_for_bootloader(port, seconds):
    print("Listening: hold S1, tap and release RESET, then release S1.", flush=True)
    deadline = time.monotonic() + seconds
    previous_timeout = port.timeout
    last_notice = time.monotonic()
    last_error = None
    stage = "connection"
    port.timeout = 0.3
    try:
        while time.monotonic() < deadline:
            port.reset_input_buffer()
            try:
                stage = "connection"
                command(port, b"\x12", False)
                stage = "identity"
                print("Connection ACK received; checking identity...", flush=True)
                # A stray zero during reset is not proof of a BSL connection.
                # Require a complete CRC-checked identity before any unlock/write.
                port.timeout = previous_timeout
                return identify(port, connected=True)
            except serial.SerialException:
                raise
            except (TimeoutError, IOError) as error:
                now = time.monotonic()
                detail = f"{stage}: {error}"
                # Routine silence is summarized every five seconds. Unexpected
                # data or a failed identity is reported immediately, then capped.
                unexpected = stage == "identity" or not isinstance(error, TimeoutError)
                if (unexpected and detail != last_error) or now-last_notice >= 5:
                    if unexpected:
                        print(f"Handshake failed ({detail}). No flash erased; retry S1 + RESET.", flush=True)
                    else:
                        print("Still waiting: no UART reply. Retry S1 + RESET while this runs.", flush=True)
                    last_notice = now
                    last_error = detail
                port.timeout = 0.3
                continue
        raise TimeoutError("No BSL connection received; no flash erased")
    finally:
        port.timeout = previous_timeout


def main():
    global TRACE
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("port", help="XDS110 Application/User UART, e.g. COM45")
    parser.add_argument("--probe", action="store_true", help="identify only; no unlock or writes")
    parser.add_argument("--image", type=Path, default=Path(__file__).parent / "Debug/AEQ_L1306.bin")
    parser.add_argument("--wait", type=float, default=120,
                        help="seconds to wait for S1/reset (default 120)")
    parser.add_argument("--trace", action="store_true", help="show received bytes during discovery only")
    args = parser.parse_args()
    TRACE = args.trace
    if not 0 < args.wait <= 300:
        parser.error("--wait must be greater than zero and at most 300")
    data = None if args.probe else application(args.image)
    with serial.Serial(args.port, 9600, timeout=3, write_timeout=3) as port:
        wait_for_bootloader(port, args.wait)
        TRACE = False
        if not args.probe:
            update(port, data, connected=True, identified=True)


if __name__ == "__main__":
    main()

"""UART framing shared by the AEQ full and light updaters."""
import struct


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



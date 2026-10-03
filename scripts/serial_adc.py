"""Decoder for the XIAO analog-microphone binary block protocol."""

from __future__ import annotations

import struct
from dataclasses import dataclass


BAUD_RATE = 921_600
SAMPLE_RATE_HZ = 8_000
ADC_RESOLUTION_BITS = 14
ADC_MAX_VALUE = (1 << ADC_RESOLUTION_BITS) - 1
ADC_HALF_SCALE = 1 << (ADC_RESOLUTION_BITS - 1)
ADC_FULL_SCALE_VOLTS = 1.2
FRAME_START = 0x7E
FRAME_END = 0x7F
FRAME_ESCAPE = 0x7D
FRAME_ESCAPE_XOR = 0x20
MAX_FRAME_BODY_SIZE = 2 + 4 + 1 + 255 * 4 + 2


@dataclass(frozen=True)
class AdcBlock:
    sequence: int
    timestamp_us: int
    time_s: float
    raw: tuple[int, ...]
    filtered: tuple[int, ...]
    lost_before: int

    @property
    def sample_count(self) -> int:
        return len(self.raw)


def crc16_ccitt_false(data: bytes | bytearray | memoryview) -> int:
    crc = 0xFFFF
    for value in data:
        crc ^= value << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc


class SerialAdcBlockParser:
    """Incrementally unescape, validate and timestamp binary ADC blocks."""

    def __init__(self) -> None:
        self._in_frame = False
        self._escaped = False
        self._body = bytearray()
        self._last_micros: int | None = None
        self._micros_wrap_offset = 0
        self._first_extended_micros: int | None = None
        self._expected_sequence: int | None = None
        self.crc_errors = 0
        self.framing_errors = 0
        self.lost_blocks = 0

    def feed(self, data: bytes | bytearray | memoryview) -> list[AdcBlock]:
        blocks: list[AdcBlock] = []
        for value in data:
            if value == FRAME_START:
                if self._in_frame and self._body:
                    self.framing_errors += 1
                self._in_frame = True
                self._escaped = False
                self._body.clear()
                continue
            if not self._in_frame:
                continue
            if self._escaped:
                self._body.append(value ^ FRAME_ESCAPE_XOR)
                self._escaped = False
            elif value == FRAME_ESCAPE:
                self._escaped = True
                continue
            elif value == FRAME_END:
                block = self._decode_body()
                if block is not None:
                    blocks.append(block)
                self._in_frame = False
                self._escaped = False
                self._body.clear()
                continue
            else:
                self._body.append(value)

            if len(self._body) > MAX_FRAME_BODY_SIZE:
                self.framing_errors += 1
                self._in_frame = False
                self._escaped = False
                self._body.clear()
        return blocks

    def _decode_body(self) -> AdcBlock | None:
        # sequence:u16 + timestamp:u32 + count:u8 + crc:u16
        if self._escaped or len(self._body) < 9:
            self.framing_errors += 1
            return None
        sequence, timestamp_us, sample_count = struct.unpack_from("<HIB", self._body)
        expected_length = 7 + sample_count * 4 + 2
        if sample_count == 0 or len(self._body) != expected_length:
            self.framing_errors += 1
            return None
        received_crc = struct.unpack_from("<H", self._body, expected_length - 2)[0]
        if crc16_ccitt_false(memoryview(self._body)[:-2]) != received_crc:
            self.crc_errors += 1
            return None

        samples = struct.unpack_from(f"<{sample_count * 2}H", self._body, 7)
        raw = tuple(samples[0::2])
        filtered = tuple(samples[1::2])
        if any(value > ADC_MAX_VALUE for value in raw) or any(
            value > ADC_MAX_VALUE for value in filtered
        ):
            self.framing_errors += 1
            return None

        if self._last_micros is not None and timestamp_us < self._last_micros:
            if self._last_micros - timestamp_us > 0x80000000:
                self._micros_wrap_offset += 1 << 32
        self._last_micros = timestamp_us
        extended_micros = timestamp_us + self._micros_wrap_offset
        if self._first_extended_micros is None:
            self._first_extended_micros = extended_micros
        time_s = (extended_micros - self._first_extended_micros) / 1_000_000.0

        lost_before = 0
        if self._expected_sequence is not None:
            lost_before = (sequence - self._expected_sequence) & 0xFFFF
            # A large backwards jump is more likely a device restart than loss.
            if lost_before > 0x7FFF:
                lost_before = 0
        self._expected_sequence = (sequence + 1) & 0xFFFF
        self.lost_blocks += lost_before
        return AdcBlock(sequence, timestamp_us, time_s, raw, filtered, lost_before)

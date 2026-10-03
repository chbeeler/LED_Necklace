"""Shared parser and helpers for the XIAO analog-microphone serial stream."""

from __future__ import annotations

import time
from dataclasses import dataclass


@dataclass(frozen=True)
class AdcSample:
    time_s: float
    raw: int
    filtered: int
    device_timed: bool


class SerialAdcParser:
    """Parse timestamp/raw/filtered CSV and unwrap the 32-bit micros counter."""

    def __init__(self) -> None:
        self._first_host_time = time.monotonic()
        self._last_micros: int | None = None
        self._wrap_offset = 0
        self._first_device_micros: int | None = None

    def parse(self, line: bytes | str) -> AdcSample | None:
        try:
            text = line.decode("ascii", errors="strict") if isinstance(line, bytes) else line
            fields = [field.strip() for field in text.strip().split(",")]
            if len(fields) == 3:
                device_timed = True
                micros = int(fields[0]) & 0xFFFFFFFF
                raw = int(fields[1])
                filtered = int(fields[2])
                if self._last_micros is not None and micros < self._last_micros:
                    # micros() wraps approximately every 71 minutes.
                    if self._last_micros - micros > 0x80000000:
                        self._wrap_offset += 1 << 32
                self._last_micros = micros
                extended = micros + self._wrap_offset
                if self._first_device_micros is None:
                    self._first_device_micros = extended
                sample_time = (extended - self._first_device_micros) / 1_000_000.0
            elif len(fields) == 2:
                # Backward compatibility with the original firmware format.
                device_timed = False
                raw = int(fields[0])
                filtered = int(fields[1])
                sample_time = time.monotonic() - self._first_host_time
            else:
                return None
        except (UnicodeDecodeError, ValueError):
            return None

        if not (0 <= raw <= 4095 and 0 <= filtered <= 4095):
            return None
        return AdcSample(sample_time, raw, filtered, device_timed)

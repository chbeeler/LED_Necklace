"""Interactive audio-to-LED brightness laboratory.

Accepts an audio file, a live microphone, or system-output loopback and feeds
the samples to a selectable brightness algorithm.  The GUI deliberately uses
Tkinter so that only the audio/numerical packages need to be installed.
"""

from __future__ import annotations

import argparse
import math
import queue
import sys
import threading
from dataclasses import dataclass
from pathlib import Path
from typing import Protocol

try:
    import numpy as np
    import serial
    import soundcard as sc
    import sounddevice as sd
    import soundfile as sf
    from serial.tools import list_ports
except ImportError as exc:
    missing = getattr(exc, "name", "a required package")
    raise SystemExit(
        f"Missing {missing}. Install the dependencies with:\n"
        "  py -m pip install -r scripts/audio_brightness_requirements.txt"
    ) from exc

import tkinter as tk
from tkinter import filedialog, messagebox, ttk

from serial_adc import ADC_HALF_SCALE, BAUD_RATE, SAMPLE_RATE_HZ, SerialAdcBlockParser


def smoothing_coefficient(seconds: float, dt: float) -> float:
    """Return a frame-rate-independent one-pole smoothing coefficient."""
    return 1.0 - math.exp(-dt / max(seconds, 0.001))


class BrightnessAlgorithm(Protocol):
    def reset(self) -> None: ...

    def process(self, samples: np.ndarray, sample_rate: int, sensitivity: float) -> float: ...

    @property
    def diagnostics(self) -> str: ...


class AdaptiveSpeechBeat:
    """AGC-like loudness following plus transient emphasis for musical beats.

    `floor` follows quiet input and `ceiling` follows typical loud input, both
    slowly.  Normalizing between them makes the result largely independent of
    microphone gain and venue volume.  A fast positive-change detector adds a
    short flash on speech consonants and beats.
    """

    def __init__(self) -> None:
        self.reset()

    def reset(self) -> None:
        self.envelope = 0.0
        self.floor = 0.001
        self.ceiling = 0.02
        self.previous_level = 0.0
        self.flash = 0.0
        self.output = 0.0

    def process(self, samples: np.ndarray, sample_rate: int, sensitivity: float) -> float:
        if samples.size == 0:
            return self.output
        dt = samples.size / sample_rate
        rms = float(np.sqrt(np.mean(np.square(samples), dtype=np.float64) + 1e-12))

        # Quick attack makes syllables/notes visible; slower release avoids flicker.
        env_tau = 0.025 if rms > self.envelope else 0.180
        self.envelope += smoothing_coefficient(env_tau, dt) * (rms - self.envelope)

        # The noise floor may fall quickly after moving to a quieter place, but it
        # rises very cautiously and only while the signal is relatively quiet.
        floor_tau = 1.5 if rms < self.floor else 18.0
        if rms < self.ceiling * 0.35:
            self.floor += smoothing_coefficient(floor_tau, dt) * (rms - self.floor)

        # A leaky peak reference adapts down in a quiet room and up at a festival.
        ceiling_target = max(self.envelope, self.floor + 0.004)
        ceiling_tau = 0.35 if ceiling_target > self.ceiling else 6.0
        self.ceiling += smoothing_coefficient(ceiling_tau, dt) * (
            ceiling_target - self.ceiling
        )

        span = max(self.ceiling - self.floor, 0.003)
        level = float(np.clip((self.envelope - self.floor) / span, 0.0, 1.0))
        level = float(np.clip(level * sensitivity, 0.0, 1.0))

        onset = max(0.0, level - self.previous_level)
        self.previous_level = level
        self.flash = max(self.flash * math.exp(-dt / 0.13), min(1.0, onset * 3.2))

        target = max(level**1.35, self.flash)
        out_tau = 0.018 if target > self.output else 0.110
        self.output += smoothing_coefficient(out_tau, dt) * (target - self.output)
        return float(np.clip(self.output, 0.0, 1.0))

    @property
    def diagnostics(self) -> str:
        return f"floor {self.floor:.4f}   ceiling {self.ceiling:.4f}   envelope {self.envelope:.4f}"


class LegacyAverage:
    """Desktop equivalent of the current firmware's smoothed abs amplitude."""

    def __init__(self) -> None:
        self.reset()

    def reset(self) -> None:
        self.filtered = 0.0
        self.output = 0.0

    def process(self, samples: np.ndarray, sample_rate: int, sensitivity: float) -> float:
        average = float(np.mean(np.abs(samples))) if samples.size else 0.0
        self.filtered += 0.15 * (average - self.filtered)
        # 0.08 is a useful desktop full-scale reference; sensitivity remains live.
        self.output = float(np.clip(self.filtered / 0.08 * sensitivity, 0.0, 1.0))
        return self.output

    @property
    def diagnostics(self) -> str:
        return f"filtered average-absolute level {self.filtered:.4f}"


ALGORITHMS: dict[str, type[BrightnessAlgorithm]] = {
    "Adaptive speech + beat": AdaptiveSpeechBeat,
    "Legacy smoothed loudness": LegacyAverage,
}


@dataclass(frozen=True)
class LiveSource:
    label: str
    device_id: object
    kind: str
    sample_rate: int = 48_000
    channels: int = 2


class LiveAudioCapture:
    """Continuously record a microphone or WASAPI loopback on a worker thread."""

    def __init__(
        self,
        blocks: queue.Queue[np.ndarray],
        errors: queue.Queue[str],
        sample_rate: int = 48_000,
    ) -> None:
        self.blocks = blocks
        self.errors = errors
        self.sample_rate = sample_rate
        self.source: LiveSource | None = None
        self._stop = threading.Event()
        self._thread: threading.Thread | None = None
        self._running = threading.Event()

    @staticmethod
    def enumerate_sources() -> list[LiveSource]:
        """Return physical microphones followed by speaker-loopback devices."""
        result: list[LiveSource] = []
        used_labels: dict[str, int] = {}

        # PortAudio is more tolerant of physical microphone drivers. Prefer its
        # WASAPI view on Windows to avoid listing the same endpoint once per API.
        devices = list(sd.query_devices())
        host_apis = list(sd.query_hostapis())
        wasapi_ids = {
            index
            for index, api in enumerate(host_apis)
            if "WASAPI" in str(api["name"]).upper()
        }
        input_indexes = [
            index
            for index, device in enumerate(devices)
            if int(device["max_input_channels"]) > 0
        ]
        wasapi_inputs = [
            index for index in input_indexes if int(devices[index]["hostapi"]) in wasapi_ids
        ]
        if wasapi_inputs:
            input_indexes = wasapi_inputs
        for index in input_indexes:
            device = devices[index]
            base = f"Microphone: {device['name']}"
            used_labels[base] = used_labels.get(base, 0) + 1
            suffix = f" ({used_labels[base]})" if used_labels[base] > 1 else ""
            result.append(
                LiveSource(
                    base + suffix,
                    index,
                    "microphone",
                    int(round(float(device["default_samplerate"]))),
                    min(2, int(device["max_input_channels"])),
                )
            )

        # SoundCard exposes native WASAPI loopback endpoints, which PortAudio's
        # high-level API does not currently make available.
        loopbacks = [
            device
            for device in sc.all_microphones(include_loopback=True)
            if device.isloopback
        ]
        for device in loopbacks:
            base = f"System audio: {device.name}"
            used_labels[base] = used_labels.get(base, 0) + 1
            suffix = f" ({used_labels[base]})" if used_labels[base] > 1 else ""
            result.append(LiveSource(base + suffix, device.id, "loopback"))

        for port in list_ports.comports():
            label = f"Serial ADC: {port.device} — {port.description}"
            result.append(LiveSource(label, port.device, "serial", SAMPLE_RATE_HZ, 1))
        return result

    @property
    def running(self) -> bool:
        return self._running.is_set()

    def start(self, source: LiveSource) -> None:
        self.stop()
        self.source = source
        self.sample_rate = source.sample_rate
        self._stop.clear()
        self._running.set()
        self._thread = threading.Thread(
            target=self._record,
            name="audio-live-capture",
            daemon=True,
        )
        self._thread.start()

    def _record(self) -> None:
        source = self.source
        if source is None:
            return
        try:
            if source.kind == "serial":
                self._record_serial_adc(source)
                return
            if source.kind == "microphone":
                self._record_microphone(source)
                return
            # Retrieve a fresh object because audio devices can be reconfigured or
            # unplugged after the source list was populated.
            microphone = sc.get_microphone(
                source.device_id,
                include_loopback=True,
            )
            with microphone.recorder(
                samplerate=self.sample_rate,
                blocksize=2048,
            ) as recorder:
                while not self._stop.is_set():
                    data = recorder.record(numframes=1024)
                    if data is None or data.size == 0:
                        continue
                    mono = np.mean(data, axis=1, dtype=np.float32)
                    try:
                        self.blocks.put_nowait(mono)
                    except queue.Full:
                        # Real-time capture takes priority over stale visualization.
                        try:
                            self.blocks.get_nowait()
                            self.blocks.put_nowait(mono)
                        except queue.Empty:
                            pass
        except Exception as exc:
            hint = ""
            if source.kind == "microphone":
                hint = (
                    " Check Settings > Privacy & security > Microphone and enable "
                    "microphone access for desktop apps."
                )
            elif source.kind == "loopback":
                hint = " Make sure this is the Windows output device currently playing audio."
            else:
                hint = " Close the serial scope or monitor if it already has this port open."
            try:
                self.errors.put_nowait(f"Could not capture {source.label}: {exc}.{hint}")
            except queue.Full:
                pass
        finally:
            self._running.clear()

    def _record_microphone(self, source: LiveSource) -> None:
        def callback(indata, _frames, _time, status) -> None:
            if status:
                # PortAudio status flags are transient; successful blocks remain
                # useful, so they do not stop the capture.
                pass
            mono = np.mean(indata, axis=1, dtype=np.float32).copy()
            try:
                self.blocks.put_nowait(mono)
            except queue.Full:
                try:
                    self.blocks.get_nowait()
                    self.blocks.put_nowait(mono)
                except queue.Empty:
                    pass

        with sd.InputStream(
            device=int(source.device_id),
            samplerate=source.sample_rate,
            channels=source.channels,
            dtype="float32",
            blocksize=1024,
            callback=callback,
        ):
            self._stop.wait()

    def _record_serial_adc(self, source: LiveSource) -> None:
        parser = SerialAdcBlockParser()
        dc_level: float | None = None
        self.sample_rate = SAMPLE_RATE_HZ
        with serial.Serial(str(source.device_id), BAUD_RATE, timeout=0.2) as connection:
            connection.reset_input_buffer()
            while not self._stop.is_set():
                chunk = connection.read(connection.in_waiting or 1)
                for adc_block in parser.feed(chunk):
                    normalized: list[float] = []
                    for raw in adc_block.raw:
                        value = float(raw)
                        if dc_level is None:
                            dc_level = value
                        # Remove the microphone module's mid-supply bias slowly.
                        dc_level += 0.001 * (value - dc_level)
                        normalized.append((value - dc_level) / ADC_HALF_SCALE)
                    data = np.asarray(normalized, dtype=np.float32)
                    try:
                        self.blocks.put_nowait(data)
                    except queue.Full:
                        try:
                            self.blocks.get_nowait()
                            self.blocks.put_nowait(data)
                        except queue.Empty:
                            pass

    def stop(self) -> None:
        self._stop.set()
        thread = self._thread
        if thread is not None and thread is not threading.current_thread():
            thread.join(timeout=1.5)
        self._thread = None
        self._running.clear()


class AudioPlayer:
    """Thread-safe file player whose callback also supplies analysis samples."""

    def __init__(self, blocks: queue.Queue[np.ndarray]) -> None:
        self.blocks = blocks
        self.audio: np.ndarray | None = None
        self.sample_rate = 48_000
        self.position = 0
        self.playing = False
        self.loop = True
        self.stream: sd.OutputStream | None = None
        self.lock = threading.Lock()
        self.path: Path | None = None

    def load(self, path: Path) -> None:
        audio, sample_rate = sf.read(path, dtype="float32", always_2d=True)
        if audio.size == 0:
            raise ValueError("The selected file contains no audio samples.")
        self.close()
        with self.lock:
            self.audio = audio
            self.sample_rate = sample_rate
            self.position = 0
            self.path = path
            self.playing = False
        self.stream = sd.OutputStream(
            samplerate=sample_rate,
            channels=audio.shape[1],
            dtype="float32",
            blocksize=1024,
            callback=self._callback,
        )
        self.stream.start()

    def _callback(self, outdata: np.ndarray, frames: int, _time, status) -> None:
        del status
        outdata.fill(0)
        analysis_parts: list[np.ndarray] = []
        with self.lock:
            if not self.playing or self.audio is None:
                return
            written = 0
            while written < frames and self.playing:
                available = min(frames - written, len(self.audio) - self.position)
                if available:
                    part = self.audio[self.position : self.position + available]
                    outdata[written : written + available] = part
                    analysis_parts.append(np.mean(part, axis=1, dtype=np.float32))
                    self.position += available
                    written += available
                if self.position >= len(self.audio):
                    if self.loop:
                        self.position = 0
                    else:
                        self.playing = False
        if analysis_parts:
            block = np.concatenate(analysis_parts)
            try:
                self.blocks.put_nowait(block)
            except queue.Full:
                # The display may lag, but audio playback must never block.
                pass

    def toggle(self) -> bool:
        with self.lock:
            if self.audio is None:
                return False
            if self.position >= len(self.audio):
                self.position = 0
            self.playing = not self.playing
            return self.playing

    def rewind(self) -> None:
        with self.lock:
            self.position = 0

    def pause(self) -> None:
        with self.lock:
            self.playing = False

    def close(self) -> None:
        if self.stream is not None:
            self.stream.stop()
            self.stream.close()
            self.stream = None


class BrightnessLab:
    def __init__(self, root: tk.Tk, initial_file: Path | None) -> None:
        self.root = root
        self.root.title("LED Necklace — Audio Brightness Lab")
        self.root.minsize(880, 500)
        self.blocks: queue.Queue[np.ndarray] = queue.Queue(maxsize=12)
        self.errors: queue.Queue[str] = queue.Queue(maxsize=4)
        self.player = AudioPlayer(self.blocks)
        self.capture = LiveAudioCapture(self.blocks, self.errors)
        self.live_sources: dict[str, LiveSource] = {}
        self.algorithm: BrightnessAlgorithm = AdaptiveSpeechBeat()
        self.brightness = 0.0
        self.history = [0.0] * 160

        self.algorithm_name = tk.StringVar(value=next(iter(ALGORITHMS)))
        self.source_name = tk.StringVar(value="Audio file")
        self.sensitivity = tk.DoubleVar(value=1.0)
        self.gain = tk.DoubleVar(value=1.0)
        self.loop = tk.BooleanVar(value=True)
        self.status = tk.StringVar(value="Load an audio file to begin")
        self.diagnostics = tk.StringVar(value="")
        self._build_ui()
        self.refresh_sources()
        self.root.protocol("WM_DELETE_WINDOW", self.close)

        if initial_file:
            self.load(initial_file)
        self.root.after(16, self.update)

    def _build_ui(self) -> None:
        controls = ttk.Frame(self.root, padding=12)
        controls.pack(fill="x")
        ttk.Button(controls, text="Load audio…", command=self.choose_file).pack(side="left")
        self.play_button = ttk.Button(controls, text="Play", command=self.toggle)
        self.play_button.pack(side="left", padx=(8, 0))
        ttk.Button(controls, text="Restart", command=self.restart).pack(side="left", padx=8)
        ttk.Checkbutton(controls, text="Loop", variable=self.loop).pack(side="left")

        source_controls = ttk.Frame(self.root, padding=(12, 0, 12, 8))
        source_controls.pack(fill="x")
        ttk.Label(source_controls, text="Source:").pack(side="left")
        self.source_selector = ttk.Combobox(
            source_controls,
            textvariable=self.source_name,
            state="readonly",
            width=66,
        )
        self.source_selector.pack(side="left", fill="x", expand=True, padx=(6, 8))
        self.source_selector.bind("<<ComboboxSelected>>", self.change_source)
        ttk.Button(source_controls, text="Refresh devices", command=self.refresh_sources).pack(
            side="left"
        )

        algorithm_controls = ttk.Frame(self.root, padding=(12, 0, 12, 8))
        algorithm_controls.pack(fill="x")
        ttk.Label(algorithm_controls, text="Algorithm:").pack(side="left")
        selector = ttk.Combobox(
            algorithm_controls,
            textvariable=self.algorithm_name,
            values=list(ALGORITHMS),
            state="readonly",
            width=27,
        )
        selector.pack(side="left", padx=(6, 0))
        selector.bind("<<ComboboxSelected>>", self.change_algorithm)

        tuning = ttk.Frame(self.root, padding=(12, 0, 12, 10))
        tuning.pack(fill="x")
        ttk.Label(tuning, text="Input gain").grid(row=0, column=0, sticky="w")
        ttk.Scale(tuning, from_=0.1, to=4.0, variable=self.gain).grid(
            row=0, column=1, sticky="ew", padx=(8, 25)
        )
        ttk.Label(tuning, text="Sensitivity").grid(row=0, column=2, sticky="w")
        ttk.Scale(tuning, from_=0.25, to=3.0, variable=self.sensitivity).grid(
            row=0, column=3, sticky="ew", padx=(8, 0)
        )
        tuning.columnconfigure(1, weight=1)
        tuning.columnconfigure(3, weight=1)

        self.canvas = tk.Canvas(self.root, bg="#101218", highlightthickness=0)
        self.canvas.pack(fill="both", expand=True, padx=12, pady=(0, 8))
        ttk.Label(self.root, textvariable=self.status, padding=(12, 0)).pack(anchor="w")
        ttk.Label(self.root, textvariable=self.diagnostics, padding=(12, 2, 12, 10)).pack(anchor="w")

    def choose_file(self) -> None:
        name = filedialog.askopenfilename(
            title="Choose audio",
            filetypes=[("Audio files", "*.wav *.flac *.ogg *.mp3"), ("All files", "*.*")],
        )
        if name:
            self.load(Path(name))

    def load(self, path: Path) -> None:
        self.capture.stop()
        try:
            self.player.load(path)
        except Exception as exc:
            messagebox.showerror("Could not load audio", str(exc))
            return
        self.algorithm.reset()
        self.source_name.set("Audio file")
        self.status.set(f"Loaded {path.name} — {self.player.sample_rate} Hz")
        self.play_button.configure(text="Play")

    def toggle(self) -> None:
        source = self.live_sources.get(self.source_name.get())
        if source is None:
            playing = self.player.toggle()
            self.play_button.configure(text="Pause" if playing else "Play")
            if not playing and self.player.audio is None:
                self.status.set("Load an audio file first, or select a live source")
            return

        if self.capture.running:
            self.capture.stop()
            self.play_button.configure(text="Start listening")
            self.status.set(f"Stopped — {source.label}")
        else:
            self.capture.start(source)
            self.play_button.configure(text="Stop listening")
            self.status.set(f"Listening — {source.label} at {self.capture.sample_rate} Hz")

    def restart(self) -> None:
        if self.source_name.get() == "Audio file":
            self.player.rewind()
        self.algorithm.reset()

    def refresh_sources(self) -> None:
        selected = self.source_name.get()
        try:
            sources = LiveAudioCapture.enumerate_sources()
        except Exception as exc:
            sources = []
            self.status.set(f"Could not enumerate live audio devices: {exc}")
        self.live_sources = {source.label: source for source in sources}
        values = ["Audio file", *self.live_sources]
        self.source_selector.configure(values=values)
        if selected not in values:
            self.capture.stop()
            self.source_name.set("Audio file")
            self.play_button.configure(text="Play")

    def change_source(self, _event=None) -> None:
        self.player.pause()
        self.capture.stop()
        self._discard_queued_audio()
        self.algorithm.reset()
        source = self.live_sources.get(self.source_name.get())
        if source is None:
            self.play_button.configure(text="Play")
            if self.player.path is None:
                self.status.set("Audio file selected — load a file to begin")
            else:
                self.status.set(f"Audio file selected — {self.player.path.name}")
            return
        self.capture.start(source)
        self.play_button.configure(text="Stop listening")
        self.status.set(f"Listening — {source.label} at {self.capture.sample_rate} Hz")

    def _discard_queued_audio(self) -> None:
        while True:
            try:
                self.blocks.get_nowait()
            except queue.Empty:
                return

    def change_algorithm(self, _event=None) -> None:
        self.algorithm = ALGORITHMS[self.algorithm_name.get()]()

    def update(self) -> None:
        self.player.loop = self.loop.get()
        try:
            error = self.errors.get_nowait()
        except queue.Empty:
            error = None
        if error:
            self.status.set(error)
            self.play_button.configure(text="Start listening")

        live_selected = self.source_name.get() in self.live_sources
        sample_rate = self.capture.sample_rate if live_selected else self.player.sample_rate
        got_audio = False
        while True:
            try:
                block = self.blocks.get_nowait()
            except queue.Empty:
                break
            block = np.clip(block * self.gain.get(), -1.0, 1.0)
            self.brightness = self.algorithm.process(
                block, sample_rate, self.sensitivity.get()
            )
            got_audio = True
        if got_audio:
            self.history.append(self.brightness)
            self.history.pop(0)
        elif not (self.player.playing or self.capture.running):
            self.brightness *= 0.90
        self.diagnostics.set(
            f"brightness {self.brightness * 100:5.1f}%   |   {self.algorithm.diagnostics}"
        )
        self.draw()
        self.root.after(16, self.update)

    def draw(self) -> None:
        c = self.canvas
        c.delete("all")
        w, h = max(c.winfo_width(), 2), max(c.winfo_height(), 2)
        level = float(np.clip(self.brightness, 0.0, 1.0))

        # Virtual warm-white filament with several glow layers.
        x1, x2, y = 55, w - 135, h * 0.45
        for width, color, scale in [
            (38, "#3b2410", 0.25),
            (22, "#7a3d0d", 0.45),
            (11, "#e07814", 0.70),
            (4, "#fff1b0", 1.0),
        ]:
            intensity = level * scale
            base = tuple(int(color[i : i + 2], 16) for i in (1, 3, 5))
            rgb = tuple(int(v * intensity) for v in base)
            shown = "#%02x%02x%02x" % rgb
            c.create_line(x1, y, x2, y, fill=shown, width=width, capstyle="round")
        c.create_text((x1 + x2) / 2, y - 48, text="virtual LED filament", fill="#aab0bd")

        # Brightness bar.
        bx1, bx2, by1, by2 = w - 90, w - 48, 35, h - 45
        c.create_rectangle(bx1, by1, bx2, by2, outline="#626977", width=2)
        fill_top = by2 - (by2 - by1) * level
        c.create_rectangle(bx1 + 3, fill_top, bx2 - 3, by2 - 3, fill="#ffb52e", width=0)
        c.create_text((bx1 + bx2) / 2, by2 + 20, text=f"{level * 100:.0f}%", fill="#d9dce3")

        # Recent output history makes attack/release behaviour easy to compare.
        graph_y1, graph_y2 = h * 0.68, h - 25
        if len(self.history) > 1 and x2 > x1:
            points: list[float] = []
            for i, value in enumerate(self.history):
                points.extend(
                    [x1 + i * (x2 - x1) / (len(self.history) - 1), graph_y2 - value * (graph_y2 - graph_y1)]
                )
            c.create_line(*points, fill="#4fc3f7", width=2)
        c.create_text(x1, graph_y1 - 10, text="brightness history", anchor="w", fill="#aab0bd")

    def close(self) -> None:
        self.capture.stop()
        self.player.close()
        self.root.destroy()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("audio_file", nargs="?", type=Path, help="optional WAV/FLAC/OGG/MP3 file")
    args = parser.parse_args()
    root = tk.Tk()
    BrightnessLab(root, args.audio_file)
    root.mainloop()
    return 0


if __name__ == "__main__":
    sys.exit(main())

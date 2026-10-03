"""Record and inspect the XIAO analog-microphone stream interactively."""

from __future__ import annotations

import csv
import queue
import threading
import time
from collections import deque
from pathlib import Path

try:
    import matplotlib
    import numpy as np
    import serial
    from serial.tools import list_ports
except ImportError as exc:
    missing = getattr(exc, "name", "a required package")
    raise SystemExit(
        f"Missing {missing}. Install the dependencies with:\n"
        "  py -m pip install -r scripts/audio_brightness_requirements.txt"
    ) from exc

matplotlib.use("TkAgg")
from matplotlib.backends.backend_tkagg import FigureCanvasTkAgg, NavigationToolbar2Tk
from matplotlib.figure import Figure

import tkinter as tk
from tkinter import filedialog, messagebox, ttk

from serial_adc import BAUD_RATE, SAMPLE_RATE_HZ, AdcBlock, SerialAdcBlockParser


MAX_RETAINED_SAMPLES = 500_000
MAX_DISPLAY_SAMPLES = 100_000
MAX_PLOT_POINTS = 2_000
GUI_POLL_MS = 20
PLOT_INTERVAL_S = 0.10
STATUS_INTERVAL_S = 0.50
RECORD_FLUSH_INTERVAL_S = 1.0


class SerialAdcScope:
    def __init__(self, root: tk.Tk) -> None:
        self.root = root
        self.root.title("LED Necklace — Serial ADC Scope")
        self.root.minsize(900, 600)

        self.blocks: queue.Queue[AdcBlock] = queue.Queue(maxsize=1_000)
        self.errors: queue.Queue[str] = queue.Queue(maxsize=4)
        self.stop_event = threading.Event()
        self.reader_thread: threading.Thread | None = None
        self.times: deque[float] = deque(maxlen=MAX_RETAINED_SAMPLES)
        self.raw_values: deque[int] = deque(maxlen=MAX_RETAINED_SAMPLES)
        self.filtered_values: deque[int] = deque(maxlen=MAX_RETAINED_SAMPLES)
        # Plotting uses a separate, bounded tail. Converting all retained samples
        # to NumPy arrays on every frame creates seconds of serial backlog.
        self.display_times: deque[float] = deque(maxlen=MAX_DISPLAY_SAMPLES)
        self.display_raw: deque[int] = deque(maxlen=MAX_DISPLAY_SAMPLES)
        self.display_filtered: deque[int] = deque(maxlen=MAX_DISPLAY_SAMPLES)

        self.ports: dict[str, str] = {}
        self.port_name = tk.StringVar()
        self.window_seconds = tk.DoubleVar(value=5.0)
        self.status = tk.StringVar(value="Select a serial port and connect")
        self.paused = False
        self.record_file = None
        self.record_writer = None
        self.record_path: Path | None = None
        self.recorded_rows = 0
        self.dropped_samples = 0
        self.lost_blocks = 0
        self.crc_errors = 0
        self.framing_errors = 0
        self.live_background = None
        self.live_window_seconds: float | None = None
        now = time.monotonic()
        self.last_plot_time = now
        self.last_status_time = now
        self.last_record_flush_time = now

        self._build_ui()
        self.refresh_ports()
        self.root.protocol("WM_DELETE_WINDOW", self.close)
        self.root.after(GUI_POLL_MS, self.update)

    def _build_ui(self) -> None:
        controls = ttk.Frame(self.root, padding=10)
        controls.pack(fill="x")
        ttk.Label(controls, text="Port:").pack(side="left")
        self.port_selector = ttk.Combobox(
            controls, textvariable=self.port_name, state="readonly", width=42
        )
        self.port_selector.pack(side="left", fill="x", expand=True, padx=(6, 6))
        ttk.Button(controls, text="Refresh", command=self.refresh_ports).pack(side="left")
        self.connect_button = ttk.Button(controls, text="Connect", command=self.toggle_connection)
        self.connect_button.pack(side="left", padx=(6, 0))
        self.pause_button = ttk.Button(controls, text="Pause display", command=self.toggle_pause)
        self.pause_button.pack(side="left", padx=(6, 0))
        ttk.Button(controls, text="Clear", command=self.clear).pack(side="left", padx=(6, 0))

        recording = ttk.Frame(self.root, padding=(10, 0, 10, 8))
        recording.pack(fill="x")
        self.record_button = ttk.Button(recording, text="Start recording…", command=self.toggle_recording)
        self.record_button.pack(side="left")
        ttk.Button(recording, text="Save retained data…", command=self.save_retained).pack(
            side="left", padx=(6, 18)
        )
        ttk.Label(recording, text="Live window (s):").pack(side="left")
        ttk.Spinbox(
            recording,
            from_=0.1,
            to=120.0,
            increment=0.5,
            textvariable=self.window_seconds,
            width=7,
        ).pack(side="left", padx=(6, 0))

        figure = Figure(figsize=(9, 5), dpi=100)
        self.axes = figure.add_subplot(111)
        self.axes.set_xlabel("Time (s)")
        self.axes.set_ylabel("12-bit ADC count")
        self.axes.set_ylim(0, 4095)
        self.axes.grid(True, alpha=0.25)
        (self.raw_line,) = self.axes.plot([], [], color="#2980b9", lw=0.8, label="raw")
        (self.filtered_line,) = self.axes.plot(
            [], [], color="#e67e22", lw=1.2, label="low-pass filtered"
        )
        self.axes.legend(loc="upper right")
        self.canvas = FigureCanvasTkAgg(figure, master=self.root)
        self.canvas.draw()
        NavigationToolbar2Tk(self.canvas, self.root, pack_toolbar=True)
        canvas_widget = self.canvas.get_tk_widget()
        canvas_widget.pack(fill="both", expand=True)
        canvas_widget.bind("<Configure>", self._canvas_resized)
        ttk.Label(self.root, textvariable=self.status, padding=(10, 5, 10, 8)).pack(
            anchor="w"
        )

    def refresh_ports(self) -> None:
        selected_device = self.ports.get(self.port_name.get())
        self.ports = {
            f"{port.device} — {port.description}": port.device for port in list_ports.comports()
        }
        self.port_selector.configure(values=list(self.ports))
        matching = next(
            (label for label, device in self.ports.items() if device == selected_device), None
        )
        if matching:
            self.port_name.set(matching)
        elif self.ports:
            self.port_name.set(next(iter(self.ports)))
        else:
            self.port_name.set("")

    def toggle_connection(self) -> None:
        if self.reader_thread and self.reader_thread.is_alive():
            self.disconnect()
            return
        port = self.ports.get(self.port_name.get())
        if not port:
            messagebox.showinfo("No serial port", "Connect the XIAO and refresh the port list.")
            return
        self.stop_event.clear()
        self.reader_thread = threading.Thread(
            target=self._read_serial, args=(port,), daemon=True, name="serial-adc-reader"
        )
        self.reader_thread.start()
        self.connect_button.configure(text="Disconnect")
        self.status.set(f"Connecting to {port} at {BAUD_RATE:,} baud…")

    def _read_serial(self, port: str) -> None:
        parser = SerialAdcBlockParser()
        try:
            with serial.Serial(port, BAUD_RATE, timeout=0.2) as connection:
                connection.reset_input_buffer()
                while not self.stop_event.is_set():
                    chunk = connection.read(connection.in_waiting or 1)
                    for block in parser.feed(chunk):
                        self.lost_blocks += block.lost_before
                        try:
                            self.blocks.put_nowait(block)
                        except queue.Full:
                            # Keep recent data visible if the GUI temporarily falls behind.
                            try:
                                dropped = self.blocks.get_nowait()
                                self.dropped_samples += dropped.sample_count
                                self.blocks.put_nowait(block)
                            except queue.Empty:
                                pass
                    self.crc_errors = parser.crc_errors
                    self.framing_errors = parser.framing_errors
        except Exception as exc:
            try:
                self.errors.put_nowait(f"Serial capture failed on {port}: {exc}")
            except queue.Full:
                pass

    def disconnect(self) -> None:
        self.stop_event.set()
        if self.reader_thread:
            self.reader_thread.join(timeout=0.5)
        self.reader_thread = None
        self.connect_button.configure(text="Connect")
        self.status.set("Disconnected")

    def toggle_pause(self) -> None:
        self.paused = not self.paused
        self.pause_button.configure(text="Resume display" if self.paused else "Pause display")
        if self.paused:
            self.status.set("Display paused; capture and CSV recording continue")
            self._draw_paused()
        else:
            self.live_background = None
            self._redraw()

    def _canvas_resized(self, _event=None) -> None:
        self.live_background = None

    def clear(self) -> None:
        self.times.clear()
        self.raw_values.clear()
        self.filtered_values.clear()
        self.display_times.clear()
        self.display_raw.clear()
        self.display_filtered.clear()
        self.raw_line.set_data([], [])
        self.filtered_line.set_data([], [])
        self.live_background = None
        self.canvas.draw_idle()

    def toggle_recording(self) -> None:
        if self.record_file:
            self.stop_recording()
            return
        filename = filedialog.asksaveasfilename(
            title="Record ADC samples",
            defaultextension=".csv",
            filetypes=[("CSV files", "*.csv"), ("All files", "*.*")],
        )
        if not filename:
            return
        try:
            self.record_file = open(filename, "w", newline="", encoding="utf-8")
            self.record_writer = csv.writer(self.record_file)
            self.record_writer.writerow(["time_s", "raw_adc", "filtered_adc"])
        except OSError as exc:
            self.record_file = None
            messagebox.showerror("Could not start recording", str(exc))
            return
        self.record_path = Path(filename)
        self.recorded_rows = 0
        self.record_button.configure(text="Stop recording")

    def stop_recording(self) -> None:
        if self.record_file:
            self.record_file.flush()
            self.record_file.close()
        path = self.record_path
        count = self.recorded_rows
        self.record_file = None
        self.record_writer = None
        self.record_path = None
        self.record_button.configure(text="Start recording…")
        if path:
            self.status.set(f"Recorded {count:,} samples to {path.name}")

    def save_retained(self) -> None:
        if not self.times:
            messagebox.showinfo("No data", "There are no retained samples to save.")
            return
        filename = filedialog.asksaveasfilename(
            title="Save retained ADC samples",
            defaultextension=".csv",
            filetypes=[("CSV files", "*.csv"), ("All files", "*.*")],
        )
        if not filename:
            return
        try:
            with open(filename, "w", newline="", encoding="utf-8") as output:
                writer = csv.writer(output)
                writer.writerow(["time_s", "raw_adc", "filtered_adc"])
                writer.writerows(zip(self.times, self.raw_values, self.filtered_values))
        except OSError as exc:
            messagebox.showerror("Could not save data", str(exc))

    def update(self) -> None:
        new_samples = 0
        # Drain everything currently available before doing any drawing. Serial
        # acquisition therefore catches up on every GUI tick instead of showing
        # progressively older queued data.
        while True:
            try:
                block = self.blocks.get_nowait()
            except queue.Empty:
                break
            sample_times = block.time_s + np.arange(block.sample_count) / SAMPLE_RATE_HZ
            self.times.extend(sample_times)
            self.raw_values.extend(block.raw)
            self.filtered_values.extend(block.filtered)
            if self.display_times:
                gap = sample_times[0] - self.display_times[-1]
                if gap > 1.5 / SAMPLE_RATE_HZ:
                    # Break the plotted line across a genuine acquisition gap;
                    # otherwise Matplotlib draws a misleading diagonal bridge.
                    self.display_times.append(self.display_times[-1] + gap / 2.0)
                    self.display_raw.append(float("nan"))
                    self.display_filtered.append(float("nan"))
            self.display_times.extend(sample_times)
            self.display_raw.extend(block.raw)
            self.display_filtered.extend(block.filtered)
            if self.record_writer:
                self.record_writer.writerows(
                    (f"{sample_time:.6f}", raw, filtered)
                    for sample_time, raw, filtered in zip(sample_times, block.raw, block.filtered)
                )
                self.recorded_rows += block.sample_count
            new_samples += block.sample_count

        try:
            error = self.errors.get_nowait()
        except queue.Empty:
            error = None
        if error:
            self.disconnect()
            messagebox.showerror("Serial capture error", error)

        now = time.monotonic()
        if (
            new_samples
            and self.record_file
            and now - self.last_record_flush_time >= RECORD_FLUSH_INTERVAL_S
        ):
            self.record_file.flush()
            self.last_record_flush_time = now
        if new_samples and not self.paused and now - self.last_plot_time >= PLOT_INTERVAL_S:
            self._redraw()
            self.last_plot_time = now
        if new_samples and self.times and now - self.last_status_time >= STATUS_INTERVAL_S:
            rate = 0.0
            if len(self.times) > 1 and self.times[-1] > self.times[0]:
                count = min(len(self.times), 2000)
                elapsed = self.times[-1] - self.times[-count]
                if elapsed > 0:
                    rate = (count - 1) / elapsed
            recording = f"; recording {self.recorded_rows:,}" if self.record_file else ""
            dropped = f"; dropped {self.dropped_samples:,}" if self.dropped_samples else ""
            protocol_errors = ""
            if self.lost_blocks or self.crc_errors or self.framing_errors:
                protocol_errors = (
                    f"; lost blocks {self.lost_blocks:,}; CRC errors {self.crc_errors:,}; "
                    f"framing errors {self.framing_errors:,}"
                )
            self.status.set(
                f"Retained {len(self.times):,}; approximately {rate:,.0f} samples/s; "
                f"queue {self.blocks.qsize():,} blocks{recording}{dropped}{protocol_errors}"
            )
            self.last_status_time = now
        self.root.after(GUI_POLL_MS, self.update)

    def _redraw(self) -> None:
        times = np.asarray(self.display_times, dtype=np.float64)
        if not times.size:
            return
        raw = np.asarray(self.display_raw, dtype=np.float32)
        filtered = np.asarray(self.display_filtered, dtype=np.float32)
        try:
            window_seconds = max(0.1, self.window_seconds.get())
        except tk.TclError:
            window_seconds = 5.0
        start = max(0.0, times[-1] - window_seconds)
        first = int(np.searchsorted(times, start))
        times, raw, filtered = times[first:], raw[first:], filtered[first:]
        relative_times = times - times[-1]
        raw_x, raw_y = self._decimate_envelope(relative_times, raw)
        filtered_x, filtered_y = self._decimate_envelope(relative_times, filtered)

        # Keep the axes fixed during live capture. Repainting only the line
        # artists is dramatically cheaper than asking TkAgg to lay out ticks,
        # grid, legend and toolbar on every update.
        if self.live_background is None or self.live_window_seconds != window_seconds:
            self.axes.set_xlabel("Time before newest sample (s)")
            self.axes.set_xlim(-window_seconds, 0.0)
            self.raw_line.set_data([], [])
            self.filtered_line.set_data([], [])
            self.canvas.draw()
            self.live_background = self.canvas.copy_from_bbox(self.axes.bbox)
            self.live_window_seconds = window_seconds

        self.raw_line.set_data(raw_x, raw_y)
        self.filtered_line.set_data(filtered_x, filtered_y)
        self.canvas.restore_region(self.live_background)
        self.axes.draw_artist(self.raw_line)
        self.axes.draw_artist(self.filtered_line)
        self.canvas.blit(self.axes.bbox)
        self.canvas.flush_events()

    def _draw_paused(self) -> None:
        times = np.asarray(self.display_times, dtype=np.float64)
        if not times.size:
            return
        raw = np.asarray(self.display_raw, dtype=np.float32)
        filtered = np.asarray(self.display_filtered, dtype=np.float32)
        try:
            window_seconds = max(0.1, self.window_seconds.get())
        except tk.TclError:
            window_seconds = 5.0
        start = max(0.0, times[-1] - window_seconds)
        first = int(np.searchsorted(times, start))
        times, raw, filtered = times[first:], raw[first:], filtered[first:]
        raw_x, raw_y = self._decimate_envelope(times, raw)
        filtered_x, filtered_y = self._decimate_envelope(times, filtered)
        self.raw_line.set_data(raw_x, raw_y)
        self.filtered_line.set_data(filtered_x, filtered_y)
        self.axes.set_xlabel("Time (s)")
        self.axes.set_xlim(start, max(start + 0.001, times[-1]))
        self.live_background = None
        self.canvas.draw()

    @staticmethod
    def _decimate_envelope(
        times: np.ndarray, values: np.ndarray
    ) -> tuple[np.ndarray, np.ndarray]:
        """Reduce plot cost without losing short microphone peaks."""
        if len(times) <= MAX_PLOT_POINTS:
            return times, values
        bin_count = MAX_PLOT_POINTS // 2
        starts = np.linspace(0, len(values), bin_count + 1, dtype=np.int64)[:-1]
        minimum = np.fmin.reduceat(values, starts)
        maximum = np.fmax.reduceat(values, starts)
        next_starts = np.r_[starts[1:], len(values)]
        midpoints = (starts + next_starts - 1) // 2
        x = np.repeat(times[midpoints], 2)
        y = np.column_stack((minimum, maximum)).ravel()
        gap_times = times[np.isnan(values)]
        if gap_times.size:
            x = np.concatenate((x, gap_times))
            y = np.concatenate((y, np.full(gap_times.shape, np.nan)))
            order = np.argsort(x, kind="stable")
            x, y = x[order], y[order]
        return x, y

    def close(self) -> None:
        self.disconnect()
        self.stop_recording()
        self.root.destroy()


def main() -> None:
    root = tk.Tk()
    SerialAdcScope(root)
    root.mainloop()


if __name__ == "__main__":
    main()

# AEQ on LP-MSPM0L1306

**Current build: GPAMP comparison. Follow [GPAMP_COMPARISON.md](GPAMP_COMPARISON.md)
for the current wiring and BSL sequence. The original OPA-only wiring below is
retained as historical reference and is not sufficient for this build.**

Import this directory as the CCS project AEQ_L1306. The root AEQ project targets
C1104. Build in CCS or run python build.py (Python 3.9+).
The script accepts --sdk and --ccs installation paths.

The green RGB LED uses PA13 / TIMG0 C1. Keep J3 installed.
S1 / PA18 is Plus, active high, with J11 installed and the onboard pulldown.
S2 / PA14 is Minus, active low, with the internal pullup enabled.

Use the included L1306 target configuration and XDS110/SWD to load MAIN flash.
The application starts at zero and uses the factory ROM BSL configuration.

## Microphone sampling

Wire microphone OUT to PA25 (OPA0+) and PA15 (raw ADC A9).
Connect 4.7 uF from PA24 to GND. Open J1, J4 and J5. Keep J11 for S1.
OPA0 uses 16x AC gain with external capacitor feedback and its output pad disabled.
VREF is internal 2.5 V. TIMG1 starts a pair every 62.5 us.
Acquisition times are 5 us for OPA CH12 and 10 us for raw A9.

DMA channel 0 transfers 64 packed sample pairs into alternating buffers.
Each 256-pair window reports DC mean, AC RMS and raw minimum/maximum.
Watch micOpa, micRaw, micDmaBlocks, micWindows, micMissedBlocks,
micAdcOverruns and micTriggerOverruns. ADC units are 12-bit counts.

The first 500 ms allow the microphone, OPA capacitor and reference to settle.
The trigger timer pauses during debugger halts. Validate sustained sampling after
a reset, because debugger halts can still interrupt an in-flight conversion.

On the attached board, a ten-second run with the Adafruit SPW2430 connected to
both inputs produced matching means near 1402 counts and zero overrun/missed-block
counters. This circuit currently has no external microphone low-pass filter.

## Shared application

The CCS links reuse aeq_led.cpp from the C1104 project and adaptive_brightness.cpp
and button_gestures.cpp from this repository's top-level src directory.
Shared headers remain in the top-level include directory; no second clone is needed.
No copies of the shared algorithm or button implementation are maintained here.

OPA RMS is divided by micOpaGain (initially 16), then converted from the 2.5 V
ADC reference to equivalent counts at 3.3 V for the shared application's input.
Initial SPW2430 settings are micNoiseFloor=0.25 and micInputGain=12.
An OPA minimum below 256 or maximum above 3967 selects the raw channel for the
whole window. Eight consecutive windows inside 384..3839 restore the OPA path.
These margins and the nominal gain need validation with loud signals and the
final microphone/filter circuit. Firmware cannot recover microphone clipping.

The shared default brightness is 100/4095. ledDisplayGain=16 makes the LaunchPad
LED easier to see; it is a board display setting, not microphone gain.

LED PWM edges that fall inside an ADC acquisition disturb that sample. With the
former free-running 4096-count PWM this added about 26 counts RMS to the OPA
channel in silence, against about 3.4 counts with the PWM stopped. The PWM now
runs at one period per ADC frame (2000 counts, 16 kHz), phase-aligned so the
turn-on edge falls in the idle gap after a frame's conversions. The turn-off
edge still moves with the duty, but it hits every frame at the same point, so
it adds a constant offset, not noise. A new duty is applied by the DMA
interrupt only between two 16 ms windows, one window after it was computed.
The 0..4095 brightness scale is kept; the PWM resolves 2000 steps of it.
This behaviour has host tests only and still needs a hardware noise capture.
S1/S2 short presses increase/decrease brightness. Hold S1 for one second to
switch AEQ/constant mode. Hold S2 for one second to advance the AEQ settings menu.
The shared modifier gestures also remain available. Shutdown is not implemented
in this target yet; the application's two-button power-off request is ignored.

Watch micInputRms, micUsingRaw, micRangeChanges, micRawClippedWindows,
micOpaRmsPeak, micRawRmsPeak, ledBrightness, ledBrightnessPeak and ledDuty.
Peak values reset when the application restarts.

## Validation

Both build.py and a CCS command-line import/build passed with SDK 2.11.00.07,
SysConfig 1.28.1 and TI Arm Clang 5.1.1.LTS. The integrated application uses
9760 bytes of flash and 1247 bytes of allocated RAM, including the linker-default
256-byte stack reservation. Stack high-water usage has not yet been measured.
The factory NONMAIN configuration was compared before/after programming and
remained unchanged. Flash this target with Erase MAIN memory only.

Ten-second SPW2430 tests on the connected LaunchPad (no external low-pass filter):
- Quiet: OPA window RMS peak 6.40 counts; raw RMS peak 3.47 counts. LED stayed
  at idle (0.0009768 before display gain), with no brightness spikes.
- Music: OPA window RMS peak 22.41 counts; raw RMS peak 4.87 counts. LED peak
  0.02360, about 97% of the default 100/4095 brightness limit.
- Both runs: zero missed DMA blocks, ADC overruns or trigger overruns.
- Neither run clipped the OPA; physical range-switching remains to be tested.

These RMS peaks are the largest 16 ms windows, not averaged microphone noise
specifications. The unfiltered circuit can alias out-of-band sound/noise.

The standalone host test tests range normalization, both clipping rails,
recovery hysteresis and invalid gain overrides. For example, from this folder:

    gcc -std=c11 -Wall -Wextra -Werror -I. tests/audio_range_test.c audio_range.c -lm -o range_test.exe
    ./range_test.exe

Keep host test executables outside the source directory when committing.

The user also verified S1 long-press AEQ/constant switching and S1/S2 brightness
adjustment on the board after the music test.

## Menu serial output

UART0 TX is PA23 and RX is PA22, sharing the factory BSL wiring. The application
uses 115200 baud, 8N1. Open the XDS110 Application/User UART (currently COM45).
On menu entry/selection and each adjustment, one line reports only that setting:
SENS: 50 (sensitivity), GLOW: 60 (milliseconds), or IDLE: 4 (percent).
Normal brightness presses outside the menu produce no messages.
The main loop drains a 64-byte queue without blocking audio. If full, a complete
line is dropped and menuDebugDroppedLines increments. C1104 keeps a silent hook.
The host regression test in tests/menu_report_test.cpp checks menu events,
values and absence of repeated output; link with ../aeq_led.cpp and the shared
adaptive_brightness.cpp/button_gestures.cpp, with the necklace include directory.

## ROM BSL updates

The L1306 uses its factory ROM BSL, not the C1104 flash loader. With power off,
remove J101 RXD/TXD shunts and wire their XDS110-side pins: RXD to PA22, TXD to
PA23 (labels describe the target direction). Remove J6 to isolate PA22 from the
photodiode circuit. Keep J11 for S1. Existing OPA microphone wiring stays in place.

Build with python build.py to generate Debug/AEQ_L1306.bin. A CCS-only build does
not regenerate this binary; run build.py before using this updater.
Install pyserial, close serial terminals and stop CCS debugging, then run:

    python l1306/update_bsl.py COM45

After Listening appears, hold S1, tap/release RESET, then release S1. The script
waits up to 120 seconds; the ROM requires a connection within 10 seconds of entry.
Use --probe to identify only, or --image PATH to select another MAIN-only binary
linked at address zero. The ROM identity check validates the expected configuration
and buffer size, not the exact silicon part number: use this tool only on L1306.

The updater uses 9600 baud, CRC32 framing and the factory all-FF password. It makes
one password attempt, erases MAIN only, programs/verifies the body before writing
the reset vectors, verifies the full image CRC, then starts the application.
It never issues factory-reset or NONMAIN-write commands. Do not use the default
password after changing the device's BSL password. Interrupted updates can be
retried by invoking ROM BSL again; there is no rollback image.

After success, menu output is at 115200 baud:

    python -m serial.tools.miniterm COM45 115200

Exit the monitor with Ctrl+] before updating again. Python and pyserial suffice
for flashing an existing binary; UniFlash/CCS are not required by the updater.
The user confirmed successful BSL flashing and operation of the new firmware.
Protocol regression check (no hardware access):

    python l1306/tests/bsl_protocol_test.py

Protocol reference: https://www.ti.com/lit/ug/slau887a/slau887a.pdf

## Silence and quiet-music measurements

Build with build.py and update via BSL as above. Close miniterm before capture.
From the repository root, keep the microphone still in a quiet room, then run:

    python l1306/capture_noise.py COM45 quiet --output captures/quiet.csv

Next play the quietest music you want detected, at the intended distance/volume:

    python l1306/capture_noise.py COM45 music --output captures/music.csv --compare captures/quiet.csv

Both captures default to 20 seconds. Existing files are never overwritten: use
another --output filename to repeat. Keep hardware, gain and microphone position
unchanged; do not press buttons during recording. This mode measures the current
circuit, including its missing external low-pass filter. Both channels are read
before AEQ noise-floor subtraction or sensitivity processing.

The script sends M to start and X to stop measurement at 115200 baud. Reset also
turns measurement off. Menu messages remain available outside recording. The TX
queue is now 256 bytes. The LED algorithm and its sensitivity range are unchanged.
The firmware emits N1 CSV summaries after each 16 audio windows (256 ms). Each
channel reports average DC, RMS from the average of the 16 window powers, maximum
16 ms RMS, and ADC min/max. DC and RMS fields on the wire are counts times 1000;
the saved CSV uses ADC counts. At the 2.5 V reference, one count is about 0.610 mV
at the ADC input. OPA values are amplified; raw values are not. Do not compare their
counts directly as an OPA gain/noise estimate without accounting for bandwidth,
ADC quantization and actual gain.

Each summary also carries sequence/time and cumulative missed-block, ADC overrun,
trigger overrun and UART-drop counters. The host flags errors; repeat a recording
from a fresh reset if counters are nonzero. It does not record raw waveforms or
count AEQ false flashes. The reported percentage describes 256 ms summaries
containing at least one 16 ms window above the quiet capture's maximum window RMS.

Comparison estimates signal RMS as sqrt(max(music_RMS^2 - quiet_RMS^2, 0)), assuming
independent and unchanged background noise. It reports 20*log10(signal/noise) dB
and log2(signal/noise) amplitude bits, not ADC ENOB. This measures the complete
microphone/room/amplifier/ADC system. A separate correctly biased quiet-input test
is needed to isolate amplifier noise.

Measurement build: 15864 bytes flash and 1748 bytes allocated RAM (including the
256-byte stack reservation). Build and host simulations passed; streaming under
real hardware load remains to be verified by the capture error counters.
Host analysis regression: python l1306/tests/noise_capture_test.py

## Debug UART commands (115200 baud)

Single characters, no line ending. Settings reply with one `NAME: value` line.

| Command | Effect |
|---|---|
| `M` / `X` | Start / stop the N2 measurement lines |
| `O` `R` `G` `H` `A` | Raw capture of ADC slot 0, slot 1 (raw microphone), slot 2, slot 3, or all four interleaved (only while measuring) |
| `L` / `l` | Stop / restart the LED PWM |
| `1` | LED algorithm uses the OPA, one conversion per frame |
| `2` | OPA, average of three conversions per frame (boot default) |
| `3` | GPAMP, one conversion per frame |
| `4` | GPAMP, average of three conversions per frame |
| `+` / `-` | AEQ sensitivity up / down by 5 (0..100), reply `SENS` |
| `F` / `f` | Noise-floor cutoff up / down by 0.01 input-referred counts, reply `FLOOR` in thousandths |
| `K` / `k` | Input gain up / down by 1 (1..64), reply `GAIN` |
| `?` | Reports `PATH`, `SENS`, `FLOOR`, `GAIN` and `LEDPK`, the largest LED duty (0..4095) since the previous `?` |

A path command points ADC slots 0, 2 and 3 at the selected amplifier (slot 1
stays on the raw microphone) and pauses sampling for about 0.2 ms. The N2
fields and captures labelled "opa" and "gpamp" are slot 0 and slot 2, so both
show whichever amplifier the path selects. Paths 3 and 4 switch the GPAMP and
its PA22 output on; return to path 1 or 2 (or reset) before a BSL update. Allow
about a second after selecting a GPAMP path for its feedback capacitor to
settle; the LED may flash once at the switch. Settings are not stored.
The path and sensitivity commands have host tests only so far.

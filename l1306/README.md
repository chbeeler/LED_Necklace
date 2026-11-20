# AEQ on LP-MSPM0L1306

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

# LP-MSPM0L1306: OPA and GPAMP comparison

This setup uses LaunchPad signal labels, not final RGE-24 package pin numbers.
Use 1% resistors and the existing filtered microphone output, shared with OPA+
and PA15. Leave the OPA PA24 capacitor and existing microphone supply unchanged.

## GPAMP circuit

- Filtered microphone output to PA26 (GPAMP+), as well as existing PA25 and PA15.
- PA22 (GPAMP output) to 300 kohm feedback resistor to PA18 (GPAMP-).
- PA18 to 20 kohm resistor to a new 4.7 uF capacitor to GND.
- 300 kohm can be 200 kohm plus 100 kohm in series.
- No separate bias divider or microphone AC-coupling capacitor is needed here.
- Use a separate capacitor for this feedback leg; do not share PA24's capacitor.

This is a non-inverting stage with nominal audio gain 1+300k/20k = 16 and DC
gain 1. The feedback-leg pole is about 1.7 Hz for a true 4.7 uF capacitor.
A feedback capacitor across 300 kohm is not included in this first comparison.
These are calculated starting values, not bench-validated noise or stability
results. Keep the PA18 feedback wiring short. Actual OPA and GPAMP bandwidths
and gain errors can differ; same nominal gain does not guarantee equal bandwidth.

## Jumpers for RECORDING (power off while changing wiring)

Remove J11 (S1 and pulldown on PA18), J12 (red LED on PA26), and the J101 BSL
shunt (debugger invoke output to PA18). Keep J1, J4, J5, J6 open as in the
microphone experiment. J6 isolates PA22 from the photodiode feedback circuit.
Keep J3 for the green LED, plus power and SWD jumpers.

Remove the wire from the XDS110-side J101 RXD pin to PA22. Refit the J101 RXD
shunt and set J17 to XDS, pins 1-2. That routes the PC transmit signal to PA9.
Do not also attach the old PA22 wire. Leave the J101 TXD shunt removed and keep
the wire from PA23 to the XDS110-side J101 TXD pin for PC receive.

S1 cannot be used for the application's Plus control in this setup. Firmware
returns Plus=false. S2 still exists, but do not press buttons during recordings.

## Programming and starting a recording

The factory ROM BSL pins and NONMAIN are unchanged. BSL still receives on PA22,
whereas this application's UART receives on PA9. GPAMP and its output pad remain
disabled after boot until the capture tool sends M. X disables both again.

For a BSL update, close the serial monitor/capture tool. Power off, temporarily
fit J11 for the S1 invoke button, remove the J101 RXD shunt, and move the PC transmit
wire back to PA22. Keep the PA23 wire. Power up, run update_bsl.py COM45, and use
S1 + RESET as before. The application starts with GPAMP disabled, so it is safe
to finish the update in that wiring state. Do not run capture_noise.py yet.
Power off, REMOVE J11 again, remove the PA22 UART wire, and restore the recording
jumper arrangement above. Power up, then run the capture tool. Alternatively,
load Debug/AEQ_L1306.out through SWD with MAIN-only erase using recording wiring.
Never connect the PC UART transmitter to PA22 while GPAMP is enabled.

    python l1306/capture_noise.py COM45 quiet --output captures/gpamp-quiet.csv
    python l1306/capture_noise.py COM45 music --output captures/gpamp-music.csv --compare captures/gpamp-quiet.csv

The first 64 audio windows (1.024 seconds) after M are discarded for GPAMP and
capacitor settling. Inspect DC stability and repeat if the circuit is still
settling. If a capture is interrupted before X arrives, reset/power-cycle before
moving the UART wire back to PA22. The tool sends X on normal exit or Ctrl+C.

## Sampling and validation

At every 62.5 us timer event the ADC converts CH12 OPA, CH9 raw, CH14 GPAMP,
then CH14 again as FIFO packing padding. The last result is discarded. Each
useful channel is sampled at 16 kHz, sequentially rather than simultaneously.
OPA and GPAMP acquire for 5 us; raw for 10 us, at a 4 MHz ADC clock.
The 64-word DMA buffers hold 32 complete frames; a 256-frame RMS window remains
16 ms. No larger DMA buffers were needed. The LED algorithm still uses OPA/raw.

N2 UART summaries append GPAMP fields before the four error counters. The capture
tool supports N1 and N2, but record a fresh three-channel quiet baseline. GPAMP
values are in ADC counts at the same 2.5 V reference as OPA. Divide either amplified
channel by its actual gain to refer values back to the microphone input.

Build passed with SDK 2.11.00.07 and TI Arm Clang 5.1.1.LTS. Flash: 17728 bytes;
allocated SRAM: 1838 bytes, including a 256-byte stack reservation. Host tests
exercise actual audio.c FIFO decoding with distinct channel patterns, discarded
padding, RMS and timebase, plus GPAMP enable/disable, settling and N2 formatting.
Hardware timing, analog settling, gain and noise remain to be tested. Inspect all
capture error counters before interpreting measurements.

Host tests from the project root (write executables to a temporary directory):

    gcc -std=c11 -Il1306/tests/audio_dma -Il1306 l1306/tests/audio_dma/test.c -lm -o AUDIO_TEST_EXE
    gcc -std=c11 -Il1306/tests/gpamp_stream -Il1306 l1306/tests/gpamp_stream/test.c l1306/menu_debug.c -lm -o STREAM_TEST_EXE
    python l1306/tests/noise_capture_test.py

References:
- https://www.ti.com/lit/pdf/SLAU869 (LaunchPad jumper map and schematic)
- https://www.ti.com/lit/ds/symlink/mspm0l1306.pdf (pin functions and ADC channel map)

# Audio brightness lab

This desktop tool takes audio from a file, a live microphone, the computer's
speaker output, or the XIAO serial ADC stream and sends the sample blocks to a
brightness algorithm. It shows the result as a glowing virtual filament, a
percentage bar, and a short history graph. Algorithm, source, and tuning
controls can be changed while it runs.

## Setup on Windows

Install Python 3.11 or newer, making sure the installer adds Python to `PATH`.
Then, from the project directory:

```powershell
py -m pip install -r scripts/audio_brightness_requirements.txt
py scripts/audio_brightness_lab.py
```

If the Windows Python launcher does not see an already installed interpreter,
use that interpreter directly. On the current development machine this is:

```powershell
& "$env:LOCALAPPDATA\Programs\Python\Python313\python.exe" -m pip install -r scripts/audio_brightness_requirements.txt
& "$env:LOCALAPPDATA\Programs\Python\Python313\python.exe" scripts/audio_brightness_lab.py
```

An audio file can also be opened directly:

```powershell
py scripts/audio_brightness_lab.py "C:\Music\test.mp3"
```

Recent versions of `soundfile` can decode MP3. If an older installation cannot,
upgrade it with `py -m pip install --upgrade soundfile` or use WAV/FLAC/OGG.

## Audio sources

- **Audio file** plays and analyzes a loaded WAV/FLAC/OGG/MP3 file.
- **Microphone** analyzes a live recording device without playing it back.
- **System audio** uses Windows WASAPI loopback to analyze everything sent to
  the selected speaker or headphones. It does not duplicate the sound.
- **Serial ADC** reads the raw analog-microphone samples sent by the
  `analog_mic_a2_test` firmware. It removes the microphone's slowly changing DC
  bias before passing normalized samples to the brightness algorithm.

Selecting a live source starts listening immediately. Use **Stop listening** to
pause it and **Refresh devices** after plugging in or removing audio hardware.
Windows may ask for microphone permission the first time a live input is used.

Loopback belongs to a particular output device. Select the entry matching the
speakers or headphones through which Windows is currently playing. If it stays
silent, start audio playback first, check the Windows output device, and refresh
the list after Bluetooth/headphone changes.

Only one program can open a serial port at a time. Disconnect the serial scope
or PlatformIO monitor before selecting the same port in the brightness lab.

## Serial ADC scope

For recording and inspecting the analog microphone waveform, run:

```powershell
py scripts/serial_adc_scope.py
```

Select the XIAO's port and click **Connect**. **Pause display** freezes the graph
while capture and CSV recording continue. Use Matplotlib's magnifier and hand
buttons to zoom and pan the paused trace. **Start recording** writes every
received sample directly to CSV; **Save retained data** exports the in-memory
history, which is capped at 500,000 samples.

The firmware sends `timestamp_us,raw_adc,filtered_adc`. Device timestamps make
the time axis and measured sample rate independent of USB buffering. The scope
also accepts the earlier two-column `raw_adc,filtered_adc` format, using computer
arrival time as a less accurate fallback.

## Extending it

Add a class with `reset()`, `process(...)`, and `diagnostics`, then add it to
`ALGORITHMS` near the top of `audio_brightness_lab.py`. The algorithm receives
mono floating-point samples in the range -1 to +1 and returns brightness from
0 to 1.

The input-gain slider is useful for replaying the exact same recording as a
quiet-room or high-volume test. The adaptive algorithm should settle to a useful
range in both cases. Live microphone mode is the more realistic final check for
crowd noise, microphone placement, and automatic gain behaviour.

# LED_Necklace


This repository contains the Xiao necklace firmware and the MSPM0 experiments.
One clone contains both targets and their shared AEQ/button sources.

| Location | Purpose |
| --- | --- |
| platformio.ini, src/, include/, lib/, scripts/ | Existing Xiao nRF52840 project |
| src/adaptive_brightness.cpp, src/button_gestures.cpp, include/ | Shared algorithm/button code, linked by the MSP projects |
| mspm0/ | MSPM0C1104 CCS project and flash bootloader |
| mspm0/l1306/ | MSPM0L1306 CCS project, ROM BSL tools and OPA/GPAMP comparisons |

The Xiao layout stays unchanged to keep this migration small. Shared code remains
in src/include for now; a future move to shared/ can be a separate change.

See [docs/firmware-setup.md](docs/firmware-setup.md) for setup on another computer,
which branch to clone, building each target and the history migration.

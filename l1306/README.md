# AEQ on LP-MSPM0L1306

Import this directory as the CCS project AEQ_L1306. The root AEQ project targets
C1104. Build in CCS or run python build.py (Python 3.9+).
The script accepts --sdk and --ccs installation paths.

The green RGB LED uses PA13 / TIMG0 C1. Keep J3 installed.
S1 / PA18 is Plus, active high, with J11 installed and the onboard pulldown.
S2 / PA14 is Minus, active low, with the internal pullup enabled.

Use the included L1306 target configuration and XDS110/SWD to load MAIN flash.
The application starts at zero and uses the factory ROM BSL configuration.

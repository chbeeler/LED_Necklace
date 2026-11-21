# Experimental AEQ UART updates (LP-MSPM0C1104)

The C1104 runs a loader from main flash. Reset normally to start AEQ, or hold S2 (PA16 low), tap and release RESET, then release S2 to stay in the loader. It also waits for an update if the application vectors are blank or invalid. It has no idle timeout. The microphone and LED connections do not change. Leave J101 5:6 and 7:8 fitted for UART RX (PA26) and TX (PA27).

**Build configurations.** Use the AEQ project's CCS build configuration selector. Build **Debug** for the original full loader, or **Light** for the smaller one. Each build produces both TI-TXT files in its own directory.

| Configuration | Application address | Images directory | UART updater |
| --- | --- | --- | --- |
| Debug (full, default) | 0x1800 | Debug/boot-images/ | bootloader/update.py |
| Light | 0x1000 | Light/boot-images/ | bootloader/update_light.py |

**AEQ-install.txt** contains the loader and application for installation or recovery over SWD. **AEQ-update.txt** contains just the application for serial updates. To change profiles on a programmed LaunchPad, install the new combined image once over SWD. To switch back, install the Debug combined image. Both UART scripts reject the other loader before erasing.

**Update over UART.** Hold S2, tap and release RESET, then release S2. From the project root, for Light run:

    python bootloader/update_light.py COM42 --probe
    python bootloader/update_light.py COM42

For full use **bootloader/update.py** instead. After success tap RESET without S2. COM42 is the XDS110 Application/User UART on this PC; close other serial terminals first. Both updaters are project code implementing TI's MSPM0C UART BSL protocol. They need Python 3, pyserial (python -m pip install pyserial), and a working serial port. The update host does not need UniFlash, CCS or the SDK.

Full verifies every block by readback. Light disables raw readout, mass erase and baud-rate changes. It uses 9600 baud and keeps identification, application-range erase, programming and CRC verification. It checks the programmed body CRC before writing the reset vectors, then checks the complete image CRC. Neither profile has a rollback slot or signature check. If interrupted, re-enter the loader and repeat the update.

Builds use SDK 2.11.00.07 and TI Arm Clang 5.1.1.LTS. To repackage an already built configuration run **python bootloader/build.py --profile full** or **--profile light**. The build script accepts MSPM0_SDK_INSTALL_DIR and TICLANG_ARMCOMPILER path overrides.

**UniFlash.** Select the MSPM0C1104 and XDS110, set erase to **Erase MAIN memory only**, then Load Image using **AEQ-install.txt**. Reset normally. UniFlash 9.7.1 on this PC has an SWD target for the C1104 but no corresponding serial bootloader target. Use the Python updater for UART. A normal CCS download of AEQ.out alone with default MAIN erase removes the loader; use the combined install file to restore it. To go back to the previous firmware, rebuild main and flash its ordinary AEQ.out over SWD, erasing MAIN only. Neither procedure needs a factory reset.

**Design.** The loader is adapted from TI's `examples/nortos/LP_MSPM0C1104/bsl/flash_bsl` in SDK 2.11.00.07. The original TI source notices are retained. Boot configuration is ordinary const data in main flash. There is no hardware protection or NONMAIN image. UART range/program commands enforce the application bounds, full-loader mass erase erases only the application, and factory reset is refused. The full loader reserves 6 KB and light reserves 4 KB. AEQ owns the remaining MAIN flash up to 0x3fff. Both use the same 1 KB of RAM at different times. CCS includes makefile.init to select the application layout instead of SysConfig's default layout. The packaging step rejects overlaps, out-of-range bytes and invalid reset vectors.

References: [TI LaunchPad guide](https://www.ti.com/lit/ug/slau908b/slau908b.pdf), [TI UniFlash guide](https://software-dl.ti.com/msp430/esd/MSPM0-SDK/latest/docs/english/tools/doc_guide/doc_guide-srcs/uniflash_guide.html).

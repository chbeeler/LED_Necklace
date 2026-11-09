# Experimental AEQ UART updates (LP-MSPM0C1104)

The C1104 runs a loader from main flash. Reset normally to start AEQ, or hold S2 (PA16 low), tap and release RESET, then release S2 to stay in the loader. It also waits for an update if the application vectors are blank or invalid. It has no idle timeout. The microphone and LED connections do not change. Leave J101 5:6 and 7:8 fitted for UART RX (PA26) and TX (PA27).

**Try it on the installed board.** From the AEQ project directory, enter the loader as above, then run:

```powershell
python bootloader/update.py COM42 --probe
python bootloader/update.py COM42
```

COM42 is the XDS110 Application/User UART on this PC. If it changes, use the port shown under that name in Device Manager. Close serial terminals first. The updater uses pyserial (installed here), 9600 baud, and TI's MSPM0C CRC16 packet format. It erases only the application, programs and reads back each block, and writes the initial stack pointer and reset vector last. After verification, tap RESET without S2. If an update is interrupted, enter the loader again and repeat it. This is a single-image experiment: there is no rollback slot or signature check.

**Build.** Build the AEQ Debug configuration in CCS. Then run `python bootloader/build.py` from the project root. It uses TI SDK 2.11.00.07 and TI Arm Clang 5.1.1.LTS; paths can be overridden with `MSPM0_SDK_INSTALL_DIR` and `TICLANG_ARMCOMPILER`. Images are generated under `Debug/boot-images/`:

| File | Use |
| --- | --- |
| AEQ-install.txt | Combined loader + AEQ. Initial install or recovery over XDS110/SWD. |
| AEQ-update.txt | Application only, starting at 0x1800. Used by the UART updater. |

**UniFlash.** Select the MSPM0C1104 and XDS110, set erase to **Erase MAIN memory only**, then Load Image using **AEQ-install.txt**. Reset normally. UniFlash 9.7.1 on this PC has an SWD target for the C1104 but no corresponding serial bootloader target. Use the Python updater for UART. A normal CCS download of AEQ.out alone with default MAIN erase removes the loader; use the combined install file to restore it. To go back to the previous firmware, rebuild main and flash its ordinary AEQ.out over SWD, erasing MAIN only. Neither procedure needs a factory reset.

**Design.** The loader is adapted from TI's `examples/nortos/LP_MSPM0C1104/bsl/flash_bsl` in SDK 2.11.00.07. The original TI source notices are retained. Boot configuration is ordinary const data in main flash. There is no hardware protection or NONMAIN image. UART range/program commands enforce the application bounds, mass erase erases only the application, and factory reset is refused. The loader occupies 0x0000..0x17ff; AEQ owns 0x1800..0x3fff. Both use the same 1 KB of RAM at different times. CCS includes makefile.init to select the application layout instead of SysConfig's default layout. The packaging step rejects overlaps, out-of-range bytes and invalid reset vectors.

References: [TI LaunchPad guide](https://www.ti.com/lit/ug/slau908b/slau908b.pdf), [TI UniFlash guide](https://software-dl.ti.com/msp430/esd/MSPM0-SDK/latest/docs/english/tools/doc_guide/doc_guide-srcs/uniflash_guide.html).

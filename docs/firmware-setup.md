# Firmware checkout and build

The unified working branch is migration/unified-firmware. Until it has been
pushed, it exists only locally. After publishing that branch:

    git clone --branch migration/unified-firmware https://github.com/chbeeler/LED_Necklace.git
    cd LED_Necklace

No submodules or sibling firmware checkouts are required. Any checkout directory
works; the CCS source links and include paths are relative to this repository.

## Git identity

Set the repository-local identity on each new computer (Git config is not cloned):

    git config user.name "Christoph Beeler"
    git config user.email "89337039+chbeeler@users.noreply.github.com"

## Xiao nRF52840

Install PlatformIO and use the existing root platformio.ini. Build from the root:

    pio run -e seeed_xiao_nrf52840_sense_no_nfc

The NFC-enabled and analog microphone test environments remain available in
platformio.ini. The pre-existing platform/library specifications are unchanged.
PlatformIO downloads the dependencies when first building on another computer.

## MSPM0 projects

Install CCS with TI Arm Clang 5.1.1.LTS, SysConfig 1.28.1 and MSPM0 SDK 2.11.00.07.
Import mspm0 as project AEQ for C1104, or mspm0/l1306 as AEQ_L1306 for L1306.
If an old project with the same name is already imported, remove that workspace
entry WITHOUT deleting its files, then import the new location. Close old debug
sessions first. Generated build directories are deliberately not migrated.

L1306 command-line build, from the repository root (Python 3.9+):

    python mspm0/l1306/build.py

Default installations are C:/ti/ccs2101/ccs and C:/ti/mspm0_sdk_2_11_00_07.
For other locations:

    python mspm0/l1306/build.py --ccs PATH_TO_CCS --sdk PATH_TO_SDK

The output is mspm0/l1306/Debug/AEQ_L1306.out plus the MAIN-only .bin for UART
updates. The C1104 project's Debug and Light configurations package their own
flash bootloader; its makefile post-build step requires python on PATH. Override
MSPM0_SDK_INSTALL_DIR and TICLANG_ARMCOMPILER if their default locations differ.

For existing MCU instructions, first change directory to mspm0. Paths beginning
l1306/ or bootloader/ in those documents are relative to that directory:

    cd mspm0
    python -m pip install pyserial
    python l1306/update_bsl.py COM45

COM45 is specific to the first PC; choose the XDS110 Application/User UART port
on the second computer. Programming and recording need different GPAMP jumper
arrangements: follow [GPAMP comparison](../mspm0/l1306/GPAMP_COMPARISON.md).
Do not use a C1104 updater for the L1306.

Keep recordings under mspm0/captures/ (ignored). Existing recordings remain in the
original C:/Daten/MSP/AEQ/captures folder and are not part of the source history.
Copy any recordings you want separately. UART scripts require Python and pyserial;
flashing a prebuilt image does not require CCS or UniFlash.

## Migration and history

Work-email author AND committer identities were corrected in both original repos.
Personal Gmail author and committer identities were also changed to noreply. Repository-local user identity
is now the personal GitHub noreply identity. Rewriting authors changes commit IDs,
but each original commit's file tree was checked to remain identical.

The latest Xiao baseline is feature/V4. MSPM0 history was imported from the AEQ
experiment/l1306-opa branch. The import preserves its full ancestor history. A
separate move-only commit places those files under mspm0; subsequent commits
change build paths and documentation. The existing Xiao source layout is intact.

The old AEQ folder remains available as a local recovery/reference checkout.
Use this unified repository for future development; do not maintain two divergent
copies. Pre-rewrite Git bundles, configs and refs are retained separately under
C:/Daten/repo-backups/firmware-before-migration-20261121-132709/.

No GitHub history was force-pushed during migration. Publish the unified branch
with a normal explicit branch push. Existing remote branches retain their old
history until deliberately migrated; merging those old branches into corrected
history can reintroduce the work-email commits. Do not use git push --mirror.

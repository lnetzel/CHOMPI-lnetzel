---
name: build-firmware
description: 'Use when: compiling, building, or making any CHOMPI firmware (.bin) on this Windows machine. Trigger words: compile, build, make, flash, firmware binary. Covers chompi-tape, chompi-tempo, chompi-wave, chompi-usb-storage, and the v6.4 bootloader, including the pinned toolchains and MSYS2 invocation.'
---

# Build CHOMPI Firmware on Windows

All app firmwares build with GNU make from an MSYS2 shell. The bootloader and
TEMPO use a different, pinned toolchain - see the table.

## Toolchains (pinned - do not substitute)

| Firmware | Toolchain | Path |
|---|---|---|
| tape / wave / usb-storage | Arm GNU 10.3-2021.10 (GCC 10.3.1) | C:\armgnu\10.3-2021.10 (installed) |
| tempo | Arm GNU 13.3.rel1 (GCC 13.3.1) | NOT installed on this machine - install before building |
| bootloader v6.4 | Arm GNU 13.3.rel1 (GCC 13.3.1) | set CHOMPI_TOOLCHAIN_BIN to its bin/ dir |

Newer compilers than 10.3 overflow the tape/wave SRAM region and fail at link
time. TEMPO is the exception: it REQUIRES 13.3.rel1.

## Standard build command (from PowerShell)

Single quotes around the bash command are mandatory - PowerShell expands $PATH
to empty inside double quotes.

    C:\msys64\usr\bin\bash.exe -lc 'export PATH="/c/armgnu/10.3-2021.10/bin:$PATH"; cd /c/Dev/CHOMPI-lnetzel/firmware/<PROJECT>/code/src && make -j4'

make lives at /usr/bin/make in MSYS2. Output lands in code/src/build/<TARGET>.bin.

## Per-project table

| Project | Build dir | Makefile TARGET | Output binary | Notes |
|---|---|---|---|---|
| chompi-tape | firmware/chompi-tape/code/src | CHOMPI_TAPE_2.0-lnetzel.1 | build/CHOMPI_TAPE_2.0-lnetzel.1.bin | SRAM ~99% full; relink-only builds take seconds |
| chompi-tempo | firmware/chompi-tempo/code/src | CHOMPI | build/CHOMPI.bin | needs 13.3.rel1 toolchain first on PATH; same layout as tape |
| chompi-wave | firmware/chompi-wave/code/src | CHOMPI | build/CHOMPI.bin | make program-boot installs bin/CHOMPI_Bootloader_V6_2_0.bin |
| chompi-usb-storage | firmware/chompi-usb-storage/code/src | chompi_usb_storage_v1.5 | build/chompi_usb_storage_v1.5.bin | run make check-toolchain first; reuses wave libDaisy build and tape LED helper - those folders must exist |
| chompi-bootloader-v6.4-beta | firmware/chompi-bootloader-v6.4-beta | - | bootloader/build/chompi_bootloader_v6_4.bin | run ./build-bootloader.sh (clean arg purges first); correct build is byte-identical to release: 119,612 bytes, md5 580b187fec405849fb401eb699281e4c |
| chompi-drone | - | - | - | empty folder, nothing to build |

## Library dependencies

App firmwares link prebuilt ../libs/libDaisy/build/libdaisy.a and
../libs/DaisySP/build/libdaisysp.a. If missing, run make once in each lib dir
first. Incremental rebuilds after editing one source file take seconds.

## Gotchas (hard-won - check here first)

- TARGET must not contain spaces. libDaisy's core Makefile passes it unquoted
  to g++/objcopy; a space splits the link into bogus args like
  No rule to make target '2.0-lnetzel.1.hex'.
- make: command not found from a PowerShell-invoked bash means $PATH got
  expanded by PowerShell before bash saw it. Re-quote with single quotes.
- Terminal goes silent after long builds - verify results with file tools
  (check for the .bin in build/, read the tail of a captured log) instead of
  re-running the same command.
- Install: copy the .bin to the SD card root, delete any other .bin, power on;
  the bootloader installs it (slow rainbow pattern).
- tape is at SRAM capacity (SRAM_EXEC 99.80%, SRAM 99.05%) - added features need
  something removed to fit.

## Verification after build

1. Confirm build/<TARGET>.bin exists with a fresh timestamp.
2. Check the Memory region usage printed at link: SRAM_EXEC/SRAM must be <=100%.
3. For the bootloader, compare md5 against the released hash in the table above.

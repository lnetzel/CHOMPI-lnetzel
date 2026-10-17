---
name: build-organ
description: 'Use when: compiling, building, or making the chompi-organ firmware (.bin) on this Windows machine, running the organ host tests, or diagnosing any organ build/compile/link/test error. Trigger words: compile, build, make, organ, MSYS2, MSYS64, arm-none-eabi, link error, test failure, check-toolchain. Rule 0: the README MSYS64 recipe is always the first thing to try; then consult the troubleshooting table of known organ build gotchas.'
---

# Build & troubleshoot chompi-organ

## Rule 0: README first

The "Building on Windows" section of
[firmware/chompi-organ/README.md](../../../firmware/chompi-organ/README.md)
is the doc of record and solves most problems. Try its exact recipe before
anything else:

- **MSYS2** shell + `pacman -S --needed make` (one-time)
- **Arm GNU Toolchain 10.3-2021.10** at `C:\armgnu\10.3-2021.10` — exactly
  this version; newer compilers can overflow SRAM
- **chompi-wave** must sit next to chompi-organ (libDaisy/DaisySP/coreJSON
  are reused via relative paths; there is no libs copy in chompi-organ)

From an MSYS2 UCRT64 shell:

```sh
export PATH="/c/armgnu/10.3-2021.10/bin:$PATH"
cd /c/Dev/CHOMPI-lnetzel/firmware/chompi-organ/code/src
make check-toolchain   # must print GCC 10.3.1
make -j4 DEBUG=0 OPT=-O3
```

From PowerShell (single quotes are mandatory — see gotcha 1):

```powershell
C:\msys64\usr\bin\bash.exe -lc 'export PATH="/c/armgnu/10.3-2021.10/bin:$PATH"; cd /c/Dev/CHOMPI-lnetzel/firmware/chompi-organ/code/src && make -j4 DEBUG=0 OPT=-O3'
```

Host tests (need the UCRT64 host g++, installed 2026-10-15):

```powershell
C:\msys64\usr\bin\bash.exe -lc 'export PATH="/ucrt64/bin:$PATH"; cd /c/Dev/CHOMPI-lnetzel/firmware/chompi-organ/code/tests && make test'
```

CPU-meter build: append `ORGAN_CPU_METER=1` to the make line.

Output: `code/src/build/CHOMPI_ORGAN_1.0.bin` → copy to SD card root (delete
other .bin files), power on. Never flash the bootloader for app updates.

## Troubleshooting table (check here second)

| Symptom | Root cause | Fix |
|---|---|---|
| `make: command not found` from a PowerShell-invoked bash | PowerShell expanded `$PATH` inside **double** quotes before bash saw it | Re-quote the bash command in **single** quotes |
| `&& is not a valid statement separator` | `&&` used in PowerShell 5.1 | Use `;` between PowerShell commands (`&&` is fine *inside* the single-quoted bash string) |
| bare `bash.exe script.sh`: ls/stat/make "not found" | no `-l` → broken PATH | Always `bash.exe -lc '...'` |
| Terminal goes silent / hangs mid-build | flaky sync terminal | Redirect to a log (`... > /tmp/organ_build.log 2>&1`) and read `C:\msys64\tmp\organ_build.log` with file tools instead of re-running. If fully hung, start one async command for a fresh terminal |
| `make` says "Nothing to be done" after editing a **header** | stale mtimes (an async baseline build ran past the edits) | `touch` the changed sources and rebuild |
| Fatal error: deleted header missing, or weird stale deps | stale `.d` files in `code/src/build` | `rm -rf build` and do a clean rebuild |
| `make ORGAN_CPU_METER=1` (any `VAR=1`) rebuilds nothing | make does not track flag changes | `touch` sources or clean first; verify `-DORGAN_CPU_METER` appears on the compile lines |
| `arm-none-eabi-g++: command not found` | export line missing/wrong | Must be exactly `/c/armgnu/10.3-2021.10/bin`; run `make check-toolchain` |
| Link can't find `libdaisy.a` / `libdaisysp.a` | prebuilt libs missing | `make` once in `../chompi-wave/code/libs/libDaisy` and `../chompi-wave/code/libs/DaisySP` |
| `No rule to make target '2.0-lnetzel.1.hex'`-style bogus args | Makefile `TARGET` contains a **space** (passed unquoted to g++/objcopy) | Remove spaces from `TARGET` |
| Link error on `static constexpr float arr[]` used via subscript | C++14 in-class constexpr arrays need an out-of-class definition when odr-used | Use a static function returning the value instead |
| `MidiHandler` ambiguous / clashes | name collision with `daisy::MidiHandler` (daisy.h) | Prefix organ classes with `Organ*` |
| Test exe won't start: `libstdc++-6.dll` missing | host g++ runtime dir not on PATH | `export PATH="/ucrt64/bin:$PATH"` inside the bash line |
| Host g++ missing entirely | not installed | `pacman -S mingw-w64-ucrt-x86_64-gcc` |
| Can't verify `.d` deps with the editor's text search | search skips ignored `build/` dirs | Use bash `grep -r` inside `build/` instead |
| Memory-usage % looks stale/mangled in terminal output | flaky terminal replay | Trust `arm-none-eabi-size -A` on the ELF, not the replayed log |

## Known-failing tests (do not chase these)

`tonewheel_character_test` (L273) and `vibrato_test` (L91) fail at HEAD —
pre-existing, unrelated to new changes. All other host tests pass. If your
change touches those files, diff against the pre-existing failure before
assuming you broke something.

## Runtime issue worth knowing (looks like a build problem)

**Boot hangs only when a preset file exists on the SD card** = FatFS `FIL`/
buffers placed on the stack. The organ stack is in DTCMRAM, which the SDMMC
IDMA cannot reach → the read poll stalls forever. Rule: no FatFS `FIL`/
`FATFS`/data buffers on the organ stack — make them members/globals (D1 SRAM
is DMA-safe).

## Verify a good build

1. `code/src/build/CHOMPI_ORGAN_1.0.bin` exists with a fresh timestamp.
2. Link output: SRAM_EXEC / SRAM / RAM_D2 all ≤ 100% (organ currently ~72%
   worst region — plenty of headroom, unlike tape).
3. Confirm the link line actually ran (a flaky terminal can replay a stale
   log); when in doubt, `arm-none-eabi-size -A` the ELF.

## Hard limits learned on hardware

- **6 voices is the CPU ceiling** for this engine on the H7 — a 7-voice
  attempt was reverted; voice-count increases need per-voice CPU savings
  first. Check worst-case CPU with an `ORGAN_CPU_METER=1` build.

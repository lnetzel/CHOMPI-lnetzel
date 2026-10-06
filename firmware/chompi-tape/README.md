# CHOMPI — TAPE v2.0-lnetzel.1 Firmware for Multi-Firmware Launcher

Based on upstream origianl 2.0 this is now a modified firmware.


## NEW in v2.0-lnetzel.2
- Removed the factory hardware self-test to free SRAM. Holding the GAIN encoder
  at startup no longer has any effect; the firmware always boots into the normal
  instrument. (The old diagnostic code remains in git history if ever needed.)
- Added a third page to the GAIN encoder: looper playback volume (see below).

### GAIN encoder pages

Short clicks on the GAIN encoder cycle three pages:

1. **Master output volume**
2. **Input monitor volume**
3. **Looper playback volume** (yellow LED; brightness follows the setting,
   with a dim floor so the page stays identifiable at mute)

The looper playback page scales loop playback linearly from mute to 100% of the
normal loop level. It starts at 100% on every power-up, is never written to the
SD card, and is kept for the whole session — including across loop clear, load,
append, and sample changes. It adjusts playback only: stored audio, recording
level, and overdub feedback are untouched.

Audio-routing notes:

- The gain affects loop playback on both the headphone and line outputs.
- The gain is applied before any post-looper effects, so effect tails may
  briefly remain after muting the loop.
- Resampling captures the adjusted audible mix, including the loop volume
  setting.

## NEW in v2.0-lnetzel.1
- Compatible with Multi-Firmware Launcher
- Added paste-append when copying a preset to looper buffer with overdub key.

---

The flagship sampler firmware for **CHOMPI**: the main firmware every CHOMPI ships with.

## Firmware description

TAPE is a 7-voice sampler and varispeed tape looper. Samples stream from the microSD card; the
looper and sample buffer record into SDRAM. Seven streaming voices, varispeed playback,
tape-style looper, delay and reverb, and MIDI in and out over TRS and USB.

## Building

Toolchain: GNU Arm Embedded 10.3-2021.10. Newer compilers overflow the firmware's SRAM region
and fail at the link step.

Warning: This firmware is close to capacity. After removal of the hardware self-test
(2026-10-06 build), measured headroom is 8,712 bytes in SRAM_EXEC (96.40% used) and
3,476 bytes in SRAM (98.77% used). Any additional tweaks or features will very likely
require sacrificing something to free up the necessary code space.

## Repository layout

```
code/src/                 the firmware
code/libs/                vendored libDaisy, DaisySP, coreJSON (MIT)
code/Chompi_Bootloader/   the bootloader this firmware is loaded by
code/bms_test/            standalone battery-management bring-up example
bin/                      bootloader binary and install script
```

## SD card layout

The card holds the firmware binary, the sample banks, and two JSON files: `options.json`
(global settings) and `presets.json` (per-slot knob positions). Samples are named
`<instrument>_<bank><slot>.wav` — 48 kHz, 16-bit stereo — with a matching `_double` variant
used for high-pitched playback.

## Support Guidelines

This is a discontinuation open-source release. As such, this repo is intended to be a permanent
source for files and documentation, and will likely not be receiving updates in the future. If you wish
to customize your own project, we recommend cloning this repo into your own GitHub.

## Community

Even though this version of CHOMPI is now discontinued, the CLUB is expanding. If you want to
discuss this project, share your creations, see what other users have made on their CHOMPI, feel
free to check out the CHOMPI Open Source channel on the Chase Bliss Discord.

## License

MIT — see [`LICENSE`](../../LICENSE) at the root of this repo. [`THIRD_PARTY.md`](../../THIRD_PARTY.md)
lists the work this builds on. The CHOMPI name and marks are not covered by the license — see
[`TRADEMARKS.md`](../../TRADEMARKS.md).

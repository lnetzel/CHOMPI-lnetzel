# CHOMPI — TAPE v2.0-lnetzel.4 Firmware for Multi-Firmware Launcher

Based on upstream origianl 2.0 this is now a modified firmware.


## NEW in TAPE-lnetzel-1.1
- Undo overdub:
  - One undo level. An overdub session is everything written from entering
    overdub until leaving it, including multiple loop revolutions.
  - How to use it: While holding SHIFT key press the transport wheel. CHOMPI LED will blink red and the two wheel direction LEDs will blink green. You are basically being asked "Are you sure you want to remove last overdub?"
    CHOMPI press confirms and will remove the last overdub, Transport Wheel press will cancel.
  - Maximum loop buffer length has been cut in half to implement this so it's now 82.5 seconds (was about 165 seconds).
  - There is no redo, once undone, it's gone.
  - You cannot undo an append-paste to looper, that's not the same as an overdub session. 

## NEW in TAPE-lnetzel-1.0
- Reliable preset start/end trimming down to 5 ms range:
  - Fixed mixed byte/frame units in the trim boundary checks. The old code
    compared absolute file bytes (including the WAV header) against
    stereo-frame counts, so it could reject valid trims or accept trims that
    click at the playhead.
  - Trim edits now validate every targeted voice before changing any of them
    (all-or-none). Previously the seven JAMMI voices could end up with mixed
    start/end boundaries.
  - Rejected trims roll the encoder value and MIDI feedback back to the
    retained boundary in both turn directions and for MIDI absolute input,
    and the saved preset only changes on accepted trims.
  - Stopped/cached voices and outward trim moves are no longer blocked by the
    current file position. Active playback still rejects trims that would land
    inside the already-queued audio region (16,384-byte margin = one streaming
    FIFO).
- Added a host-compilable test target for the trim validation arithmetic under
  `code/tests`. Run it with `make -C firmware/chompi-tape/code/tests test` on
  any machine with a host C++14 compiler.

Verification (2026-10-07): clean build with GNU Arm Embedded 10.3-2021.10
passes (bin 239,192 bytes; SRAM_EXEC 98.87% used / 2,728 bytes free, SRAM
98.85% used / 3,252 bytes free). Exact trim margin thresholds are compile-time
checked via static_asserts in `code/tests`. On-device checks per
`plans/trim_controls_checklist.md` are NOT yet performed.

## NEW in v2.0-lnetzel.2
- Removed the factory hardware self-test to free SRAM. Holding the GAIN encoder
  at startup no longer has any effect; the firmware always boots into the normal
  instrument. (The old diagnostic code remains in git history if ever needed.)
- Looper Playback Volume - Added a third page to the GAIN encoder to adjust the looper playback volume
- Trimmed Paste to Looper - When pasting or append-pasting a preset to the looper only the section between start and end position is included.  

## NEW in v2.0-lnetzel.1
- Added new feature to append a copy/paste to the end of the looper buffer. Could be seen as pattern chaining in a very basic way.

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

Warning: This firmware is close to capacity. With the undo-overdub changes
(2026-10-08 build), measured headroom is 1,200 bytes in SRAM_EXEC (99.50% used)
and 3,184 bytes in SRAM (98.87% used). Any additional tweaks or features will
very likely require sacrificing something to free up the necessary code space.

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

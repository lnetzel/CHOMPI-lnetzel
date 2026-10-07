# TAPE Trim Controls — On-Device Verification Checklist

Scope: trim (start/end) reliability changes from
[trim_sample_position_enhancements_plan.md](trim_sample_position_enhancements_plan.md),
implemented 2026-10-07 in firmware `CHOMPI_TAPE_2.0-lnetzel.3`.

Automated coverage already done (no hardware needed): validation arithmetic,
exact 16,384-byte margin thresholds, ordering/min-length, short/empty files,
RAM element/frame mapping, no-op/inactive/pending-I/O cases, and all-or-none
multi-voice preflight — see `code/tests` (compile-time static_asserts verified
with the ARM 10.3 toolchain during the build; run-time tests need a host C++
compiler: `make -C firmware/chompi-tape/code/tests test`).

**Everything below requires hardware and is NOT yet performed.**

Firmware under test: `code/src/build/CHOMPI_TAPE_2.0-lnetzel.3.bin`
(clean build 2026-10-07, GNU Arm Embedded 10.3-2021.10; bin 239,192 bytes;
SRAM_EXEC 2,728 B free, SRAM 3,252 B free).

## 1. Boundary accept/reject vs. playhead

- [ ] Forward playback, shrink END toward the playhead: trim is rejected while
      the boundary is inside ~1 FIFO (16,384 bytes ≈ 85 ms @ 48 kHz) past the
      read region; no click or pop when rejected
- [ ] Forward playback, move END outward: always accepted, no glitch
- [ ] Reverse playback, advance START toward the playhead: rejected near the
      read region; no click when rejected
- [ ] Reverse playback, move START outward: always accepted
- [ ] Stopped/cached voices: trims accepted even when the last file position
      is past the proposed boundary
- [ ] Trim during rapid retriggering: no stuck boundaries, no silence

## 2. Encoder and MIDI behavior (UI)

- [ ] START knob, rejected trim rolls the on-screen/knob value back on
      **positive** turns
- [ ] START knob, rejected trim rolls back on **negative** turns (previously
      not rolled back)
- [ ] END knob, rejected trim rolls back on **both** turn directions
      (previously only negative)
- [ ] MIDI absolute CC to a colliding/invalid position: value snaps back to
      the retained boundary; no preset write occurs
- [ ] Repeated rapid knob turns: no clicks, no desync between knob value and
      audible boundary
- [ ] UI minimum-gap rejection (start within 0.01 of end): leaves preset
      unchanged

## 3. Atomic multi-voice updates

- [ ] JAMMI: hold a 7-voice chord, trim into a collision: **all** voices keep
      the old boundary (notes stay consistent across the chord)
- [ ] JAMMI: valid trim applies to all seven voices uniformly
- [ ] CUBBI: trim affects only the latest voice

## 4. Playback modes and pitch

- [ ] Looping and one-shot, forward and reverse, for each check above
- [ ] Normal speed and crossing the >1.5x double-file threshold while trimming
- [ ] SD presets (short and long) and RAM buffer playback (slot 15)

## 5. Persistence

- [ ] Save preset after accepted trims; reload: boundaries survive exactly
- [ ] Rejected trim is not present after save/reload
- [ ] Saved preset values match MIDI feedback values after accept and reject

## 6. Regression of existing workflows

- [ ] Sample-window shifting (menu page) still moves start/end together
- [ ] Trimmed copy/paste to the looper still applies the saved boundaries
- [ ] Recording a new sample resets trims to defaults

## Results log

| Date | Firmware | Case(s) | Result | Notes |
| --- | --- | --- | --- | --- |
|  |  |  |  |  |

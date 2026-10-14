# CHOMPI Organ: Modulation Mode (organic per-pipe volume & Character drift)

## Problem and approach

Add slow, free-running, randomly-timed modulation of each pipe's volume and
tonewheel Character to give the organ a living, organic movement. Eight
independent modulators (4 pipes x {volume, Character}) run continuously in
the engine. A new UI layer, **Modulation Mode**, is entered with the hardware
toggle switch next to SHIFT/CHOMPI; the existing behavior becomes **Normal
Mode**. In Modulation Mode pipe encoders 1-4 set the per-pipe modulation
amounts on two pages.

## Confirmed behavior

### Mode switch
- Toggle **DOWN = Normal Mode**, **UP = Modulation Mode**.
- Polarity verified against CHOMPI-Tape: recording/input monitoring is active
  when `switch_state == false` (`NormalPage.h` `SetInputMonitor(!switch_state)`,
  `SetSwitchState(true)` stops recording) and Tape records only in the UP
  position, so **UP == `hw.GetToggleState() == false`**. Encode this once as a
  named helper with a comment referencing Tape.
- Use the existing debounced `GetToggleState()` (tog_state hysteresis in
  `ProcessAllControls`).

### Modulation Mode pages (per pipe, like Normal Mode)
- Each pipe encoder push toggles that pipe between mod pg.1 and mod pg.2
  (separate state from the Normal Mode page, which is preserved).
- **pg.1 - volume modulation amount**, knob LED white.
- **pg.2 - Character modulation amount**, knob LED blue.
- 32 steps, 0..100% (~3% per click), clamped, no wrap.
- Turn: sets that pipe's amount. **SHIFT+turn**: apply delta to the turned
  pipe, then broadcast the resulting value to all 4 pipes (same pattern as
  Normal Mode).
- Value bar on the 15 white keys for 500 ms, same rounding as other 32-step
  bars: pg.1 uniformly white, pg.2 uniformly blue.
- Knob LED brightness = 0.2 + 0.8 x amount (floor keeps page visible);
  muted pipe stays solid red.

### Modulation depth and window
- Work in each parameter's normalized domain: volume `x = level_step/32`
  (gain stays `x^2`), Character `a = char_step/64` (before smoothstep easing).
- Full-scale (100% amount) window = **40% total (+-20%)**; amount scales the
  half-width: `d = 0.20 * amount_step/32`.
- Window `[c-d, c+d]`, slid (not clipped) to stay inside [0,1]:
  - `lo<0` -> `[0, 2d]`; `hi>1` -> `[1-2d, 1]`.
  - Examples at 100%: 95% -> 55..100 (5 over, 35 below); 50% -> 30..70;
    5% -> 0..40 (5 below, 35 above); **0% -> 0..40 (pipe becomes audible,
    as requested)**.
- Modulated value = `lo + (hi-lo) * u`, u in [0,1] from the modulator.
- Mute still forces silence. Amount 0 = exact unmodulated center (bit-exact
  with today's behavior).
- User/MIDI-facing values (`GetPipeStep`, `GetPipeCharacterStep`, CCs,
  Normal-Mode bars) remain the un-modulated center.

### "Slow movement" modulator
- Smooth raised-cosine swing between window edges (no corners):
  `u = (1 - cos(pi * phase)) / 2`, alternating rising/falling halves.
- Each half picks its own random duration 2.5-10 s, so every full cycle
  (bottom->top->bottom) is 5-20 s and asymmetric.
- 8 independent modulators, each with its own PRNG state (xorshift32,
  distinct seeds) and a randomized start phase/direction so pipes are
  desynchronized from boot.
- Free-running regardless of mode or amount (changing the amount never
  restarts it).

### Defaults / scope
- All modulation amounts boot at 0 (sound identical to today until used).
- Amended 2026-10-14 (preset_management_plan.md): the amounts are stored
  in the SD-card presets as `vol_mod[4]` / `char_mod[4]`; they still boot
  at 0 and are applied only when a preset slot is loaded. No new MIDI CCs.
- In Modulation Mode only pipe encoder turn/push behavior changes.
  SHIFT+pipe-encoder press (mute), volume encoder ENC_6 pages, transport
  wheel, keys, octave, latch and all LED indicators other than the pipe knob
  LEDs and bars behave as in Normal Mode.
- Flipping the switch mid-bar: the bar for the old mode is cancelled; knob
  LEDs immediately show the active mode's pages.

## Implementation

### 1. Modulator DSP (`code/src/DriftModulator.h`, new, header-only)
- `DriftModulator`: `Init(sample_rate, seed)`, `Advance(num_samples)`
  (block-rate, once per audio block), `Value()` -> u in [0,1].
- Per-half duration draw 2.5-10 s, raised-cosine shape, one `cosf` per
  modulator per block (8 x 2 kHz -- negligible), no heap.
- Static helper `ApplyWindow(center, amount01, u)` implementing the sliding
  +-20% window (pure function, unit-testable).

### 2. Engine integration (`code/src/OrganEngine.h`, `TonewheelCharacter.h`)
- Add `kMaxModSteps = 32`, per-pipe `vol_mod_step_[]`, `char_mod_step_[]`,
  8 `DriftModulator`s; `Set/GetPipeVolModStep`, `Set/GetPipeCharModStep`
  (clamped).
- `Prepare()` (per block): advance modulators; compute modulated volume
  target `muted ? 0 : x_mod^2` into `pipe_gain_target_` (existing ~1 ms
  per-sample slew smooths block steps); compute modulated Character target.
- Refactor `SetPipeStep`/`SetPipeMuted` so the gain target is derived in one
  place (center + modulation + mute) without changing amount-0 behavior.
- `TonewheelCharacter`: add `SetModulatedTarget(pipe, float 0..1)` that sets
  `target_` only (keeps `steps_` as the user center); existing ~10 ms linear
  ramp smooths it. `SetStep` behavior unchanged.

### 3. UI (`code/src/chompi_main.cpp`)
- `mod_mode_` (volatile) sampled in `PollControls` from the toggle.
- `mod_page_[pipe]` (0 = volume mod, 1 = Character mod).
- Pipe push / turn / SHIFT+turn branches for Modulation Mode; Normal Mode
  branches unchanged.
- New bar kinds `kBarModVolume` (white) and `kBarModCharacter` (blue) via
  dedicated `bar_page_` sub-states; knob LED rendering per mode.
- Mode-change edge: cancel stale bar.

### 4. Tests (`code/tests/`)
- New `drift_modulator_test.cpp` + Makefile target/run/clean entries:
  - Window math: the four examples above at 100%, 50% amount scaling,
    amount 0 returns center exactly, output always within [0,1] and
    width == 2d.
  - Half durations within 2.5-10 s, full cycles within 5-20 s, durations
    vary across cycles, rising/falling halves differ.
  - Continuity: bounded per-block delta, u in [0,1], reaches both edges.
  - Independence: 8 seeded modulators are not in lockstep.
  - Determinism for a fixed seed; both 48 kHz and 32 kHz.
- Extend `tonewheel_character_test.cpp` for `SetModulatedTarget`
  (ramp-follow, `GetStep` unchanged, center restore exact).

### 5. Docs
- `README.md`: rename current controls as Normal Mode, add Modulation Mode
  section (switch, pages, LEDs, SHIFT+turn, depth/window, timing, defaults).
- `plans/chompi-organ-specification.md`: new FR (Modulation Mode UI), new
  DSP section (drift modulator), defaults and acceptance criteria.
- `todo.md`: mark the LFO volume/Character items as covered (leave the
  distortion-destination idea open).

### 6. Validation
- Run the new test, then the full native organ test suite.
- Load the `build-firmware` skill and build the organ ARM firmware with its
  pinned toolchain.
- Inspect diff; remove temporary artifacts.
- Report hardware checks (switch polarity on the device, encoder/LED feel,
  listening, CPU) as pending unless verified on hardware.

## Notes
- The separate, not-yet-implemented `distortion_tone_control_plan.md` also
  edits `chompi_main.cpp` / `OrganEngine.h`; changes are independent (volume
  encoder vs pipe encoders) but whichever lands second needs a rebase-style
  merge of those files.
- No changes to TAPE/WAVE/TEMPO/bootloader/shared libs.

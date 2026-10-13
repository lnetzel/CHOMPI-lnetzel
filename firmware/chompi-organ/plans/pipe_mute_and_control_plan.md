# Plan: Pipe mute (SHIFT+press) + global all-pipe set (SHIFT+turn)

Date: 2026-10-13

## Overview

Two new pipe-control gestures on the 4 pipe encoders (ENC_1..4):

1. **SHIFT + press** encoder 1-4 toggles MUTE/UNMUTE for that pipe. While
   muted, the knob LED above the encoder (PTH 1-4) goes solid red.
2. **SHIFT + turn** encoder 1-4 changes the value for ALL 4 pipes at once,
   per the turned pipe's current page:
   - pg-1: level of all 4 pipes set to the same value
   - pg-2: Character of all 4 pipes set to the same value
   - pg-3: attack of all 4 pipes set to the same value
   - pg-4: release of all 4 pipes set to the same value

## Design

- Mute is folded into `pipe_gain_target_` (`muted ? 0 : StepToGain(step)`),
  reusing the existing ~1 ms `fonepole` slew in `OrganEngine::Process()`
  (OrganEngine.h L441) → click-free mute/unmute, level steps preserved
  while muted and restored on unmute.
- Global turn is an ABSOLUTE broadcast: the turned pipe's new clamped
  value is written to all 4 pipes ("set ... to the same"), not a relative
  delta.
- Mute state is session-only and boots unmuted (SD persistence was removed
  2026-10-11). No MIDI CC for mute (consistent with the attack/release
  no-CC decision); mute does not respond to CC.
- SHIFT+press fully consumes the press: the page does NOT cycle while
  SHIFT is held.
- The page used for a global set is the TURNED pipe's own page; other
  pipes' pages are untouched.
- Muted pipes keep running their oscillators at gain 0 (no CPU trade-off
  needed; SRAM_EXEC headroom is ample at ~50%).

## Steps

### Phase 1 — Engine (`code/src/OrganEngine.h`)

1. Add private member `bool pipe_muted_[kNumPipes]` (member block ~L655,
   near `pipe_gain_target_`); init all false in the `Init()` pipe-param
   loop (L128-L137).
2. Add `SetPipeMuted(int pipe, bool muted)` (bounds-checked like the other
   setters): sets the flag and recomputes
   `pipe_gain_target_[pipe] = muted ? 0.f : StepToGain(pipe_steps[pipe])`.
   Add `GetPipeMuted(int pipe) const` and `TogglePipeMuted(int pipe)`.
3. Modify `SetPipeStep` (L162-L170) so the target respects mute:
   `pipe_gain_target_[pipe] = pipe_muted_[pipe] ? 0.f : StepToGain(step)`.
   This keeps level steps editable while muted and preserves mute across
   MIDI CC 70-73 level changes (MidiHandler.h L85 path needs no change).
   No `Process()` change needed — the existing gain smoothing handles it.

### Phase 2 — UI input (`code/src/chompi_main.cpp`, PollControls)

Depends on Phase 1.

4. Press block (L318-L335): inside the rising-edge branch, if
   `shift_held_` → `engine.TogglePipeMuted(p)` and skip the page cycle +
   bar re-arm (mute is a state, not a value; the persistent red knob LED
   is the feedback). Else the existing page-cycle behavior is unchanged.
5. Pipe turn branch (L552-L582): wrap each `pipe_page_[p]` case with
   `if(shift_held_)` → broadcast: apply `delta` to the turned pipe via the
   existing setter, read back the clamped value, then write that same
   value to all 4 pipes.
   - pg-1 level: `SetPipeStep` all 4; set `cc_pipe_step_[q]` +
     `cc_pipe_dirty_[q]` for ALL pipes (coalesced CC out in MainLoop).
   - pg-2 Character: `SetPipeCharacterStep` all 4;
     `cc_character_step_[q]` / `cc_character_dirty_[q]` for all.
   - pg-3 attack / pg-4 release: `SetPipeAttackStep` /
     `SetPipeReleaseStep` all 4 (no CC, per existing decision).
   - Unshifted path byte-identical to today. Bar display unchanged
     (`bar_pipe_` = turned pipe, `bar_page_` = its page) — the value shown
     represents all 4 pipes.

### Phase 3 — LED (`code/src/chompi_main.cpp`, UpdateLeds)

Depends on Phase 1; parallel with Phase 2.

6. Pipe knob LED loop (L766-L804): check `engine.GetPipeMuted(p)` first;
   if muted → `SetPthLedFloat(p + 1, 1.f, 0.f, 0.f)` solid red, overriding
   the page color/brightness. Unmuted → existing switch unchanged.

### Phase 4 — Docs

Parallel with Phases 2/3/6.

7. Spec `plans/chompi-organ-specification.md`: extend FR-2b (SHIFT+press =
   mute toggle; SHIFT+turn = broadcast to all pipes per page), UI-1 LED
   bullet (muted knob LED solid red, priority over page color), defaults
   table row (pipe mute: unmuted at boot, session-only), no-CC note for
   mute.
8. README `README.md`: controls section — add the two SHIFT gestures to
   the pipe-encoder description; note red LED = muted.

### Phase 5 — Build & verify

Depends on all above.

9. Build via MSYS2 recipe:
   `C:\msys64\usr\bin\bash.exe -lc 'export PATH="/c/armgnu/10.3-2021.10/bin:$PATH"; cd /c/Dev/CHOMPI-lnetzel/firmware/chompi-organ/code/src && make -j4'`
   Expect EXIT=0, no warnings, SRAM_EXEC ~50.4% (+a few hundred bytes at
   most vs the 119,616 B envelope-pages baseline).
10. Hardware verification checklist (on device):
    - SHIFT+press each pipe encoder → pipe silences, knob LED red; press
      again → level returns smoothly to prior step (no click/pop), LED
      back to page color.
    - 4 pipes at different levels, SHIFT+turn one encoder on pg-1 → all 4
      jump to the same level (bar shows it); same for Character (pg-2,
      blue), attack (pg-3, purple), release (pg-4, yellow); all 4 knob
      LEDs reflect the equal values.
    - Unshifted press still cycles pages; unshifted turn still per-pipe;
      octave shift (SHIFT+KEY_16/17), latch, wheel press, ENC_6 SHIFT
      behaviors (fine-tune / drive type / decay) all unaffected.
    - MIDI CC 70-73 in on a muted pipe changes the step but stays silent
      until unmuted; global level/Character set emits CCs for all 4 pipes.
    - Mute a pipe mid-note and with latch on → click-free; no CPU load
      change.

## Relevant files

- `code/src/OrganEngine.h` — add `pipe_muted_`,
  `SetPipeMuted`/`GetPipeMuted`/`TogglePipeMuted`; touch `SetPipeStep`
  (L162), `Init` pipe loop (L128), member block (L655). Gain smoothing at
  L441 is reused.
- `code/src/chompi_main.cpp` — press block (L318-L335), pipe turn branch
  (L552-L582), knob LED loop (L766-L804). `shift_held_` already sampled at
  L312.
- `plans/chompi-organ-specification.md` — FR-2b, UI-1, defaults table.
- `README.md` — controls section.

## Decisions

- Global-turn semantics: ABSOLUTE broadcast of the turned pipe's clamped
  value, matching "set the volume on all 4 pipes to the same".
- Mute click-free via `pipe_gain_target_` fold, NOT by zeroing `pipe_env`
  or the per-sample mix.
- Mute session-only, boots unmuted; no MIDI CC in/out for mute.
- Muted LED = solid red at full brightness, overriding page color;
  white-key value bars unchanged when turning a muted pipe's encoder.
- No new native host test file: the mute logic lives in OrganEngine
  (DaisySP-dependent, not host-testable like the header-only modules).
  Existing tests must still syntax-check clean.

## Further considerations

1. Mute state per pipe is the natural foundation for a future "solo"
   gesture (solo = mute the others). Out of scope here.

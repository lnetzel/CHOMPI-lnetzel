# Plan: Modulation-mode UI changes

## Problem and approach

Make the organ UI more direct by replacing Modulation-mode encoder pages
with a shared destination selected using SHIFT + a physical black key.
Move attack and release editing out of Normal mode, and restrict preset
management and octave selection to Normal mode.

Reuse the existing engine parameters and setters. No DSP changes, preset
schema changes, or distortion modulation are included.

## Confirmed behavior

### Destination selection (switch UP)

Black keys are numbered physically from left to right.

| Black key | Color | Encoders 1-4 adjust |
| --- | --- | --- |
| 1 - C#3 | Purple | Actual pipe attack time |
| 2 - D#3 | Yellow | Actual pipe release time |
| 3 - F#3 | White | Volume modulation amount |
| 4 - G#3 | Blue | Character modulation amount |
| 5 onward | Off | No destination; SHIFT presses do nothing |

- SHIFT + a destination key selects that destination globally for all four
  pipe encoders.
- Volume modulation is selected at boot. Remember the selected destination
  across mode changes until reboot; do not store it in presets.
- Unselected destination keys use 75% of their configured RGB intensity;
  the selected key uses 100%.
- Destination LEDs are always visible in Modulation mode, including when
  SHIFT is not held, during value bars, and during the distortion-tone
  overlay.
- All remaining black-key LEDs stay off in Modulation mode, even when
  those keys play notes.
- Encoder LEDs use the selected destination color and retain existing
  amount-dependent brightness with a 20% floor.
- Muted pipes retain the solid-red encoder LED override.

### Encoder gestures

- In Modulation mode, unshifted pipe-encoder presses do nothing.
- SHIFT + pipe-encoder press still toggles that pipe's mute.
- A turn adjusts the selected parameter for that pipe.
- SHIFT + turn retains the existing absolute-broadcast behavior: apply
  the delta to the turned pipe, read its clamped result, and set all four
  pipes to that value.
- Attack and release edit actual envelope times, not modulation depths.
  Preserve the existing 32-step logarithmic 5 ms to 10 s mapping.
- Preserve the existing volume and Character modulation ranges and
  asymmetric, smooth, free-running behavior.
- Normal-mode pipe pages become Volume (page 1) -> Character (page 2) ->
  Volume. Page selection remains independent per pipe and is preserved
  across mode changes.

### Keyboard and preset restrictions

- Preset load, save, copy, delete, and confirmation are unavailable while
  the switch is UP.
- Entering Modulation mode cancels unconfirmed preset selections.
- Previously confirmed storage operations finish in the background, but
  their preset animations must not take over the Modulation-mode UI.
- Octave selection is unavailable while UP. Retain the current octave.
- Without SHIFT, all 25 keys still play notes.
- With SHIFT in Modulation mode, white keys still play notes; the first
  four black keys select destinations, and all other black keys are inert.
- Note releases and existing latch behavior must continue correctly
  across SHIFT, destination, and mode changes.

### Unchanged behavior and display priority

- Wheel, ENC_6, MIDI, latch, distortion, and the audio engine remain
  unchanged except for the explicitly agreed UI restrictions.
- Existing modulation continues running regardless of switch position.
- Keep the existing continuous distortion-tone display while SHIFT is
  held on ENC_6's distortion page, including in Modulation mode.
- This tone overlay takes priority over pipe value bars while active.
  Destination LEDs and encoder colors remain visible.
- No distortion modulation is implemented. Black key 5 is reserved and
  dark, like all later black keys.
- The separate [init-patch plan](init_patch.md) is outside this scope.

## Implementation todos

### 1. Replace modulation pages with a shared destination

Update [chompi_main.cpp](../code/src/chompi_main.cpp):

- Replace per-pipe modulation-page state with one four-choice destination.
- Map physical black keys using the existing hardware key/LED mappings.
- Route turns to the existing attack, release, volume-modulation, and
  Character-modulation setters.
- Remove attack/release from Normal-mode page cycling and turn handling.
- Preserve each pipe's Normal-mode page when switching modes.
- Cancel stale value bars when switching modes or destinations.
- Disable unshifted pipe-encoder press behavior in Modulation mode while
  preserving SHIFT + press mute.

### 2. Make preset and octave gestures Normal-mode-only

Depends on todo 1.

Update control polling in [chompi_main.cpp](../code/src/chompi_main.cpp):

- Sample and process the mode switch before preset confirmation so that a
  fresh SHIFT press cannot confirm an operation on the same polling pass
  that enters Modulation mode.
- Gate all preset gestures and confirmation while UP.
- Cancel unconfirmed selections on entry without undoing already
  confirmed cache changes or interrupting background storage writes.
- Prevent preset animations from consuming Modulation-mode keys or LEDs.
- Route SHIFT + black-key selection before octave or musical note-on
  handling; do not change octave while UP.
- Keep white keys playable with SHIFT while UP, regardless of SD-card
  availability.
- Preserve note-release and latch handling for notes started before a
  mode or gesture transition.

### 3. Update LED rendering and feedback

Depends on todo 1.

Update [chompi_main.cpp](../code/src/chompi_main.cpp):

- Separate white-key feedback from persistent black-key destination LEDs.
- Render destination RGB values at precisely 75% or 100% intensity.
- Apply the Modulation-mode black-key display after keyboard overlays so
  bars, tone display, notes, octave hints, and preset operation indicators
  cannot overwrite it.
- Use the selected destination color on all four pipe encoder LEDs,
  retaining amount-dependent brightness and the solid-red mute override.
- Show matching white-key value bars for attack, release, volume
  modulation, and Character modulation on encoder turns.
- Keep the existing continuous distortion-tone overlay priority.
- Keep all Normal-mode LED behavior except removed attack/release pages.

### 4. Preserve presets and existing sound

Keep [OrganPreset.h](../code/src/OrganPreset.h) and
[OrganEngine.h](../code/src/OrganEngine.h) behavior unchanged:

- No preset schema/version change or migration.
- Existing preset attack, release, mute, and modulation values load
  exactly as before.
- Keep capture/apply and JSON field meanings unchanged.
- Keep UI destination and Normal-mode page selections outside presets.
- Avoid DSP changes or modulation restarts on mode/destination changes.

### 5. Add regression coverage, documentation, and verification

Final verification depends on todos 1-4.

- Update the [organ README](../README.md) to describe the two Normal-mode
  pages, four destinations, gestures, LED behavior, mode restrictions, and
  tone-overlay priority.
- Add focused host-test coverage for the UI rules. Use a small
  hardware-independent helper only where needed to exercise the actual
  production logic; avoid duplicating firmware logic in test-only models.
- Cover physical destination mapping, boot default, remembered selection,
  two-page Normal-mode cycling, encoder press behavior, SHIFT broadcast,
  and clamping.
- Cover blocked preset/octave gestures, cancellation-before-confirmation
  ordering, and no-SD behavior.
- Verify exact destination LED intensity ratios and overlay priorities.
- Run existing preset, envelope, and drift regression tests, including
  loading representative existing presets with nondefault envelope and
  modulation values.
- Build the organ firmware using the repository's pinned build workflow.

## Hardware acceptance checks

- All four destinations select correctly and persist across switch
  changes; reboot selects Volume modulation.
- Unselected keys are 75%, selected key is 100%, and unused black keys
  remain dark with and without SHIFT, during notes and all value bars.
- Encoder colors match selection, retain their brightness floor, and
  muted pipes remain red.
- Unshifted encoder presses do nothing while UP; SHIFT + press still
  mutes; SHIFT + turn broadcasts the turned pipe's resulting value.
- Normal mode cycles only Volume and Character, independently per pipe.
- Preset management and octave changes work only while DOWN.
- Switching UP during an unconfirmed preset operation cancels it, even
  when SHIFT is freshly pressed on that same transition.
- Already-confirmed writes finish without taking over Modulation-mode
  keys or LEDs.
- White keys play with SHIFT while UP; black selector keys do not trigger
  notes with SHIFT; all 25 keys remain playable without SHIFT.
- Held, released, latched, and MIDI notes behave correctly across mode
  and destination changes, without stuck notes.
- No-SD operation still allows destination selection and performance.
- Existing presets restore the same sound and parameter values.
- On ENC_6's distortion page, SHIFT continuously shows the tone bar while
  destination LEDs remain visible, as explicitly agreed.

## Status

Design choices are confirmed. This document is an implementation plan;
firmware changes have not been made as part of saving it.

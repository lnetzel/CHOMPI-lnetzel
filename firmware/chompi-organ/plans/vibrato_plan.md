# CHOMPI ORGAN Vibrato Plan

Status: planned, not implemented.

Add global pitch vibrato to CHOMPI ORGAN. Reuse the Transport Wheel to select
rotary or vibrato depth. Keep rotary behavior independent and add no new control
or MIDI assignment.

## Confirmed Decisions

- Pressing the Transport Wheel toggles the selected control between rotary and
  vibrato. Boot with rotary selected.
- The left LED indicates rotary selection; the right LED indicates vibrato
  selection. Use purple for rotary and pink for vibrato. Keep the selected-mode
  LED lit until the next toggle; leave the other LED off.
- Wheel rotation changes only the selected level. Preserve rotary's existing
  control resolution and behavior.
- Vibrato uses a fixed 6 Hz rate. Its depth has 51 integer values, 0 through 50,
  with one-cent increments. Map value directly to pitch deviation in cents.
- At depth 50, pitch moves by +/-50 cents around the note: one semitone
  peak-to-peak. Depth 0 is exact no-vibrato behavior.
- Start vibrato depth at 0 on every boot. Do not add nonvolatile persistence.
- Show the selected level on the existing 15-white-key bar, using purple for
  rotary and pink for vibrato. Keep the existing timed-bar behavior and restore
  held-note LEDs after its timeout.
- Vibrato is global pitch modulation across all organ voices, separate from
  rotary-speaker Doppler. Use one shared, free-running LFO.
- Keep distortion-selector feedback at higher keybar priority.

## Verified Code Anchors

- `PollControls` maps `enc[4]` rotation to `OrganEngine::SetRotaryStep`; the
  current rotary range is 0-32. The encoder's debounced press edge is available
  but has no rotary/vibrato selection action.
- `Hardware::Init` wires the wheel switch through `seed::D10`. `ChompiEncoder`
  exposes `RisingEdge()` and `FallingEdge()`.
- `UpdateLeds` owns the timed keybar and held-note restoration. The keybar uses
  the 15 white-key LEDs and the rotary level currently renders purple.
- Distortion selector feedback currently has higher priority than the ordinary
  value bar. Preserve that priority.
- PTH LED indices 5 and 6 are candidates for the physical pair above the wheel,
  but their physical mapping is not confirmed in firmware. Verify the Rev4
  hardware mapping before assigning them.
- `OrganEngine::Process` renders six voices with four sine oscillators per
  voice. `UpdateFreqs` sets oscillator frequency on note start and tuning
  changes; there is no existing per-sample pitch modulation.
- Audio order is voice mix -> distortion -> rotary -> plate reverb -> master
  gain -> headphone and master output pairs. Rotary already produces localized
  horn Doppler modulation; vibrato must remain a separate all-voice effect.
- ORGAN runs at 48 kHz with 24-sample blocks, giving the audio callback a
  0.5 ms deadline.
- Native assert-based DSP tests and a test Makefile exist under `code/tests`.
  Nearby patterns are in `add_tonewheel_character.md` and
  `distortion_selector_plan.md`.

## Implementation Steps

### Phase 1: Vibrato DSP and Focused Tests

1. Add a small ORGAN-owned, host-testable vibrato helper or equivalent
   `OrganEngine` state. Generate a phase-continuous, shared 6 Hz LFO. Convert
   depth 0-50 to 0-50 cents peak deviation and apply the resulting pitch change
   to every voice without resetting oscillator phase or accumulating frequency
   drift. Keep depth zero exactly dry. Keep per-sample work bounded and avoid
   unnecessary transcendental calls in the oscillator loop.

2. Add engine setters/getters for vibrato depth. Initialize depth to zero on
   boot. Preserve the current rotary range, rotary default, tuning, and existing
   effect routing. Keep the vibrato stage separate from the rotary processor.

3. Add focused native tests using the existing `code/tests` conventions. Cover
   depth clamping and mapping, the 6 Hz rate, +/-50-cent endpoint, phase
   continuity, stable base tuning, exact zero-depth behavior, and finite output.
   Run this focused test before starting UI integration. Measure or inspect DSP
   cost against the 48 kHz callback budget.

### Phase 2: Wheel UI, LEDs, and Keybar

4. Update `PollControls` so a debounced wheel-press edge toggles selected mode.
   Start in rotary mode. Wheel rotation updates only rotary or vibrato according
   to the active selection, clamped to each control's range. Follow the existing
   ISR-to-main-loop state conventions.

5. Update `UpdateLeds` to render left/right mode indicators and the selected
   control's keybar. Use purple for rotary and pink for vibrato. Map 0-50
   vibrato depth across the existing 15 white-key LEDs; do not imply that the
   hardware has 50 display segments. Preserve bar timeout, held-note restore,
   distortion-selector priority, and other LED behavior.

6. Confirm the two physical wheel LED indices against Rev4 PCB/enclosure
   documentation before assigning firmware indices. Do not assume PTH 5/6
   without that check.

### Phase 3: Documentation and Acceptance

7. Document wheel toggling, LED colors, boot selection, depth range, 6 Hz rate,
   pitch excursion, and boot depth in the existing ORGAN documentation. Avoid
   changing unrelated rotary defaults or controls.

8. Run focused native tests, the existing ORGAN native test suite, and a clean
   ARM release build. On hardware, verify mode toggling, independent parameter
   adjustment, LED/keybar feedback, timeout restoration, zero and maximum depth,
   held/new notes, tuning, distortion, and rotary independence. Measure worst-
   case callback time with six voices and all four pipes active. Clearly report
   hardware checks that were not run.

## Relevant Files

Paths below are relative to the ORGAN directory.

| File | Planned Role |
| --- | --- |
| `code/src/chompi_main.cpp` | Wheel mode, rotation handling, keybar, and LED feedback. |
| `code/src/OrganEngine.h` | Vibrato depth API and pitch-modulation integration. |
| Proposed: `code/src/Vibrato.h` | Small testable LFO/depth helper, if separate ownership is useful. |
| `code/src/temp_led_stuff.h` | Existing PTH LED buffer and setters; reference for indicator rendering. |
| `code/src/hardware.h` | Wheel and LED hardware mappings; verify physical indicator indices. |
| `code/tests/Makefile` | Add focused vibrato test target using existing conventions. |
| `code/tests/` | Native DSP test source and existing regression suite. |
| `README.md` and organ specification, if present | User-facing controls and audio behavior. |
| `code/src/RotarySpeaker.h` | Reference only; keep rotary Doppler separate. |

## Verification

1. Native DSP tests: depth 0 and 50 endpoints, intermediate mapping, clamp
   behavior, 6 Hz modulation, phase continuity, base-frequency preservation,
   finite output, and exact zero-depth bypass.
2. Firmware: run existing native tests and a clean ARM release build. Check
   callback cost against 0.5 ms at 48 kHz with 24-sample blocks.
3. Hardware UI: boot selects rotary; each wheel press toggles selection; left
   and right LEDs show the correct steady mode/color; rotation changes only the
   selected control; keybar colors and levels match the active control; bar
   timeout restores held-note LEDs; distortion selector keeps priority.
4. Audio: vibrato depth 0 leaves pitch unchanged; mid and maximum values sound
   correct on held and newly played notes; maximum excursion is one semitone
   peak-to-peak; tuning and rotary remain independent; no clicks or dropouts.

## Scope Boundaries

Included: independent global pitch vibrato, shared fixed-rate LFO, wheel mode
selection, two mode LEDs, colored keybar feedback, focused tests, and usage
documentation.

Excluded: changes to rotary range/default, rotary algorithm, new MIDI controls,
nonvolatile persistence, distortion behavior, bootloader changes, and unrelated
bug fixes.

## Risks and Notes

- Apply pitch modulation without compounding the current oscillator frequency
  on each update; retain a stable base frequency for tuning and note changes.
- The wheel is polled in the audio callback while keybar LEDs update in the main
  loop. Keep shared mode/value state consistent with existing cross-context
  synchronization.
- Confirm physical LED indexing before implementation; firmware does not yet
  name the pair above the wheel.
- `OrganEngine::kDefaultRotaryStep` is currently 10, while the organ
  specification says rotary should boot at 0. This pre-existing discrepancy is
  out of scope; preserve current rotary behavior unless separately approved.
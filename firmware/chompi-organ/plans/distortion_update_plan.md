## Plan: ORGAN Distortion Character Update

**Status:** superseded 2026-10-13 by distortion_update_2_plan.md — the Warm type introduced here was subsequently replaced by an exact port of the CHOMPI-TAPE tape saturation (slot 0 unchanged), and BrokenSpeaker gained a 200 Hz high-pass plus the stereo Dropout stage (DSP-8).

Replace the original overdrive with three selectable characters in `firmware/chompi-organ`: position 1 Warm vintage soft clipping, position 2 Fuzz, position 3 a heavily destroyed speaker sound. Keep the existing drive amount, selector controls, LED groups, routing, and transition behavior. The broken-speaker recommendation is the user-selected narrow speaker-like bandpass with harsh breakup clipping and resonant coloration.

**Steps**
1. Update `Distortion` type order and default: `kWarm = 0`, `kFuzz = 1`, `kBrokenSpeaker = 2`; retain three-way 10 ms crossfade and no-wrap clamping. Boot to position 1 (Warm), consistent with current first-entry default; drive amount still boots at 0.
2. Remove the old Existing/tanh branch and its dedicated gain/post-gain work. Preserve current Warm and Fuzz algorithms as positions 1 and 2. Add a position-3 DSP branch combining aggressive nonlinear breakup with sample-rate-derived high-pass/low-pass speaker band limits and resonant coloration. Keep state priming, silent settling, and exact zero-drive bypass behavior; calculate coefficients in initialization, not the sample loop. Do not add noise, bit reduction, oversampling, or unrelated routing changes.
3. Update `OrganEngine.h` and `chompi_main.cpp` descriptions/default references. Keep the existing control path and LED mapping: selector positions 0, 1, 2 still light white-key groups 1-5, 6-10, and 11-15. Leave encoder behavior, amount retention, page controls, and the rest of the UI unchanged.
4. Revise `code/tests/distortion_test.cpp`: remove the Existing formula regression, then assert Warm/Fuzz/BrokenSpeaker order and boot default; test clamping, amount retention, zero bypass, silence settling, output bounds, normalized crossfade weights and retargeting, plus measurable broken-speaker band limiting/resonant character. Keep Warm asymmetric and Fuzz symmetric characterization checks.
5. Update ORGAN-only docs to name the three types and new default, document position-3 character, and remove stale Existing references: `README.md`, `plans/chompi-organ-specification.md` (FR-4b and DSP-5), and `plans/distortion_selector_plan.md`. Do not edit files outside `firmware/chompi-organ`.
6. Run focused native tests first, then the documented ARM build from `code/src`. Review remaining type-name references under `firmware/chompi-organ`; report hardware listening and CPU timing as pending unless run on device.

**Relevant files**
- `firmware/chompi-organ/code/src/Distortion.h` — enum, algorithms, filter state, crossfades, bypass.
- `firmware/chompi-organ/code/src/OrganEngine.h` — boot type and processor descriptions; keep amount and chain ownership intact.
- `firmware/chompi-organ/code/src/chompi_main.cpp` — selector labels/comments and position-to-LED overlay.
- `firmware/chompi-organ/code/tests/distortion_test.cpp` — replace removed-branch regression and cover new character.
- `firmware/chompi-organ/code/tests/Makefile` — existing `test` target runs distortion tests; change only if new test needs no build-system changes.
- `firmware/chompi-organ/README.md` — user-facing selector names/order.
- `firmware/chompi-organ/plans/chompi-organ-specification.md` — normative selector, default, and DSP behavior.
- `firmware/chompi-organ/plans/distortion_selector_plan.md` — replace now-stale implementation record.

**Verification**
1. Run `make -C firmware/chompi-organ/code/tests test` after DSP and tests change.
2. Confirm new branch remains bounded, reaches silence after signal stops, gives exact dry output at settled zero, and has distinct frequency response under representative inputs.
3. Run `make DEBUG=0 OPT=-O3` from `firmware/chompi-organ/code/src`; confirm clean ARM build and linker fit.
4. On hardware, compare Warm/Fuzz/BrokenSpeaker across low/mid/high notes, chords, and drive levels; check no clicks, excessive DC, or callback deadline misses. Treat listening and timing as pending if hardware is unavailable.

**Decisions**
- Scope is only `firmware/chompi-organ`.
- Positions mean selectable order, with zero-based enum values 0-2.
- Position 1 replaces the current boot default: Warm boots selected; amount still starts at zero. User confirmed.
- Position 3 means narrow speaker-like bandpass plus harsh breakup and resonant coloration. User selected this voicing.
- Keep controls, LED group locations, no-wrap behavior, crossfade timing, amount, routing, persistence, and MIDI behavior unchanged.

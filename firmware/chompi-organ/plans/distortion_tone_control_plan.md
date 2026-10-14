# CHOMPI Organ: fuzz-only distortion with tone control

## Problem and approach

Volume encoder page 2 currently controls a 32-step distortion amount, with
SHIFT+turn selecting Tape Saturation, Fuzz or BrokenSpeaker. Keep only the
existing Fuzz algorithm and replace type selection with a continuously
darkening tone control. Remove the now-unneeded BrokenSpeaker stereo
dropout/crackle/filter-skew stage.

The existing Fuzz already includes a ~10 kHz one-pole low-pass. Make its
cutoff adjustable rather than adding another filter. At 100%, preserve
that original coefficient, clipping curve, gain mapping and normalization,
so there is no additional filtering and the settled sound matches today's
Fuzz exactly. "Bypass tone filtering at 100%" means bypassing additional
darkening, not removing Fuzz's original low-pass.

## Confirmed behavior

- Distortion is always Fuzz; no type selector or three-group key display.
- Unshifted volume page 2 remains the existing 32-step amount control.
  Amount defaults to zero and settled zero remains bit-exact dry at every
  tone setting.
- SHIFT+turn on page 2 changes tone by 1 percentage point per accepted
  encoder click, clamped to 0-100%, with no wrap or acceleration.
- Tone defaults to 100% on every boot and is session-only, with no new
  MIDI CC or SD persistence.
- Cutoff follows `500 * pow(10000 / 500, tone_percent / 100)` Hz:
  0% is 500 Hz, 100% is the original 10 kHz. Counterclockwise darkens;
  0% is not a mute.
- While SHIFT is held on page 2, show the tone immediately and continuously
  as a red bar across the 15 white keys. Rounded percentage mapping: none
  lit at 0%, eight at 50%, all fifteen at 100%. Other keyboard LEDs are off;
  notes, octave gestures, latch and dedicated LEDs remain functional.
- Releasing SHIFT on page 2 restores the unchanged amount bar for 500 ms.
  Leaving page 2 must show the newly selected page, not a stale amount bar.
- The page-2 knob LED continues to reflect drive amount, including while
  the tone bar is shown.
- Page-1 master volume/fine tuning, page-3 reverb mix/decay and all pipe,
  wheel, MIDI and note controls remain unchanged.

## Todos

### 1. Simplifying distortion DSP and adding tone

Update `firmware/chompi-organ/code/src/Distortion.h`:

- Remove Tape Saturation, BrokenSpeaker, type enums/accessors, type weights,
  crossfades and type-specific diagnostics.
- Retain Fuzz's gain, symmetric clipper, reference normalization and
  target-driven wet/dry ramp, including exact dry bypass at settled zero.
- Add clamped tone-percent setter/getter with a boot default of 100%.
- Compute log-mapped target filter coefficients outside the sample loop.
  Smooth coefficient changes with a finite sample-rate-derived ~10 ms ramp,
  retaining filter history and snapping to the exact endpoint. Retarget
  from the live coefficient during rapid turns. No heap allocation or
  per-sample transcendental work.
- Preserve the original 100% coefficient expression and initial state.

### 2. Wiring engine controls and removing BrokenSpeaker processing

Update `firmware/chompi-organ/code/src/OrganEngine.h`:

- Replace drive-type APIs with drive-tone-percent APIs using the DSP's
  stored target as the source of truth.
- Remove the type initialization and every Dropout include, member,
  initialization, per-block preparation and stereo processing call.
- Keep distortion -> rotary -> plate reverb -> master routing and existing
  amount behavior intact.
- Remove organ-only `code/src/Dropout.h` and `code/tests/dropout_test.cpp`,
  plus the dedicated target/dependencies/run/clean entries in
  `code/tests/Makefile`. Do not change other firmware projects.

### 3. Replacing the type selector UI with tone

Update `firmware/chompi-organ/code/src/chompi_main.cpp`:

- Route page-2 SHIFT+turn to tone percent, without touching drive amount.
- Rename selector overlay/edge state to tone state.
- Replace five-key type groups with the percentage-based red tone bar.
- Retain overlay priority, release timeout, page-change guards and
  independent knob LED behavior.

### 4. Updating focused tests and current documentation

Update `firmware/chompi-organ/code/tests/distortion_test.cpp`:

- Replace obsolete type/Tape/BrokenSpeaker tests with fuzz-only and tone
  coverage; retain applicable symmetry, harmonic, bounded-output, silence
  and wet/dry-transition checks.
- Capture an independent reference for the previous settled Fuzz DSP before
  changing it; verify sample-for-sample equality at tone 100% across all
  nonzero drive steps and representative signals at 48 kHz and 32 kHz.
  Exclude the removed type-selection fade from this comparison.
- Verify boot default, setter clamping, unchanged drive when tone changes,
  cutoff endpoints and monotonic mapping.
- Check exact dry bypass at every tone percentage, including transitions
  to zero with residual filter state and a nonzero smoothed drive value.
- Quantify darkening: at minimum tone, a steady 6 kHz sine's fundamental
  must be attenuated by at least 12 dB relative to tone 100%; verify
  monotonic high-frequency attenuation at intermediate settings and
  nonzero low-frequency output at tone 0%.
- Test finite ramp settlement, rapid mid-ramp retargeting and bounded
  sample-to-sample coefficient movement at both sample rates. Check
  finite/bounded output through tone and drive sweeps and silence settling.

Update `firmware/chompi-organ/README.md` and the active specification
`firmware/chompi-organ/plans/chompi-organ-specification.md`:

- Revise controls, defaults, signal flow, FR-4b, DSP-5, related acceptance
  criteria and references to type selection; remove obsolete DSP-8 content.
- Document tone range, default, smoothing, key display and exact bypass.
- Leave historical implementation plans untouched and avoid unrelated
  corrections to pre-existing documentation discrepancies.

### 5. Validating and reporting

- Run the focused native distortion test first, then the small existing
  native DSP regression suite, ensuring deleted Dropout targets are gone.
- Check editor diagnostics and search active organ code/documentation for
  stale type-selector and BrokenSpeaker/Dropout references.
- Load the build-firmware skill before compilation and follow its pinned
  Windows/MSYS2/toolchain workflow for the organ ARM firmware build.
- Inspect the final diff for unrelated changes and remove only temporary
  artifacts produced by this work.
- Verify UI behavior on hardware if available; otherwise explicitly report
  physical encoder/LED, listening and real-time CPU validation as pending,
  not as completed by host tests or compilation.

## Dependencies and boundaries

- DSP changes precede engine integration and the reference/regression tests.
- Engine integration precedes UI wiring; final docs/tests cover the finished
  controls. Validation follows all implementation and documentation work.
- No changes to TAPE, WAVE, TEMPO, bootloader, shared libraries or card assets.
- No oversampling, new effect types, brightness-linked knob behavior,
  gain compensation changes or extra configuration layers.
- Implementation will begin only after approval; repository is currently clean.

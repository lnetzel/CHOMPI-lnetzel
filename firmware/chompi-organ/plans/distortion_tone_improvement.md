# Distortion tone: 12 dB/octave low-pass

## Problem and agreed approach

The ORGAN fuzz currently uses a single post-clipping one-pole low-pass,
which provides only a gentle approximately 6 dB/octave roll-off. Replace
it with two identical cascaded one-pole stages for approximately
12 dB/octave roll-off, without resonance or a new filter abstraction.

Confirmed choices:
- Apply the cascade throughout the tone range, including tone 100.
  Preserving the old bit-exact tone-100 sound is explicitly out of scope.
- Keep the logarithmic 500 Hz to 10 kHz coefficient mapping, boot tone
  100, 0..100 clamping, encoder controls, LEDs and preset representation.
- Preserve exact settled zero-drive bypass and existing tone smoothing.

The frequency range is the existing nominal **per-stage** range, not a
promise that the combined filter has its -3 dB point there. Two identical
poles have a gentler knee than a second-order Butterworth filter; in the
low-frequency analog approximation their combined -3 dB point is about
0.644 times the per-stage cutoff. The existing discrete-time coefficient
mapping also deviates from the analog response near Nyquist. Documentation
and numerical tests must describe the actual cascade, not a Butterworth
response or an exact 12 dB/octave slope at every digital frequency.

## Todos

### 1. Updating the DSP cascade

File: firmware\chompi-organ\code\src\Distortion.h

- Retain the existing first-stage state and add one second-stage state.
  Initialize both to zero on every Init().
- Process the shaped sample through stage one, then process its updated
  output through stage two. Return stage two's output to the existing
  wet/dry crossfade and RMS measurement.
- Use the same live tone coefficient for both stages, ramped once per
  sample by the existing code. Preserve both histories on tone changes.
- Preserve clipping, drive mapping, normalization, coefficient endpoints,
  bypass transitions, output-leveling algorithm and public APIs.
- Leave the filter in its current position: after clipping, before rotary,
  plate reverb and master volume. No engine routing changes are required.
- Update comments describing the filter and remove historical
  tone-100 output-equality claims.
- Cost: one additional state float and one extra one-pole update per
  sample; no allocation or new per-sample transcendental work.

### 2. Updating and strengthening native tests

File: firmware\chompi-organ\code\tests\distortion_test.cpp

- Replace the old one-pole output-equality regression with an independent
  two-stage reference, clearly named and documented. Cover all nonzero
  drive steps, representative signals and 48 kHz / 32 kHz as before.
- Keep exact coefficient-endpoint and monotonic-mapping checks: the
  coefficient expressions are unchanged even though the output is not.
- Add measured frequency-response checks against the analytical digital
  cascade magnitude:
  H1 = c / sqrt(1 + (1-c)^2 - 2*(1-c)*cos(omega)); Hcascade = H1^2.
  Use settled coherent signals and normalize against the shaped input
  fundamental so clipping is not mistaken for filter attenuation.
- At tone 0, measure the 2 kHz to 4 kHz roll-off at both sample rates;
  require attenuation in a justified approximately 10..13 dB interval,
  and agreement with the analytical digital response within 0.5 dB.
  These frequencies test the stopband away from both the knee and
  Nyquist, rather than demanding exactly 12 dB at every frequency.
- Strengthen the existing 6 kHz darkening test to at least 30 dB
  attenuation at tone 0 relative to tone 100; retain monotonic intermediate
  settings and the low-frequency non-mute check.
- Verify deterministic reinitialization clears both filter histories.
- Retain checks for silence settlement, symmetry, odd harmonics, bounded
  output, coefficient ramp/retargeting, exact zero-drive bypass (including
  residual state), and output leveling. Adjust a settling window only if
  the measured additional pole requires it; do not weaken invariants.
- Remove obsolete comments claiming the old shaping/filter output is
  unchanged, while retaining tests that leveling does not alter DSP output.

### 3. Updating directly related documentation

Files:
- firmware\chompi-organ\README.md
- firmware\chompi-organ\code\src\OrganEngine.h
- firmware\chompi-organ\code\src\chompi_main.cpp

Describe two cascaded one-pole stages, approximately 12 dB/octave
stopband roll-off, the unchanged per-stage frequency mapping, and the
intentional darker tone-100 endpoint. Remove "historical Fuzz exactly"
and "no additional darkening" statements. Update the README test summary
to describe the new reference and measured response checks.

Saved tone values remain compatible without migration, but existing
presets will sound darker with drive enabled. Controls, LEDs, preset
schema and MIDI behavior remain unchanged. Leave unrelated repository
plans and other worktree changes untouched.

### 4. Validating the change

- Use the existing host Makefile to build and run only distortion_test
  first, with its existing C++14 optimization and warning flags.
- Run the existing organ_preset_test to check tone serialization/defaults
  remain compatible. Do not add tooling or dependencies unless validation
  reports a missing required tool.
- Inspect the final diff for stale historical-output claims and unrelated
  changes. Remove only test artifacts generated by this work.
- A firmware binary build or flashing is not part of this request.
  Hardware listening remains a recommended follow-up, not something
  numerical tests can certify. Listen at low/mid/high tone and drive,
  including rapid tone movement and return to zero drive.

## Dependencies

- Test updates depend on the DSP cascade.
- Documentation updates depend on the DSP cascade.
- Validation depends on DSP, tests and documentation updates.

## Acceptance criteria

- Engaged fuzz uses exactly two cascaded stages at all tone settings.
- Measured response matches the cascade and demonstrates the stronger
  stopband roll-off and specified high-frequency attenuation.
- Existing controls, coefficient mapping, initialization, smoothing,
  preset compatibility, exact dry bypass and bounded leveling remain
  covered by passing native tests.
- Documentation explicitly acknowledges the intentional tone-100 sound
  change and per-stage frequency interpretation.

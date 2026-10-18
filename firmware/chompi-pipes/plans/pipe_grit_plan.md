# Plan: CHOMPI-PIPES 8'-pipe bass grit — more energy in the low end

The 8' pipe (pipe 0) sounds weak and tame at the bottom even with the
Tonewheel Character turned up. The existing "bass grit" only multiplies
two gentle harmonic partials, so it reads as warmth, not grit. This plan
collects the options, ordered cheap→structural. **Tier 1 is implemented**
(constants calibration); the rest are kept here as future notes.

## Why the current grit is tame (checked in the code)

- The Character coloration is purely **additive harmonic shading**: max
  4.5% 2nd + 10% 3rd harmonic relative to the fundamental
  ([TonewheelCharacter.h](../code/src/TonewheelCharacter.h), `kMaxSecond`
  / `kMaxThird`). The grit boost only multiplies those two numbers by
  `(1 + kGritBoost * BassMask(folded_hz))` in `UpdateVoiceCoeffs` —
  with `kGritBoost = 2` that is ~13%/30% partials at the bottom octave
  against a dominant fundamental. Audible warmth, not grit.
- `c.norm = 1/(1+|g2|+|g3|)` is computed **from the boosted gains**, so
  boosting never adds level — it only re-balances an already-clean wave.
- The shaper is **linear**: additive partials create no intermodulation.
  Real growl (overdriven tonewheel bus, tube preamp) is nonlinear —
  sum/difference tones between simultaneous notes. No amount of additive
  gain produces that.
- The smoothstep `EasedAmount` flattens the low end of the Character
  knob, so grit arrives late in the knob travel.
- `kGritBoost = 2` was a guess flagged "calibrate on hardware by ear" —
  it was never actually tuned.

## Tier 1 — constants calibration (IMPLEMENTED 2026-10-18)

All in [TonewheelCharacter.h](../code/src/TonewheelCharacter.h),
`UpdateVoiceCoeffs`; pipes 1–3 stay bit-exact (literal shared caps and
the literal `1.f` multiplier are kept on the non-grit path):

- `kGritFullHz` 150 → 200, `kGritZeroHz` 400 → 700: the psychoacoustic
  band now covers the whole bottom octave plus lower midrange instead of
  just the lowest keys.
- `kGritBoost` 2 → 4: bottom-octave multiplier ×5.
- Pipe-0-specific harmonic caps `kGritMaxSecond` = 9% / `kGritMaxThird`
  = 20% (×2 the shared `kMaxSecond`/`kMaxThird`): the 8' pipe is no
  longer limited by the "still organ-like" caps chosen for pipes 1–3.

Bottom octave at full Character: pre-norm g2 ≈ .45, g3 ≈ 1.0 → after
norm roughly 40% fundamental / 18% 2nd / 41% 3rd — properly reedy.
Deliberately past the target: the Character knob scales it down, and
step 0 stays bit-exact clean, so overshooting is safer than
undershooting for by-ear calibration. CALIBRATE on hardware; if too
hairy, back `kGritBoost` down first.

No test asserts these constants (grep: no hits in `code/tests/`), so no
test changes are needed. The known-failing `test_harmonic_content` is a
pre-existing DFT bin-index bug, unrelated to values.

## Tier 2 — richer harmonic ladder (future)

Higher partials make bass buzzy/reedy and readable on small speakers
(missing-fundamental effect — already the cited rationale for the grit
band). The 4th and 5th harmonics are derivable from the same normalized
sine sample `s` with pure polynomials, same style as the existing terms:

- cos(4ph) = 8 s⁴ − 8 s² + 1
- sin(5ph) = 16 s⁵ − 20 s³ + 5 s

Add pipe-0-only gains to `PipeCoeffs`, extend `norm` to
1/(1+Σ|gᵢ|), and reuse the existing `AaMask` pattern per harmonic
(safe: low-note harmonics pass fully). A few multiplies per sample, no
new oscillators, still native-testable.

## Tier 3 — true nonlinear grit: saturate the 8' bus (future, the real one)

Sum pipe 0's contributions across voices into a sub-bus in the voice
loop of [OrganEngine.h](../code/src/OrganEngine.h) (`Process`, the
`sig += psig * pipe_gain_[p] * ...` accumulation) and run that sum
through a bounded soft-knee clipper (same curve family as
[Distortion.h](../code/src/Distortion.h)) before adding it to `mix`:

- Intermodulation between simultaneous bass notes → genuine growl.
- Clipper output is bounded (|out| ≤ 1), so the 1/24 mix bound
  (`kMixScale`) gets *safer* — pipe 0's 6-voice contribution collapses
  to ≤ 1.
- Drive scales with the Character knob; bypass the clipper exactly when
  the eased amount is 0 so step 0 stays bit-exact clean.
- Per-voice pre-gain by `BassMask(folded_hz)` before the sum keeps the
  effect frequency-targeted even though the bus mixes notes.

Combines well with Tier 2: feed the richer wave into the clipper.

## Tier 4 — perception/level tricks (future, optional)

- Key-click / attack transient on pipe 0, reusing the percussion-gate
  pattern (`Percussion.h` tap + per-voice `perc_gate` in
  [OrganEngine.h](../code/src/OrganEngine.h)) — note-on definition reads
  as "energy".
- Modest 8' level lift (+2–3 dB) with a matching `kMixScale` adjustment
  — probably unnecessary once the timbre is fixed.
- Dedicated 16' sub-oscillator: costs a 5th oscillator per voice; avoid.

## Relevant files

- Edit (Tier 1 done): [TonewheelCharacter.h](../code/src/TonewheelCharacter.h)
- Reference: [OrganEngine.h](../code/src/OrganEngine.h) (voice loop,
  `kMixScale`, pipe gain mapping), [Distortion.h](../code/src/Distortion.h)
  (bounded soft-knee curve to reuse for Tier 3),
  [Percussion.h](../code/src/Percussion.h) (gate pattern for Tier 4 click)

## Risks / open questions

- **Calibration pending**: Tier 1 values are deliberately hot; final
  `kGritBoost` / cap values must be set by ear on hardware.
- **Bit-exactness**: pipes 1–3 keep the literal shared caps and the
  literal `1.f` multiplier — verified bit-exact by construction; the
  pipe-0 path changes voice coeffs only where `pipe == kGritPipe`.
- **Loudness parity**: stronger harmonics change perceived loudness of
  the 8' pipe vs. the others; `norm` keeps peak amplitude bounded, but
  if the bottom now reads too *loud*, prefer reducing `kGritBoost` over
  touching pipe gain mapping.
- **Tier 3 CPU**: one extra accumulator + one shaper per sample is cheap;
  check headroom anyway if combined with Tier 2.

# Plan: CHOMPI-PIPES delay page 3 — combined mix → feedback control

Rework the volume encoder's page 3 (stereo delay) into one combined
0–100 control: the lower half of the key bar sweeps the dry/wet mix
from 0% to 50%, the mix then locks at 50% and the upper half raises the
delay feedback from today's fixed 35% to 100% ("full delay feedback" at
a full bar). Turning back down dials feedback back to 35% and, below
the 50% pivot, goes back to controlling the mix. SHIFT+turn (delay
time) is unchanged. The combined value is stored in the existing
`delay_mix` preset field — no preset format change.

Decisions confirmed with the user:
- In the upper half the mix **locks at 50%** (never goes fully wet).
- Feedback floor at the pivot is the **current 35%**, so crossing 50%
  is seamless (the delay sounds exactly like 50% mix does today).
- The combined 0–100 value is stored in the **existing `delay_mix`
  preset field**.

## What the firmware actually supports (checked in the code)

- **Page 3 of ENC_6 is the delay page**: `kVolPageDelay = 2` in
  [chompi_main.cpp](../code/src/chompi_main.cpp#L372); the push-switch
  cycles master → drive → delay → reverb at
  [chompi_main.cpp](../code/src/chompi_main.cpp#L791).
- **The page-3 turn handler** is
  [chompi_main.cpp](../code/src/chompi_main.cpp#L1241): 1% per click,
  clamped 0–100, calling
  `engine.SetDelayMixPercent(engine.GetDelayMixPercent() + delta)` and
  showing the yellow `kVolPageDelay` bar. SHIFT+turn on page 3 is the
  delay time (±5 ms, `kVolBarDelayTime`) at
  [chompi_main.cpp](../code/src/chompi_main.cpp#L1209) — untouched by
  this plan.
- **The engine setter** `OrganEngine::SetDelayMixPercent` in
  [OrganEngine.h](../code/src/OrganEngine.h#L613) clamps 0–100, stores
  `delay_mix_percent_` and calls `delay_.SetMix(percent * .01f)`.
  `GetDelayMixPercent()` is the single source for the bar, the knob LED
  and the preset capture.
- **Feedback is a compile-time constant today**: `kFeedback = .35f` in
  [StereoBbdDelay.h](../code/src/StereoBbdDelay.h#L59), used in the
  write `buf_[write_] = SoftClip(in + kFeedback * hp)`. The bounded
  cubic soft-clipper (|y| ≤ 2/3) plus the 2800 Hz darkening low-pass
  and 40 Hz DC blocker inside the loop mean **feedback 1.0 cannot run
  away** — echoes sustain, bounded and darkening (file header docs).
- **The mix already has a click-free ~10 ms ramp** with exact endpoint
  snap (`kMixRampSeconds`, `mix_/mix_target_/mix_inc_`, advanced in
  `Process()`); a feedback ramp can mirror it exactly.
- **UI needs no code change**: the delay bar renders
  `GetDelayMixPercent()` across the 15 white keys in yellow
  ([chompi_main.cpp](../code/src/chompi_main.cpp#L1727)) and the PTH 9
  knob LED tracks the same value in yellow
  ([chompi_main.cpp](../code/src/chompi_main.cpp#L2095)) — both keep
  working if the combined value stays in `GetDelayMixPercent()`.
- **Preset**: `delay_mix` is `uint8_t` 0–100 in
  [OrganPreset.h](../code/src/OrganPreset.h#L84), captured at L332 and
  applied at L364 via `SetDelayMixPercent` — reusing the field means no
  format change and old files still parse.
- **Tests**: `StereoBbdDelay.h` is header-only and native-testable;
  [stereo_bbd_delay_test.cpp](../code/tests/stereo_bbd_delay_test.cpp)
  already exists. `OrganEngine.h` is not covered by native tests (the
  test folder covers DSP headers only), so the mapping math itself is
  verified by inspection + on-device.
- **Docs**: the README documents page 3 in detail — "0% is exact dry
  and 100% is delayed signal only" and "a fixed BBD-inspired voicing
  (no feedback/tone controls)" in
  [README.md](../README.md#L238) — both statements become wrong and
  must be rewritten.

## Steps

### Phase 1 – variable feedback in the DSP (`code/src/StereoBbdDelay.h`)

1. Keep `kFeedback` as the default (rename to `kDefaultFeedback = .35f`
   for clarity) and add `kMaxFeedback = 1.f`.
2. Add state `feedback_ / feedback_target_ / feedback_inc_`, mirroring
   the mix ramp (`kMixRampSeconds`, ~10 ms, exact endpoint snap).
   `Init()` sets all three to the settled `.35f` default, so the boot
   sound is bit-exact what it is today.
3. Add `void SetFeedback(float)` (clamped 0..1, retargets the ramp) and
   `float GetFeedback() const` (returns the target, like `GetMix()`).
4. In `Process()`, advance the feedback ramp next to the mix ramp and
   replace `kFeedback` with `feedback_` in the two `SoftClip(in + f * hp)`
   writes.
5. Update the `@file` header comment: feedback is now a 0.35–1.0
   control; restate that the soft-clipper keeps the loop bounded.

### Phase 2 – combined mapping in the engine (`code/src/OrganEngine.h`)

6. Add constants next to the delay constants at
   [OrganEngine.h](../code/src/OrganEngine.h#L180):
   `kDelayMixPivotPercent = 50`, `kDelayMixAtPivot = .5f`,
   `kDelayFeedbackFloor = .35f`, `kDelayFeedbackMax = 1.f`.
7. Rework `SetDelayMixPercent(int)` (keep the name and the 0–100 range
   so [chompi_main.cpp](../code/src/chompi_main.cpp#L1241),
   `ApplyToEngine` and `CaptureFromEngine` stay untouched) to interpret
   the value as the combined control:
   - `percent ≤ 50`: `mix = percent * (kDelayMixAtPivot / 50)`;
     `feedback = kDelayFeedbackFloor`
   - `percent > 50`: `mix = kDelayMixAtPivot`;
     `feedback = kDelayFeedbackFloor + (percent - 50) * .02f *
     (kDelayFeedbackMax - kDelayFeedbackFloor)` (35% → 100%)
   Call `delay_.SetMix(...)` and `delay_.SetFeedback(...)`, store
   `delay_mix_percent_` as before, and update the doc comment.
   `GetDelayMixPercent()` is unchanged. Boot stays at 0 → exact dry,
   feedback parked at 35%.

### Phase 3 – main file (`code/src/chompi_main.cpp`, comments only)

8. Update the page-3 turn comment at L1241 to describe the combined
   mix → feedback control. The bar (L1727), knob LED (L2095), page
   cycling, SHIFT+turn time and MIDI are all untouched — no code
   changes.

### Phase 4 – native tests (`code/tests/stereo_bbd_delay_test.cpp`)

9. Add tests: `SetFeedback` clamps 0..1; boot feedback is .35; the
   ramp settles to the target; an impulse at feedback 1.0 rings far
   longer than at .35 (sustained "full feedback") yet stays bounded;
   the settled default path is unchanged (bit-exact against the old
   fixed-feedback behavior). Build and run per the tests Makefile.

### Phase 5 – docs (`firmware/chompi-pipes/README.md`)

10. Rewrite the page-3 section (L238–264): the new 0–50% = mix /
    50–100% = feedback mapping with the mix locked at 50% in the upper
    half, drop the "no feedback/tone controls" line, and note the
    preset field now stores the combined value.

## Relevant files

- Edit:
  - [StereoBbdDelay.h](../code/src/StereoBbdDelay.h): variable feedback + ramp
  - [OrganEngine.h](../code/src/OrganEngine.h): combined mapping in `SetDelayMixPercent`, new constants
  - [chompi_main.cpp](../code/src/chompi_main.cpp): comment at L1241 only
  - [stereo_bbd_delay_test.cpp](../code/tests/stereo_bbd_delay_test.cpp): new tests
  - [README.md](../README.md): page-3 docs
- Reference only:
  - [OrganPreset.h](../code/src/OrganPreset.h): `delay_mix` field, capture/apply

## Risks / open questions

- **Old presets**: a saved preset with `delay_mix > 50` previously meant
  ">50% mix" and now loads as "50% mix + feedback". Accepted per the
  storage decision; values 0–50 load with identical sound.
- **Feedback 100% never decays**: echoes sustain until the mix or
  feedback is turned down. That is the requested "full delay feedback";
  the loop is still bounded by the soft-clipper and DC blocker.
- **Zipper noise**: feedback moves ride a ~10 ms ramp like the mix, and
  the encoder steps 1% per click, so sweeps stay smooth.
- **Pivot continuity**: both sides of 50% are mix 50% / feedback 35%,
  so crossing the pivot is seamless.
- **Granularity**: 1 click = 1 mix point in the lower half, 1.3
  feedback points in the upper half — fine for a performance control;
  no finer stepping planned.

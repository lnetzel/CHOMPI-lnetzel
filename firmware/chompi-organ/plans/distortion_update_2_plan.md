# Plan: Organ distortion — Tape Saturation swap + BrokenSpeaker stereo dropouts

**Status:** implemented 2026-10-13. Supersedes the Warm type introduced by distortion_update_plan.md.

## TL;DR
Replace the Warm distortion type (slot 0) in chompi-organ with an exact port of the CHOMPI-TAPE tape-saturation algorithm (log-mapped 1–13.9× pre-gain into the stmlib `SoftClip`/`SoftLimit` rational waveshaper with output compensation gain). Rework BrokenSpeaker: high-pass lowered 400 Hz → 200 Hz, plus a NEW stereo post-rotary "dropout" stage (new `Dropout.h` module) that adds full-mute one-channel tape dropouts (rate/depth scale with drive), torn-cone crackle, and per-channel filter skew (4.0 kHz L / 3.2 kHz R). Distortion itself stays mono in its current position (pre-rotary); the new stereo stage runs between the rotary speaker and the plate reverb, gated by the distortion's live BrokenSpeaker weight × wet amount so it is an exact bypass for other types and at zero drive.

## User decisions (locked)
- **Architecture**: Keep distortion mono pre-rotary; add separate stereo dropout stage post-rotary (NOT moving distortion after rotary).
- **Dropouts**: Full-mute dropouts; rate and depth scale with drive amount.
- **Low end**: BrokenSpeaker HP lowered 400 Hz → ~200 Hz (not removed).
- **Extra grit**: Torn-cone crackle + per-channel filter skew (no bandwidth wobble).

## Steps

### Phase 1 — Tape Saturation replaces Warm (Distortion.h only)
1. In `code/src/Distortion.h`:
   - Rename enum `kWarm` → `kTapeSat` (keep value 0; `kFuzz`=1, `kBrokenSpeaker`=2 unchanged → LED key mapping and boot-default slot unchanged).
   - Remove Warm voicing constants (`kWarmGainSpan`, `kWarmBias`, `kWarmDcHz`, `kWarmLpHz`) and all warm members (`warm_dc_r_`, `warm_lp_c_`, `warm_bias_tanh_`, `g_warm_`, `post_warm_`, `warm_dc_x1_`, `warm_dc_y1_`, `warm_lp_`, `wet_warm_internal_`).
   - Add local `SoftLimit`/`SoftClip` static functions ported from pichenettes/stmlib (`SoftLimit(x) = x*(27+x²)/(27+9x²)`; `SoftClip(x) = ±1` beyond |x|>3, else SoftLimit) with an attribution comment matching THIRD_PARTY.md (MIT, via DaisySP). Reference: `firmware/chompi-tape/code/libs/DaisySP/Source/Utility/dsp.h` L219–232. NOTE: this is NOT the same curve as the existing `FuzzClip` — do not reuse it.
   - Add named tape constants mirroring chompi-tape `DSPEngine.h`: log coefficient 1.7, gain scale 13 (`amt = logf(1.7f*d + 1.f) * 13.f + 1.f` → 1–13.9×), comp drive 0.4, comp depth 0.7. Reference: `firmware/chompi-tape/code/src/DSPEngine.h` `SetSaturate` (L1189–1193) and `ApplyFx` saturation core (L296–300).
   - `PrepareBlock()`: replace warm gain computation with per-block `tape_amt_` (log-mapped) and `tape_comp_ = 1.f - SoftClip(.4f*(tape_amt_-1.f)) * .7f`.
   - `Process()`: replace Warm branch with `wet = SoftClip(tape_amt_ * in) * tape_comp_` — memoryless, so remove `WarmPrime()` and the priming call (keep Fuzz/Broken priming). Rename `wet_warm_internal_` → `wet_tape_`.
   - `Init()`: drop warm coefficient/state init; weights still init one-hot on slot 0; `PrepareBlock(0)` unchanged.
   - Update the file-header doc comment: describe Tape Saturation (port of CHOMPI-TAPE page-2 FX) instead of Warm.
2. Level sanity: at d=1, SoftClip output ≈ 1.0 and comp ≈ 0.3 → output ≈ 0.30, close to the kRefLevel 0.33 convention of the other types — no extra normalization needed.

### Phase 2 — BrokenSpeaker low-end fix + new stereo Dropout stage
3. `Distortion.h` BrokenSpeaker voicing: `kBrokenHpHz` 400.f → 200.f (comment updated; coefficient is sample-rate-derived in `Init`, no other change). Keep mono 4 kHz LP and hard clip ±0.7 as-is.
4. Add `float GetBrokenWetAmount() const { return weight_[kBrokenSpeaker] * wet_broken_; }` accessor to `Distortion` — the exact per-sample activity level for the stereo stage (handles both the 10 ms type crossfade and the drive→0 wet→dry ramp).
5. New header `code/src/Dropout.h` (class `chompi::Dropout`, header-only, no MCU deps, native-testable — mirror Distortion.h style):
   - API: `Init(float sample_rate)`, `PrepareBlock(float drive)` (per-block drive-scaled rates/depths), `Process(float &l, float &r, float amount)` per sample; `amount` = `distortion_.GetBrokenWetAmount()`. Exact bit-pass-through when `amount == 0`.
   - Per-channel dropout engine: xorshift32 PRNG (independent L/R state, fixed seeds in Init for deterministic tests); per-channel state machine IDLE→DROPOUT with linear ~4 ms gain ramps (click-free); event interval random, lerped by drive (sparse ~1.2–3 s at low drive → dense ~0.15–0.8 s at full drive); depth random up to full mute (target gain ~0–0.15 at full drive, shallower at low drive); duration random 20–140 ms. Dropout depth scaled by `amount` so the 10 ms type crossfade can't click.
   - Torn-cone crackle: sparse random impulses per channel (density ∝ drive, ~2–20/s; amplitude ±0.05–0.2 scaled by `amount`), added pre-skew-filter.
   - Per-channel filter skew: one-pole low-pass per channel, L = 4.0 kHz, R = 3.2 kHz, blended `out = in + amount*(lp(in) - in)` so amount=0 is exact bypass.
6. `code/src/OrganEngine.h`:
   - Add `Dropout dropout_;` member; `dropout_.Init(sample_rate)` in `Init()`; `dropout_.PrepareBlock(drive_target_)` in `Prepare()`.
   - In `Process()` (current chain at L556–568): after `rotary_.Process(out_sig, out_l, out_r);` insert `dropout_.Process(out_l, out_r, distortion_.GetBrokenWetAmount());` before `plate_.Process(...)`.
   - Update header docs (L1–49 DSP-5 description: Tape/Fuzz/BrokenSpeaker; document new stereo dropout stage in the chain comment at L549–567).

### Phase 3 — Tests, UI comments, docs (parallel after Phases 1–2)
7. `code/tests/distortion_test.cpp`: remove Warm tests; add tape-saturation tests (SoftClip curve spot values incl. |x|>3 clamp, log mapping endpoints amt(0)=1 / amt(1)≈13.9, comp gain floor 0.3, exact zero-drive bypass); update `test_init_and_clamp` (boots to kTapeSat), `test_weight_transitions` (Tape→Fuzz→BrokenSpeaker), `test_amount_retention`, `test_voicing_distinction` (tape vs fuzz curves differ; Broken HP now 200 Hz).
8. New `code/tests/dropout_test.cpp`: bypass is bit-exact at amount 0; mono input → L/R identical when amount ramps 0; dropouts occur within bounded time at amount 1 / drive 1 (deterministic seeds); ramps are click-free (max sample-to-sample delta bounded); per-channel independence (dropouts don't fire simultaneously in both channels persistently); skew LP darkens R more than L. Wire into `code/tests/Makefile` (add to `test:` target, build rule, and `clean` list — mirror distortion_test lines 7, 20, 35).
9. `code/src/chompi_main.cpp`: update comments only — distortion selector overlay comment (Warm → Tape) near L430–442; LED mapping (KEY_1–5 / 6–10 / 11–15) unchanged.
10. Docs:
    - `README.md`: header type list + page-2 distortion description (Tape Saturation/Fuzz/BrokenSpeaker).
    - `plans/chompi-organ-specification.md`: FR-4b (types, boot default Tape Saturation), DSP-5 (tape algorithm + BrokenSpeaker 200 Hz HP; document Dropout stage, either as DSP-5 extension or new DSP-7 block).
    - `plans/distortion_update_plan.md`: add status note that Warm was subsequently replaced by the CHOMPI-TAPE tape saturation port.

## Relevant files
- `code/src/Distortion.h` — enum, Warm→TapeSat branch replacement, Broken HP 200 Hz, `GetBrokenWetAmount()` accessor; preserve crossfade/weights/priming architecture (`AdvanceWeights`, `RampToward`, `PrepareBlock` B4-style per-block gains).
- `code/src/Dropout.h` — NEW stereo dropout/crackle/skew module.
- `code/src/OrganEngine.h` — L556–568 chain insert, member/Init/Prepare wiring, docs L1–49.
- `code/src/chompi_main.cpp` — comments only (~L430–442).
- `code/tests/distortion_test.cpp`, `dropout_test.cpp` (new), `Makefile` (L7, L20, L33–37 pattern).
- `README.md`, `plans/chompi-organ-specification.md` (FR-4b, DSP-5), `plans/distortion_update_plan.md`.
- Reference only: `firmware/chompi-tape/code/src/DSPEngine.h` L1189–1193 + L296–300; `firmware/chompi-tape/code/libs/DaisySP/Source/Utility/dsp.h` L219–232.

## Verification
1. Native tests: `make -C firmware/chompi-organ/code/tests test` — all 6 binaries pass (incl. new dropout_test).
2. ARM firmware build: `make -C firmware/chompi-organ/code/src DEBUG=0 OPT=-O3` — no errors/warnings.
3. Hardware listening tests:
   - Tape Saturation: compare against CHOMPI-TAPE page-2 saturation at matched drive; smooth warm breakup, no clicks at drive 0↔wet transitions; boot defaults to it.
   - BrokenSpeaker: low octave notes retain body (200 Hz HP); dropouts audibly hit one channel at a time, more frequent/deeper at high drive; crackle present at high drive; no clicks on dropout entry/exit or type crossfade.
   - Fuzz unchanged vs. current firmware (A/B).
   - CPU headroom: no audio dropouts/glitches with all voices + reverb + BrokenSpeaker active.

## Decisions
- Distortion remains mono pre-rotary; stereo lo-fi behavior lives in the new post-rotary Dropout stage, gated by `weight_[kBrokenSpeaker] * wet_broken_` so bypass/crossfade semantics match the existing architecture exactly.
- Tape saturation ported exactly (log mapping, 1–13.9× gain, stmlib SoftClip, 0.7 comp depth) rather than reusing the organ's existing FuzzClip curve — DaisySP's SoftClip is the rational `x(27+x²)/(27+9x²)`, a different curve.
- Reimplement SoftClip locally in Distortion.h (keeps header dependency-free/native-testable, consistent with existing FuzzClip duplication); stmlib/DaisySP MIT attribution comment — DaisySP already covered in THIRD_PARTY.md.
- Slot/order unchanged (Tape=0, Fuzz=1, BrokenSpeaker=2): no LED, key-mapping, or boot-default logic changes.
- BrokenSpeaker mono clip (±0.7) and 4 kHz LP unchanged; the "more broken" character comes from dropouts + crackle + skew + 200 Hz HP.
- Excluded: bandwidth-wobble modulation (user opted out); moving distortion post-rotary; changes to chompi-tape firmware.

## Further considerations
1. Dropout rate/depth ranges (0.15–0.8 s dense, 20–140 ms duration, etc.) are initial voicing values — expect a tuning pass after hardware listening, same as the original voicings.
2. If tape saturation feels too quiet at max drive (comp floor 0.3 vs 0.33 reference), a small makeup gain can be folded into `tape_comp_`; decide by ear.

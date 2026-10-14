/** @file Distortion.h
 *  @brief Fuzz distortion with tone control for ORGAN (DSP-5).
 *
 *  A single fuzz character shares OrganEngine's existing 32-step drive
 *  amount and its per-sample smoothed drive value; step 0 is an exact
 *  dry bypass at every tone setting:
 *
 *  - Fuzz: strong quadratic pre-gain g = 1 + 127 d^2 (1..128x) into a
 *    symmetric bounded soft-knee clipper (near-hard saturation), then a
 *    one-pole post low-pass. Same 0.33 normalization. Symmetric curve:
 *    no intentional DC, no bit reduction.
 *  - Tone: SHIFT + volume-encoder turn on page 2 sweeps the low-pass
 *    cutoff logarithmically from 500 Hz (tone 0, dark) to the original
 *    10 kHz (tone 100, boot default). Tone 100 preserves the historical
 *    Fuzz exactly - the original coefficient expression, clipping
 *    curve, gain mapping and normalization are unchanged, so there is
 *    no additional darkening and the settled sound matches bit-for-bit.
 *    Counterclockwise darkens; 0% is not a mute.
 *
 *  Fuzz runs fully wet while the amount is up, but crossfades wet->dry
 *  over ~10 ms when the amount target reaches zero (and dry->wet when
 *  it rises again). The smoothed amount tails toward zero exponentially
 *  (~2 s) and filtering at unity gain is audible colour, so bypass must
 *  not wait for the smoothed value to settle: the finite target-driven
 *  transition is the "settle, then snap" path that makes settled zero
 *  an exact dry bypass with no residual filtered tail and no abrupt
 *  step.
 *
 *  Tone changes compute the log-mapped target coefficient once per
 *  setter call (no per-sample transcendental work, no heap); the live
 *  coefficient ramps linearly toward the target over ~10 ms
 *  (sample-rate derived), snapping to the exact endpoint, and
 *  retargeting during rapid turns continues from the live coefficient.
 *  The filter history is retained across tone changes, so darkening
 *  never clicks or drops out.
 *
 *  Per-block gain and normalization are computed in PrepareBlock()
 *  (called once per audio block from OrganEngine::Prepare, B4-style) -
 *  no expensive coefficient work inside the sample loop.
 *
 *  Header-only, no MCU/Daisy dependencies: native-testable.
 */
#pragma once
#include <stdint.h>
#include <math.h>

namespace chompi
{

class Distortion
{
  public:
    /** Finite linear ramp lengths, sample-rate derived (~10 ms). */
    static constexpr float kWetRampSeconds  = .010f;
    static constexpr float kToneRampSeconds = .010f;

    /** Fuzz voicing (initial values): pre-gain 1..128; the post
     *  low-pass cutoff is swept by the tone control from 500 Hz (dark)
     *  to the original 10 kHz (tone 100, boot default). */
    static constexpr float kFuzzGainSpan = 127.f;
    static constexpr float kFuzzLpHz     = 10000.f;
    static constexpr float kToneMinLpHz  = 500.f;

    /** Input-referenced FUZZ output leveling (proper gain leveling).
     *  The fuzz is a hard limiter: its settled output level at high
     *  drive is ~0.33 RMS regardless of how loud the organ mix is, so
     *  any fixed trim under- or over-compensates depending on the
     *  registration and polyphony. Instead, the per-block RMS of the
     *  dry distortion input and of the untrimmed fuzz output is
     *  measured, smoothed ~50 ms, and the trim servo-matches the fuzzed
     *  level down to the dry level: trim = in_rms / out_rms, clamped
     *  to <= 1 (never boosts, never touches the clean sound) and
     *  floored at -30 dB. At drive 0 the output IS the input, so the
     *  ratio is exactly 1: exact unity, exact bypass. Applied by
     *  OrganEngine after the rotary and before the plate reverb, so
     *  the leveled level also feeds the reverb; Process() output and
     *  all shaping/filtering are unchanged, and there is no user
     *  control. */
    static constexpr float kLevelFloor         = 0.0316f; /* -30 dB */
    static constexpr float kLevelSmoothSeconds = .050f;

    void Init(float sample_rate)
    {
        sample_rate_      = sample_rate;
        wet_ramp_inc_     = 1.f / (kWetRampSeconds * sample_rate);
        tone_ramp_inc_    = 1.f / (kToneRampSeconds * sample_rate);
        tone_percent_     = 100; /* boot default: the original Fuzz */
        fuzz_lp_c_        = ToneCoeff(100); /* original 10 kHz expression */
        fuzz_lp_c_target_ = fuzz_lp_c_;
        wet_fuzz_         = 0.f;
        drive_target_     = 0.f;
        fuzz_lp_          = 0.f;
        level_in_acc_ = level_out_acc_ = 0.f;
        level_count_                   = 0;
        in_level_     = out_level_     = 0.f;
        level_trim_                    = 1.f; /* no trim until measured */
        PrepareBlock(0.f); /* unity gain + normalized post */
    }

    /** Set the tone (0..100 percent, clamped, no wrap): 0 = 500 Hz
     *  low-pass (dark), 100 = the original 10 kHz (no additional
     *  darkening). Only the log-mapped target coefficient is computed
     *  here (outside the sample loop); the live coefficient ramps
     *  toward it over ~10 ms inside Process(). Never changes the drive
     *  amount. */
    void SetTonePercent(int percent)
    {
        percent           = percent < 0 ? 0
                                        : (percent > 100 ? 100 : percent);
        tone_percent_     = percent;
        fuzz_lp_c_target_ = ToneCoeff(percent);
    }

    int GetTonePercent() const { return tone_percent_; }

    /** Per-block gain/normalization from the drive TARGET (B4: once per
     *  block, not a divide per sample). Call from
     *  OrganEngine::Prepare() before each block's Process() run. */
    void PrepareBlock(float drive_target)
    {
        drive_target_ = drive_target < 0.f ? 0.f
                                           : (drive_target > 1.f ? 1.f
                                                                 : drive_target);
        const float d = drive_target_;
        g_fuzz_    = 1.f + d * d * kFuzzGainSpan;
        post_fuzz_ = kRefLevel / FuzzClip(kRefLevel * g_fuzz_);

        /* Input-referenced output leveling, once per block (no per-
         * sample transcendental work): convert the accumulated squares
         * to block RMS, smooth ~50 ms with a per-block one-pole (about
         * four cycles of the lowest organ fundamental - stable, no
         * pumping), then servo the trim toward in_rms / out_rms,
         * clamped to [kLevelFloor, 1]. Near silence the fuzz output is
         * near-silent too - hold the current trim rather than dividing
         * noise by noise. */
        if(level_count_ > 0)
        {
            const float inv_n   = 1.f / static_cast<float>(level_count_);
            const float in_rms  = sqrtf(level_in_acc_ * inv_n);
            const float out_rms = sqrtf(level_out_acc_ * inv_n);
            level_in_acc_ = level_out_acc_ = 0.f;
            float a = static_cast<float>(level_count_)
                      * (1.f / (kLevelSmoothSeconds * sample_rate_));
            a            = a > 1.f ? 1.f : a;
            level_count_ = 0;
            in_level_  += a * (in_rms - in_level_);
            out_level_ += a * (out_rms - out_level_);
            if(out_level_ > 1e-5f)
            {
                float ratio = in_level_ / out_level_;
                ratio       = ratio > 1.f ? 1.f : ratio;
                level_trim_ = ratio < kLevelFloor ? kLevelFloor : ratio;
            }
        }
    }

    /** Process one mono sample. `drive` is OrganEngine's per-sample
     *  smoothed amount; the wet gate follows the drive TARGET, so
     *  settled zero is a bit-exact dry bypass even while the smoothed
     *  amount is still tailing off. */
    float Process(float in, float drive)
    {
        (void)drive; /* gating follows the target, not the smoothed value */
        const float wet_target = drive_target_ > 0.f ? 1.f : 0.f;
        wet_fuzz_  = RampToward(wet_fuzz_, wet_target, wet_ramp_inc_);
        fuzz_lp_c_ = RampToward(fuzz_lp_c_, fuzz_lp_c_target_,
                                tone_ramp_inc_);
        const float wet = FuzzFiltered(FuzzShaped(in));
        const float out = in + wet_fuzz_ * (wet - in);
        /* leveling followers: accumulate dry-in and UNTRIMMED fuzz-out
         * energy for the per-block RMS pair in PrepareBlock() (the trim
         * itself is applied by OrganEngine, so this cannot hunt) */
        level_in_acc_  += in * in;
        level_out_acc_ += out * out;
        level_count_++;
        return out;
    }

    /** Live and target one-pole low-pass coefficients; diagnostics and
     *  native tests. */
    float FuzzLpCoeff()       const { return fuzz_lp_c_; }
    float FuzzLpCoeffTarget() const { return fuzz_lp_c_target_; }

    /** FUZZ activity-scaled output trim for OrganEngine: exactly 1 at
     *  settled bypass (wet == 0) and exactly 1 whenever the fuzz is as
     *  quiet as the dry signal (level_trim_ == 1, incl. drive 0, where
     *  output equals input exactly), ramping click-free with the ~10 ms
     *  wet crossfade (the wet fade follows the drive target and reaches
     *  exact bypass in finite time, even while the smoothed drive value
     *  is still nonzero). Folded through the live wet amount - no extra
     *  smoother state. */
    float GetOutputGain() const
    {
        return 1.f - (1.f - level_trim_) * wet_fuzz_;
    }

    /** Per-block leveling trim (1 = no trim) and the smoothed dry/wet
     *  RMS estimates it is computed from; diagnostics and native
     *  tests. */
    float TrimGain()  const { return level_trim_; }
    float InLevel()   const { return in_level_; }
    float OutLevel()  const { return out_level_; }

  private:
    static constexpr float kTwoPi    = 6.283185307179586f;
    static constexpr float kRefLevel = .33f; /* level-anchor reference */
    static constexpr float kThird    = .333333333333333f;
    static constexpr float kFuzzPeak = .666666666666667f; /* clip bound */

    static float RampToward(float cur, float target, float inc)
    {
        if(cur < target)
        {
            cur += inc;
            return cur > target ? target : cur; /* snap: exact endpoint */
        }
        if(cur > target)
        {
            cur -= inc;
            return cur < target ? target : cur;
        }
        return cur;
    }

    /** Symmetric bounded soft-knee clipper: y = t - t^3/3 inside the
     *  knee, clamped to +/-2/3 outside (C1-continuous at the knee,
     *  |y| <= 2/3). Near-hard saturation at fuzz gains. */
    static float FuzzClip(float t)
    {
        if(t > 1.f)
            return kFuzzPeak;
        if(t < -1.f)
            return -kFuzzPeak;
        return t - t * t * t * kThird;
    }

    float FuzzShaped(float x) const
    {
        return FuzzClip(g_fuzz_ * x) * post_fuzz_;
    }

    /** Fuzz filter: one-pole low-pass with the tone-controlled cutoff
     *  (no DC blocker needed - the clipper is symmetric and produces no
     *  intentional DC). */
    float FuzzFiltered(float s)
    {
        fuzz_lp_ += fuzz_lp_c_ * (s - fuzz_lp_);
        return fuzz_lp_;
    }

    /** Log-mapped tone -> one-pole coefficient: cutoff =
     *  500 * (10000/500)^(percent/100) Hz, so 0% is 500 Hz and 100% is
     *  the original 10 kHz. The 100% endpoint keeps the original
     *  coefficient expression verbatim so the settled Fuzz matches the
     *  historical sound bit-for-bit. Called only from Init() and
     *  SetTonePercent() - never per sample. */
    float ToneCoeff(int percent) const
    {
        if(percent >= 100)
            return 1.f - expf(-kTwoPi * kFuzzLpHz / sample_rate_);
        const float hz = kToneMinLpHz
                         * powf(kFuzzLpHz / kToneMinLpHz,
                                static_cast<float>(percent) * .01f);
        return 1.f - expf(-kTwoPi * hz / sample_rate_);
    }

    float        sample_rate_;
    float        wet_ramp_inc_;  /* wet<->dry crossfade per sample */
    float        tone_ramp_inc_; /* coefficient ramp per sample */
    volatile int tone_percent_;  /* ISR-written, main-loop read (LEDs) */
    float        fuzz_lp_c_, fuzz_lp_c_target_;
    float        wet_fuzz_;      /* wet<->dry crossfade state */
    float        drive_target_;
    float        g_fuzz_, post_fuzz_;
    float        fuzz_lp_; /* Fuzz filter state (retained across tone) */
    /* input-referenced leveling state */
    float        level_in_acc_, level_out_acc_; /* per-block x^2 sums */
    int          level_count_;
    float        in_level_, out_level_; /* ~50 ms smoothed RMS pair */
    float        level_trim_;         /* servo-matched trim, <= 1 */
};

} // namespace chompi

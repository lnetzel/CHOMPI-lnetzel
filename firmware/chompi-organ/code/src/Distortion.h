/** @file Distortion.h
 *  @brief Selectable-character distortion stage for ORGAN (DSP-5).
 *
 *  Three types share OrganEngine's existing 32-step drive amount and its
 *  per-sample smoothed drive value; step 0 is an exact dry bypass for
 *  every type:
 *
 *  - Tape Saturation: exact port of the CHOMPI-TAPE page-2 saturation
 *    (firmware/chompi-tape/code/src/DSPEngine.h): log-mapped pre-gain
 *    amt = logf(1.7*d + 1) * 13 + 1 (1..13.9x) into the stmlib
 *    SoftClip rational waveshaper x*(27+x^2)/(27+9*x^2) (clamped to
 *    +/-1 beyond |x| > 3), with output compensation gain
 *    1 - SoftClip(.4*(amt-1)) * .7 (floor 0.3) so loudness stays
 *    anchored near the 0.33 reference level. Memoryless: no filters,
 *    no priming, no intentional DC (the curve is symmetric).
 *  - Fuzz: strong quadratic pre-gain g = 1 + 127 d^2 (1..128x) into a
 *    symmetric bounded soft-knee clipper (near-hard saturation), then a
 *    ~10 kHz one-pole low-pass to moderate harsh highs. Same 0.33
 *    normalization. Symmetric curve: no intentional DC, no bit reduction.
 *  - BrokenSpeaker: aggressive harsh breakup with narrow speaker-like
 *    bandpass filtering and resonant coloration. Nonlinear hard clipping
 *    produces rich harmonics; high-pass and low-pass filtering (derived
 *    from sample rate) create a narrow vocal-like ~200 Hz-4 kHz presence
 *    band and suppress extreme lows/highs. Resonant state priming from
 *    the live signal prevents audible tails when switching in. Fully wet
 *    while amount is > 0, exact bypass at settled zero. The stereo
 *    dropout/crackle/skew part of the BrokenSpeaker character lives in
 *    the post-rotary Dropout stage (Dropout.h, DSP-8), gated by
 *    GetBrokenWetAmount().
 *
 *  Tape/Fuzz/BrokenSpeaker run fully wet while the amount is up, but
 *  crossfade wet->dry over ~10 ms when the amount target reaches zero
 *  (and dry->wet when it rises again). The smoothed amount tails toward
 *  zero exponentially (~2 s) and filtering at unity gain is audible
 *  colour, so bypass must not wait for the smoothed value to settle: the
 *  finite target-driven transition is the "settle, then snap" path that
 *  makes settled zero an exact dry bypass with no residual filtered tail
 *  and no abrupt step.
 *
 *  Type switching crossfades the per-type outputs with a three-element
 *  nonnegative weight vector that ramps linearly (~10 ms, sample-rate
 *  derived) toward the selected one-hot target while always summing to
 *  one; retargeting mid-fade continues from the current weights, never
 *  from a stale mode. GetType() returns the selected target immediately
 *  for the LED overlay. In stable mode only the selected branch is
 *  computed; the inactive branches' filter histories are primed from the
 *  live signal so activating Fuzz/BrokenSpeaker exposes no stale tails.
 *
 *  Per-block gains and normalization are computed in PrepareBlock()
 *  (called once per audio block from OrganEngine::Prepare, B4-style) -
 *  no expensive coefficient work inside the sample loop, no heap.
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
    /** Distortion characters in SHIFT+turn order: clockwise moves
     *  Tape Saturation -> Fuzz -> BrokenSpeaker (clamped at the ends,
     *  no wrap). */
    enum Type : uint8_t
    {
        kTapeSat       = 0,
        kFuzz          = 1,
        kBrokenSpeaker = 2
    };
    static constexpr int kNumTypes = 3;

    /** Finite linear ramp lengths, sample-rate derived (~10 ms). */
    static constexpr float kTypeRampSeconds = .010f;
    static constexpr float kWetRampSeconds  = .010f;

    /** Tape Saturation voicing (exact CHOMPI-TAPE port): log-mapped
     *  pre-gain 1..13.9x and output compensation gain. */
    static constexpr float kTapeLogCoef   = 1.7f;
    static constexpr float kTapeGainScale = 13.f;
    static constexpr float kTapeCompDrive = .4f;
    static constexpr float kTapeCompDepth = .7f;

    /** Fuzz voicing (initial values): pre-gain 1..128, ~10 kHz post
     *  low-pass. */
    static constexpr float kFuzzGainSpan = 127.f;
    static constexpr float kFuzzLpHz     = 10000.f;

    /** BrokenSpeaker voicing (initial values): aggressive pre-gain 1..200,
     *  harsh hard clipping, ~200 Hz high-pass (keeps the low octave's
     *  body), ~4 kHz low-pass (narrow presence peak around vocal
     *  formants), and resonant state priming. */
    static constexpr float kBrokenGainSpan = 199.f;
    static constexpr float kBrokenHpHz     = 200.f;
    static constexpr float kBrokenLpHz     = 4000.f;
    static constexpr float kBrokenClipPeak = 0.7f; /* hard clip bound */

    void Init(float sample_rate)
    {
        sample_rate_    = sample_rate;
        type_ramp_inc_  = 1.f / (kTypeRampSeconds * sample_rate);
        wet_ramp_inc_   = 1.f / (kWetRampSeconds * sample_rate);
        fuzz_lp_c_      = 1.f - expf(-kTwoPi * kFuzzLpHz / sample_rate);
        broken_hp_c_    = 1.f - expf(-kTwoPi * kBrokenHpHz / sample_rate);
        broken_lp_c_    = 1.f - expf(-kTwoPi * kBrokenLpHz / sample_rate);

        type_ = kTapeSat;
        for(int t = 0; t < kNumTypes; t++)
            weight_[t] = (t == kTapeSat) ? 1.f : 0.f;
        wet_tape_ = wet_fuzz_ = wet_broken_ = 0.f;
        drive_target_           = 0.f;
        fuzz_lp_ = 0.f;
        broken_hp_x1_ = broken_hp_y1_ = broken_lp_ = 0.f;
        PrepareBlock(0.f); /* unity gains + normalized posts */
    }

    /** Select the distortion type (0..kNumTypes-1, clamped at the ends,
     *  no wrap). The crossfade retargets from the CURRENT weights; the
     *  getter reflects the new target immediately (LED overlay). */
    void SetType(int type)
    {
        type  = type < 0 ? 0
                         : (type > kNumTypes - 1 ? kNumTypes - 1 : type);
        type_ = static_cast<Type>(type);
    }

    int GetType() const { return static_cast<int>(type_); }

    /** Per-block gains/normalization from the drive TARGET (B4: once per
     *  block, not a tanhf + divide per sample). Call from
     *  OrganEngine::Prepare() before each block's Process() run. */
    void PrepareBlock(float drive_target)
    {
        drive_target_ = drive_target < 0.f ? 0.f
                                           : (drive_target > 1.f ? 1.f
                                                                 : drive_target);
        const float d = drive_target_;
        /* Tape Saturation: exact CHOMPI-TAPE mapping - log-spaced
         * 1..13.9x pre-gain, then output compensation so the level
         * stays anchored as the clip depth grows (floor 0.3). */
        tape_amt_  = logf(kTapeLogCoef * d + 1.f) * kTapeGainScale + 1.f;
        tape_comp_ = 1.f - SoftClip(kTapeCompDrive * (tape_amt_ - 1.f))
                             * kTapeCompDepth;
        g_fuzz_    = 1.f + d * d * kFuzzGainSpan;
        post_fuzz_ = kRefLevel / FuzzClip(kRefLevel * g_fuzz_);
        g_broken_  = 1.f + d * d * kBrokenGainSpan;
        post_broken_ = kRefLevel / kBrokenClipPeak; /* normalized to peak */
    }

    /** Process one mono sample. `drive` is OrganEngine's per-sample
     *  smoothed amount; gates full wet while > 0, exact bypass at settled 0. */
    float Process(float in, float drive)
    {
        AdvanceWeights();
        const float wet_target = drive_target_ > 0.f ? 1.f : 0.f;
        wet_tape_   = RampToward(wet_tape_, wet_target, wet_ramp_inc_);
        wet_fuzz_   = RampToward(wet_fuzz_, wet_target, wet_ramp_inc_);
        wet_broken_ = RampToward(wet_broken_, wet_target, wet_ramp_inc_);

        float out = 0.f;
        if(weight_[kTapeSat] > 0.f)
        {
            /* Tape Saturation is memoryless: no filter histories to
             * prime, nothing to do while the branch is inactive. */
            const float wet = SoftClip(tape_amt_ * in) * tape_comp_;
            out += weight_[kTapeSat] * (in + wet_tape_ * (wet - in));
        }
        if(weight_[kFuzz] > 0.f)
        {
            const float wet = FuzzFiltered(FuzzShaped(in));
            out += weight_[kFuzz] * (in + wet_fuzz_ * (wet - in));
        }
        else
        {
            FuzzPrime(in);
        }
        if(weight_[kBrokenSpeaker] > 0.f)
        {
            const float wet = BrokenFiltered(BrokenShaped(in));
            out += weight_[kBrokenSpeaker] * (in + wet_broken_ * (wet - in));
        }
        else
        {
            BrokenPrime(in);
        }
        return out;
    }

    /** Current crossfade weights (always summing to 1); diagnostics and
     *  native tests. */
    float Weight(int t) const
    {
        return (t < 0 || t >= kNumTypes) ? 0.f : weight_[t];
    }

    /** Live BrokenSpeaker activity (type weight x wet amount): the exact
     *  per-sample drive of the post-rotary stereo Dropout stage (DSP-8),
     *  so that stage is an exact bypass for other types and at zero
     *  drive and ramps click-free through the ~10 ms crossfades. */
    float GetBrokenWetAmount() const
    {
        return weight_[kBrokenSpeaker] * wet_broken_;
    }

    /** Per-block tape-saturation parameters; diagnostics and native
     *  tests. */
    float TapeAmt()  const { return tape_amt_; }
    float TapeComp() const { return tape_comp_; }

    /** Soft limiting/clipping ported from pichenettes/stmlib (MIT), via
     *  DaisySP's Utility/dsp.h - the exact curve CHOMPI-TAPE's tape
     *  saturation uses. NOTE: not the same curve as FuzzClip. */
    static float SoftLimit(float x)
    {
        return x * (27.f + x * x) / (27.f + 9.f * x * x);
    }

    static float SoftClip(float x)
    {
        if(x < -3.f)
            return -1.f;
        if(x > 3.f)
            return 1.f;
        return SoftLimit(x);
    }

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

    /** Linear ~10 ms ramp of the weight vector toward the selected
     *  one-hot: move one increment of mass to the selected weight per
     *  sample and scale the others down proportionally, so the vector
     *  always sums to one and reaches the exact one-hot in finite time.
     *  Retargeting mid-ramp just changes type_: the ramp continues from
     *  the current weights. */
    void AdvanceWeights()
    {
        const int   sel  = static_cast<int>(type_);
        float      &ws   = weight_[sel];
        const float need = 1.f - ws;
        if(need <= 0.f)
            return; /* already one-hot */
        const float mass  = need < type_ramp_inc_ ? need : type_ramp_inc_;
        const float scale = (1.f - (ws + mass)) / need; /* need > 0 */
        ws += mass;
        for(int t = 0; t < kNumTypes; t++)
        {
            if(t != sel)
                weight_[t] *= scale;
        }
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

    /** Fuzz filter: ~10 kHz one-pole low-pass (no DC blocker needed -
     *  the clipper is symmetric and produces no intentional DC). */
    float FuzzFiltered(float s)
    {
        fuzz_lp_ += fuzz_lp_c_ * (s - fuzz_lp_);
        return fuzz_lp_;
    }

    void FuzzPrime(float in) { FuzzFiltered(in); }

    /** BrokenSpeaker: aggressive hard clipping for rich harmonics. */
    static float BrokenClip(float t)
    {
        /* Hard clip at ±kBrokenClipPeak for harsh breakup. */
        if(t > kBrokenClipPeak)
            return kBrokenClipPeak;
        if(t < -kBrokenClipPeak)
            return -kBrokenClipPeak;
        return t;
    }

    float BrokenShaped(float x) const
    {
        return BrokenClip(g_broken_ * x) * post_broken_;
    }

    /** BrokenSpeaker filters: ~200 Hz high-pass (keeps the low end's
     *  body), then ~4 kHz low-pass (creates narrow vocal-like
     *  presence). */
    float BrokenFiltered(float s)
    {
        /* High-pass filter (DC blocker, ~200 Hz cutoff). */
        const float y = s - broken_hp_x1_ + (1.f - broken_hp_c_) * broken_hp_y1_;
        broken_hp_x1_ = s;
        broken_hp_y1_ = y;
        
        /* Low-pass filter (~4 kHz, creates presence peak). */
        broken_lp_ += broken_lp_c_ * (y - broken_lp_);
        return broken_lp_;
    }

    void BrokenPrime(float in) { BrokenFiltered(in); }

    float        sample_rate_;
    float        type_ramp_inc_; /* mass per sample for a 0..1 transfer */
    float        wet_ramp_inc_;  /* wet<->dry crossfade per sample */
    float        fuzz_lp_c_;
    float        broken_hp_c_, broken_lp_c_; /* BrokenSpeaker filter coeffs */
    volatile Type type_; /* ISR-written, main-loop read (LED overlay) */
    float        weight_[kNumTypes];
    float        wet_tape_, wet_fuzz_, wet_broken_; /* crossfade states */
    float        drive_target_;
    float        tape_amt_, tape_comp_; /* per-block tape params (B4) */
    float        g_fuzz_, post_fuzz_;
    float        g_broken_, post_broken_;
    float        fuzz_lp_; /* Fuzz filter state */
    float        broken_hp_x1_, broken_hp_y1_, broken_lp_; /* Broken filter state */
};

} // namespace chompi

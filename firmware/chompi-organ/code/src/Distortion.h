/** @file Distortion.h
 *  @brief Selectable-character distortion stage for ORGAN (DSP-5).
 *
 *  Three types share OrganEngine's existing 32-step drive amount and its
 *  per-sample smoothed drive value; step 0 is an exact dry bypass for
 *  every type:
 *
 *  - Existing: the original overdrive-pedal stage, arithmetic unchanged -
 *    quadratic pre-gain g = 1 + 63 d^2 into tanhf(g*x), post gain
 *    0.33/tanh(0.33*g) anchoring a 1/3-full-scale input to the same
 *    output level. Fully wet whenever the smoothed amount is > 0, exact
 *    bypass at settled zero. No filtering in this branch.
 *  - Warm: gentler quadratic pre-gain g = 1 + 15 d^2 (1..16x) into a
 *    smooth asymmetric tanh saturation (a small input bias gives modestly
 *    different positive/negative knees -> even harmonics), then a ~20 Hz
 *    DC blocker (the asymmetry creates DC) and a ~6 kHz one-pole
 *    low-pass for rounded highs. Same 0.33 reference-level normalization.
 *    These are initial voicing values to finalize by ear, not a model of
 *    a specific vintage circuit.
 *  - Fuzz: strong quadratic pre-gain g = 1 + 127 d^2 (1..128x) into a
 *    symmetric bounded soft-knee clipper (near-hard saturation), then a
 *    ~10 kHz one-pole low-pass to moderate harsh highs. Same 0.33
 *    normalization. Symmetric curve: no intentional DC, no bit reduction.
 *
 *  Type switching crossfades the per-type outputs with a three-element
 *  nonnegative weight vector that ramps linearly (~10 ms, sample-rate
 *  derived) toward the selected one-hot target while always summing to
 *  one; retargeting mid-fade continues from the current weights, never
 *  from a stale mode. GetType() returns the selected target immediately
 *  for the LED overlay. In stable mode only the selected branch is
 *  computed; the inactive branches' one-pole filter histories are primed
 *  from the live signal so activating Warm/Fuzz exposes no stale tails.
 *
 *  Warm/Fuzz run fully wet like Existing while the amount is up, but
 *  crossfade wet->dry over ~10 ms when the amount target reaches zero
 *  (and dry->wet when it rises again). The smoothed amount tails toward
 *  zero exponentially (~2 s) and a 6/10 kHz low-pass at unity gain is
 *  audible colour, so bypass must not wait for the smoothed value to
 *  settle: the finite target-driven transition is the "settle, then
 *  snap" path that makes settled zero an exact dry bypass with no
 *  residual filtered tail and no abrupt step.
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
     *  Existing -> Warm -> Fuzz (clamped at the ends, no wrap). */
    enum Type : uint8_t
    {
        kExisting = 0,
        kWarm     = 1,
        kFuzz     = 2
    };
    static constexpr int kNumTypes = 3;

    /** Finite linear ramp lengths, sample-rate derived (~10 ms). */
    static constexpr float kTypeRampSeconds = .010f;
    static constexpr float kWetRampSeconds  = .010f;

    /** Warm voicing (initial values): pre-gain 1..16, asymmetric bias,
     *  ~20 Hz DC blocker, ~6 kHz post low-pass. */
    static constexpr float kWarmGainSpan = 15.f;
    static constexpr float kWarmBias     = .15f;
    static constexpr float kWarmDcHz     = 20.f;
    static constexpr float kWarmLpHz     = 6000.f;

    /** Fuzz voicing (initial values): pre-gain 1..128, ~10 kHz post
     *  low-pass. */
    static constexpr float kFuzzGainSpan = 127.f;
    static constexpr float kFuzzLpHz     = 10000.f;

    void Init(float sample_rate)
    {
        sample_rate_    = sample_rate;
        type_ramp_inc_  = 1.f / (kTypeRampSeconds * sample_rate);
        wet_ramp_inc_   = 1.f / (kWetRampSeconds * sample_rate);
        warm_dc_r_      = expf(-kTwoPi * kWarmDcHz / sample_rate);
        warm_lp_c_      = 1.f - expf(-kTwoPi * kWarmLpHz / sample_rate);
        fuzz_lp_c_      = 1.f - expf(-kTwoPi * kFuzzLpHz / sample_rate);
        warm_bias_tanh_ = tanhf(kWarmBias);

        type_ = kExisting;
        for(int t = 0; t < kNumTypes; t++)
            weight_[t] = (t == kExisting) ? 1.f : 0.f;
        wet_warm_ = wet_fuzz_ = 0.f;
        drive_target_         = 0.f;
        warm_dc_x1_ = warm_dc_y1_ = warm_lp_ = fuzz_lp_ = 0.f;
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
        const float d  = drive_target_;
        /* same expression order as the original engine code, so the
         * Existing branch arithmetic is bit-identical (B4) */
        g_exist_       = 1.f + d * d * 63.f;
        post_exist_    = kRefLevel / tanhf(kRefLevel * g_exist_);
        g_warm_        = 1.f + d * d * kWarmGainSpan;
        post_warm_     = kRefLevel
                     / (tanhf(kRefLevel * g_warm_ + kWarmBias)
                        - warm_bias_tanh_);
        g_fuzz_    = 1.f + d * d * kFuzzGainSpan;
        post_fuzz_ = kRefLevel / FuzzClip(kRefLevel * g_fuzz_);
    }

    /** Process one mono sample. `drive` is OrganEngine's per-sample
     *  smoothed amount; it gates the Existing branch exactly like the
     *  original code (fully wet while > 0, exact bypass at settled 0). */
    float Process(float in, float drive)
    {
        AdvanceWeights();
        const float wet_target = drive_target_ > 0.f ? 1.f : 0.f;
        wet_warm_ = RampToward(wet_warm_, wet_target, wet_ramp_inc_);
        wet_fuzz_ = RampToward(wet_fuzz_, wet_target, wet_ramp_inc_);

        float out = 0.f;
        if(weight_[kExisting] > 0.f)
        {
            /* exact original DSP-5 arithmetic, no filtering */
            const float e
                = drive > 0.f ? tanhf(g_exist_ * in) * post_exist_ : in;
            out += weight_[kExisting] * e;
        }
        if(weight_[kWarm] > 0.f)
        {
            const float wet = WarmFiltered(WarmShaped(in));
            out += weight_[kWarm] * (in + wet_warm_ * (wet - in));
        }
        else
        {
            WarmPrime(in); /* live filter histories: no stale tails */
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
        return out;
    }

    /** Current crossfade weights (always summing to 1); diagnostics and
     *  native tests. */
    float Weight(int t) const
    {
        return (t < 0 || t >= kNumTypes) ? 0.f : weight_[t];
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

    /** Warm: asymmetric tanh - the bias shifts the operating point for
     *  modestly different positive/negative knees (even harmonics). */
    float WarmShaped(float x) const
    {
        return (tanhf(g_warm_ * x + kWarmBias) - warm_bias_tanh_)
               * post_warm_;
    }

    /** Warm filters: ~20 Hz DC blocker, then ~6 kHz one-pole low-pass. */
    float WarmFiltered(float s)
    {
        const float y = s - warm_dc_x1_ + warm_dc_r_ * warm_dc_y1_;
        warm_dc_x1_ = s;
        warm_dc_y1_ = y;
        warm_lp_ += warm_lp_c_ * (y - warm_lp_);
        return warm_lp_;
    }

    void WarmPrime(float in) { WarmFiltered(in); }

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

    float        sample_rate_;
    float        type_ramp_inc_; /* mass per sample for a 0..1 transfer */
    float        wet_ramp_inc_;  /* wet<->dry crossfade per sample */
    float        warm_dc_r_, warm_lp_c_, fuzz_lp_c_, warm_bias_tanh_;
    volatile Type type_; /* ISR-written, main-loop read (LED overlay) */
    float        weight_[kNumTypes];
    float        wet_warm_, wet_fuzz_;
    float        drive_target_;
    float        g_exist_, post_exist_; /* per-block gains (B4) */
    float        g_warm_, post_warm_;
    float        g_fuzz_, post_fuzz_;
    float        warm_dc_x1_, warm_dc_y1_, warm_lp_;
    float        fuzz_lp_;
};

} // namespace chompi

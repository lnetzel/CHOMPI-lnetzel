/** @file Dropout.h
 *  @brief BrokenSpeaker-only stereo lo-fi stage for ORGAN (DSP-8).
 *
 *  Sits between the rotary speaker and the plate reverb and adds the
 *  stereo "broken" part of the BrokenSpeaker distortion character:
 *
 *  - Tape-style dropouts: random full-mute-class events that hit ONE
 *    channel at a time (independent per-channel state machines driven
 *    by independent xorshift32 PRNGs). Rate and depth scale with the
 *    BrokenSpeaker drive: sparse and shallow (~1.2-3 s apart, 50-80%
 *    residual gain) at low drive, dense and deep (~0.15-0.8 s apart,
 *    0-15% residual gain) at full drive; durations 20-140 ms. Entry
 *    and exit use linear ~4 ms gain ramps, so dropouts never click.
 *  - Torn-cone crackle: sparse random impulses per channel (density
 *    ~2-20/s scaling with drive, amplitude +/-0.05-0.2), added before
 *    the skew filter.
 *  - Per-channel filter skew: a one-pole low-pass per channel (4.0 kHz
 *    left, 3.2 kHz right), like a worn speaker with mismatched cones.
 *
 *  Every contribution is scaled per sample by `amount` - the live
 *  BrokenSpeaker activity (Distortion::GetBrokenWetAmount() = type
 *  weight x wet amount) - so the stage is an exact bit-pass-through
 *  bypass for the other distortion types and at zero drive, and the
 *  ~10 ms type/drive crossfades ramp every effect in and out
 *  click-free. The dropout gain itself is folded through `amount`, so
 *  even a mid-dropout type change cannot click.
 *
 *  Fixed PRNG seeds make the behaviour deterministic for native tests.
 *  Header-only, no MCU/Daisy dependencies: native-testable.
 */
#pragma once
#include <stdint.h>
#include <math.h>

namespace chompi
{

class Dropout
{
  public:
    /** Per-channel skew low-pass cutoffs: the right channel is a touch
     *  darker, like a speaker with mismatched worn cones. */
    static constexpr float kSkewLpHzL = 4000.f;
    static constexpr float kSkewLpHzR = 3200.f;
    /** Click-free linear gain ramp into/out of a dropout (~4 ms). */
    static constexpr float kRampSeconds = .004f;

    void Init(float sample_rate)
    {
        sample_rate_ = sample_rate;
        ramp_inc_    = 1.f / (kRampSeconds * sample_rate);
        skew_c_[0]   = 1.f - expf(-kTwoPi * kSkewLpHzL / sample_rate);
        skew_c_[1]   = 1.f - expf(-kTwoPi * kSkewLpHzR / sample_rate);
        drive_       = 0.f;
        for(int c = 0; c < 2; c++)
        {
            /* fixed, per-channel seeds: deterministic native tests */
            rng_[c]         = (c == 0) ? 0x2545F491u : 0x8F3A7B15u;
            gain_[c]        = 1.f;
            gain_target_[c] = 1.f;
            idle_left_[c]   = -1; /* scheduled by the first PrepareBlock */
            drop_left_[c]   = 0;
            skew_lp_[c]     = 0.f;
        }
    }

    /** Per-block BrokenSpeaker drive (0..1, clamped): dropout rate/depth
     *  and crackle density all scale with it. Call once per audio block
     *  from OrganEngine::Prepare(). */
    void PrepareBlock(float drive)
    {
        drive_ = drive < 0.f ? 0.f : (drive > 1.f ? 1.f : drive);
        for(int c = 0; c < 2; c++)
        {
            if(idle_left_[c] < 0)
                idle_left_[c] = IntervalSamples(c);
        }
    }

    /** Process one stereo sample pair in place. `amount` is the live
     *  BrokenSpeaker activity (type weight x wet amount); 0 is an exact
     *  bit-pass-through. */
    void Process(float &l, float &r, float amount)
    {
        float *io[2];
        io[0] = &l;
        io[1] = &r;
        for(int c = 0; c < 2; c++)
        {
            if(drop_left_[c] > 0)
            {
                /* mid-dropout: count down, then release the gain and
                 * schedule the next event */
                drop_left_[c]--;
                if(drop_left_[c] == 0)
                {
                    gain_target_[c] = 1.f;
                    idle_left_[c]   = IntervalSamples(c);
                }
            }
            else if(idle_left_[c] > 0)
            {
                idle_left_[c]--;
                if(idle_left_[c] == 0)
                {
                    /* start a dropout: random depth (deeper at higher
                     * drive) and random 20-140 ms duration */
                    const float depth = RandFloat(c);
                    const float lo    = .5f - .5f * drive_;  /* .5 -> 0 */
                    const float hi    = .8f - .65f * drive_; /* .8 -> .15 */
                    gain_target_[c]   = lo + depth * (hi - lo);
                    drop_left_[c]     = DurationSamples(c);
                }
            }

            /* linear ~4 ms gain ramp toward the target (click-free) */
            float g = gain_[c];
            if(g < gain_target_[c])
            {
                g += ramp_inc_;
                gain_[c] = g > gain_target_[c] ? gain_target_[c] : g;
            }
            else if(g > gain_target_[c])
            {
                g -= ramp_inc_;
                gain_[c] = g < gain_target_[c] ? gain_target_[c] : g;
            }

            float x = *io[c];
            /* torn-cone crackle, added pre-skew-filter */
            x += Crackle(c) * amount;
            /* dropout gain, folded through amount so a mid-dropout type
             * crossfade stays click-free and amount 0 is exact bypass */
            x *= 1.f - (1.f - gain_[c]) * amount;
            /* per-channel skew low-pass, blended by amount */
            skew_lp_[c] += skew_c_[c] * (x - skew_lp_[c]);
            *io[c] = x + amount * (skew_lp_[c] - x);
        }
    }

  private:
    static constexpr float kTwoPi = 6.283185307179586f;

    /** xorshift32: cheap, deterministic, per-channel state. */
    static uint32_t XorShift(uint32_t &s)
    {
        s ^= s << 13;
        s ^= s >> 17;
        s ^= s << 5;
        return s;
    }

    /** Uniform random float in [0, 1). */
    float RandFloat(int c)
    {
        return static_cast<float>(XorShift(rng_[c]) >> 8)
               * (1.f / 16777216.f);
    }

    int SecondsToSamples(float seconds) const
    {
        const int n = static_cast<int>(seconds * sample_rate_ + .5f);
        return n < 1 ? 1 : n;
    }

    /** Random interval until the next dropout: sparse ~1.2-3 s at low
     *  drive, dense ~0.15-0.8 s at full drive. */
    int IntervalSamples(int c)
    {
        const float r  = RandFloat(c);
        const float lo = 1.2f - 1.05f * drive_;
        const float hi = 3.f - 2.2f * drive_;
        return SecondsToSamples(lo + r * (hi - lo));
    }

    /** Random dropout duration, 20-140 ms. */
    int DurationSamples(int c)
    {
        return SecondsToSamples(.02f + RandFloat(c) * .12f);
    }

    /** Torn-cone crackle: sparse random impulses, density ~2-20/s
     *  scaling with drive, amplitude +/-0.05-0.2 (0 = no impulse this
     *  sample). */
    float Crackle(int c)
    {
        const float rate_hz = 2.f + 18.f * drive_;
        if(RandFloat(c) >= rate_hz * (1.f / sample_rate_))
            return 0.f;
        const float amp = .05f + .15f * RandFloat(c);
        return RandFloat(c) < .5f ? -amp : amp;
    }

    float    sample_rate_;
    float    ramp_inc_;
    float    skew_c_[2];
    float    drive_;
    uint32_t rng_[2];
    float    gain_[2];
    float    gain_target_[2];
    int      idle_left_[2]; /* samples until next dropout (-1: unscheduled) */
    int      drop_left_[2]; /* samples left in the current dropout */
    float    skew_lp_[2];
};

} // namespace chompi

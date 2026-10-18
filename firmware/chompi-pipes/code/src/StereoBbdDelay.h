/** @file StereoBbdDelay.h
 *  @brief Warm, stereo BBD-inspired delay for ORGAN, placed after the
 *  rotary speaker (and its fuzz-output leveling) and before the plate
 *  reverb and master gain.
 *
 *  BBD-inspired DSP, not a physical circuit model: two independent
 *  float delay lines with
 *   - subtle, bounded L/R timing differences (a fixed +/-0.5% stereo
 *     spread plus slow independent sine modulation of each side), so
 *     the stereo image is preserved and gently widened - stereo, NOT
 *     ping-pong (no cross-channel feedback, no mono summing)
 *   - a variable feedback (boot 35%, up to 120%) through a darkening
 *     one-pole low-pass, so repeats get progressively darker; at 100%
 *     echoes ring for many seconds, and past ~100% the loop
 *     self-oscillates into a bounded drone whose pitch follows the
 *     delay time
 *   - a one-pole low-cut/DC blocker inside the loop, so no DC or rumble
 *     accumulates across repeats
 *   - a bounded gentle soft-clip saturation on the written signal
 *     (cubic soft knee, |y| <= 2/3), so loud repeats compress warmly
 *     and even a self-oscillating loop can never run away
 *   - no hiss, no clock noise; self-oscillation happens only as a
 *     deliberate control above 100% feedback.
 *
 *  Controls: mix (0..1, linear complementary crossfade with a ~10 ms
 *  click-free ramp and exact endpoints - settled 0 is bit-exact dry,
 *  settled 1 is wet only), feedback (0..1.2 with the same ~10 ms ramp;
 *  boot 35% is the classic voicing, 1.0 rings for many seconds, past
 *  ~1.0 the delay self-oscillates - always bounded by the
 *  soft-clipper) and delay time (40..500 ms). Time changes
 *  slew the read position through a ~80 ms one-pole, giving the smooth
 *  BBD-like pitch glide of an analog clock change.
 *
 *  The two 128 KiB delay lines are caller-owned (ORGAN places them in
 *  external SDRAM, out of the tight SRAM regions); only filter,
 *  modulation, index and control state lives here. Init() explicitly
 *  initializes ALL state and clears the buffers (on the MCU, call only
 *  after the SDRAM controller is configured - never from a global
 *  constructor, and never on preset loads). The buffers keep running
 *  at dry mix and across parameter changes, so tails and history
 *  survive.
 *
 *  All filter/slew/LFO coefficients are computed in Init() or the
 *  setters; the per-sample hot path is a handful of multiply-adds with
 *  no transcendental calls and no heap. Header-only, no MCU/Daisy
 *  dependencies: native-testable.
 */
#pragma once
#include <stdint.h>
#include <math.h>

namespace chompi
{

class StereoBbdDelay
{
  public:
    /** Delay-line length per channel in float samples (power of two):
     *  2 x 32768 x 4 B = 256 KiB total. 682 ms at 48 kHz - comfortably
     *  covers the 500 ms maximum plus the stereo spread/modulation. */
    static constexpr int kBufferSize = 32768;

    /** Delay-time range and boot default. */
    static constexpr float kMinDelayMs     = 40.f;
    static constexpr float kMaxDelayMs     = 500.f;
    static constexpr float kDefaultDelayMs = 250.f;

    /** Mix ramp length, sample-rate derived (~10 ms). */
    static constexpr float kMixRampSeconds = .010f;

    /** Feedback range: boots settled at 35% (the classic moderate-
     *  repeats voicing, bit-exact the old fixed constant). 1.0 rings
     *  for many seconds; past ~1.0 the midrange loop gain crosses 1
     *  and the delay SELF-OSCILLATES - a dark drone, bounded by the
     *  soft-clipper, whose pitch follows the delay time. The 1.2 max
     *  keeps a playable sweet-spot zone just past the onset. Set via
     *  SetFeedback(). */
    static constexpr float kDefaultFeedback = .35f;
    static constexpr float kMaxFeedback     = 1.2f;
    /** Fixed voicing (implementation constants, not user controls). */
    static constexpr float kDarkenLpHz  = 2800.f; /* loop low-pass */
    static constexpr float kLowCutHz    = 40.f;   /* loop DC blocker */
    static constexpr float kStereoSpread = .01f;  /* +/-0.5% L/R timing */
    static constexpr float kModDepthLMs = .5f;   /* slow clock wobble */
    static constexpr float kModDepthRMs = .7f;
    static constexpr float kModLfoLHz   = .25f;
    static constexpr float kModLfoRHz   = .37f;
    /** One-pole time constant for delay-time changes: the BBD-like
     *  pitch glide while the "clock" slews. */
    static constexpr float kTimeSlewSeconds = .080f;

    /** Initialize every piece of state and clear both delay lines.
     *  `left`/`right` must each point to kBufferSize writable floats.
     *  Safe to call repeatedly. Boots dry (mix 0) at the default time,
     *  fully settled. */
    void Init(float sample_rate, float *left, float *right)
    {
        sample_rate_ = sample_rate;
        buf_l_       = left;
        buf_r_       = right;
        for(int i = 0; i < kBufferSize; i++)
        {
            buf_l_[i] = 0.f;
            buf_r_[i] = 0.f;
        }
        write_ = 0;

        mix_ = mix_target_ = 0.f;
        mix_inc_ = 1.f / (kMixRampSeconds * sample_rate);

        /* feedback boots settled at the 35% default: the same sound as
         * the old fixed constant */
        feedback_     = feedback_target_ = kDefaultFeedback;
        feedback_inc_ = mix_inc_;

        /* loop filters: darkening low-pass + DC-blocker high-pass */
        lp_coeff_ = 1.f - expf(-6.283185307179586f * kDarkenLpHz
                               / sample_rate);
        hp_coeff_ = expf(-6.283185307179586f * kLowCutHz / sample_rate);
        lp_l_ = lp_r_ = 0.f;
        hp_x_l_ = hp_y_l_ = 0.f;
        hp_x_r_ = hp_y_r_ = 0.f;

        /* delay-time slew + hard sample bounds for the read position
         * (time bounds, then buffer capacity) */
        slew_coeff_ = 1.f - expf(-1.f / (kTimeSlewSeconds * sample_rate));
        min_samples_ = kMinDelayMs * .001f * sample_rate;
        max_samples_ = kMaxDelayMs * .001f * sample_rate;
        if(max_samples_ > static_cast<float>(kBufferSize - 2))
            max_samples_ = static_cast<float>(kBufferSize - 2);
        target_ms_     = kDefaultDelayMs;
        target_samples_ = target_ms_ * .001f * sample_rate;
        delay_samples_ = target_samples_; /* boots settled: no glide */

        /* slow per-channel clock modulation: one shared sine table,
         * independent phases/rates */
        for(int i = 0; i < kLfoTableSize; i++)
            lfo_table_[i]
                = sinf(6.283185307179586f * static_cast<float>(i)
                       / kLfoTableSize);
        lfo_inc_l_ = kModLfoLHz * kLfoTableSize / sample_rate;
        lfo_inc_r_ = kModLfoRHz * kLfoTableSize / sample_rate;
        lfo_pos_l_ = 0.f;
        lfo_pos_r_ = kLfoTableSize * .5f; /* anti-phase start */
        mod_l_samples_ = kModDepthLMs * .001f * sample_rate;
        mod_r_samples_ = kModDepthRMs * .001f * sample_rate;
    }

    /** Set the wet mix (clamped 0..1). Retargets the ~10 ms ramp from
     *  the current value; the ramp snaps to exact endpoints, so settled
     *  0 is bit-exact dry and settled 1 is wet only. The delay lines
     *  keep running at every mix (live tails, preserved history). */
    void SetMix(float mix)
    {
        mix_target_ = mix < 0.f ? 0.f : (mix > 1.f ? 1.f : mix);
    }

    float GetMix() const { return mix_target_; }

    /** Set the loop feedback (clamped 0..kMaxFeedback). Retargets the
     *  same ~10 ms click-free ramp as the mix, so feedback sweeps do
     *  not zipper. The boot default is 35%; past ~1.0 the loop
     *  self-oscillates into a bounded, slowly-growing drone, darkened
     *  by the loop filters. */
    void SetFeedback(float feedback)
    {
        feedback_target_ = feedback < 0.f ? 0.f
                           : (feedback > kMaxFeedback ? kMaxFeedback
                                                      : feedback);
    }

    float GetFeedback() const { return feedback_target_; }

    /** Set the delay time in milliseconds (clamped 40..500). The read
     *  position slews to the new time with the ~80 ms pitch-glide
     *  one-pole; nothing is cleared and echoes keep ringing. */
    void SetDelayMs(float ms)
    {
        ms = ms < kMinDelayMs ? kMinDelayMs
                              : (ms > kMaxDelayMs ? kMaxDelayMs : ms);
        target_ms_     = ms;
        target_samples_ = ms * .001f * sample_rate_;
    }

    float GetDelayMs() const { return target_ms_; }

    /** Process one stereo sample. Inputs are taken by value, so the
     *  outputs may alias the inputs (in-place use). The buffers,
     *  filters and modulation run unconditionally - dry bypass keeps
     *  the history alive; only the output crossfade is gated by the
     *  mix. */
    inline void Process(float in_l, float in_r, float &out_l, float &out_r)
    {
        /* advance the mix ramp one sample with exact endpoint snap
         * (float accumulation can land a fraction of an ULP short). */
        const float dmix = mix_target_ - mix_;
        if(dmix > mix_inc_)
            mix_ += mix_inc_;
        else if(dmix < -mix_inc_)
            mix_ -= mix_inc_;
        else
            mix_ = mix_target_;

        /* advance the feedback ramp the same way (click-free moves,
         * exact endpoint snap) */
        const float dfb = feedback_target_ - feedback_;
        if(dfb > feedback_inc_)
            feedback_ += feedback_inc_;
        else if(dfb < -feedback_inc_)
            feedback_ -= feedback_inc_;
        else
            feedback_ = feedback_target_;

        /* slew the base delay time toward its target (pitch glide);
         * snap the last fraction of a sample so the settled base is
         * exact and only the LFO modulation keeps moving. */
        float d = delay_samples_
                  + (target_samples_ - delay_samples_) * slew_coeff_;
        if(fabsf(target_samples_ - d) < .05f)
            d = target_samples_;
        delay_samples_ = d;

        /* per-channel read delays: fixed +/-0.5% stereo spread plus
         * slow independent sine modulation, clamped to the supported
         * time bounds (endpoint-safe at 40/500 ms) */
        const float d_l = ClampRead(d * (1.f - .5f * kStereoSpread)
                                    + mod_l_samples_ * Lfo(lfo_pos_l_));
        const float d_r = ClampRead(d * (1.f + .5f * kStereoSpread)
                                    + mod_r_samples_ * Lfo(lfo_pos_r_));
        lfo_pos_l_ += lfo_inc_l_;
        if(lfo_pos_l_ >= kLfoTableSize)
            lfo_pos_l_ -= kLfoTableSize;
        lfo_pos_r_ += lfo_inc_r_;
        if(lfo_pos_r_ >= kLfoTableSize)
            lfo_pos_r_ -= kLfoTableSize;

        /* interpolated reads (linear; safe wrapping by the power-of-two
         * mask) */
        const float rd_l = Read(buf_l_, d_l);
        const float rd_r = Read(buf_r_, d_r);

        /* darkening low-pass on the reads: the first echo is already
         * dark and every repeat passes through the loop filter again */
        lp_l_ += lp_coeff_ * (rd_l - lp_l_);
        lp_r_ += lp_coeff_ * (rd_r - lp_r_);

        /* DC blocker / low-cut inside the feedback loop */
        const float hp_l = lp_l_ - hp_x_l_ + hp_coeff_ * hp_y_l_;
        hp_x_l_          = lp_l_;
        hp_y_l_          = hp_l;
        const float hp_r = lp_r_ - hp_x_r_ + hp_coeff_ * hp_y_r_;
        hp_x_r_          = lp_r_;
        hp_y_r_          = hp_r;

        /* write input + feedback through the bounded gentle saturation
         * (the loop can never exceed |2/3| at the clipper) */
        buf_l_[write_] = SoftClip(in_l + feedback_ * hp_l);
        buf_r_[write_] = SoftClip(in_r + feedback_ * hp_r);
        write_         = (write_ + 1) & (kBufferSize - 1);

        /* linear complementary crossfade with exact endpoints */
        if(mix_ == 0.f)
        {
            out_l = in_l; /* bit-exact dry */
            out_r = in_r;
        }
        else if(mix_ == 1.f)
        {
            out_l = lp_l_; /* wet only */
            out_r = lp_r_;
        }
        else
        {
            out_l = in_l * (1.f - mix_) + lp_l_ * mix_;
            out_r = in_r * (1.f - mix_) + lp_r_ * mix_;
        }
    }

  private:
    static constexpr int kLfoTableSize = 256;

    /** Bounded gentle saturation: cubic soft knee, slope -> 0 at the
     *  +/-1 knee, bounded |y| <= 2/3. Cheap: no divide, no
     *  transcendental. */
    static float SoftClip(float x)
    {
        if(x > 1.f)
            x = 1.f;
        else if(x < -1.f)
            x = -1.f;
        return x - x * x * x * (1.f / 3.f);
    }

    float ClampRead(float samples) const
    {
        return samples < min_samples_
                   ? min_samples_
                   : (samples > max_samples_ ? max_samples_ : samples);
    }

    /** Fractional read `delay` samples behind the write pointer,
     *  linear interpolation, power-of-two wrap. */
    float Read(const float *buf, float delay) const
    {
        const int   di   = static_cast<int>(delay); /* floor (delay >= 0) */
        const float frac = delay - static_cast<float>(di);
        const int   r0   = (write_ - di) & (kBufferSize - 1);
        const int   r1   = (r0 - 1) & (kBufferSize - 1);
        return buf[r0] + frac * (buf[r1] - buf[r0]);
    }

    /** Sine-table look-up with linear interpolation; pos in 0..255. */
    float Lfo(float pos) const
    {
        const int   i0   = static_cast<int>(pos);
        const float frac = pos - static_cast<float>(i0);
        const int   i1   = (i0 + 1) & (kLfoTableSize - 1);
        return lfo_table_[i0]
               + frac * (lfo_table_[i1] - lfo_table_[i0]);
    }

    float *buf_l_;
    float *buf_r_;
    int    write_;

    float sample_rate_;
    float mix_;
    float mix_target_;
    float mix_inc_;
    float feedback_;
    float feedback_target_;
    float feedback_inc_;

    float lp_coeff_; /* darkening low-pass coefficient */
    float hp_coeff_; /* DC-blocker feedback coefficient */
    float lp_l_, lp_r_;
    float hp_x_l_, hp_y_l_;
    float hp_x_r_, hp_y_r_;

    float slew_coeff_;
    float min_samples_;
    float max_samples_;
    float target_ms_;
    float target_samples_;
    float delay_samples_;

    float lfo_table_[kLfoTableSize];
    float lfo_inc_l_, lfo_inc_r_;
    float lfo_pos_l_, lfo_pos_r_;
    float mod_l_samples_, mod_r_samples_;
};

} // namespace chompi

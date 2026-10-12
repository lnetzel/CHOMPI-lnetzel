// Copyright 2014 Emilie Gillet.
//
// Author: Emilie Gillet (emilie.o.gillet@gmail.com)
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.
//
// See http://creativecommons.org/licenses/MIT/ for more information.
//
// -----------------------------------------------------------------------------
//
/** @file PlateReverb.h
 *  @brief Compact vintage-1960s-inspired plate reverb for CHOMPI ORGAN.
 *
 *  Adapted from chompi-wave's vendored reverb (see
 *  ../../../chompi-wave/code/src/reverb.h): the Griesinger topology
 *  described in the Dattorro paper - 4 allpass diffusers on the input,
 *  then a cross-feedback tank of 2x (2 allpasses + 1 long delay), with
 *  slow LFO modulation of the two long-delay taps. Plate-inspired; the
 *  decay time is user-adjustable (0.5-20 s), the rest of the voicing is
 *  fixed. Not a claim of accurate physical EMT emulation.
 *
 *  Differences from the WAVE original:
 *   - The 64 KiB delay buffer is caller-owned (ORGAN keeps it in SDRAM,
 *     out of the tight SRAM regions); the engine/filter/control state
 *     stays here in normal SRAM.
 *   - Init() explicitly initializes ALL state: delay memory, write
 *     pointer, LFOs, damping histories, input low-cut state, and the
 *     mix/ramp state (the old code left these implicit).
 *   - The wet tank is processed independently and mixed outside the
 *     engine: out = dry * (1 - mix) + wet * mix, a linear complementary
 *     crossfade with exact endpoints (0% is bit-exact dry, 100% is wet
 *     only). The WAVE original blended inside Process with `amount`.
 *   - The mix is ramped (~10 ms, sample-rate derived) with retargeting
 *     from the current value and exact-endpoint snaps.
 *   - Decay (T60) maps seconds to the tank loop feedback, and the input
 *     gain scales with (1 - feedback) so the 16-bit tank keeps constant
 *     headroom - and a constant sustained-tone wet level - across the
 *     whole decay range.
 *   - A ~120 Hz one-pole high-pass on the mono tank send keeps organ low
 *     end from building up/rumbling inside the 16-bit tank.
 *
 *  The tank ALWAYS runs (even at 0% mix or with idle voices), so tails
 *  stay live and no stale state accumulates. No heap allocation, no
 *  per-sample transcendental coefficient recalculation, and no bulk
 *  clearing happens inside Process().
 *
 *  Header-only apart from the caller-owned buffer; the only dependency
 *  is WAVE's fx_engine.h (read-only relative include), so this file is
 *  native-testable on a host compiler.
 */
#pragma once

#include <math.h>
#include <stdint.h>

/* fx_engine.h uses TWOPI_F but does not include DaisySP's dsp.h (in WAVE
 * it is always included after daisysp.h). Provide it for standalone/host
 * use without touching the vendored file. */
#ifndef TWOPI_F
#define TWOPI_F 6.283185307179586f
#endif

#include "../../../chompi-wave/code/src/fx_engine.h"

namespace chompi
{

class PlateReverb
{
  public:
    /** Delay-buffer size in uint16_t elements (64 KiB). The caller owns
     *  the storage; ORGAN places it in SDRAM. */
    static constexpr size_t kDelayBufferSize = 32768;

    /** Mix ramp length, sample-rate derived (~10 ms). */
    static constexpr float kMixRampSeconds = .010f;

    /* Fixed plate voicing (implementation constants, not user controls),
     * except the decay time - see SetDecaySeconds(). */
    static constexpr float kDiffusion = .625f; /**< input/tank AP coeff */
    static constexpr float kDampingLp = .45f;  /**< ~4.5 kHz dark damping */
    static constexpr float kInputGain = .4f;   /**< send gain at the default
                                                *   decay; scales with
                                                *   (1 - feedback) so the
                                                *   16-bit tank keeps
                                                *   constant headroom */
    static constexpr float kWetGain   = 1.f;   /**< wet make-up gain */
    static constexpr float kLowCutHz  = 120.f; /**< tank-send low-cut */
    static constexpr float kLfo1Hz    = .5f;   /**< slow shimmer/chorus */
    static constexpr float kLfo2Hz    = .3f;

    /** Decay (T60) range in seconds; boots at the minimum. */
    static constexpr float kMinDecaySeconds     = .5f;
    static constexpr float kMaxDecaySeconds     = 20.f;
    static constexpr float kDefaultDecaySeconds = kMinDecaySeconds;

    /** Sum of the six tank loop delays in the Memory layout below
     *  (4501+2690+2182 + 6312+2525+2197): one full round trip of the
     *  cross-feedback tank, for the seconds->feedback mapping. */
    static constexpr float kLoopSamples = 20407.f;

    /** Initialize every piece of state. `delay_buffer` must point to
     *  kDelayBufferSize uint16_t elements and must be writable (on the
     *  MCU, call only after the SDRAM controller is configured - never
     *  from a global constructor). Safe to call repeatedly. */
    void Init(float sample_rate, uint16_t *delay_buffer)
    {
        sample_rate_ = sample_rate;
        engine_.Init(delay_buffer); /* buffer pointer + LFO phase/freq */
        engine_.SetLFOFrequency(daisysp::LFO_1, kLfo1Hz / sample_rate);
        engine_.SetLFOFrequency(daisysp::LFO_2, kLfo2Hz / sample_rate);
        engine_.Clear(); /* zero delay memory + write pointer */

        lp_decay_1_ = 0.f;
        lp_decay_2_ = 0.f;
        hp_state_   = 0.f;
        hp_coeff_
            = 1.f - expf(-6.283185307179586f * kLowCutHz / sample_rate);

        mix_ = mix_target_ = 0.f; /* boots exact dry */
        mix_inc_ = 1.f / (kMixRampSeconds * sample_rate);

        SetDecaySeconds(kDefaultDecaySeconds); /* feedback_ + input_gain_ */
    }

    /** Set the wet mix (clamped 0..1). Retargets the ~10 ms ramp from
     *  the current value; the ramp snaps to exact endpoints, so settled
     *  0 is bit-exact dry and settled 1 is wet only. */
    void SetMix(float mix)
    {
        mix_target_ = mix < 0.f ? 0.f : (mix > 1.f ? 1.f : mix);
    }

    float GetMix() const { return mix_target_; }

    /** Set the tank decay time (T60), clamped to 0.5..20 s. One full
     *  round trip of the cross-feedback tank applies feedback^2 over
     *  kLoopSamples samples, so feedback = 10^(-1.5 * T_roundtrip / T60).
     *  The input gain scales with (1 - feedback): the tank's worst-case
     *  buildup and the sustained-tone wet level stay the same at every
     *  decay setting (only the tail duration changes). Control-rate
     *  only: two powf per call, nothing per-sample. */
    void SetDecaySeconds(float seconds)
    {
        seconds = seconds < kMinDecaySeconds
                      ? kMinDecaySeconds
                      : (seconds > kMaxDecaySeconds ? kMaxDecaySeconds
                                                    : seconds);
        decay_seconds_   = seconds;
        const float t_rt = kLoopSamples / sample_rate_;
        feedback_        = powf(10.f, -1.5f * t_rt / seconds);
        const float fb_default
            = powf(10.f, -1.5f * t_rt / kDefaultDecaySeconds);
        input_gain_
            = kInputGain * (1.f - feedback_) / (1.f - fb_default);
    }

    float GetDecaySeconds() const { return decay_seconds_; }

    /** Process one stereo sample. Inputs are taken by value, so the
     *  outputs may alias the inputs (in-place use). The tank runs
     *  unconditionally; only the crossfade is gated by the mix. */
    inline void Process(float in_l, float in_r, float &out_l, float &out_r)
    {
        /* advance the mix ramp one sample. Snap when within one
         * increment of the target: float accumulation of mix_inc_ can
         * land a fraction of an ULP short of the endpoint, and this
         * guarantees the endpoints are reached EXACTLY (settled 0 is
         * bit-exact dry, settled 1 is wet only). */
        const float dmix = mix_target_ - mix_;
        if(dmix > mix_inc_)
            mix_ += mix_inc_;
        else if(dmix < -mix_inc_)
            mix_ -= mix_inc_;
        else
            mix_ = mix_target_;

        /* Griesinger/Dattorro tank, as vendored in WAVE's reverb.h:
         * 4 AP diffusers on the input, then a loop of 2x (2AP + 1Delay);
         * slow LFO modulation of the long-delay taps. */
        typedef E::Reserve<150,
            E::Reserve<214,
                E::Reserve<319,
                    E::Reserve<527,
                        E::Reserve<2182,
                            E::Reserve<2690,
                                E::Reserve<4501,
                                    E::Reserve<2525,
                                        E::Reserve<2197,
                                            E::Reserve<6312>>>>>>>>>>
            Memory;
        E::DelayLine<Memory, 0> ap1;
        E::DelayLine<Memory, 1> ap2;
        E::DelayLine<Memory, 2> ap3;
        E::DelayLine<Memory, 3> ap4;
        E::DelayLine<Memory, 4> dap1a;
        E::DelayLine<Memory, 5> dap1b;
        E::DelayLine<Memory, 6> del1;
        E::DelayLine<Memory, 7> dap2a;
        E::DelayLine<Memory, 8> dap2b;
        E::DelayLine<Memory, 9> del2;
        typename E::Context c;

        const float kap = kDiffusion;
        const float klp = kDampingLp;
        const float krt = feedback_;

        float lp_1 = lp_decay_1_;
        float lp_2 = lp_decay_2_;

        float wet_l, wet_r;
        float apout = 0.f;
        engine_.Start(&c);

        /* normalized mono send, low-cut to keep LF rumble out of the
         * 16-bit tank */
        c.Read(.5f * (in_l + in_r), input_gain_);
        c.Hp(hp_state_, hp_coeff_);

        /* diffuse through 4 allpasses */
        c.Read(ap1 TAIL, kap);
        c.WriteAllPass(ap1, -kap);
        c.Read(ap2 TAIL, kap);
        c.WriteAllPass(ap2, -kap);
        c.Read(ap3 TAIL, kap);
        c.WriteAllPass(ap3, -kap);
        c.Read(ap4 TAIL, kap);
        c.WriteAllPass(ap4, -kap);
        c.Write(apout);

        /* main reverb loop, side 1 -> left wet */
        c.Load(apout);
        c.Interpolate(del2, 6261.f, daisysp::LFO_2, 50.f, krt);
        c.Lp(lp_1, klp);
        c.Read(dap1a TAIL, -kap);
        c.WriteAllPass(dap1a, kap);
        c.Read(dap1b TAIL, kap);
        c.WriteAllPass(dap1b, -kap);
        c.Write(del1, 2.0f);
        c.Write(wet_l, 0.0f);

        /* side 2 -> right wet */
        c.Load(apout);
        c.Interpolate(del1, 4460.f, daisysp::LFO_1, 40.f, krt);
        c.Lp(lp_2, klp);
        c.Read(dap2a TAIL, kap);
        c.WriteAllPass(dap2a, -kap);
        c.Read(dap2b TAIL, -kap);
        c.WriteAllPass(dap2b, kap);
        c.Write(del2, 2.0f);
        c.Write(wet_r, 0.0f);

        lp_decay_1_ = lp_1;
        lp_decay_2_ = lp_2;

        /* linear complementary crossfade, exact endpoints: at mix_ == 0
         * the dry factor is exactly 1 and the wet term exactly 0; at
         * mix_ == 1 vice versa. */
        const float wl = wet_l * kWetGain;
        const float wr = wet_r * kWetGain;
        out_l = in_l * (1.f - mix_) + wl * mix_;
        out_r = in_r * (1.f - mix_) + wr * mix_;
    }

  private:
    typedef daisysp::FxEngine<kDelayBufferSize, daisysp::FORMAT_16_BIT> E;
    E engine_;

    float lp_decay_1_; /**< tank damping histories */
    float lp_decay_2_;
    float hp_state_;   /**< tank-send low-cut state */
    float hp_coeff_;

    float mix_;        /**< current (ramped) mix */
    float mix_target_;
    float mix_inc_;    /**< per-sample ramp step, sample-rate derived */

    float sample_rate_;
    float decay_seconds_; /**< current T60 */
    float feedback_;      /**< tank loop feedback, from decay_seconds_ */
    float input_gain_;    /**< send gain, scaled by (1 - feedback_) */
};

} // namespace chompi

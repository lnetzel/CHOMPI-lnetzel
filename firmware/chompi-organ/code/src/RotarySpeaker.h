/** @file RotarySpeaker.h
 *  @brief 60s Hammond-style rotary speaker (Leslie) simulation (DSP-6).
 *
 *  An 800 Hz one-pole crossover splits the mono organ mix into a drum band
 *  (low) and a horn band (high). Two virtual mics 180 degrees apart turn
 *  each band into stereo:
 *   - Horn mic: Doppler pitch wobble via a modulated fractional delay tap
 *     plus sinusoidal amplitude modulation, both locked to the horn phase
 *     (the real horn does the same: loudest and pitch-bent when it points
 *     at the mic)
 *   - Drum: deep soft AM throb (one squared-sine pulse per revolution),
 *     no Doppler (bass wavelengths barely pitch-shift), running ~13%
 *     slower than the horn (mechanical slip), so the two rotors beat
 *     against each other like the real cabinet
 *  Amount 0 is an exact bypass. The amount also ramps the rotor speed
 *  from a slow chorale-like swirl to the classic fast tremolo
 *  (2 -> 6.8 Hz), like a Leslie spinning up.
 */
#pragma once
#include "daisysp.h"

namespace chompi
{

class RotarySpeaker
{
  public:
    void Init(float sample_rate)
    {
        sample_rate_ = sample_rate;
        amount_      = amount_target_ = 0.f;
        horn_ph_     = 0.f;
        drum_ph_     = .25f; /* horn and drum never lock mechanically */
        lp_          = 0.f;
        lp_coeff_    = 1.f - expf(-kTwoPi * kCrossoverHz / sample_rate);
        wr_          = 0;
        for(int i = 0; i < kDlyLen; i++)
            dly_[i] = 0.f;
    }

    void SetAmount(float a)
    {
        amount_target_ = a < 0.f ? 0.f : (a > 1.f ? 1.f : a);
    }

    float GetAmount() const { return amount_target_; }

    /** Mono in -> stereo out, one sample. Amount 0 is an exact bypass. */
    inline void Process(float in, float &out_l, float &out_r)
    {
        daisysp::fonepole(amount_, amount_target_, .001f); /* ~1 ms slew */
        if(amount_ <= 0.f)
        {
            out_l = in;
            out_r = in;
            return;
        }

        /* 800 Hz crossover: lows to the drum, highs to the horn */
        lp_ += lp_coeff_ * (in - lp_);
        const float low  = lp_;
        const float high = in - low;

        /* Rotor speed ramps with the amount (Leslie spin-up feel) */
        const float horn_hz
            = kHornRateMin + (kHornRateMax - kHornRateMin) * amount_;
        horn_ph_ += horn_hz / sample_rate_;
        if(horn_ph_ >= 1.f)
            horn_ph_ -= 1.f;
        drum_ph_ += horn_hz * kDrumRatio / sample_rate_;
        if(drum_ph_ >= 1.f)
            drum_ph_ -= 1.f;

        dly_[wr_] = high; /* horn band into the shared Doppler delay */

        const float dop = kDopplerMs * .001f * sample_rate_ * amount_;
        const float ham = kHornAmDepth * amount_;
        const float dam = kDrumAmDepth * amount_;

        /* two mics on opposite sides of the cabinet (180 deg apart) */
        out_l = HornMic(horn_ph_, dop, ham) + low * DrumGain(drum_ph_, dam);
        out_r = HornMic(horn_ph_ + .5f, dop, ham)
                + low * DrumGain(drum_ph_ + .5f, dam);

        wr_ = (wr_ + 1) & (kDlyLen - 1);
    }

  private:
    static constexpr float kTwoPi       = 6.283185307179586f;
    static constexpr float kCrossoverHz = 800.f;
    static constexpr float kHornRateMin = 2.f;  /* Hz at low amounts */
    static constexpr float kHornRateMax = 6.8f; /* classic fast tremolo */
    static constexpr float kDrumRatio   = .87f; /* drum slips behind horn */
    static constexpr float kHornAmDepth = .45f;
    static constexpr float kDrumAmDepth = .7f;
    static constexpr float kDopplerMs   = .4f;  /* max delay wobble */
    static constexpr float kBaseDlyMs   = 1.f;  /* horn-to-mic distance */
    static constexpr int   kDlyLen      = 256;  /* 5.3 ms @ 48 kHz, pow2 */

    /** One virtual horn mic: Doppler delay tap + AM sharing the phase. */
    inline float HornMic(float phase, float dop, float am) const
    {
        phase -= floorf(phase);
        const float s  = sinf(phase * kTwoPi);
        /* delay swings +/-dop around the base delay (Doppler pitch shift) */
        float rd = static_cast<float>(wr_)
                   - (kBaseDlyMs * .001f * sample_rate_
                      + dop * (.5f + .5f * s));
        while(rd < 0.f)
            rd += static_cast<float>(kDlyLen);
        const int   i0   = static_cast<int>(rd) & (kDlyLen - 1);
        const float frac = rd - floorf(rd);
        const float tap
            = dly_[i0] + frac * (dly_[(i0 + 1) & (kDlyLen - 1)] - dly_[i0]);
        /* loudest when the horn points at the mic */
        return tap * (1.f - am + am * (.5f + .5f * s));
    }

    /** Drum rotor: AM throb only, one soft (squared) pulse per revolution. */
    inline float DrumGain(float phase, float am) const
    {
        phase -= floorf(phase);
        const float p = .5f + .5f * sinf(phase * kTwoPi);
        return 1.f - am + am * p * p;
    }

    float sample_rate_;
    float amount_, amount_target_;
    float horn_ph_, drum_ph_;
    float lp_, lp_coeff_;
    int   wr_;
    float dly_[kDlyLen];
};

} // namespace chompi

/** @file DriftModulator.h
 *  @brief Slow, free-running "organic drift" modulator for ORGAN's
 *  Modulation Mode.
 *
 *  Twelve independent instances (4 pipes x {volume, Character, pipe
 *  length}) run continuously in the engine, giving each pipe's level,
 *  Character and pitch a living, slow movement. An optional
 *  per-instance rate multiplier (Init's third argument) lets the pipes
 *  drift at audibly different speeds. The output u in [0,1] sweeps
 *  between the window edges with a smooth raised-cosine shape (no
 *  corners):
 *
 *      rising half:  u = (1 - cos(pi * phase)) / 2   (0 -> 1)
 *      falling half: u = (1 + cos(pi * phase)) / 2   (1 -> 0)
 *
 *  Each half draws its own random duration from 2.5..10 s, so every full
 *  bottom->top->bottom cycle takes 5..20 s and is asymmetric. The slope
 *  is zero at both edges, so reversing direction never clicks.
 *
 *  Randomness comes from one xorshift32 PRNG per instance (distinct
 *  seeds, no heap, no shared state); Init() randomizes the start phase
 *  and direction so the pipes are desynchronized from boot. The
 *  modulator is free-running: it advances once per audio block
 *  regardless of mode or modulation amount, so changing the amount
 *  never restarts it. Cost is one cosf per instance per block
 *  (8 x 2 kHz - negligible).
 *
 *  Header-only, no MCU/Daisy dependencies: native-testable.
 */
#pragma once
#include <stdint.h>
#include <math.h>

namespace chompi
{

class DriftModulator
{
  public:
    /** Half-swing duration bounds in seconds: each rising or falling
     *  half draws its own random duration from this range, so a full
     *  cycle takes 2 x [kMinHalfSeconds..kMaxHalfSeconds] = 5..20 s. */
    static constexpr float kMinHalfSeconds = 2.5f;
    static constexpr float kMaxHalfSeconds = 10.f;

    /** Modulation window half-width at 100% amount (+/-20%, i.e. a 40%
     *  total full-scale window); the amount scales the half-width. */
    static constexpr float kFullScaleHalfWidth = .20f;

    /** seed must be distinct per instance; 0 is remapped (xorshift32's
     *  all-zero state is a fixed point). The start phase, direction and
     *  first half-duration are all drawn from the PRNG. `rate` scales
     *  the drift speed (every drawn duration is divided by it): 1 is
     *  the base rate, 2 drifts twice as fast. */
    void Init(float sample_rate, uint32_t seed, float rate = 1.f)
    {
        sample_rate_ = sample_rate;
        rate_        = rate > 0.f ? rate : 1.f;
        state_       = seed != 0u ? seed : 0x9E3779B9u;
        phase_       = RandFloat(); /* randomized start phase... */
        rising_      = (Next() & 1u) != 0u; /* ...and direction */
        NewDuration();
        ComputeValue();
    }

    /** Advance by one audio block (block-rate; u is constant within a
     *  block). At a half boundary the overshoot phase carries into the
     *  next half, keeping long-term timing exact. */
    void Advance(int num_samples)
    {
        if(num_samples <= 0)
            return;
        phase_ += static_cast<float>(num_samples) * phase_inc_;
        while(phase_ >= 1.f)
        {
            phase_ -= 1.f;
            rising_ = !rising_;
            NewDuration();
        }
        ComputeValue();
    }

    /** Current modulator output, 0..1. */
    float Value() const { return value_; }

    /** Map a modulator output u in [0,1] onto a normalized parameter
     *  center (0..1) with a modulation amount (0..1): the modulated
     *  value swings inside the window [center-d, center+d] with the
     *  half-width d = kFullScaleHalfWidth * amount01, the window SLID
     *  (not clipped) to stay inside [0,1]:
     *      lo < 0 -> [0, 2d];  hi > 1 -> [1-2d, 1]
     *  so a 95% center at full amount swings 60..100% and a 0% center
     *  swings 0..40% (the pipe becomes audible). The modulated value is
     *  lo + (hi-lo) * u, always within [0,1], with constant width 2d.
     *  Amount 0 returns the center exactly (bit-exact unmodulated
     *  behavior). Pure function, unit-testable. */
    static float ApplyWindow(float center, float amount01, float u)
    {
        const float d  = kFullScaleHalfWidth * amount01;
        float       lo = center - d;
        float       hi = center + d;
        if(lo < 0.f)
        {
            lo = 0.f;
            hi = 2.f * d;
        }
        else if(hi > 1.f)
        {
            hi = 1.f;
            lo = 1.f - 2.f * d;
        }
        return lo + (hi - lo) * u;
    }

  private:
    static constexpr float kPi = 3.14159265358979f;

    /** xorshift32: deterministic, no heap, no shared state. */
    uint32_t Next()
    {
        uint32_t s = state_;
        s ^= s << 13;
        s ^= s >> 17;
        s ^= s << 5;
        state_ = s;
        return s;
    }

    /** Uniform float in [0,1). */
    float RandFloat()
    {
        return static_cast<float>(Next()) * (1.f / 4294967296.f);
    }

    /** Draw a new random half duration and derive the per-sample phase
     *  increment from it; the per-instance rate divides the duration. */
    void NewDuration()
    {
        const float seconds
            = kMinHalfSeconds
              + (kMaxHalfSeconds - kMinHalfSeconds) * RandFloat();
        phase_inc_ = rate_ / (seconds * sample_rate_);
    }

    void ComputeValue()
    {
        /* Raised cosine, zero slope at both edges; the falling half
         * mirrors the rising one so direction changes are corner-free. */
        const float rise = .5f * (1.f - cosf(kPi * phase_));
        value_           = rising_ ? rise : 1.f - rise;
    }

    float    sample_rate_;
    float    rate_;      /* speed multiplier, 1 = base rate */
    uint32_t state_;
    float    phase_;     /* position within the current half, 0..1 */
    float    phase_inc_; /* per-sample phase advance */
    float    value_;     /* last computed u, 0..1 */
    bool     rising_;    /* current half direction */
};

} // namespace chompi

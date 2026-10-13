/** @file Vibrato.h
 *  @brief Global pitch vibrato for ORGAN: one shared, free-running 6 Hz LFO.
 *
 *  The depth is an integer 0..50 meaning +/-0..50 cents of peak pitch
 *  deviation (one-cent steps; depth 50 is one semitone peak-to-peak).
 *  The LFO runs continuously, independent of note state, so every voice
 *  sees the same instantaneous pitch multiplier:
 *
 *      mult = 2^(depth_cents/1200 * sin(2*pi*phase))
 *
 *  Depth 0 is an exact no-vibrato bypass: Process() returns exactly 1.f
 *  and performs no transcendental work. Changing depth mid-cycle only
 *  scales the deviation - the phase is never reset, so there are no
 *  clicks and no accumulated drift; the oscillator frequencies are
 *  recomputed from their stored base frequencies every sample (see
 *  OrganEngine.h), never compounded.
 *
 *  Per-sample cost is bounded and shared across all voices: one sinf +
 *  one exp2f per sample total (never inside the per-oscillator loop).
 *
 *  Header-only, no MCU/Daisy dependencies: native-testable.
 */
#pragma once
#include <stdint.h>
#include <math.h>

namespace chompi
{

class Vibrato
{
  public:
    /** Depth range: 51 integer values 0..50, one cent each. */
    static constexpr int kMaxDepthCents = 50;

    /** Fixed LFO rate. */
    static constexpr float kRateHz = 6.f;

    void Init(float sample_rate)
    {
        sample_rate_ = sample_rate;
        phase_inc_   = kRateHz / sample_rate;
        phase_       = 0.f;
        depth_       = 0; /* boot: exact no-vibrato */
    }

    /** Set the peak pitch deviation in cents (0..kMaxDepthCents,
     *  clamped). Exact endpoints: 0 is exactly dry. */
    void SetDepth(int cents)
    {
        depth_ = static_cast<uint8_t>(
            cents < 0 ? 0 : (cents > kMaxDepthCents ? kMaxDepthCents : cents));
    }

    int GetDepth() const { return depth_; }

    /** Advance the shared LFO by one sample and return this sample's
     *  pitch multiplier 2^(depth/1200 * sin(2*pi*phase)). Free-running:
     *  phase advances every sample regardless of note state; exactly
     *  1.f whenever depth is 0. */
    float Process()
    {
        phase_ += phase_inc_;
        if(phase_ >= 1.f)
            phase_ -= 1.f;
        if(depth_ == 0)
            return 1.f;
        const float s = sinf(6.28318530717959f * phase_);
        return exp2f(s * (static_cast<float>(depth_) * (1.f / 1200.f)));
    }

    /** Current LFO phase in cycles, 0..1 (tests/diagnostics). */
    float Phase() const { return phase_; }

  private:
    float   sample_rate_;
    float   phase_;
    float   phase_inc_;
    uint8_t depth_;
};

} // namespace chompi

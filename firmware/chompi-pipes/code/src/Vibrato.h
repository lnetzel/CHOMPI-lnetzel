/** @file Vibrato.h
 *  @brief Global pitch vibrato for ORGAN: one shared, free-running 5 Hz LFO
 *  with an optional note-on delay envelope.
 *
 *  The depth is an integer 0..50 meaning +/-0..50 cents of peak pitch
 *  deviation (one-cent steps; depth 50 is one semitone peak-to-peak).
 *  The LFO runs continuously, independent of note state, so every voice
 *  sees the same instantaneous pitch multiplier:
 *
 *      mult = 2^(depth*env/1200 * sin(2*pi*phase))
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
 *  Delay envelope: Trigger() (a note-on from an all-notes-released
 *  state, see OrganEngine::NoteOn) holds the multiplier at exactly 1.f
 *  for the configured delay (0..5 s in 100 ms steps), then fades the
 *  depth in linearly over a fixed 250 ms ramp to the full set depth.
 *  The effective deviation is depth*env: env 1 is a bit-exact multiply,
 *  so the settled sound is identical to the undelayed one, and the
 *  delay window itself is exact dry with no transcendental work.
 *  Delay 0 (boot default, init patch) disables the envelope entirely:
 *  Trigger() is a no-op and Process() behaves exactly as before.
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
    static constexpr float kRateHz = 5.f;

    /** Fixed fade-in time once a triggered delay elapses. */
    static constexpr float kRampMs = 250.f;

    void Init(float sample_rate)
    {
        sample_rate_   = sample_rate;
        phase_inc_     = kRateHz / sample_rate;
        phase_         = 0.f;
        depth_         = 0; /* boot: exact no-vibrato */
        ramp_inc_      = 1000.f / (kRampMs * sample_rate);
        delay_samples_ = 0; /* boot: delay envelope off */
        delay_count_   = 0;
        env_           = 1.f;
    }

    /** Set the peak pitch deviation in cents (0..kMaxDepthCents,
     *  clamped). Exact endpoints: 0 is exactly dry. */
    void SetDepth(int cents)
    {
        depth_ = static_cast<uint8_t>(
            cents < 0 ? 0 : (cents > kMaxDepthCents ? kMaxDepthCents : cents));
    }

    int GetDepth() const { return depth_; }

    /** Set the note-on delay in milliseconds (0..5000, clamped). 0 ms
     *  disables the envelope: Trigger() becomes a no-op and the
     *  multiplier is exactly the undelayed one again (env forced to
     *  exactly 1, so the depth*env multiply is bit-exact). Changing the
     *  delay mid-envelope only retargets the next Trigger(); the
     *  in-flight envelope plays out. */
    void SetDelayMs(float ms)
    {
        if(ms < 0.f)
            ms = 0.f;
        if(ms > 5000.f)
            ms = 5000.f;
        delay_samples_ = static_cast<uint32_t>(ms * sample_rate_ * .001f
                                               + .5f);
        if(delay_samples_ == 0)
        {
            delay_count_ = 0;
            env_         = 1.f;
        }
    }

    float GetDelayMs() const
    {
        return static_cast<float>(delay_samples_) * 1000.f / sample_rate_;
    }

    /** Restart the delay envelope (note-on from an all-notes-released
     *  state). No-op when the delay is 0: an unset delay never dips the
     *  vibrato. */
    void Trigger()
    {
        if(delay_samples_ == 0)
            return;
        delay_count_ = delay_samples_;
        env_         = 0.f;
    }

    /** Current envelope value 0..1 (tests/diagnostics): 0 through the
     *  delay window, linear up over kRampMs, exactly 1 at rest. */
    float DelayEnv() const { return env_; }

    /** Advance the shared LFO by one sample and return this sample's
     *  pitch multiplier 2^(depth*env/1200 * sin(2*pi*phase)). Free-
     *  running: phase advances every sample regardless of note state;
     *  exactly 1.f whenever depth is 0 or the delay window is running. */
    float Process()
    {
        phase_ += phase_inc_;
        if(phase_ >= 1.f)
            phase_ -= 1.f;
        if(delay_count_ > 0)
        {
            /* triggered delay window: exactly dry, no transcendental
             * work (the LFO phase keeps advancing, free-running) */
            delay_count_--;
            return 1.f;
        }
        if(env_ < 1.f)
        {
            /* 250 ms linear fade-in; the clamp lands on exactly 1, so
             * the settled depth*env multiply below is bit-exact */
            env_ += ramp_inc_;
            if(env_ > 1.f)
                env_ = 1.f;
        }
        if(depth_ == 0)
            return 1.f;
        const float s = sinf(6.28318530717959f * phase_);
        return exp2f(s * (static_cast<float>(depth_) * env_
                          * (1.f / 1200.f)));
    }

    /** Current LFO phase in cycles, 0..1 (tests/diagnostics). */
    float Phase() const { return phase_; }

  private:
    float    sample_rate_;
    float    phase_;
    float    phase_inc_;
    float    ramp_inc_;      /* 1/(kRampMs in samples) */
    float    env_;           /* delay envelope 0..1 (1 = full depth) */
    uint32_t delay_samples_; /* configured delay, 0 = envelope off */
    uint32_t delay_count_;   /* samples left in the delay window */
    uint8_t  depth_;
};

} // namespace chompi

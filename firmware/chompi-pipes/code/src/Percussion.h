/** @file Percussion.h
 *  @brief CPU-conscious Hammond-style percussion for ORGAN (shared decay
 *  envelope + enable transition + per-voice click-prevention gates).
 *
 *  Hammond-inspired percussion articulation: a decaying burst of the
 *  2 2/3' pipe (the third-harmonic-like registration, equal-tempered at
 *  2^(19/12) x the fundamental - reusing the existing pipe instead of an
 *  exact 3x harmonic is an accepted tuning/CPU trade-off) added to each
 *  sounding voice. One SHARED exponential decay envelope drives all
 *  voices; there is no per-voice percussion oscillator or envelope, so
 *  the per-sample cost is a few multiplies total, never per-voice
 *  transcendental work.
 *
 *  Classic single triggering (the engine owns the policy): only a
 *  note-on from an all-notes-released state calls Trigger(); legato
 *  notes and chord members share the remaining tail instead of
 *  restarting it. Enabling percussion while notes are held synthesizes
 *  no attack - the next trigger waits for an all-released transition.
 *
 *  Fixed sound constants (implementation defaults, not measured Hammond
 *  circuit timings):
 *   - 1 ms attack smoothing on trigger (linear, click-free restart
 *     even mid-tail)
 *   - 60 ms half-life exponential decay (-60 dB after 600 ms), then an
 *     explicit snap to exact zero: no denormals, no residual work
 *   - 5 ms linear fades for the enable transition and the per-voice
 *     note-release / steal gates
 *
 *  Settled-off is an exact bypass: Level() and EnableRamp() are both
 *  exactly 0, so the engine's rendering and normalization stay
 *  bit-identical to the percussion-less path.
 *
 *  All sample-rate-dependent coefficients are computed in Init(), never
 *  in the sample loop. Header-only, no MCU/Daisy dependencies:
 *  native-testable.
 */
#pragma once
#include <stdint.h>
#include <math.h>

namespace chompi
{

class Percussion
{
  public:
    /** Peak percussion amplitude as a fraction of a full-scale single
     *  pipe (before the engine's mix scaling). 0.75 = +50% over the
     *  original 0.5 per user request (too quiet against the pipes);
     *  the engine's normalization bound is derived from this value. */
    static constexpr float kPeakGain = .75f;
    /** Trigger attack smoothing. */
    static constexpr float kAttackSeconds = .001f; /* 1 ms */
    /** Exponential decay half-life. */
    static constexpr float kHalfLifeSeconds = .060f; /* 60 ms */
    /** Enable toggle and per-voice release/steal gate fade. */
    static constexpr float kFadeSeconds = .005f; /* 5 ms */
    /** Decay snaps to exact zero below this (-60 dB): no denormals. */
    static constexpr float kSettleLevel = .001f;

    /** Compute every sample-rate-dependent coefficient. Safe to call
     *  repeatedly. Boots disabled and fully settled (exact bypass). */
    void Init(float sample_rate)
    {
        sample_rate_ = sample_rate;
        attack_inc_  = 1.f / (kAttackSeconds * sample_rate);
        fade_inc_    = 1.f / (kFadeSeconds * sample_rate);
        /* per-sample multiplier for the 60 ms half-life: 2^(-t/T) */
        decay_mult_  = powf(2.f, -1.f / (kHalfLifeSeconds * sample_rate));
        enabled_     = 0;
        enable_ramp_ = 0.f;
        decay_       = 0.f;
        attack_      = 1.f; /* no attack ramp before the first trigger */
    }

    /** Toggle percussion on/off. The audible transition is the 5 ms
     *  enable ramp (click-free); toggling never retriggers the decay. */
    void SetEnabled(bool on) { enabled_ = on ? 1 : 0; }

    bool IsEnabled() const { return enabled_ != 0; }

    /** Restart the shared decay from its peak. The 1 ms attack ramp
     *  restarts from zero, so a retrigger mid-tail never clicks. The
     *  engine calls this only for a note-on from an all-notes-released
     *  state while enabled (classic single triggering). */
    void Trigger()
    {
        decay_  = 1.f;
        attack_ = 0.f;
    }

    /** Advance the shared state by ONE sample (never once per voice)
     *  and return the percussion envelope level 0..1 (decay x attack
     *  smoothing, WITHOUT the enable ramp - the engine folds that in
     *  together with the normalization interpolation). Exactly 0 when
     *  settled, so the engine can bypass without changing its output. */
    float Process()
    {
        /* 5 ms enable ramp, exact endpoints */
        if(enabled_)
        {
            if(enable_ramp_ < 1.f)
            {
                enable_ramp_ += fade_inc_;
                if(enable_ramp_ > 1.f)
                    enable_ramp_ = 1.f;
            }
        }
        else if(enable_ramp_ > 0.f)
        {
            enable_ramp_ -= fade_inc_;
            if(enable_ramp_ < 0.f)
                enable_ramp_ = 0.f;
        }

        /* 1 ms trigger attack smoothing */
        if(attack_ < 1.f)
        {
            attack_ += attack_inc_;
            if(attack_ > 1.f)
                attack_ = 1.f;
        }

        /* exponential decay, settled to exact zero at -60 dB */
        if(decay_ > 0.f)
        {
            decay_ *= decay_mult_;
            if(decay_ < kSettleLevel)
                decay_ = 0.f;
        }

        return decay_ * attack_;
    }

    /** Enable transition 0..1, exact endpoints when settled. The engine
     *  uses this both to fade the percussion sound on a toggle and to
     *  interpolate the mix normalization between the 24-unit (off) and
     *  28.5-unit (on) bounds. */
    float EnableRamp() const { return enable_ramp_; }

    /** Raw decay state 0..1 (tests/diagnostics). */
    float Decay() const { return decay_; }

    /** True while any percussion work can be audible or in transition
     *  (enabled, enable ramp mid-fade, or decay tail alive). When false
     *  the engine skips the whole percussion path bit-exactly. */
    bool PathActive() const
    {
        return enabled_ != 0 || enable_ramp_ > 0.f || decay_ > 0.f;
    }

    /** Per-voice click-prevention gate: ramps up over ~1 ms while the
     *  voice sounds (ATTACK/SUSTAIN), down over ~5 ms on release or
     *  steal, so the raw pipe tap never enters or leaves the percussion
     *  mix with a step. Independent of the pipe envelopes' own
     *  attack/release times. */
    float AdvanceGate(float gate, bool sounding) const
    {
        if(sounding)
        {
            gate += attack_inc_;
            return gate > 1.f ? 1.f : gate;
        }
        gate -= fade_inc_;
        return gate < 0.f ? 0.f : gate;
    }

  private:
    float            sample_rate_;
    float            attack_inc_;  /* 1 ms linear ramp step */
    float            fade_inc_;    /* 5 ms linear ramp step */
    float            decay_mult_;  /* per-sample 60 ms half-life factor */
    volatile uint8_t enabled_;     /* ISR-written, main-loop LED read */
    float            enable_ramp_;
    float            decay_;
    float            attack_;
};

} // namespace chompi

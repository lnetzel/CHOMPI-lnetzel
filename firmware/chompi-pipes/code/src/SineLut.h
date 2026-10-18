/** @file SineLut.h
 *  @brief Lookup-table sine oscillator: the OrganEngine voice-core
 *  replacement for daisysp::Oscillator (WAVE_SIN).
 *
 *  daisysp::Oscillator calls libm sinf() per sample; with 6 voices x 4
 *  pipes that is 24 sinf per sample and one of the largest steady-state
 *  CPU costs of the engine. This oscillator returns the same sine from a
 *  2048-entry table with linear interpolation: max abs error ~1.2e-6
 *  (-118 dB), far below the analog output noise floor, and thousands
 *  of times smaller than
 *  TonewheelCharacter's deliberate per-note variation - so the swap is
 *  audibly transparent.
 *
 *  Behavior matches the daisysp oscillator semantics the engine relies
 *  on:
 *   - free-running phase, NEVER reset by SetFreq() (click-free,
 *     phase-continuous retunes for vibrato and note changes)
 *   - SetFreq() is one multiply (hz * sr_recip), cheap enough for the
 *     per-sample vibrato path
 *   - output amplitude is always full scale; the engine scales
 *     externally (daisysp SetAmp was only ever set to 1)
 *
 *  Phase is kept in cycles [0,1) instead of daisysp's radians [0,2pi),
 *  which also accumulates less float rounding over very long sustains.
 *
 *  The table is built once with sinf() at the first Init() call (boot,
 *  single-threaded); 8 KiB static .bss, no heap, no transcendental work
 *  per sample. Header-only, no MCU/Daisy dependencies: native-testable.
 */
#pragma once
#include <stdint.h>
#include <math.h>

namespace chompi
{

class SineLut
{
  public:
    /** Table length; power of two so the read index wraps with a mask. */
    static constexpr int kTableSize = 2048;

    void Init(float sample_rate)
    {
        table_     = Table();
        sr_recip_  = 1.f / sample_rate;
        phase_     = 0.f;
        phase_inc_ = 0.f;
    }

    /** Phase-continuous retune: updates the increment only. */
    void SetFreq(float hz) { phase_inc_ = hz * sr_recip_; }

    /** One interpolated sine sample; phase advances and wraps in [0,1). */
    float Process()
    {
        const float x    = phase_ * static_cast<float>(kTableSize);
        const int   i0   = static_cast<int>(x); /* truncation, x >= 0 */
        const float frac = x - static_cast<float>(i0);
        const int   i1   = (i0 + 1) & (kTableSize - 1);
        const float out  = table_[i0] + frac * (table_[i1] - table_[i0]);
        phase_ += phase_inc_;
        if(phase_ >= 1.f)
            phase_ -= 1.f;
        return out;
    }

    /** Current phase in cycles, 0..1 (tests/diagnostics). */
    float Phase() const { return phase_; }

  private:
    /** Shared table, built once at first Init (boot): sinf is fine off
     *  the audio path. Function-local statics keep the header self-
     *  contained in C++14 without an out-of-line definition. */
    static const float *Table()
    {
        static float t[kTableSize];
        static bool  built = false;
        if(!built)
        {
            for(int i = 0; i < kTableSize; i++)
                t[i] = sinf(6.283185307179586f * static_cast<float>(i)
                            / static_cast<float>(kTableSize));
            built = true;
        }
        return t;
    }

    const float *table_;
    float        sr_recip_;
    float        phase_;
    float        phase_inc_;
};

} // namespace chompi

/** @file sine_lut_test.cpp
 *  @brief Native tests for SineLut.h (table-lookup sine oscillator):
 *  interpolation accuracy vs libm sinf, wrap/bounds over long runs,
 *  phase-continuous retunes, coherent-DFT frequency accuracy and
 *  spurious floor, zero-Hz behavior, and deterministic re-init.
 *
 *  Build & run: make -C firmware/chompi-pipes/code/tests test
 */
#include "SineLut.h"
#include <cassert>
#include <cmath>
#include <cstdio>
#include <vector>

using chompi::SineLut;

static const double kTwoPi = 6.283185307179586;

/** Sweep every table entry at fracs {0, .25, .5, .75}: inc = 1/8192
 *  cycles/sample is an exact power of two, so the float phase is
 *  bit-exact and the comparison isolates table + lerp error. */
static void test_accuracy_vs_sinf()
{
    SineLut     o;
    const float sr = 48000.f;
    o.Init(sr);
    o.SetFreq(sr / 8192.f);
    double phase = 0.;
    float  max_err = 0.f;
    for(int i = 0; i < 4 * 8192; i++) /* 4 full cycles */
    {
        const float got = o.Process();
        const float ref = static_cast<float>(sin(kTwoPi * phase));
        const float err = fabsf(got - ref);
        if(err > max_err)
            max_err = err;
        phase += 1. / 8192.;
        if(phase >= 1.)
            phase -= 1.;
    }
    /* 2048-entry lerp bound is ~1.2e-6 (-118 dB); assert with margin */
    assert(max_err < 5e-6f);
}

/** Long 440 Hz run: output bounded, phase always wrapped to [0,1),
 *  no NaNs. (No long-run absolute-phase reference: float phase
 *  accumulation drift is inherent to any float oscillator, daisysp
 *  included; frequency accuracy is covered by the DFT test below.) */
static void test_wrap_and_bounds()
{
    SineLut     o;
    const float sr = 48000.f;
    o.Init(sr);
    o.SetFreq(440.f);
    for(int i = 0; i < 200000; i++)
    {
        const float got = o.Process();
        assert(got >= -1.0001f && got <= 1.0001f);
        assert(o.Phase() >= 0.f && o.Phase() < 1.f);
    }
}

/** Retune mid-cycle: the phase never resets, so the sample-to-sample
 *  step stays within the new frequency's max sine slope. */
static void test_phase_continuous_retune()
{
    SineLut     o;
    const float sr = 48000.f;
    o.Init(sr);
    o.SetFreq(220.f);
    float prev = 0.f;
    for(int i = 0; i < 1000; i++)
        prev = o.Process();
    o.SetFreq(440.f);
    /* max |d(sin)/dt| per sample at 440 Hz: 2*pi*440/48000 ~ 0.0576 */
    for(int i = 0; i < 2000; i++)
    {
        const float cur = o.Process();
        assert(fabsf(cur - prev) < .1f); /* no jump at/after retune */
        prev = cur;
    }
}

/** Coherent DFT: 468.75 Hz = bin 40 of 4096 samples at 48 kHz (the
 *  increment 5/512 is exact in binary, so there is no accumulator
 *  drift in the window). Peak at N/2 within 0.1%, spurious floor
 *  below -80 dB (LUT lerp noise sits near -118 dB). */
static void test_dft_frequency_and_spurious()
{
    SineLut     o;
    const float sr = 48000.f;
    o.Init(sr);
    o.SetFreq(468.75f);
    const int        N = 4096;
    std::vector<float> cap(N);
    for(int i = 0; i < N; i++)
        cap[i] = o.Process();

    auto mag_at = [&](int bin) {
        double re = 0., im = 0.;
        for(int i = 0; i < N; i++)
        {
            const double ph = kTwoPi * bin * i / N;
            re += cap[i] * cos(ph);
            im -= cap[i] * sin(ph);
        }
        return sqrt(re * re + im * im) / N * 2.;
    };

    const double peak = mag_at(40);
    assert(peak > .999 && peak < 1.001);
    double max_spur = 0.;
    for(int b = 1; b < N / 2; b++)
    {
        if(b >= 39 && b <= 41)
            continue; /* skip the peak and its immediate shoulders */
        const double m = mag_at(b);
        if(m > max_spur)
            max_spur = m;
    }
    assert(max_spur / peak < 1e-4); /* < -80 dBc */
}

/** Zero frequency is a constant output; re-init restarts phase at 0
 *  deterministically; two instances are bit-identical. */
static void test_zero_and_reinit()
{
    SineLut a, b;
    a.Init(48000.f);
    b.Init(48000.f);
    a.SetFreq(0.f);
    for(int i = 0; i < 100; i++)
        assert(a.Process() == 0.f); /* phase 0 held: sin(0) exactly */

    a.SetFreq(333.f);
    b.SetFreq(333.f);
    for(int i = 0; i < 1000; i++)
        assert(a.Process() == b.Process()); /* bit-identical instances */

    a.Init(48000.f); /* re-init: phase back to 0 */
    a.SetFreq(333.f);
    SineLut c;
    c.Init(48000.f);
    c.SetFreq(333.f);
    for(int i = 0; i < 1000; i++)
        assert(a.Process() == c.Process());
}

int main()
{
    test_accuracy_vs_sinf();
    test_wrap_and_bounds();
    test_phase_continuous_retune();
    test_dft_frequency_and_spurious();
    test_zero_and_reinit();
    printf("sine_lut_test: all tests passed\n");
    return 0;
}

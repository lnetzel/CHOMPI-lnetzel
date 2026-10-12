/** @file distortion_test.cpp
 *  @brief Native assert-based numerical tests for Distortion.h.
 *
 *  Pure host C++: no MCU/Daisy dependencies. Covers:
 *   - init defaults, type setter clamping (no wrap), immediate getter
 *   - Existing regression: bit-faithful to the pre-extraction DSP-5
 *     formula at representative inputs and amounts, incl. the
 *     smoothed-amount bypass gate
 *   - exact dry bypass at settled zero amount for every type
 *   - silent input -> silence after filter settling (all types)
 *   - type crossfade: weights stay nonnegative and sum to 1, converge
 *     to the exact one-hot in ~10 ms, retarget mid-ramp from the
 *     current weights without a jump (48 kHz and 32 kHz)
 *   - amount retention: SetType alone never disturbs the drive path
 *   - voicing distinction: Warm is asymmetric (even harmonics, blocked
 *     DC), Fuzz is symmetric and bounded, both differ from Existing
 *   - bounded output across the full amount range
 *   - no discontinuities during rapid type retargeting (checked
 *     relative to the local input slope)
 *
 *  Run: make -C firmware/chompi-organ/code/tests test
 */
#include <cassert>
#include <cmath>
#include <cstdio>
#include "Distortion.h"

using chompi::Distortion;

static const float kPi = 3.14159265358979f;

/** Pre-extraction DSP-5 arithmetic (the original overdrive stage). */
static float OriginalExisting(float x, float d)
{
    const float g    = 1.f + d * d * 63.f;
    const float post = .33f / tanhf(.33f * g);
    return d > 0.f ? tanhf(g * x) * post : x;
}

/** In-place DFT magnitude at harmonic h of nsamp samples, coherent cycles. */
static float BinMag(const float *buf, int nsamp, int h)
{
    double re = 0.0, im = 0.0;
    for(int i = 0; i < nsamp; i++)
    {
        const double ph = 2.0 * kPi * h * i / nsamp;
        re += buf[i] * cos(ph);
        im -= buf[i] * sin(ph);
    }
    return static_cast<float>(2.0 * sqrt(re * re + im * im) / nsamp);
}

static void test_init_and_clamp()
{
    Distortion d;
    d.Init(48000.f);
    assert(d.GetType() == Distortion::kExisting); /* boot default */
    assert(d.Weight(0) == 1.f && d.Weight(1) == 0.f && d.Weight(2) == 0.f);

    d.SetType(Distortion::kWarm);
    assert(d.GetType() == Distortion::kWarm); /* immediate, pre-ramp */
    d.SetType(Distortion::kFuzz);
    assert(d.GetType() == Distortion::kFuzz);
    d.SetType(99); /* clamped at the top end, no wrap */
    assert(d.GetType() == Distortion::kFuzz);
    d.SetType(-4); /* clamped at the bottom end */
    assert(d.GetType() == Distortion::kExisting);
}

static void test_existing_formula()
{
    Distortion d;
    d.Init(48000.f); /* weights settle one-hot Existing from Init */
    static const float drives[] = {1.f / 32.f, .25f, .5f, 1.f};
    static const float ins[]
        = {-1.f, -.5f, -.33f, -.1f, 0.f, .1f, .33f, .5f, 1.f};
    for(int di = 0; di < 4; di++)
    {
        const float drv = drives[di];
        d.PrepareBlock(drv);
        for(int xi = 0; xi < 9; xi++)
        {
            const float got  = d.Process(ins[xi], drv);
            const float want = OriginalExisting(ins[xi], drv);
            assert(fabsf(got - want) <= 1e-6f);
        }
    }
    /* the smoothed-amount gate: drive 0 is dry at any prepared gain */
    d.PrepareBlock(1.f);
    assert(d.Process(.4f, 0.f) == .4f);
    assert(d.Process(-.7f, 0.f) == -.7f);
}

static void test_zero_bypass_exact()
{
    for(int t = 0; t < Distortion::kNumTypes; t++)
    {
        Distortion d;
        d.Init(48000.f);
        d.SetType(t);
        d.PrepareBlock(0.f);
        /* settle the ~10 ms weight + wet ramps (30 ms of signal) */
        for(int i = 0; i < 1440; i++)
            d.Process(.3f * sinf(2.f * kPi * i * .01f), 0.f);
        assert(d.Weight(t) == 1.f);
        static const float xs[] = {-.9f, -.33f, -.02f, 0.f, .02f, .33f, .9f};
        for(int xi = 0; xi < 7; xi++)
            assert(d.Process(xs[xi], 0.f) == xs[xi]); /* bit-exact dry */
    }
}

static void test_silence_settles()
{
    for(int t = 0; t < Distortion::kNumTypes; t++)
    {
        Distortion d;
        d.Init(48000.f);
        d.SetType(t);
        d.PrepareBlock(1.f);
        /* warm up with signal so the filters hold state, then go silent */
        for(int i = 0; i < 480; i++)
            d.Process(.5f, 1.f);
        float out = 1.f;
        int   n   = 0;
        for(; n < 24000; n++) /* 500 ms budget */
        {
            out = d.Process(0.f, 1.f);
            if(fabsf(out) < 1e-6f)
                break;
        }
        assert(n < 24000); /* silence after filter settling */
    }
}

static void test_weight_transitions(float sr)
{
    Distortion d;
    d.Init(sr);
    d.PrepareBlock(.5f);

    const int ramp_samples = static_cast<int>(.010f * sr + .5f);

    /* Existing -> Warm: monotone target weight, sum 1, exact one-hot */
    d.SetType(Distortion::kWarm);
    float prev_w = d.Weight(Distortion::kWarm);
    int    n     = 0;
    for(; n < ramp_samples + 2; n++)
    {
        d.Process(.2f, .5f);
        const float w0 = d.Weight(0), w1 = d.Weight(1), w2 = d.Weight(2);
        assert(w0 >= 0.f && w1 >= 0.f && w2 >= 0.f);
        assert(fabsf((w0 + w1 + w2) - 1.f) < 1e-5f);
        assert(w1 >= prev_w);
        prev_w = w1;
        if(w1 == 1.f)
            break;
    }
    assert(n <= ramp_samples); /* finite ~10 ms convergence */
    assert(d.Weight(0) == 0.f && d.Weight(2) == 0.f);

    /* mid-ramp retarget Warm -> Fuzz: weights continue from their
     * current values (no jump larger than one increment per sample) */
    Distortion e;
    e.Init(sr);
    e.SetType(Distortion::kWarm);
    const int half = ramp_samples / 2;
    for(int i = 0; i < half; i++)
        e.Process(.2f, .5f);
    const float b0 = e.Weight(0), b1 = e.Weight(1), b2 = e.Weight(2);
    assert(b1 > 0.f && b1 < 1.f); /* genuinely mid-ramp */
    e.SetType(Distortion::kFuzz);
    assert(e.GetType() == Distortion::kFuzz); /* target immediately */
    const float inc = 1.f / (.010f * sr);
    for(int n2 = 0; n2 < ramp_samples + 2; n2++)
    {
        const float p0 = e.Weight(0), p1 = e.Weight(1), p2 = e.Weight(2);
        e.Process(.2f, .5f);
        const float q0 = e.Weight(0), q1 = e.Weight(1), q2 = e.Weight(2);
        assert(fabsf(q0 - p0) <= inc + 1e-6f);
        assert(fabsf(q1 - p1) <= inc + 1e-6f);
        assert(fabsf(q2 - p2) <= inc + 1e-6f);
        assert(fabsf((q0 + q1 + q2) - 1.f) < 1e-5f);
        if(q2 == 1.f)
            break;
    }
    assert(e.Weight(2) == 1.f && e.Weight(0) == 0.f && e.Weight(1) == 0.f);
    (void)b0;
    (void)b2;
}

static void test_amount_retention()
{
    /* SetType alone (no Process calls) must not disturb the drive path:
     * same amount in -> same sample out. */
    Distortion d;
    d.Init(48000.f);
    d.PrepareBlock(.5f);
    d.SetType(Distortion::kWarm);
    d.SetType(Distortion::kFuzz);
    d.SetType(Distortion::kExisting); /* weights never advanced */
    assert(d.Weight(0) == 1.f);
    const float got = d.Process(.25f, .5f);
    assert(fabsf(got - OriginalExisting(.25f, .5f)) <= 1e-6f);
}

/** Run a coherent 468.75 Hz sine (bin 40 of 4096 @ 48 kHz) through one
 *  type at full amount; fill buf with the steady-state last 4096. */
static void SteadySine(int type, float *buf, int nsamp)
{
    Distortion d;
    d.Init(48000.f);
    d.SetType(type);
    d.PrepareBlock(1.f);
    for(int i = 0; i < 4800; i++) /* 100 ms settle: ramps + filters */
        d.Process(.3f * sinf(2.f * kPi * 40 * i / nsamp), 1.f);
    for(int i = 0; i < nsamp; i++)
        buf[i] = d.Process(.3f * sinf(2.f * kPi * 40 * i / nsamp), 1.f);
}

static void test_voicing_distinction()
{
    static float buf[4096];

    /* Warm: asymmetric saturation -> even harmonics, but no DC */
    SteadySine(Distortion::kWarm, buf, 4096);
    double mean = 0.0;
    for(int i = 0; i < 4096; i++)
        mean += buf[i];
    mean /= 4096;
    assert(fabs(mean) < 2e-3); /* ~20 Hz DC blocker works at 468 Hz */
    const float w1 = BinMag(buf, 4096, 40);
    const float w2 = BinMag(buf, 4096, 80);
    assert(w2 > .02f * w1); /* clearly present 2nd harmonic */

    /* Fuzz: symmetric clip -> even harmonics ~0, strong odd content */
    SteadySine(Distortion::kFuzz, buf, 4096);
    const float f1 = BinMag(buf, 4096, 40);
    const float f2 = BinMag(buf, 4096, 80);
    const float f3 = BinMag(buf, 4096, 120);
    assert(f2 < .005f * f1);
    assert(f3 > .02f * f1);
    for(int i = 0; i < 4096; i++)
        assert(fabsf(buf[i]) <= .34f); /* bounded near the 0.33 anchor */

    /* Existing: same drive, different curve -> audibly different output */
    static float ebuf[4096];
    SteadySine(Distortion::kExisting, ebuf, 4096);
    const float e2 = BinMag(ebuf, 4096, 80);
    assert(e2 < .005f * BinMag(ebuf, 4096, 40)); /* symmetric like Fuzz */
    double maxdiff = 0.0;
    for(int i = 0; i < 4096; i++)
    {
        const double df = fabs(static_cast<double>(buf[i]) - ebuf[i]);
        if(df > maxdiff)
            maxdiff = df;
    }
    assert(maxdiff > .02); /* Fuzz and Existing are not the same sound */
}

static void test_bounded_output()
{
    unsigned rng = 12345u;
    for(int t = 0; t < Distortion::kNumTypes; t++)
    {
        Distortion d;
        d.Init(48000.f);
        d.SetType(t);
        for(int s = 0; s <= 32; s++)
        {
            const float drv = static_cast<float>(s) * (1.f / 32.f);
            d.PrepareBlock(drv);
            for(int i = 0; i < 64; i++)
            {
                rng           = rng * 1664525u + 1013904223u;
                const float x = (rng >> 8) * (1.f / 8388608.f) - 1.f;
                const float y = d.Process(x, drv);
                assert(fabsf(y) <= 1.05f);
            }
        }
    }
}

static void test_no_discontinuity_on_retarget()
{
    Distortion d;
    d.Init(48000.f);
    d.PrepareBlock(.5f);
    const int steps = 48000; /* 1 s */
    float     prev_in = 0.f, prev_out = 0.f;
    for(int i = 0; i < steps; i++)
    {
        if(i > 0 && i % 240 == 0) /* retarget every 5 ms: all directions */
            d.SetType((i / 240) % Distortion::kNumTypes);
        const float in  = .3f * sinf(2.f * kPi * 1000.f * i / 48000.f);
        const float out = d.Process(in, .5f);
        if(i > 0)
        {
            const float din = fabsf(in - prev_in);
            assert(fabsf(out - prev_out) <= 8.f * din + 2e-3f);
        }
        prev_in  = in;
        prev_out = out;
    }
}

int main()
{
    test_init_and_clamp();
    test_existing_formula();
    test_zero_bypass_exact();
    test_silence_settles();
    test_weight_transitions(48000.f);
    test_weight_transitions(32000.f); /* ramps scale with the sample rate */
    test_amount_retention();
    test_voicing_distinction();
    test_bounded_output();
    test_no_discontinuity_on_retarget();
    printf("distortion_test: all checks passed\n");
    return 0;
}

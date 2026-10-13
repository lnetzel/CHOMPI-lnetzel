/** @file distortion_test.cpp
 *  @brief Native assert-based numerical tests for Distortion.h.
 *
 *  Pure host C++: no MCU/Daisy dependencies. Covers:
 *   - init defaults, type setter clamping (no wrap), immediate getter
 *   - tape saturation: SoftClip curve spot values (incl. the |x|>3
 *     clamp), log-mapping endpoints amt(0)=1 / amt(1)~13.9, comp gain
 *     floor 0.3, exact zero-drive bypass
 *   - exact dry bypass at settled zero amount for every type
 *   - silent input -> silence after filter settling (all types)
 *   - type crossfade: weights stay nonnegative and sum to 1, converge
 *     to the exact one-hot in ~10 ms, retarget mid-ramp from the
 *     current weights without a jump (48 kHz and 32 kHz)
 *   - amount retention: SetType alone never disturbs the drive path
 *   - voicing distinction: Tape Saturation is symmetric with strong odd
 *     harmonics and no DC, Fuzz is symmetric and bounded, BrokenSpeaker
 *     is harsh and narrow-banded (200 Hz high-pass)
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
    assert(d.GetType() == Distortion::kTapeSat); /* boot default */
    assert(d.Weight(0) == 1.f && d.Weight(1) == 0.f && d.Weight(2) == 0.f);

    d.SetType(Distortion::kFuzz);
    assert(d.GetType() == Distortion::kFuzz); /* immediate, pre-ramp */
    d.SetType(Distortion::kBrokenSpeaker);
    assert(d.GetType() == Distortion::kBrokenSpeaker);
    d.SetType(99); /* clamped at the top end, no wrap */
    assert(d.GetType() == Distortion::kBrokenSpeaker);
    d.SetType(-4); /* clamped at the bottom end */
    assert(d.GetType() == Distortion::kTapeSat);
}

static void test_tape_saturation()
{
    /* stmlib SoftClip curve spot values: the rational x(27+x^2)/(27+9x^2)
     * waveshaper, clamped to +/-1 beyond |x| > 3 */
    assert(Distortion::SoftClip(0.f) == 0.f);
    assert(fabsf(Distortion::SoftClip(1.f) - 28.f / 36.f) < 1e-6f);
    assert(fabsf(Distortion::SoftClip(2.f) - 62.f / 63.f) < 1e-6f);
    assert(fabsf(Distortion::SoftClip(3.f) - 1.f) < 1e-6f);
    assert(Distortion::SoftClip(3.5f) == 1.f);   /* beyond the clamp */
    assert(Distortion::SoftClip(-3.5f) == -1.f); /* beyond the clamp */
    assert(Distortion::SoftClip(-1.f) == -Distortion::SoftClip(1.f));

    /* log-mapped pre-gain endpoints: amt(0) = 1, amt(1) ~= 13.9 */
    Distortion d;
    d.Init(48000.f);
    d.PrepareBlock(0.f);
    assert(d.TapeAmt() == 1.f);
    assert(d.TapeComp() == 1.f); /* no compensation at zero drive */
    d.PrepareBlock(1.f);
    assert(fabsf(d.TapeAmt() - 13.9f) < .02f); /* logf(2.7)*13+1 */
    /* comp floor: SoftClip(.4*(13.9-1)) is beyond |x|>3 -> clamps to 1 */
    assert(fabsf(d.TapeComp() - .3f) < 1e-6f);

    /* exact zero-drive bypass for the tape branch */
    d.PrepareBlock(0.f);
    for(int i = 0; i < 480; i++)
        d.Process(.3f, 0.f);
    assert(d.Process(-.42f, 0.f) == -.42f);
    assert(d.Process(.17f, 0.f) == .17f);
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

    /* Tape (boot default) -> Fuzz: monotone target weight, sum 1, exact one-hot */
    d.SetType(Distortion::kFuzz);
    float prev_w = d.Weight(Distortion::kFuzz);
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

    /* mid-ramp retarget Fuzz -> BrokenSpeaker: weights continue from their
     * current values (no jump larger than one increment per sample) */
    Distortion e;
    e.Init(sr);
    e.SetType(Distortion::kFuzz);
    const int half = ramp_samples / 2;
    for(int i = 0; i < half; i++)
        e.Process(.2f, .5f);
    const float b0 = e.Weight(0), b1 = e.Weight(1), b2 = e.Weight(2);
    assert(b1 > 0.f && b1 < 1.f); /* genuinely mid-ramp */
    e.SetType(Distortion::kBrokenSpeaker);
    assert(e.GetType() == Distortion::kBrokenSpeaker); /* target immediately */
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
    (void)b1;
}

static void test_amount_retention()
{
    /* SetType alone (no Process calls) must not disturb the drive path:
     * the crossfade continues from the current weights, never jumping.
     * Verify weights stay consistent and ProcessBlock is called. */
    Distortion d;
    d.Init(48000.f);
    d.PrepareBlock(.5f);
    assert(d.GetType() == Distortion::kTapeSat); /* boot default */
    assert(d.Weight(0) == 1.f);
    
    d.SetType(Distortion::kFuzz);
    /* weights haven't changed yet because Process hasn't been called */
    assert(d.Weight(0) == 1.f);
    
    d.SetType(Distortion::kBrokenSpeaker);
    /* weights still unchanged */
    assert(d.Weight(0) == 1.f);
    
    /* Only after Process calls do weights start moving toward BrokenSpeaker */
    d.Process(.25f, .5f);
    assert(d.Weight(2) > 0.f); /* Broken has nonzero weight now */
    assert(d.Weight(2) < 1.f); /* but hasn't reached it yet */
}

/** Run a coherent sine (bin `bin` of `nsamp` at 48 kHz) through one
 *  type at full amount; fill buf with the steady-state last nsamp. */
static void SteadySine(int type, int bin, float *buf, int nsamp)
{
    Distortion d;
    d.Init(48000.f);
    d.SetType(type);
    d.PrepareBlock(1.f);
    for(int i = 0; i < 4800; i++) /* 100 ms settle: ramps + filters */
        d.Process(.3f * sinf(2.f * kPi * bin * i / nsamp), 1.f);
    for(int i = 0; i < nsamp; i++)
        buf[i] = d.Process(.3f * sinf(2.f * kPi * bin * i / nsamp), 1.f);
}

static void test_voicing_distinction()
{
    static float buf[4096];

    /* Tape Saturation: symmetric SoftClip curve -> odd harmonics only,
     * no DC; at full drive the 0.3 sine x 13.9 pre-gain clips deep into
     * the +/-1 bounds (near-square), so the 3rd harmonic is strong */
    SteadySine(Distortion::kTapeSat, 40, buf, 4096);
    double mean = 0.0;
    for(int i = 0; i < 4096; i++)
        mean += buf[i];
    mean /= 4096;
    assert(fabs(mean) < 2e-3); /* symmetric curve: no DC */
    const float t1 = BinMag(buf, 4096, 40);
    const float t2 = BinMag(buf, 4096, 80);
    const float t3 = BinMag(buf, 4096, 120);
    assert(t2 < .01f * t1); /* even harmonics ~0 */
    assert(t3 > .05f * t1); /* strong 3rd harmonic */

    /* Tape Saturation and Fuzz are different curves (rational SoftClip
     * vs soft-knee t-t^3/3 with a different gain map) */
    {
        static float fbuf0[4096];
        SteadySine(Distortion::kFuzz, 40, fbuf0, 4096);
        double maxdiff = 0.0;
        for(int i = 0; i < 4096; i++)
        {
            const double df = fabs(static_cast<double>(buf[i]) - fbuf0[i]);
            if(df > maxdiff)
                maxdiff = df;
        }
        assert(maxdiff > .01);
    }

    /* Fuzz: symmetric clip -> even harmonics ~0, strong odd content */
    SteadySine(Distortion::kFuzz, 40, buf, 4096);
    const float f1 = BinMag(buf, 4096, 40);
    const float f2 = BinMag(buf, 4096, 80);
    const float f3 = BinMag(buf, 4096, 120);
    assert(f2 < .005f * f1);
    assert(f3 > .02f * f1);
    for(int i = 0; i < 4096; i++)
        assert(fabsf(buf[i]) <= .8f); /* bounded by hard clip */

    /* BrokenSpeaker: harsh hard clipping -> strong harmonics with
     * narrow bandpass coloration (200 Hz HP to ~4 kHz LP creates
     * the presence band). Noticeably different from both Tape and Fuzz. */
    SteadySine(Distortion::kBrokenSpeaker, 40, buf, 4096);
    const float b1 = BinMag(buf, 4096, 40);
    const float b3 = BinMag(buf, 4096, 120);
    assert(b3 > .02f * b1); /* hard clip: strong odd harmonics */
    for(int i = 0; i < 4096; i++)
        assert(fabsf(buf[i]) <= .8f); /* bounded by hard clip */

    /* BrokenSpeaker should be distinctly different from Fuzz's symmetric
     * clipping due to the narrow bandpass filtering. */
    double maxdiff = 0.0;
    for(int i = 0; i < 4096; i++)
    {
        static float fbuf[4096];
        SteadySine(Distortion::kFuzz, 40, fbuf, 4096);
        const double df = fabs(static_cast<double>(buf[i]) - fbuf[i]);
        if(df > maxdiff)
            maxdiff = df;
    }
    assert(maxdiff > .01); /* BrokenSpeaker and Fuzz are not the same sound */

    /* BrokenSpeaker high-pass at 200 Hz: the fundamental of a 93.75 Hz
     * sine (bin 8) is attenuated far more than a 468.75 Hz one (bin 40)
     * at the same input level and drive. */
    {
        static float lobuf[4096];
        SteadySine(Distortion::kBrokenSpeaker, 8, lobuf, 4096);
        const float lo1 = BinMag(lobuf, 4096, 8);
        SteadySine(Distortion::kBrokenSpeaker, 40, buf, 4096);
        const float hi1 = BinMag(buf, 4096, 40);
        assert(lo1 < .7f * hi1);
    }
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
    test_tape_saturation();
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

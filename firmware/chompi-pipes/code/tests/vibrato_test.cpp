/** @file vibrato_test.cpp
 *  @brief Native assert-based numerical tests for Vibrato.h.
 *
 *  Pure host C++: no MCU/Daisy dependencies. Covers:
 *   - init defaults and depth clamping (0..50 cents, no wrap)
 *   - depth -> cents mapping: peak deviation equals the depth in cents
 *     at 25 and 50 (max multiplier 2^(depth/1200))
 *   - 5 Hz rate: one LFO cycle spans exactly sr/5 samples
 *   - phase continuity: no per-sample jumps, free-running wrap
 *   - stable base tuning: cycle mean of the multiplier stays ~1
 *   - exact zero-depth behavior: Process() returns exactly 1.f
 *   - finite, bounded output over long runs at both 48 kHz and 32 kHz
 *   - delay envelope: exact-dry delay window, 250 ms linear fade-in
 *     landing bit-exact on the undelayed trajectory, retrigger, no-op
 *     trigger at delay 0, mid-envelope clear via SetDelayMs(0)
 *
 *  Run: make -C firmware/chompi-pipes/code/tests test
 */
#include <cassert>
#include <cmath>
#include <cstdio>
#include "Vibrato.h"

using chompi::Vibrato;

static void test_init_and_clamp()
{
    Vibrato v;
    v.Init(48000.f);
    assert(v.GetDepth() == 0); /* boots dry */

    v.SetDepth(25);
    assert(v.GetDepth() == 25);
    v.SetDepth(51); /* clamped at the top end, no wrap */
    assert(v.GetDepth() == Vibrato::kMaxDepthCents);
    v.SetDepth(-3); /* clamped at the bottom end */
    assert(v.GetDepth() == 0);
    v.SetDepth(Vibrato::kMaxDepthCents);
    assert(v.GetDepth() == 50);
}

static void test_zero_depth_exact()
{
    /* Depth 0 must return exactly 1.f for every sample, at any phase,
     * including phase wraps. */
    Vibrato v;
    v.Init(48000.f);
    for(int i = 0; i < 100000; i++)
        assert(v.Process() == 1.f);
    /* ... even right after a nonzero depth ran the LFO mid-cycle */
    v.SetDepth(50);
    for(int i = 0; i < 1234; i++)
        v.Process();
    v.SetDepth(0);
    assert(v.Process() == 1.f);
}

/** Run one full LFO cycle at depth `cents`, returning the max/min
 *  multiplier and the sample index of the max. The float32 top of the
 *  sine is a plateau of tied samples (hundreds wide at small depths),
 *  so the reported index is the plateau CENTER, which estimates the
 *  true peak position within a sample. */
static void CycleStats(Vibrato &v, int sr, float &mx, float &mn, int &argmx)
{
    const int cycle = sr / 5; /* 5 Hz */
    mx   = 0.f;
    mn   = 1e30f;
    argmx = -1;
    int argmx_last = -1;
    for(int i = 0; i < cycle; i++)
    {
        const float m = v.Process();
        if(m > mx)
        {
            mx         = m;
            argmx      = i;
            argmx_last = i;
        }
        else if(m == mx)
        {
            argmx_last = i; /* tied top: extend the plateau */
        }
        if(m < mn)
            mn = m;
    }
    argmx = (argmx + argmx_last) / 2;
}

static void test_depth_mapping()
{
    /* Peak deviation in cents equals the depth value, one-cent steps. */
    for(int depth = 1; depth <= 50; depth += 7)
    {
        Vibrato v;
        v.Init(48000.f);
        v.SetDepth(depth);
        float mx, mn;
        int   argmx;
        CycleStats(v, 48000, mx, mn, argmx);
        const float expect = exp2f(static_cast<float>(depth) / 1200.f);
        assert(fabsf(mx - expect) < 2e-6f * expect);
        assert(fabsf(mn - 1.f / expect) < 2e-6f);
        /* sin peak sits 1/4 cycle after the (rising) zero crossing:
         * sample 9600/4 - 1 = 2399 (the plateau center) */
        assert(argmx >= 2398 && argmx <= 2401);
    }
}

static void test_rate_5hz()
{
    /* The multiplier pattern repeats every sr/5 samples: the rising
     * zero-crossings of (mult - 1) are exactly one cycle apart. */
    Vibrato v;
    v.Init(48000.f);
    v.SetDepth(50);
    int   last_cross = -1;
    float prev       = v.Process();
    for(int i = 1; i < 48000; i++) /* one second = five cycles */
    {
        const float m = v.Process();
        if(prev < 1.f && m >= 1.f)
        {
            /* +-1 sample margin for float phase accumulation */
            if(last_cross >= 0)
                assert(i - last_cross >= 48000 / 5 - 1
                       && i - last_cross <= 48000 / 5 + 1);
            last_cross = i;
        }
        prev = m;
    }
    assert(last_cross > 0); /* crossings were actually observed */

    /* Same rate at a different sample rate: 32 kHz. */
    Vibrato w;
    w.Init(32000.f);
    w.SetDepth(50);
    last_cross = -1;
    prev       = w.Process();
    for(int i = 1; i < 32000; i++)
    {
        const float m = w.Process();
        if(prev < 1.f && m >= 1.f)
        {
            if(last_cross >= 0)
                assert(i - last_cross >= 32000 / 5 - 2
                       && i - last_cross <= 32000 / 5 + 2);
            last_cross = i;
        }
        prev = m;
    }
}

static void test_phase_continuity()
{
    /* No per-sample jumps: the multiplier is a sampled sinusoid, so its
     * largest step is bounded by the peak slope a*ln2*2*pi*5/sr. At
     * depth 50 and 48 kHz that is ~2.0e-5; allow generous margin and
     * also check across the phase wrap and across depth changes. */
    Vibrato v;
    v.Init(48000.f);
    v.SetDepth(50);
    float prev = v.Process();
    for(int i = 1; i < 48000; i++)
    {
        const float m = v.Process();
        assert(fabsf(m - prev) < 1e-4f);
        prev = m;
    }
    v.SetDepth(25); /* retarget mid-cycle: amplitude step, no phase jump */
    float m = v.Process();
    if(fabsf(prev - 1.f) > 1e-3f)
    {
        /* same side of unity, smaller magnitude: phase was NOT reset */
        assert((m - 1.f) * (prev - 1.f) > 0.f);
        assert(fabsf(m - 1.f) < fabsf(prev - 1.f));
    }
    prev = m;
    for(int i = 0; i < 8000; i++)
    {
        m = v.Process();
        assert(fabsf(m - prev) < 1e-4f);
        prev = m;
    }
}

static void test_base_tuning_stable()
{
    /* The pitch center must not drift: over complete cycles the mean of
     * the multiplier is I0(a*ln2) ~ 1 + (a*ln2)^2/4 - about 0.36 cent
     * above unity at the maximum depth of 50 cents, far below audibility
     * and constant (no accumulation). */
    Vibrato v;
    v.Init(48000.f);
    v.SetDepth(50);
    double sum = 0.0;
    const int n = 10 * 9600; /* ten complete cycles */
    for(int i = 0; i < n; i++)
        sum += v.Process();
    const float mean = static_cast<float>(sum / n);
    assert(mean > 1.f && mean < 1.0004f); /* within ~0.7 cent of unity */

    /* Endpoint: depth 50 is +/-50 cents = one semitone peak-to-peak. */
    const float peak = exp2f(50.f / 1200.f);
    assert(peak > 1.0292f && peak < 1.0294f);
}

static void test_finite_and_bounded()
{
    for(int depth = 0; depth <= 50; depth++)
    {
        Vibrato v;
        v.Init(48000.f);
        v.SetDepth(depth);
        for(int i = 0; i < 20000; i++)
        {
            const float m = v.Process();
            assert(isfinite(m));
            assert(m > 0.96f && m < 1.04f);
        }
    }
}

static void test_delay_off_passthrough()
{
    /* Boot default (delay 0): Trigger() is a no-op and the multiplier
     * trajectory stays bit-identical to an untriggered one. */
    Vibrato a, b;
    a.Init(48000.f);
    b.Init(48000.f);
    a.SetDepth(50);
    b.SetDepth(50);
    assert(a.DelayEnv() == 1.f);
    a.Trigger(); /* no delay set: must do nothing */
    assert(a.DelayEnv() == 1.f);
    for(int i = 0; i < 48000; i++)
        assert(a.Process() == b.Process());
}

static void test_delay_window_exact_dry()
{
    /* 1000 ms delay at 48 kHz: exactly 1.f for the whole window, even
     * at full depth; the fade-in starts right after. */
    Vibrato v;
    v.Init(48000.f);
    v.SetDepth(50);
    v.SetDelayMs(1000.f);
    assert(fabsf(v.GetDelayMs() - 1000.f) < .01f);
    v.Trigger();
    assert(v.DelayEnv() == 0.f);
    for(int i = 0; i < 48000; i++)
        assert(v.Process() == 1.f);
    assert(v.DelayEnv() == 0.f); /* ramp not started yet */
    v.Process();
    assert(v.DelayEnv() > 0.f && v.DelayEnv() < 1.f); /* ramping */
}

static void test_delay_ramp_settles_bitexact()
{
    /* After the delay + the 250 ms ramp the delayed instance is
     * bit-identical to a free-running undelayed one started at the
     * same time (both phase trajectories advance together). */
    Vibrato v, ref;
    v.Init(48000.f);
    ref.Init(48000.f);
    v.SetDepth(50);
    ref.SetDepth(50);
    v.SetDelayMs(1000.f);
    v.Trigger();
    const int delay_n = 48000;    /* 1 s window */
    const int ramp_n  = 12000;    /* 250 ms at 48 kHz */
    float     prev_env = 0.f;
    bool      rose     = false;
    for(int i = 0; i < delay_n + ramp_n + 2; i++)
    {
        v.Process();
        ref.Process();
        const float e = v.DelayEnv();
        if(e > prev_env)
            rose = true;
        prev_env = e;
    }
    assert(rose);              /* the fade-in actually happened */
    assert(prev_env == 1.f);   /* ... and landed exactly on full */
    for(int i = 0; i < 8000; i++)
        assert(v.Process() == ref.Process()); /* settled: bit-exact */
}

static void test_delay_retrigger()
{
    /* A fresh Trigger mid-ramp restarts the delay from exact dry. */
    Vibrato v;
    v.Init(48000.f);
    v.SetDepth(50);
    v.SetDelayMs(500.f); /* 24000 samples */
    v.Trigger();
    for(int i = 0; i < 24000 + 6000; i++) /* delay + half the ramp */
        v.Process();
    assert(v.DelayEnv() > 0.f && v.DelayEnv() < 1.f); /* mid-ramp */
    v.Trigger();
    assert(v.DelayEnv() == 0.f);
    for(int i = 0; i < 24000; i++)
        assert(v.Process() == 1.f);
}

static void test_zero_depth_with_delay()
{
    /* Depth 0 stays exactly dry through trigger, window and ramp. */
    Vibrato v;
    v.Init(48000.f);
    v.SetDelayMs(200.f);
    v.Trigger();
    for(int i = 0; i < 48000; i++)
        assert(v.Process() == 1.f);
}

static void test_delay_cleared_mid_envelope()
{
    /* SetDelayMs(0) forces the envelope to exactly 1 immediately. */
    Vibrato v, ref;
    v.Init(48000.f);
    ref.Init(48000.f);
    v.SetDepth(50);
    ref.SetDepth(50);
    v.SetDelayMs(1000.f);
    v.Trigger();
    for(int i = 0; i < 1000; i++)
    {
        v.Process();
        ref.Process();
    }
    v.SetDelayMs(0.f);
    assert(v.DelayEnv() == 1.f);
    for(int i = 0; i < 8000; i++)
        assert(v.Process() == ref.Process());
}

int main()
{
    test_init_and_clamp();
    test_zero_depth_exact();
    test_depth_mapping();
    test_rate_5hz();
    test_phase_continuity();
    test_base_tuning_stable();
    test_finite_and_bounded();
    test_delay_off_passthrough();
    test_delay_window_exact_dry();
    test_delay_ramp_settles_bitexact();
    test_delay_retrigger();
    test_zero_depth_with_delay();
    test_delay_cleared_mid_envelope();
    printf("vibrato_test: all passed\n");
    return 0;
}

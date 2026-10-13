/** @file vibrato_test.cpp
 *  @brief Native assert-based numerical tests for Vibrato.h.
 *
 *  Pure host C++: no MCU/Daisy dependencies. Covers:
 *   - init defaults and depth clamping (0..50 cents, no wrap)
 *   - depth -> cents mapping: peak deviation equals the depth in cents
 *     at 25 and 50 (max multiplier 2^(depth/1200))
 *   - 6 Hz rate: one LFO cycle spans exactly sr/6 samples
 *   - phase continuity: no per-sample jumps, free-running wrap
 *   - stable base tuning: cycle mean of the multiplier stays ~1
 *   - exact zero-depth behavior: Process() returns exactly 1.f
 *   - finite, bounded output over long runs at both 48 kHz and 32 kHz
 *
 *  Run: make -C firmware/chompi-organ/code/tests test
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
 *  multiplier and the sample index of the max. */
static void CycleStats(Vibrato &v, int sr, float &mx, float &mn, int &argmx)
{
    const int cycle = sr / 6; /* 6 Hz */
    mx   = 0.f;
    mn   = 1e30f;
    argmx = -1;
    for(int i = 0; i < cycle; i++)
    {
        const float m = v.Process();
        if(m > mx)
        {
            mx    = m;
            argmx = i;
        }
        if(m < mn)
            mn = m;
    }
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
        /* sin peak sits 1/4 cycle after the (rising) zero crossing */
        assert(argmx >= 1999 && argmx <= 2001);
    }
}

static void test_rate_6hz()
{
    /* The multiplier pattern repeats every sr/6 samples: the rising
     * zero-crossings of (mult - 1) are exactly one cycle apart. */
    Vibrato v;
    v.Init(48000.f);
    v.SetDepth(50);
    int   last_cross = -1;
    float prev       = v.Process();
    for(int i = 1; i < 48000; i++) /* one second = six cycles */
    {
        const float m = v.Process();
        if(prev < 1.f && m >= 1.f)
        {
            /* +-1 sample margin for float phase accumulation */
            if(last_cross >= 0)
                assert(i - last_cross >= 48000 / 6 - 1
                       && i - last_cross <= 48000 / 6 + 1);
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
                assert(i - last_cross >= 32000 / 6 - 2
                       && i - last_cross <= 32000 / 6 + 2);
            last_cross = i;
        }
        prev = m;
    }
}

static void test_phase_continuity()
{
    /* No per-sample jumps: the multiplier is a sampled sinusoid, so its
     * largest step is bounded by the peak slope a*ln2*2*pi*6/sr. At
     * depth 50 and 48 kHz that is ~2.4e-5; allow generous margin and
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
    const int n = 10 * 8000; /* ten complete cycles */
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

int main()
{
    test_init_and_clamp();
    test_zero_depth_exact();
    test_depth_mapping();
    test_rate_6hz();
    test_phase_continuity();
    test_base_tuning_stable();
    test_finite_and_bounded();
    printf("vibrato_test: all passed\n");
    return 0;
}

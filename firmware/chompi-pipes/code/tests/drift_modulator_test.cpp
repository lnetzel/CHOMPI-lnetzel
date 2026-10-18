/** @file drift_modulator_test.cpp
 *  @brief Native assert-based numerical tests for DriftModulator.h.
 *
 *  Pure host C++: no MCU/Daisy dependencies. Covers:
 *   - ApplyWindow sliding-window math: the plan's examples at 100%
 *     amount (50% -> 30..70, 0% -> 0..40, 5% -> 0..40, 95% -> 60..100
 *     slid to [1-2d, 1]), 50% amount scaling, amount 0 returning the
 *     center bit-exactly, output always within [0,1] with width == 2d
 *   - half durations within 2.5..10 s, full cycles within 5..20 s,
 *     durations varying across cycles, both half directions present
 *   - continuity: bounded per-block delta, u always in [0,1], both
 *     window edges reached
 *   - independence: 8 seeded modulators never run in lockstep
 *   - determinism for a fixed seed, at both 48 kHz and 32 kHz
 *
 *  Run: make -C firmware/chompi-pipes/code/tests test
 */
#include <cassert>
#include <cmath>
#include <cstdio>
#include "DriftModulator.h"

using chompi::DriftModulator;

static const int kBlock = 24; /* the organ's audio block size */

static void test_window_examples()
{
    /* 100% amount: full +/-20% window, interior center */
    assert(fabsf(DriftModulator::ApplyWindow(.5f, 1.f, 0.f) - .3f) < 1e-7f);
    assert(fabsf(DriftModulator::ApplyWindow(.5f, 1.f, 1.f) - .7f) < 1e-7f);
    assert(fabsf(DriftModulator::ApplyWindow(.5f, 1.f, .5f) - .5f) < 1e-7f);

    /* center 0% -> slid to [0, 0.4] (the pipe becomes audible) */
    assert(DriftModulator::ApplyWindow(0.f, 1.f, 0.f) == 0.f);
    assert(fabsf(DriftModulator::ApplyWindow(0.f, 1.f, 1.f) - .4f) < 1e-7f);
    assert(fabsf(DriftModulator::ApplyWindow(0.f, 1.f, .5f) - .2f) < 1e-6f);

    /* center 5% -> [0, 0.4]: 5 below the center, 35 above */
    assert(fabsf(DriftModulator::ApplyWindow(.05f, 1.f, 0.f)) < 1e-7f);
    assert(fabsf(DriftModulator::ApplyWindow(.05f, 1.f, 1.f) - .4f) < 1e-6f);

    /* center 95% -> [0.6, 1.0]: 5 above the center, 35 below */
    assert(fabsf(DriftModulator::ApplyWindow(.95f, 1.f, 0.f) - .6f) < 1e-6f);
    assert(fabsf(DriftModulator::ApplyWindow(.95f, 1.f, 1.f) - 1.f) < 1e-7f);
    assert(fabsf(DriftModulator::ApplyWindow(.95f, 1.f, .5f) - .8f) < 1e-6f);

    /* center 100% -> [0.6, 1.0] */
    assert(fabsf(DriftModulator::ApplyWindow(1.f, 1.f, 0.f) - .6f) < 1e-6f);
    assert(fabsf(DriftModulator::ApplyWindow(1.f, 1.f, 1.f) - 1.f) < 1e-7f);

    /* 50% amount: d = 10% -> center 50% swings 40..60 */
    assert(fabsf(DriftModulator::ApplyWindow(.5f, .5f, 0.f) - .4f) < 1e-6f);
    assert(fabsf(DriftModulator::ApplyWindow(.5f, .5f, 1.f) - .6f) < 1e-6f);
    /* ... and center 5% at 50% amount slides to [0, 0.2] */
    assert(fabsf(DriftModulator::ApplyWindow(.05f, .5f, 0.f)) < 1e-7f);
    assert(fabsf(DriftModulator::ApplyWindow(.05f, .5f, 1.f) - .2f) < 1e-6f);

    printf("ok: window examples\n");
}

static void test_window_zero_amount_exact()
{
    /* Amount 0 must return the center bit-exactly for any u - this is
     * what keeps the engine bit-identical to the unmodulated sound. */
    const float centers[] = {0.f, .03125f, .5f, .731f, .95f, 1.f};
    const float us[]      = {0.f, .37f, 1.f};
    for(float c : centers)
        for(float u : us)
            assert(DriftModulator::ApplyWindow(c, 0.f, u) == c);
    printf("ok: zero amount returns center exactly\n");
}

static void test_window_bounds_and_width()
{
    /* Across the whole domain: output stays in [0,1] and the window
     * width is exactly 2d = 0.4 * amount (slid, never clipped). */
    for(float c = 0.f; c <= 1.f; c += .013f)
    {
        for(float a = 0.f; a <= 1.f; a += .07f)
        {
            const float lo = DriftModulator::ApplyWindow(c, a, 0.f);
            const float hi = DriftModulator::ApplyWindow(c, a, 1.f);
            assert(lo >= -1e-6f && hi <= 1.f + 1e-6f);
            assert(fabsf((hi - lo) - .4f * a) < 1e-6f);
        }
    }
    printf("ok: window bounds and width\n");
}

/** Run a modulator for `seconds`, collecting per-block outputs and
 *  direction-flip statistics. Block-rate: u is constant within a block. */
struct RunStats
{
    double min_half_s     = 1e9;
    double max_half_s     = 0.0;
    double min_cycle_s    = 1e9;
    double max_cycle_s    = 0.0;
    int    half_count     = 0;
    int    rise_count     = 0;
    int    fall_count     = 0;
    float  min_u          = 1e30f;
    float  max_u          = -1e30f;
    float  max_block_step = 0.f;
    bool   bounds_ok      = true;
};

static RunStats Run(DriftModulator &m, int sr, double seconds)
{
    RunStats st;
    const int blocks = static_cast<int>(seconds * sr / kBlock);
    float    prev_u    = m.Value();
    int      prev_sign = 0;
    long long last_flip = 0;
    double   prev_half  = -1.0;
    for(long long b = 1; b < blocks; b++)
    {
        m.Advance(kBlock);
        const float u = m.Value();
        if(!(u >= 0.f && u <= 1.f))
            st.bounds_ok = false;
        if(u < st.min_u)
            st.min_u = u;
        if(u > st.max_u)
            st.max_u = u;
        const float d = u - prev_u;
        if(fabsf(d) > st.max_block_step)
            st.max_block_step = fabsf(d);
        const int sign = (d > 0.f) - (d < 0.f);
        if(sign != 0 && prev_sign != 0 && sign != prev_sign)
        {
            const double half
                = static_cast<double>(b - last_flip) * kBlock / sr;
            if(last_flip > 0) /* skip the partial first half */
            {
                if(half < st.min_half_s)
                    st.min_half_s = half;
                if(half > st.max_half_s)
                    st.max_half_s = half;
                st.half_count++;
                if(prev_sign > 0)
                    st.rise_count++;
                else
                    st.fall_count++;
                if(prev_half > 0.0)
                {
                    const double cycle = prev_half + half;
                    if(cycle < st.min_cycle_s)
                        st.min_cycle_s = cycle;
                    if(cycle > st.max_cycle_s)
                        st.max_cycle_s = cycle;
                }
                prev_half = half;
            }
            last_flip = b;
            prev_sign = sign;
        }
        else if(sign != 0 && prev_sign == 0)
        {
            prev_sign = sign; /* first movement: direction baseline */
            last_flip = 0;
        }
        prev_u = u;
    }
    return st;
}

static void test_timing_48k()
{
    DriftModulator m;
    m.Init(48000.f, 0x12345678u);
    const RunStats st = Run(m, 48000, 400.0);

    /* enough halves were measured to be meaningful */
    assert(st.half_count >= 40);
    assert(st.rise_count >= 10 && st.fall_count >= 10);

    /* every half within 2.5..10 s (+-2 blocks of detection error),
     * every full cycle within 5..20 s */
    const double tol = 2.0 * kBlock / 48000.0;
    assert(st.min_half_s >= 2.5 - tol && st.max_half_s <= 10.0 + tol);
    assert(st.min_cycle_s >= 5.0 - 2 * tol
           && st.max_cycle_s <= 20.0 + 2 * tol);

    /* durations vary across cycles: with ~64 uniform draws from a 7.5 s
     * range the observed spread is far wider than 2 s */
    assert(st.max_half_s - st.min_half_s > 2.0);

    /* continuity: the steepest block step is bounded by the raised
     * cosine's max slope (pi/2) at the shortest half duration */
    const float bound = 1.5708f * kBlock / (2.5f * 48000.f) * 1.05f;
    assert(st.max_block_step <= bound);

    /* both window edges are reached */
    assert(st.min_u < .02f && st.max_u > .98f);
    assert(st.bounds_ok);
    printf("ok: timing/continuity at 48 kHz "
           "(halves %.3f..%.3f s, cycles %.3f..%.3f s, %d halves)\n",
           st.min_half_s, st.max_half_s, st.min_cycle_s, st.max_cycle_s,
           st.half_count);
}

static void test_timing_32k()
{
    DriftModulator m;
    m.Init(32000.f, 0x2468ACE0u);
    const RunStats st = Run(m, 32000, 400.0);

    assert(st.half_count >= 40);
    assert(st.rise_count >= 10 && st.fall_count >= 10);
    const double tol = 2.0 * kBlock / 32000.0;
    assert(st.min_half_s >= 2.5 - tol && st.max_half_s <= 10.0 + tol);
    assert(st.min_cycle_s >= 5.0 - 2 * tol
           && st.max_cycle_s <= 20.0 + 2 * tol);
    assert(st.max_half_s - st.min_half_s > 2.0);
    const float bound = 1.5708f * kBlock / (2.5f * 32000.f) * 1.05f;
    assert(st.max_block_step <= bound);
    assert(st.min_u < .02f && st.max_u > .98f);
    assert(st.bounds_ok);
    printf("ok: timing/continuity at 32 kHz\n");
}

static void test_determinism()
{
    /* Same seed -> identical block-rate sequences, at 48 kHz... */
    DriftModulator a, b;
    a.Init(48000.f, 0x0BADF00Du);
    b.Init(48000.f, 0x0BADF00Du);
    for(int i = 0; i < 200000; i++)
    {
        a.Advance(kBlock);
        b.Advance(kBlock);
        assert(a.Value() == b.Value());
    }
    /* ... and at 32 kHz */
    DriftModulator c, d;
    c.Init(32000.f, 0x0BADF00Du);
    d.Init(32000.f, 0x0BADF00Du);
    for(int i = 0; i < 100000; i++)
    {
        c.Advance(kBlock);
        d.Advance(kBlock);
        assert(c.Value() == d.Value());
    }

    /* A different seed diverges immediately (different start phase). */
    DriftModulator e;
    e.Init(48000.f, 0x0BADF00Eu);
    assert(e.Value() != a.Value());

    /* Seed 0 is remapped (xorshift32 fixed point): it still drifts. */
    DriftModulator z;
    z.Init(48000.f, 0u);
    const float z0 = z.Value();
    bool moved = false;
    for(int i = 0; i < 20000; i++)
    {
        z.Advance(kBlock);
        if(z.Value() != z0)
            moved = true;
    }
    assert(moved);
    printf("ok: determinism\n");
}

static void test_independence()
{
    /* The engine's eight fixed seeds: the modulators must never run in
     * lockstep - over a minute the spread across the 8 outputs is wide
     * most of the time. */
    static const uint32_t kSeeds[8]
        = {0x2545F491u, 0x8F3A7B15u, 0x6C8E9CF1u, 0x1F123BB5u,
           0xD1B54A35u, 0x9E3779B9u, 0x4A3C6D27u, 0x85A5E1B3u};
    DriftModulator mods[8];
    for(int i = 0; i < 8; i++)
        mods[i].Init(48000.f, kSeeds[i]);

    const int blocks = 60 * 48000 / kBlock; /* one minute */
    int       wide   = 0;
    for(int b = 0; b < blocks; b++)
    {
        float mn = 1e30f, mx = -1e30f;
        for(int i = 0; i < 8; i++)
        {
            mods[i].Advance(kBlock);
            const float u = mods[i].Value();
            if(u < mn)
                mn = u;
            if(u > mx)
                mx = u;
        }
        if(mx - mn > .2f)
            wide++;
    }
    assert(wide > blocks / 2);
    printf("ok: independence (8 seeds, %.1f%% of blocks spread > 0.2)\n",
           100.0 * wide / blocks);
}

static void test_rate_scaling()
{
    /* rate 2 halves every drawn duration: same seed -> same duration
     * sequence, about twice as many half-swings over the same window;
     * the raised-cosine shape and bounds are unchanged. */
    DriftModulator slow, fast;
    slow.Init(48000.f, 0x2468ACE0u, 1.f);
    fast.Init(48000.f, 0x2468ACE0u, 2.f);
    const RunStats s = Run(slow, 48000, 400.0);
    const RunStats f = Run(fast, 48000, 400.0);
    assert(f.half_count > s.half_count + s.half_count * 4 / 5);
    assert(f.half_count < s.half_count + s.half_count * 6 / 5);
    assert(f.bounds_ok);
    printf("ok: rate scaling (%d vs %d halves)\n", s.half_count,
           f.half_count);
}

static void test_length_seed_independence()
{
    /* The engine's four pipe-length modulation seeds (OrganEngine
     * length_mod_[]): like the volume/Character seeds, the pipe-length
     * drifts must never run in lockstep - over a minute the spread
     * across the 4 outputs is wide most of the time. */
    static const uint32_t kSeeds[4]
        = {0x7F4A7C15u, 0x1B873593u, 0xB5297A4Du, 0x02B5F5A9u};
    DriftModulator mods[4];
    for(int i = 0; i < 4; i++)
        mods[i].Init(48000.f, kSeeds[i]);

    const int blocks = 60 * 48000 / kBlock; /* one minute */
    int       wide   = 0;
    for(int b = 0; b < blocks; b++)
    {
        float mn = 1e30f, mx = -1e30f;
        for(int i = 0; i < 4; i++)
        {
            mods[i].Advance(kBlock);
            const float u = mods[i].Value();
            if(u < mn)
                mn = u;
            if(u > mx)
                mx = u;
        }
        if(mx - mn > .2f)
            wide++;
    }
    assert(wide > blocks / 2);
    printf("ok: length-seed independence (4 seeds, %.1f%% of blocks spread > 0.2)\n",
           100.0 * wide / blocks);
}

int main()
{
    test_window_examples();
    test_window_zero_amount_exact();
    test_window_bounds_and_width();
    test_timing_48k();
    test_timing_32k();
    test_determinism();
    test_independence();
    test_length_seed_independence();
    test_rate_scaling();
    printf("ALL DRIFT MODULATOR TESTS PASSED\n");
    return 0;
}

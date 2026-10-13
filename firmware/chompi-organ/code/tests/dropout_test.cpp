/** @file dropout_test.cpp
 *  @brief Native assert-based numerical tests for Dropout.h.
 *
 *  Pure host C++: no MCU/Daisy dependencies. Covers:
 *   - exact bit-pass-through bypass at amount 0 (even while dropouts
 *     and crackle run internally)
 *   - mono input -> identical L/R outputs at amount 0
 *   - dropouts occur within bounded time at full drive (deterministic
 *     fixed seeds)
 *   - dropout gain ramps are click-free (bounded sample-to-sample
 *     delta; an un-ramped full-mute event would exceed the bound)
 *   - per-channel independence: both channels are rarely deep in a
 *     dropout at the same time
 *   - per-channel skew: the 3.2 kHz right low-pass darkens a ~10 kHz
 *     sine more than the 4.0 kHz left
 *
 *  Run: make -C firmware/chompi-organ/code/tests test
 */
#include <cassert>
#include <cmath>
#include <cstdio>
#include "Dropout.h"

using chompi::Dropout;

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

static void test_bypass_bit_exact()
{
    Dropout d;
    d.Init(48000.f);
    d.PrepareBlock(1.f); /* dropouts/crackle run internally... */
    unsigned rng = 999u;
    for(int i = 0; i < 96000; i++) /* 2 s: spans several dropouts */
    {
        rng           = rng * 1664525u + 1013904223u;
        const float a = (rng >> 8) * (1.f / 8388608.f) - 1.f;
        rng           = rng * 1664525u + 1013904223u;
        const float b = (rng >> 8) * (1.f / 8388608.f) - 1.f;
        float l = a, r = b;
        d.Process(l, r, 0.f); /* ...but amount 0 is an exact bypass */
        assert(l == a && r == b);
    }
}

static void test_mono_identical_at_zero_amount()
{
    Dropout d;
    d.Init(48000.f);
    d.PrepareBlock(1.f);
    for(int i = 0; i < 4800; i++)
    {
        const float x = .3f * sinf(2.f * kPi * i * .01f);
        float       l = x, r = x;
        d.Process(l, r, 0.f);
        assert(l == r && l == x);
    }
}

static void test_dropouts_occur_bounded()
{
    /* At full drive each channel must drop out within a bounded time:
     * the dense interval range is 0.15-0.8 s, so 2 s per channel is a
     * generous bound. */
    for(int ch = 0; ch < 2; ch++)
    {
        Dropout d;
        d.Init(48000.f);
        d.PrepareBlock(1.f);
        float     min_out = 1.f;
        const int n       = 2 * 48000;
        for(int i = 0; i < n; i++)
        {
            float l = .5f, r = .5f;
            d.Process(l, r, 1.f);
            const float o = ch == 0 ? l : r;
            if(o < min_out)
                min_out = o;
        }
        assert(min_out < .2f); /* a full-mute-class dropout happened */
    }
}

static void test_dropout_ramps_click_free()
{
    /* DC input, mid amount (the skew blend then passes half of any
     * step): the per-sample delta must stay far below what an un-ramped
     * full-mute dropout would produce (~0.17 in one sample). Crackle
     * impulses are the only intentional discontinuity and are bounded
     * (~0.07 here). */
    Dropout d;
    d.Init(48000.f);
    d.PrepareBlock(1.f);
    float     prev      = .5f;
    float     max_delta = 0.f;
    const int n         = 4 * 48000;
    for(int i = 0; i < n; i++)
    {
        float l = .5f, r = .5f;
        d.Process(l, r, .5f);
        const float dl = fabsf(l - prev);
        if(dl > max_delta)
            max_delta = dl;
        prev = l;
    }
    assert(max_delta < .15f);
}

static void test_per_channel_independence()
{
    /* Over a long run at full drive, both channels are rarely deep in a
     * dropout at the same time (independent state machines/PRNGs). */
    Dropout d;
    d.Init(48000.f);
    d.PrepareBlock(1.f);
    int       both = 0, either = 0;
    const int n = 20 * 48000;
    for(int i = 0; i < n; i++)
    {
        float l = .5f, r = .5f;
        d.Process(l, r, 1.f);
        const bool ld = l < .2f, rd = r < .2f;
        if(ld && rd)
            both++;
        if(ld || rd)
            either++;
    }
    assert(either > 0);
    assert(both * 2 < either); /* far from persistently simultaneous */
}

static void test_skew_darkens_right_more()
{
    /* 9960.9 Hz coherent sine (bin 850 of 4096 at 48 kHz), drive 0 (the
     * sparse first dropout is >= 1.2 s away, so the measurement window
     * is undisturbed) and amount 1 (skew fully applied). One-pole
     * estimates: ~0.40 of input for the 4 kHz left, ~0.33 for the
     * 3.2 kHz right. */
    Dropout d;
    d.Init(48000.f);
    d.PrepareBlock(0.f);
    static float lbuf[4096], rbuf[4096];
    const int    warm = 480; /* skew LP settle */
    for(int i = 0; i < warm; i++)
    {
        const float x = .3f * sinf(2.f * kPi * 850 * i / 4096);
        float       l = x, r = x;
        d.Process(l, r, 1.f);
    }
    for(int i = 0; i < 4096; i++)
    {
        const float x = .3f * sinf(2.f * kPi * 850 * (warm + i) / 4096);
        float       l = x, r = x;
        d.Process(l, r, 1.f);
        lbuf[i] = l;
        rbuf[i] = r;
    }
    const float ml = BinMag(lbuf, 4096, 850);
    const float mr = BinMag(rbuf, 4096, 850);
    assert(ml > .08f && ml < .15f);
    assert(mr > .05f && mr < .12f);
    assert(mr < ml * .92f); /* right channel audibly darker */
}

int main()
{
    test_bypass_bit_exact();
    test_mono_identical_at_zero_amount();
    test_dropouts_occur_bounded();
    test_dropout_ramps_click_free();
    test_per_channel_independence();
    test_skew_darkens_right_more();
    printf("dropout_test: all checks passed\n");
    return 0;
}

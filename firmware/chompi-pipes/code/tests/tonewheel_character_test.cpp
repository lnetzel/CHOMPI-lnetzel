/** @file tonewheel_character_test.cpp
 *  @brief Native assert-based numerical tests for TonewheelCharacter.h.
 *
 *  Pure host C++: no MCU/Daisy dependencies. Covers:
 *   - four independent pipe amounts (and independent instances)
 *   - step clamping and invalid-index guards
 *   - exact clean endpoints (settled step 0 returns the sine bit-exactly)
 *   - ~10 ms ramp convergence + retargeting at 48 kHz and 44.1 kHz
 *   - stable note/pipe variation, independent of voice slot
 *   - cycle-mean DC of the shaped waveform
 *   - expected 2nd/3rd harmonic content (coherent DFT bin projections)
 *   - amplitude bounds (aged waveform never exceeds magnitude 1)
 *   - high-frequency harmonic suppression (anti-alias masks)
 *   - 8'-pipe bass grit: low-frequency harmonic boost, band edges,
 *     pipes 1-3 untouched, boosted harmonic content (coherent DFT)
 *   - folded-frequency sweep: bounds hold, zero amount stays exact sine
 *
 *  Run: make -C firmware/chompi-pipes/code/tests test
 */
#include <cassert>
#include <cmath>
#include <cstdio>
#include "TonewheelCharacter.h"

using chompi::TonewheelCharacter;

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

static void test_independence()
{
    TonewheelCharacter tc;
    tc.Init(48000.f);
    tc.SetStep(0, 10);
    tc.SetStep(1, 20);
    tc.SetStep(2, 33);
    tc.SetStep(3, 64);
    for(int p = 0; p < 4; p++)
    {
        const int expect = p == 0 ? 10 : (p == 1 ? 20 : (p == 2 ? 33 : 64));
        assert(tc.GetStep(p) == expect);
    }

    /* advancing/settling pipe 0 must not move the others' amounts */
    tc.SetStep(0, 64);
    for(int i = 0; i < 600; i++)
        tc.Advance();
    assert(tc.Amount(0) == 1.f);
    for(int p = 1; p < 4; p++)
    {
        const float expect = (p == 1 ? 20.f : (p == 2 ? 33.f : 64.f)) / 64.f;
        assert(fabsf(tc.Amount(p) - expect) < 1e-6f);
    }

    /* separate instances are independent too */
    TonewheelCharacter a, b;
    a.Init(48000.f);
    b.Init(48000.f);
    a.SetStep(2, 64);
    for(int i = 0; i < 600; i++)
    {
        a.Advance();
        b.Advance();
    }
    assert(a.Amount(2) == 1.f);
    assert(b.Amount(2) == 0.f);
    printf("ok: independence\n");
}

static void test_clamps_and_guards()
{
    TonewheelCharacter tc;
    tc.Init(48000.f);
    tc.SetStep(1, -5);
    assert(tc.GetStep(1) == 0);
    tc.SetStep(1, 100);
    assert(tc.GetStep(1) == 64);
    tc.SetStep(1, 32);
    assert(tc.GetStep(1) == 32);

    /* invalid indices: no array access, no crash, getters return 0 */
    tc.SetStep(-1, 10);
    tc.SetStep(4, 10);
    assert(tc.GetStep(-1) == 0);
    assert(tc.GetStep(4) == 0);
    assert(tc.Amount(-1) == 0.f);
    assert(tc.Amount(4) == 0.f);
    tc.UpdateVoiceCoeffs(-1, 60, 0, 440.f); /* must not touch anything */
    tc.UpdateVoiceCoeffs(6, 60, 0, 440.f);
    tc.UpdateVoiceCoeffs(0, 60, -1, 440.f);
    tc.UpdateVoiceCoeffs(0, 60, 4, 440.f);
    printf("ok: clamps and guards\n");
}

static void test_exact_clean_endpoints()
{
    TonewheelCharacter tc;
    tc.Init(48000.f);
    TonewheelCharacter::PipeCoeffs c;
    c.second_gain = 0.03f;
    c.third_gain  = 0.07f;
    c.norm        = 1.f / (1.f + c.second_gain + c.third_gain);

    /* boot: every pipe settled at 0 */
    for(int p = 0; p < 4; p++)
    {
        assert(tc.Amount(p) == 0.f);
        assert(tc.EasedAmount(p) == 0.f);
    }

    /* eased 0 returns the sine bit-exactly, including s == 0 */
    for(float s = -1.f; s <= 1.f; s += 0.0137f)
        assert(TonewheelCharacter::ShapeSample(s, c, 0.f) == s);
    assert(TonewheelCharacter::ShapeSample(0.f, c, 0.f) == 0.f);

    /* ramp to max and back: both endpoints settle exactly */
    tc.SetStep(0, 64);
    for(int i = 0; i < 600; i++)
        tc.Advance();
    assert(tc.Amount(0) == 1.f);
    tc.SetStep(0, 0);
    for(int i = 0; i < 600; i++)
        tc.Advance();
    assert(tc.Amount(0) == 0.f);
    assert(tc.EasedAmount(0) == 0.f);
    printf("ok: exact clean endpoints\n");
}

static void test_ramp(float sample_rate)
{
    TonewheelCharacter tc;
    tc.Init(sample_rate);
    const int full = static_cast<int>(TonewheelCharacter::kRampSeconds
                                      * sample_rate);

    /* full swing converges within ramp length + snap sample, exactly */
    tc.SetStep(0, 64);
    for(int i = 0; i < full - 1; i++)
        tc.Advance();
    assert(tc.Amount(0) < 1.f); /* finite: not instant */
    for(int i = 0; i < 4; i++)
        tc.Advance();
    assert(tc.Amount(0) == 1.f);

    /* half swing takes ~half the ramp */
    tc.SetStep(1, 32);
    for(int i = 0; i < full / 2 + 2; i++)
        tc.Advance();
    assert(fabsf(tc.Amount(1) - 0.5f) < 1e-6f);

    /* retarget mid-ramp from the CURRENT amount, no click, exact end */
    tc.SetStep(2, 64);
    for(int i = 0; i < full / 4; i++)
        tc.Advance();
    const float mid = tc.Amount(2);
    assert(mid > 0.2f && mid < 0.3f);
    tc.SetStep(2, 16); /* new target 0.25, below/above current? mid~0.25 */
    tc.SetStep(2, 0);
    for(int i = 0; i < full; i++)
        tc.Advance();
    assert(tc.Amount(2) == 0.f);

    /* amount never leaves [0,1] during a long random-ish retarget run */
    int seed = 12345;
    for(int i = 0; i < 5000; i++)
    {
        if(i % 37 == 0)
        {
            seed = seed * 1103515245 + 12345;
            tc.SetStep(3, (seed >> 16) % 80);
        }
        tc.Advance();
        assert(tc.Amount(3) >= 0.f && tc.Amount(3) <= 1.f);
    }
    printf("ok: ramp at %g Hz\n", static_cast<double>(sample_rate));
}

static void test_variation()
{
    TonewheelCharacter tc;
    tc.Init(48000.f);

    /* same (note, pipe, freq) in different voice slots: identical coeffs */
    tc.UpdateVoiceCoeffs(0, 60, 2, 440.f * 4.f);
    tc.UpdateVoiceCoeffs(5, 60, 2, 440.f * 4.f);
    const TonewheelCharacter::PipeCoeffs &c0 = tc.Coeffs(0, 2);
    const TonewheelCharacter::PipeCoeffs &c5 = tc.Coeffs(5, 2);
    assert(c0.second_gain == c5.second_gain);
    assert(c0.third_gain == c5.third_gain);
    assert(c0.norm == c5.norm);

    /* variation bounded to +/-15% where the AA mask is fully open;
     * pipe 1: no bass grit, so the plain kMax range applies exactly */
    int distinct = 0;
    float prev   = -99.f;
    for(int note = 36; note < 96; note++)
    {
        tc.UpdateVoiceCoeffs(0, note, 1, 220.f); /* harmonics << 0.35*sr */
        const TonewheelCharacter::PipeCoeffs &c = tc.Coeffs(0, 1);
        assert(fabsf(c.second_gain) <= TonewheelCharacter::kMaxSecond * 1.15f + 1e-7f);
        assert(fabsf(c.second_gain) >= TonewheelCharacter::kMaxSecond * 0.85f - 1e-7f);
        assert(fabsf(c.third_gain) <= TonewheelCharacter::kMaxThird * 1.15f + 1e-7f);
        assert(fabsf(c.third_gain) >= TonewheelCharacter::kMaxThird * 0.85f - 1e-7f);
        if(c.third_gain != prev)
        {
            distinct++;
            prev = c.third_gain;
        }
    }
    assert(distinct > 10); /* the variation actually varies by note */
    printf("ok: variation\n");
}

/** Render ncyc coherent cycles of a shaped sine into buf. */
static int render_cycles(float *buf, int cap, float fund_hz, float sr, int ncyc,
                         const TonewheelCharacter::PipeCoeffs &c, float amount)
{
    const int per = static_cast<int>(sr / fund_hz + 0.5f);
    const int n   = per * ncyc;
    assert(n <= cap);
    assert(fabsf(per * fund_hz - sr) < 1.f); /* coherent: integer samples */
    for(int i = 0; i < n; i++)
    {
        const float s = sinf(2.f * kPi * fund_hz * i / sr);
        buf[i]        = TonewheelCharacter::ShapeSample(s, c, amount);
    }
    return n;
}

static void test_cycle_mean_dc()
{
    TonewheelCharacter tc;
    tc.Init(48000.f);
    tc.UpdateVoiceCoeffs(0, 60, 0, 480.f); /* 100 samples/cycle at 48 kHz */
    const TonewheelCharacter::PipeCoeffs &c = tc.Coeffs(0, 0);

    static float buf[4096];
    const int n = render_cycles(buf, 4096, 480.f, 48000.f, 10, c, 1.f);
    double mean = 0.0;
    for(int i = 0; i < n; i++)
        mean += buf[i];
    mean /= n;
    assert(fabs(mean) < 1e-4); /* shaped wave stays zero-mean over cycles */
    printf("ok: cycle-mean DC (mean=%g)\n", mean);
}

static void test_harmonic_content()
{
    TonewheelCharacter tc;
    tc.Init(48000.f);
    tc.UpdateVoiceCoeffs(0, 57, 1, 480.f);
    const TonewheelCharacter::PipeCoeffs &c = tc.Coeffs(0, 1);

    static float buf[4096];
    const int n = render_cycles(buf, 4096, 480.f, 48000.f, 10, c, 1.f);

    /* Coherent DFT: the buffer holds 10 complete cycles, so the
     * fundamental is bin 10 and the 2nd/3rd/4th harmonics are bins
     * 20/30/40. Ratios cancel the normalization, so they must equal
     * the cached gains almost exactly (algebraic identities, float
     * error only). */
    const float b1 = BinMag(buf, n, 10);
    const float b2 = BinMag(buf, n, 20);
    const float b3 = BinMag(buf, n, 30);
    const float b4 = BinMag(buf, n, 40);
    assert(fabsf(b1 - c.norm) < 1e-3f);
    assert(fabsf(b2 / b1 - fabsf(c.second_gain)) < 1e-3f);
    assert(fabsf(b3 / b1 - fabsf(c.third_gain)) < 1e-3f);
    assert(b4 / b1 < 1e-3f); /* no 4th+ harmonic content */

    /* half amount: harmonic-to-fundamental ratio shrinks accordingly */
    render_cycles(buf, 4096, 480.f, 48000.f, 10, c, 0.5f);
    const float h1 = BinMag(buf, n, 10);
    const float h3 = BinMag(buf, n, 30);
    assert(h3 / h1 < b3 / b1);
    printf("ok: harmonic content (g2=%g g3=%g)\n",
           static_cast<double>(c.second_gain),
           static_cast<double>(c.third_gain));
}

static void test_amplitude_bounds()
{
    /* worst case: maximum variation gains on the 8' pipe with full bass
     * grit, mask fully open (pipe-0 grit caps, hotter than the shared
     * kMax ones) */
    TonewheelCharacter::PipeCoeffs c;
    c.second_gain = TonewheelCharacter::kGritMaxSecond * 1.15f
                    * (1.f + TonewheelCharacter::kGritBoost);
    c.third_gain  = TonewheelCharacter::kGritMaxThird * 1.15f
                    * (1.f + TonewheelCharacter::kGritBoost);
    c.norm = 1.f / (1.f + fabsf(c.second_gain) + fabsf(c.third_gain));

    for(float a = 0.f; a <= 1.0001f; a += 0.083f)
    {
        const float amt = a > 1.f ? 1.f : a;
        for(int i = 0; i < 20000; i++)
        {
            const float s   = sinf(2.f * kPi * i / 9973.f);
            const float out = TonewheelCharacter::ShapeSample(s, c, amt);
            assert(fabsf(out) <= 1.f + 1e-6f);
        }
    }
    printf("ok: amplitude bounds\n");
}

static void test_antialias_masks()
{
    TonewheelCharacter tc;
    tc.Init(48000.f);
    const float sr = 48000.f;

    /* probe pipe 1: no grit caps/boost, so the plain kMax bounds apply
     * exactly and the mask is tested in isolation (the mask itself is
     * pipe-independent) */

    /* low fundamental: both harmonics below 0.35*sr -> full strength */
    tc.UpdateVoiceCoeffs(0, 60, 1, 1000.f);
    assert(tc.Coeffs(0, 1).third_gain >= TonewheelCharacter::kMaxThird * 0.85f);

    /* 2nd harmonic above 0.45*sr -> exactly zero; 3rd likewise */
    tc.UpdateVoiceCoeffs(0, 60, 1, 12000.f); /* 2f=24k > .45*48k=21.6k */
    assert(tc.Coeffs(0, 1).second_gain == 0.f);
    assert(tc.Coeffs(0, 1).third_gain == 0.f);

    /* fade band: 3f between .35 and .45 * sr -> partially attenuated,
     * strictly between the variation extremes */
    tc.UpdateVoiceCoeffs(0, 60, 1, 6400.f); /* 3f=19.2k: t=0.5 in band */
    const float g3 = tc.Coeffs(0, 1).third_gain;
    assert(g3 > 0.f);
    assert(g3 < TonewheelCharacter::kMaxThird * 1.15f * 0.6f);

    /* just below the full band edge: mask exactly 1 */
    tc.UpdateVoiceCoeffs(0, 60, 1, 5500.f); /* 2f=11k, 3f=16.5k <= 16.8k */
    assert(fabsf(tc.Coeffs(0, 1).third_gain)
           >= TonewheelCharacter::kMaxThird * 0.85f);
    printf("ok: anti-alias masks (sr=%g)\n", static_cast<double>(sr));
}

static void test_bass_grit()
{
    TonewheelCharacter tc;
    tc.Init(48000.f);

    /* Same note on pipe 0, low vs mid frequency: variation and AA mask
     * are identical (fully open) in both, so the gain ratio is exactly
     * 1 + kGritBoost at the bottom of the band and 1 above it. */
    tc.UpdateVoiceCoeffs(0, 60, 0, 100.f);
    const TonewheelCharacter::PipeCoeffs &lo = tc.Coeffs(0, 0);
    tc.UpdateVoiceCoeffs(1, 60, 0, 1000.f);
    const TonewheelCharacter::PipeCoeffs &mid = tc.Coeffs(1, 0);
    const float expect = 1.f + TonewheelCharacter::kGritBoost;
    assert(fabsf(lo.second_gain / mid.second_gain - expect) < 1e-5f);
    assert(fabsf(lo.third_gain / mid.third_gain - expect) < 1e-5f);

    /* norm is computed from the BOOSTED gains: aged bound unchanged */
    assert(fabsf(lo.norm
                 - 1.f / (1.f + fabsf(lo.second_gain)
                          + fabsf(lo.third_gain)))
           < 1e-7f);

    /* Band edges: exactly full at kGritFullHz, exactly off at
     * kGritZeroHz, strictly decreasing between them. */
    float prev_ratio = expect + 1.f;
    for(float f = TonewheelCharacter::kGritFullHz;
        f <= TonewheelCharacter::kGritZeroHz;
        f += 25.f)
    {
        tc.UpdateVoiceCoeffs(2, 60, 0, f);
        const float r = tc.Coeffs(2, 0).third_gain / mid.third_gain;
        assert(r >= 1.f && r <= expect + 1e-6f);
        assert(r < prev_ratio);
        prev_ratio = r;
    }
    assert(prev_ratio == 1.f); /* exactly off at kGritZeroHz */

    /* Pipes 1-3 get no grit at any frequency: below the AA band their
     * gains are frequency-independent, so 80 Hz must equal 1000 Hz
     * bit-exactly. */
    for(int p = 1; p < 4; p++)
    {
        tc.UpdateVoiceCoeffs(0, 60, p, 80.f);
        tc.UpdateVoiceCoeffs(1, 60, p, 1000.f);
        assert(tc.Coeffs(0, p).second_gain == tc.Coeffs(1, p).second_gain);
        assert(tc.Coeffs(0, p).third_gain == tc.Coeffs(1, p).third_gain);
    }

    /* Coherent DFT at the bottom of the band: measured 2nd/3rd match
     * the boosted gains, 4th+ stays at the noise floor. The buffer holds
     * 5 complete cycles, so the fundamental is bin 5 and the 2nd/3rd/4th
     * harmonics are bins 10/15/20. */
    tc.UpdateVoiceCoeffs(0, 48, 0, 80.f);
    const TonewheelCharacter::PipeCoeffs &c = tc.Coeffs(0, 0);
    static float buf[4096];
    const int n = render_cycles(buf, 4096, 80.f, 48000.f, 5, c, 1.f);
    const float b1 = BinMag(buf, n, 5);
    assert(fabsf(BinMag(buf, n, 10) / b1 - fabsf(c.second_gain)) < 1e-3f);
    assert(fabsf(BinMag(buf, n, 15) / b1 - fabsf(c.third_gain)) < 1e-3f);
    assert(BinMag(buf, n, 20) / b1 < 1e-3f);
    printf("ok: bass grit (g2=%g g3=%g)\n",
           static_cast<double>(c.second_gain),
           static_cast<double>(c.third_gain));
}

static void test_frequency_sweep()
{
    TonewheelCharacter tc;
    tc.Init(48000.f);

    /* sweep folded frequencies across the whole organ range: bounds hold
     * at full Character, zero amount stays the exact clean sine */
    for(float f = 20.f; f <= 20000.f; f *= 1.1892f)
    {
        tc.UpdateVoiceCoeffs(0, 60, 0, f);
        const TonewheelCharacter::PipeCoeffs &c = tc.Coeffs(0, 0);
        assert(c.norm > 0.f && c.norm <= 1.f);
        for(int i = 0; i < 2000; i++)
        {
            const float s = sinf(2.f * kPi * i / 199.f);
            assert(fabsf(TonewheelCharacter::ShapeSample(s, c, 1.f))
                   <= 1.f + 1e-6f);
            assert(TonewheelCharacter::ShapeSample(s, c, 0.f) == s);
        }
    }
    printf("ok: frequency sweep\n");
}

static void test_modulated_target()
{
    /* SetModulatedTarget (Modulation Mode): the ramp follows the
     * modulated target while GetStep keeps reporting the user center;
     * restoring the exact center settles bit-exactly. */
    TonewheelCharacter tc, ref;
    tc.Init(48000.f);
    ref.Init(48000.f);
    tc.SetStep(2, 32);
    ref.SetStep(2, 32);
    for(int i = 0; i < 600; i++)
    {
        tc.Advance();
        ref.Advance();
    }
    assert(tc.Amount(2) == ref.Amount(2));

    /* retarget to a modulated value: the ramp moves, the step is
     * unchanged, and the ramp snaps to the exact target */
    tc.SetModulatedTarget(2, .9f);
    tc.Advance();
    assert(tc.Amount(2) > .5f && tc.Amount(2) < .9f); /* ramping */
    assert(tc.GetStep(2) == 32);
    for(int i = 0; i < 600; i++)
        tc.Advance();
    assert(tc.Amount(2) == .9f); /* exact endpoint snap */
    assert(tc.GetStep(2) == 32);

    /* restore the exact center expression used by OrganEngine
     * (step * 1/64): settles bit-exactly on the unmodulated amount */
    tc.SetModulatedTarget(2, 32.f * (1.f / 64.f));
    for(int i = 0; i < 600; i++)
        tc.Advance();
    assert(tc.Amount(2) == ref.Amount(2));

    /* clamps and guards */
    tc.SetModulatedTarget(1, 2.f);
    tc.SetModulatedTarget(-1, .5f); /* no-op, no crash */
    tc.SetModulatedTarget(4, .5f);  /* no-op, no crash */
    for(int i = 0; i < 600; i++)
        tc.Advance();
    assert(tc.Amount(1) == 1.f);
    tc.SetModulatedTarget(1, -1.f);
    for(int i = 0; i < 600; i++)
        tc.Advance();
    assert(tc.Amount(1) == 0.f);

    /* block-rate retargeting like OrganEngine::Prepare(): the amount
     * tracks smoothly and never overshoots the target band */
    tc.SetModulatedTarget(0, .2f);
    for(int i = 0; i < 600; i++)
        tc.Advance();
    float lo = 1.f, hi = 0.f;
    for(int b = 0; b < 200; b++)
    {
        const float t = .2f + .6f * fabsf(sinf(.13f * b));
        tc.SetModulatedTarget(0, t);
        for(int i = 0; i < 24; i++)
            tc.Advance();
        const float a = tc.Amount(0);
        if(a < lo)
            lo = a;
        if(a > hi)
            hi = a;
    }
    assert(lo >= .2f - 1e-6f && hi <= .8f + 1e-6f);
    printf("ok: modulated target\n");
}

int main()
{
    test_independence();
    test_clamps_and_guards();
    test_exact_clean_endpoints();
    test_ramp(48000.f);
    test_ramp(44100.f);
    test_variation();
    test_cycle_mean_dc();
    test_harmonic_content();
    test_amplitude_bounds();
    test_antialias_masks();
    test_bass_grit();
    test_frequency_sweep();
    test_modulated_target();
    printf("ALL TONEWHEEL CHARACTER TESTS PASSED\n");
    return 0;
}

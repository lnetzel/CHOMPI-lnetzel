/** @file distortion_test.cpp
 *  @brief Native assert-based numerical tests for Distortion.h.
 *
 *  Pure host C++: no MCU/Daisy dependencies. Covers:
 *   - init defaults (tone boots at 100%) and tone setter clamping
 *     (0..100, no wrap); changing the tone never disturbs the drive path
 *   - tone mapping: cutoff endpoints (500 Hz at 0%, the 10 kHz
 *     coefficient expression at 100%) and strict monotonicity
 *   - cascade reference regression: an independent reimplementation of
 *     the two-stage low-pass DSP matches the fuzz sample-for-sample at
 *     tone 100 across all nonzero drive steps and representative
 *     signals at 48 kHz and 32 kHz (settled first)
 *   - measured frequency response against the analytical digital
 *     cascade magnitude (H1 = c/sqrt(1+(1-c)^2-2(1-c)cos w),
 *     Hcascade = H1^2): the 2 kHz -> 4 kHz roll-off at tone 0 sits in
 *     a justified ~10..13 dB interval and within 0.5 dB of the
 *     analytical response at 48 kHz and 32 kHz
 *   - exact dry bypass at settled zero amount for every tone setting,
 *     including transitions to zero with residual filter state and a
 *     nonzero smoothed drive value
 *   - silent input -> silence after filter settling at several tones
 *   - darkening: a steady 6 kHz sine's fundamental is attenuated by at
 *     least 30 dB at tone 0 relative to tone 100, monotonic at
 *     intermediate settings, and low-frequency output stays nonzero at
 *     tone 0 (0% is not a mute)
 *   - deterministic reinitialization clears both filter histories
 *   - tone coefficient ramp: finite ~10 ms settlement with exact
 *     endpoint snap, bounded per-sample movement, and mid-ramp
 *     retargeting continuing from the live coefficient (48 kHz/32 kHz)
 *   - drive-gain ramp: pre-gain and post-normalization glide toward
 *     their per-block targets over ~10 ms with an exact endpoint snap,
 *     so encoder ticks never step the gain audibly
 *   - fuzz symmetry/harmonic content and bounded output across amount
 *     and tone sweeps
 *   - input-referenced FUZZ output leveling: exactly unity at boot, at
 *     drive 0 and at settled bypass; servo-matches the fuzzed level
 *     down to the measured dry level plus the +1.5 dB headroom
 *     (per-block RMS, ~50 ms smoothing), never boosts beyond the
 *     clamp, floors at -30 dB; deep trim on quiet material,
 *     mild trim on loud material; bounded click-free transitions at
 *     48 kHz and 32 kHz
 *
 *  Run: make -C firmware/chompi-pipes/code/tests test
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

/** Independent reference of the current settled two-stage Fuzz DSP,
 *  replicating its exact arithmetic: quadratic pre-gain
 *  g = 1 + 127 d^2, soft-knee clipper t - t^3/3 bounded to +/-2/3,
 *  0.33/FuzzClip(0.33 g) post normalization, two identical cascaded
 *  one-pole low-pass stages sharing one coefficient, ~10 ms
 *  target-driven wet ramp with exact-endpoint snap. Guards the cascade
 *  arithmetic against accidental changes; intentionally says nothing
 *  about the pre-cascade single-pole sound (tone 100 is now darker by
 *  design). */
class CascadeFuzzReference
{
  public:
    void Init(float sample_rate)
    {
        sample_rate_  = sample_rate;
        wet_ramp_inc_ = 1.f / (.010f * sample_rate);
        fuzz_lp_c_    = 1.f - expf(-kTwoPi * 10000.f / sample_rate);
        wet_fuzz_     = 0.f;
        drive_target_ = 0.f;
        fuzz_lp_      = 0.f;
        fuzz_lp2_     = 0.f;
        PrepareBlock(0.f);
    }

    void PrepareBlock(float drive_target)
    {
        drive_target_ = drive_target < 0.f ? 0.f
                                           : (drive_target > 1.f ? 1.f
                                                                 : drive_target);
        const float d = drive_target_;
        g_fuzz_    = 1.f + d * d * 127.f;
        post_fuzz_ = .33f / FuzzClip(.33f * g_fuzz_);
    }

    float Process(float in)
    {
        const float wet_target = drive_target_ > 0.f ? 1.f : 0.f;
        wet_fuzz_ = RampToward(wet_fuzz_, wet_target, wet_ramp_inc_);
        const float wet = FuzzFiltered(FuzzShaped(in));
        return in + wet_fuzz_ * (wet - in);
    }

  private:
    static constexpr float kTwoPi = 6.283185307179586f;

    static float RampToward(float cur, float target, float inc)
    {
        if(cur < target)
        {
            cur += inc;
            return cur > target ? target : cur;
        }
        if(cur > target)
        {
            cur -= inc;
            return cur < target ? target : cur;
        }
        return cur;
    }

    static float FuzzClip(float t)
    {
        if(t > 1.f)
            return .666666666666667f;
        if(t < -1.f)
            return -.666666666666667f;
        return t - t * t * t * .333333333333333f;
    }

    float FuzzShaped(float x) const
    {
        return FuzzClip(g_fuzz_ * x) * post_fuzz_;
    }

    /* two identical cascaded one-pole stages, shared coefficient */
    float FuzzFiltered(float s)
    {
        fuzz_lp_ += fuzz_lp_c_ * (s - fuzz_lp_);
        fuzz_lp2_ += fuzz_lp_c_ * (fuzz_lp_ - fuzz_lp2_);
        return fuzz_lp2_;
    }

    float sample_rate_, wet_ramp_inc_, fuzz_lp_c_;
    float wet_fuzz_, drive_target_, g_fuzz_, post_fuzz_;
    float fuzz_lp_, fuzz_lp2_;
};

static void test_init_and_clamp()
{
    Distortion d;
    d.Init(48000.f);
    assert(d.GetTonePercent() == 100); /* boot default */

    d.SetTonePercent(40);
    assert(d.GetTonePercent() == 40);
    d.SetTonePercent(137); /* clamped at the top end, no wrap */
    assert(d.GetTonePercent() == 100);
    d.SetTonePercent(-25); /* clamped at the bottom end */
    assert(d.GetTonePercent() == 0);
}

static void test_tone_mapping(float sr)
{
    Distortion d;
    d.Init(sr);

    /* endpoints: 0% = 500 Hz, 100% = the original 10 kHz coefficient
     * expression (bit-identical) */
    d.SetTonePercent(0);
    const float c0 = 1.f - expf(-6.283185307179586f * 500.f / sr);
    assert(d.FuzzLpCoeffTarget() == c0);
    d.SetTonePercent(100);
    const float c100 = 1.f - expf(-6.283185307179586f * 10000.f / sr);
    assert(d.FuzzLpCoeffTarget() == c100);
    assert(c0 < c100);

    /* strictly monotonic: higher tone = brighter (larger coefficient) */
    float prev = -1.f;
    for(int p = 0; p <= 100; p++)
    {
        d.SetTonePercent(p);
        const float c = d.FuzzLpCoeffTarget();
        assert(c > prev);
        prev = c;
    }
}

static void test_reference_equality(float sr)
{
    /* Tone 100 must reproduce the independent two-stage cascade
     * reference sample-for-sample across all nonzero drive steps and
     * representative signals; the removed type-selection fade is
     * excluded by settling before comparing. */
    for(int step = 1; step <= 32; step++)
    {
        const float drv = static_cast<float>(step) * (1.f / 32.f);
        for(int sig = 0; sig < 3; sig++)
        {
            Distortion d;
            d.Init(sr);
            d.SetTonePercent(100);
            CascadeFuzzReference r;
            r.Init(sr);
            d.PrepareBlock(drv);
            r.PrepareBlock(drv);
            unsigned rng = 12345u + sig;
            const int  total = static_cast<int>(.2f * sr); /* 200 ms */
            for(int i = 0; i < total; i++)
            {
                float in;
                if(sig == 0)
                    in = .3f * sinf(2.f * kPi * 100.f * i / sr);
                else if(sig == 1)
                    in = .3f * sinf(2.f * kPi * 1000.f * i / sr);
                else
                {
                    rng = rng * 1664525u + 1013904223u;
                    in  = (rng >> 8) * (1.f / 8388608.f) - 1.f;
                }
                const float a = d.Process(in, drv);
                const float b = r.Process(in);
                if(i > total / 2) /* settled: compare bit-for-bit */
                    assert(a == b);
            }
        }
    }
}

static void test_zero_bypass_exact()
{
    /* exact dry bypass at settled zero amount for EVERY tone setting */
    for(int tone = 0; tone <= 100; tone++)
    {
        Distortion d;
        d.Init(48000.f);
        d.SetTonePercent(tone);
        d.PrepareBlock(0.f);
        /* settle the ~10 ms coefficient ramp (30 ms of signal) */
        for(int i = 0; i < 1440; i++)
            d.Process(.3f * sinf(2.f * kPi * i * .01f), 0.f);
        static const float xs[] = {-.9f, -.33f, -.02f, 0.f, .02f, .33f, .9f};
        for(int xi = 0; xi < 7; xi++)
            assert(d.Process(xs[xi], 0.f) == xs[xi]); /* bit-exact dry */
    }
}

static void test_transition_to_zero_bypass()
{
    /* drive target -> 0 with residual filter state and a NONZERO
     * smoothed drive value: the wet ramp must settle to bit-exact dry
     * in finite ~10 ms at representative tones */
    static const int tones[] = {0, 25, 50, 75, 100};
    for(int ti = 0; ti < 5; ti++)
    {
        Distortion d;
        d.Init(48000.f);
        d.SetTonePercent(tones[ti]);
        d.PrepareBlock(1.f);
        /* warm up fully wet so the filter holds state */
        for(int i = 0; i < 4800; i++)
            d.Process(.5f * sinf(2.f * kPi * 1000.f * i / 48000.f), 1.f);
        d.PrepareBlock(0.f);
        const int ramp = static_cast<int>(.010f * 48000.f) + 2;
        for(int i = 0; i < ramp + 480; i++)
            d.Process(.3f * sinf(2.f * kPi * 440.f * i / 48000.f), .5f);
        assert(d.Process(-.42f, .5f) == -.42f); /* bit-exact dry */
        assert(d.Process(.17f, .5f) == .17f);
    }
}

static void test_silence_settles()
{
    static const int tones[] = {0, 50, 100};
    for(int ti = 0; ti < 3; ti++)
    {
        Distortion d;
        d.Init(48000.f);
        d.SetTonePercent(tones[ti]);
        d.PrepareBlock(1.f);
        /* warm up with signal so the filter holds state, then go silent */
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

/** Steady-state fundamental magnitude of a coherent sine (bin `bin` of
 *  nsamp at 48 kHz) through the fuzz at full drive and the given tone. */
static float SteadyFundMag(int tone, int bin, int nsamp)
{
    static float buf[4096];
    Distortion d;
    d.Init(48000.f);
    d.SetTonePercent(tone);
    d.PrepareBlock(1.f);
    for(int i = 0; i < 9600; i++) /* 200 ms: ramps + filter settle */
        d.Process(.3f * sinf(2.f * kPi * bin * i / nsamp), 1.f);
    for(int i = 0; i < nsamp; i++)
        buf[i] = d.Process(.3f * sinf(2.f * kPi * bin * i / nsamp), 1.f);
    return BinMag(buf, nsamp, bin);
}

static void test_tone_darkening()
{
    /* 6 kHz sine (bin 512 of 4096 at 48 kHz): at tone 0 (500 Hz
     * per stage, two cascaded poles) the fundamental must be attenuated
     * by >= 30 dB relative to tone 100, with monotonic attenuation at
     * intermediate tones */
    const float m100 = SteadyFundMag(100, 512, 4096);
    const float m75  = SteadyFundMag(75, 512, 4096);
    const float m50  = SteadyFundMag(50, 512, 4096);
    const float m25  = SteadyFundMag(25, 512, 4096);
    const float m0   = SteadyFundMag(0, 512, 4096);
    assert(m100 > m75 && m75 > m50 && m50 > m25 && m25 > m0);
    const float atten_db = 20.f * log10f(m100 / m0);
    assert(atten_db >= 30.f);

    /* tone 0 is not a mute: a 93.75 Hz sine (bin 8) still passes */
    const float lo = SteadyFundMag(0, 8, 4096);
    assert(lo > .1f);
}

static void test_tone_ramp(float sr)
{
    Distortion d;
    d.Init(sr);
    d.PrepareBlock(.5f);
    const int   ramp_samples = static_cast<int>(.010f * sr + .5f);
    const float inc          = 1.f / (.010f * sr);

    /* 100 -> 0: bounded per-sample coefficient movement, exact
     * endpoint snap in finite ~10 ms time */
    d.SetTonePercent(0);
    const float target0 = d.FuzzLpCoeffTarget();
    assert(d.FuzzLpCoeff() != target0); /* genuinely needs to ramp */
    int n = 0;
    for(; n < ramp_samples + 2; n++)
    {
        const float prev = d.FuzzLpCoeff();
        d.Process(.2f, .5f);
        const float cur = d.FuzzLpCoeff();
        assert(fabsf(cur - prev) <= inc + 1e-6f);
        if(cur == target0)
            break;
    }
    assert(n <= ramp_samples);
    assert(d.FuzzLpCoeff() == target0);

    /* rapid mid-ramp retarget 0 -> 100 -> 35: the ramp continues from
     * the LIVE coefficient (no jump larger than one increment) */
    d.SetTonePercent(100);
    const int half = ramp_samples / 2;
    for(int i = 0; i < half; i++)
        d.Process(.2f, .5f);
    d.SetTonePercent(35);
    const float live  = d.FuzzLpCoeff();
    const float t35   = d.FuzzLpCoeffTarget();
    assert(live != t35); /* genuinely mid-ramp */
    int n2 = 0;
    for(; n2 < ramp_samples + 2; n2++)
    {
        const float prev = d.FuzzLpCoeff();
        d.Process(.2f, .5f);
        const float cur = d.FuzzLpCoeff();
        assert(fabsf(cur - prev) <= inc + 1e-6f);
        if(cur == t35)
            break;
    }
    assert(n2 <= ramp_samples);
    assert(d.FuzzLpCoeff() == t35); /* exact endpoint */
}

static void test_fuzz_voicing()
{
    /* symmetric clip -> even harmonics ~0, strong odd content (tone
     * 100, coherent 468.75 Hz sine = bin 40 of 4096 at 48 kHz) */
    static float buf[4096];
    Distortion d;
    d.Init(48000.f);
    d.PrepareBlock(1.f);
    for(int i = 0; i < 4800; i++)
        d.Process(.3f * sinf(2.f * kPi * 40 * i / 4096), 1.f);
    for(int i = 0; i < 4096; i++)
        buf[i] = d.Process(.3f * sinf(2.f * kPi * 40 * i / 4096), 1.f);
    double mean = 0.0;
    for(int i = 0; i < 4096; i++)
        mean += buf[i];
    mean /= 4096;
    assert(fabs(mean) < 2e-3); /* symmetric curve: no DC */
    const float f1 = BinMag(buf, 4096, 40);
    const float f2 = BinMag(buf, 4096, 80);
    const float f3 = BinMag(buf, 4096, 120);
    assert(f2 < .005f * f1);
    assert(f3 > .02f * f1);
    for(int i = 0; i < 4096; i++)
        assert(fabsf(buf[i]) <= .8f); /* bounded by the clip */
}

static void test_bounded_output()
{
    unsigned rng = 12345u;
    static const int tones[] = {0, 25, 50, 75, 100};
    for(int ti = 0; ti < 5; ti++)
    {
        Distortion d;
        d.Init(48000.f);
        d.SetTonePercent(tones[ti]);
        for(int s = 0; s <= 32; s++)
        {
            const float drv = static_cast<float>(s) * (1.f / 32.f);
            d.PrepareBlock(drv);
            for(int i = 0; i < 64; i++)
            {
                rng           = rng * 1664525u + 1013904223u;
                const float x = (rng >> 8) * (1.f / 8388608.f) - 1.f;
                const float y = d.Process(x, drv);
                assert(isfinite(y));
                assert(fabsf(y) <= 1.05f);
            }
        }
    }

    /* continuous tone + drive sweeps: finite, bounded output through
     * every transition */
    Distortion d;
    d.Init(48000.f);
    for(int i = 0; i < 48000; i++)
    {
        if(i % 240 == 0) /* retarget every 5 ms: all directions */
        {
            d.SetTonePercent((i / 240) % 101);
            d.PrepareBlock(static_cast<float>((i / 240) % 33)
                           * (1.f / 32.f));
        }
        const float x = .3f * sinf(2.f * kPi * 1000.f * i / 48000.f);
        const float y = d.Process(x, .5f);
        assert(isfinite(y));
        assert(fabsf(y) <= 1.05f);
    }
}

static void test_output_gain()
{
    const int kBlock = 48; /* engine block cadence for the followers */

    /* initial gain is exactly unity */
    Distortion d;
    d.Init(48000.f);
    assert(d.GetOutputGain() == 1.f);
    assert(d.TrimGain() == 1.f);

    /* drive 0 with signal: the output IS the input, so the measured
     * levels match exactly and the trim stays exactly 1 (the clean
     * sound is never trimmed) */
    {
        Distortion e;
        e.Init(48000.f);
        for(int b = 0; b < 200; b++)
        {
            e.PrepareBlock(0.f);
            for(int i = 0; i < kBlock; i++)
                e.Process(.3f * sinf(2.f * kPi * 200.f * (b * kBlock + i)
                                     / 48000.f),
                          0.f);
        }
        assert(e.TrimGain() == 1.f);
        assert(e.GetOutputGain() == 1.f);
    }

    /* quiet input at full drive: deep trim that servo-matches the
     * fuzzed level down to just above the dry level (+1.5 dB) */
    {
        Distortion e;
        e.Init(48000.f);
        for(int b = 0; b < 600; b++) /* ~0.6 s: ramps + followers settle */
        {
            e.PrepareBlock(1.f);
            for(int i = 0; i < kBlock; i++)
                e.Process(.02f * sinf(2.f * kPi * 200.f * (b * kBlock + i)
                                      / 48000.f),
                          1.f);
        }
        assert(e.TrimGain() < .2f); /* deep trim on quiet material */
        assert(e.TrimGain() >= Distortion::kLevelFloor);
        /* servo accuracy: trim x fuzzed level == dry level + 1.5 dB */
        assert(fabsf(e.TrimGain() * e.OutLevel()
                     - Distortion::kLevelHeadroom * e.InLevel())
               < .05f * e.InLevel());
    }

    /* loud input at full drive: only the excess over the dry level
     * (plus headroom) is trimmed, never more (no power lost on loud
     * material) */
    {
        Distortion e;
        e.Init(48000.f);
        for(int b = 0; b < 600; b++)
        {
            e.PrepareBlock(1.f);
            for(int i = 0; i < kBlock; i++)
                e.Process(.3f * sinf(2.f * kPi * 200.f * (b * kBlock + i)
                                     / 48000.f),
                          1.f);
        }
        assert(e.TrimGain() > .5f && e.TrimGain() < 1.f);
        assert(fabsf(e.TrimGain() * e.OutLevel()
                     - Distortion::kLevelHeadroom * e.InLevel())
               < .05f * e.InLevel());
    }

    /* same quiet input at increasing drive: the fuzz gets louder with
     * drive, so the leveling trims deeper - exactly 1 at drive 0,
     * deepest at full drive */
    float trims[3];
    for(int s = 0; s < 3; s++)
    {
        Distortion e;
        e.Init(48000.f);
        const float drv = s == 0 ? 0.f : (s == 1 ? .5f : 1.f);
        for(int b = 0; b < 600; b++)
        {
            e.PrepareBlock(drv);
            for(int i = 0; i < kBlock; i++)
                e.Process(.02f * sinf(2.f * kPi * 200.f * (b * kBlock + i)
                                      / 48000.f),
                          1.f);
        }
        trims[s] = e.TrimGain();
    }
    assert(trims[0] == 1.f);
    assert(trims[1] < 1.f);
    assert(trims[2] < trims[1]);

    /* extremely quiet input: the trim clamps at the -30 dB floor */
    {
        Distortion e;
        e.Init(48000.f);
        for(int b = 0; b < 600; b++)
        {
            e.PrepareBlock(1.f);
            for(int i = 0; i < kBlock; i++)
                e.Process(.001f * sinf(2.f * kPi * 200.f * (b * kBlock + i)
                                       / 48000.f),
                          1.f);
        }
        assert(e.TrimGain() == Distortion::kLevelFloor);
    }

    /* with the leveling engaged, returning the drive target to zero
     * restores EXACT unity in finite ~10 ms time through the wet fade
     * (nonzero smoothed drive), at both sample rates; the gain stays
     * bounded between the trim and unity throughout */
    for(int si = 0; si < 2; si++)
    {
        const float sr = si == 0 ? 48000.f : 32000.f;
        Distortion  e;
        e.Init(sr);
        for(int b = 0; b < 100; b++) /* leveling engages */
        {
            e.PrepareBlock(1.f);
            for(int i = 0; i < 48; i++)
                e.Process(.2f * sinf(2.f * kPi * 200.f * (b * 48 + i) / sr),
                          .5f);
        }
        assert(e.TrimGain() < 1.f);
        const float trim = e.TrimGain();
        const int   ramp = static_cast<int>(.010f * sr) + 2;
        e.PrepareBlock(0.f);
        int n = 0;
        for(; n < ramp + 480; n++)
        {
            e.Process(.2f, .5f); /* nonzero smoothed drive */
            const float og = e.GetOutputGain();
            assert(og >= trim - 1e-4f && og <= 1.f);
            if(og == 1.f)
                break;
        }
        assert(n <= ramp); /* finite ~10 ms restore */
        assert(e.GetOutputGain() == 1.f);
    }

    /* adding the leveling must not change the shaping: Process output
     * is unchanged by the followers (the cascade reference regression
     * above covers the Process arithmetic exhaustively) */
}

static void test_gain_ramp_smooths_ticks()
{
    /* drive-gain ramp: one encoder tick (1/32) at low drive must not
     * step the pre-gain at the block boundary - the live gain glides
     * linearly over ~10 ms instead. With a constant input at a fixed
     * tone, the per-sample output movement right after the retarget is
     * only a tiny fraction of the tick's total eventual change (an
     * instant block-rate step would move most of it in one sample). */
    Distortion d;
    d.Init(48000.f);
    d.SetTonePercent(100);
    d.PrepareBlock(2.f * (1.f / 32.f)); /* drive step 2 */
    for(int i = 0; i < 9600; i++)       /* settle fully (200 ms) */
        d.Process(.1f, .09375f);
    const float before = d.Process(.1f, .09375f);
    d.PrepareBlock(3.f * (1.f / 32.f)); /* one tick up */
    const float first = d.Process(.1f, .09375f);
    float       last  = first;
    for(int i = 0; i < 9600; i++) /* settle the ramp + filter */
    {
        if(i % 48 == 0)
            d.PrepareBlock(3.f * (1.f / 32.f)); /* unchanged target:
                                                   engine-style calls */
        last = d.Process(.1f, .09375f);
    }
    const float total = fabsf(last - before);
    assert(total > 0.f); /* the tick DID change the sound */
    assert(fabsf(first - before) < .05f * total);
}

/** Analytical digital cascade magnitude in dB at frequency f for two
 *  identical one-pole low-pass stages sharing coefficient c:
 *  H1 = c / sqrt(1 + (1-c)^2 - 2 (1-c) cos w); Hcascade = H1^2. */
static float CascadeMagDb(float c, float f, float sr)
{
    const float w  = 2.f * kPi * f / sr;
    const float om = 1.f - c;
    const float h1 = c / sqrtf(1.f + om * om - 2.f * om * cosf(w));
    return 20.f * log10f(h1 * h1);
}

/** Fundamental magnitude of the SHAPED (clipped, unfiltered) fuzz
 *  signal alone; the memoryless clipper arithmetic is replicated here
 *  so the filter measurement can normalize the clipper's fundamental
 *  gain out instead of mistaking clipping for filter attenuation. */
static float ShapedFundMag(float drv, int bin, int nsamp)
{
    const float g    = 1.f + drv * drv * 127.f;
    const float t    = .33f * g;
    const float post = .33f / (t > 1.f ? .666666666666667f
                                       : t - t * t * t * .333333333333333f);
    static float buf[4608];
    for(int i = 0; i < nsamp; i++)
    {
        const float x = .3f * sinf(2.f * kPi * bin * i / nsamp);
        const float u = g * x;
        buf[i] = (u > 1.f ? .666666666666667f
                          : (u < -1.f ? -.666666666666667f
                                      : u - u * u * u * .333333333333333f))
                 * post;
    }
    return BinMag(buf, nsamp, bin);
}

/** Settled filter magnitude in dB at one coherent bin: the measured
 *  output fundamental normalized by the shaped input fundamental. At
 *  settled full drive the wet crossfade is exactly 1, so the output is
 *  exactly the filtered shaped signal. */
static float MeasuredMagDb(int tone, int bin, int nsamp, float sr)
{
    static float buf[4608];
    Distortion d;
    d.Init(sr);
    d.SetTonePercent(tone);
    d.PrepareBlock(1.f);
    const int settle = static_cast<int>(.2f * sr); /* ramps + filter settle */
    for(int i = 0; i < settle; i++)
        d.Process(.3f * sinf(2.f * kPi * bin * i / nsamp), 1.f);
    for(int i = 0; i < nsamp; i++)
        buf[i] = d.Process(.3f * sinf(2.f * kPi * bin * i / nsamp), 1.f);
    return 20.f * log10f(BinMag(buf, nsamp, bin)
                         / ShapedFundMag(1.f, bin, nsamp));
}

static void test_filter_response(float sr)
{
    /* Tone 0 (500 Hz per stage): measure the 2 kHz -> 4 kHz roll-off
     * against the analytical digital cascade. These frequencies probe
     * the stopband away from both the knee and Nyquist, where the
     * cascade's roll-off is close to - but not exactly - the nominal
     * 12 dB/octave; nsamp 4608 makes 2 kHz and 4 kHz exact coherent
     * bins at both 48 kHz (192/384) and 32 kHz (288/576). */
    const int   nsamp = 4608;
    const int   b2k   = static_cast<int>(2000.f * nsamp / sr + .5f);
    const int   b4k   = static_cast<int>(4000.f * nsamp / sr + .5f);
    assert(fabsf(2000.f * nsamp / sr - b2k) < 1e-3f);
    assert(fabsf(4000.f * nsamp / sr - b4k) < 1e-3f);
    const float c0 = 1.f - expf(-2.f * kPi * 500.f / sr);
    const float m2 = MeasuredMagDb(0, b2k, nsamp, sr);
    const float m4 = MeasuredMagDb(0, b4k, nsamp, sr);
    /* each point matches the analytical cascade within 0.5 dB */
    assert(fabsf(m2 - CascadeMagDb(c0, 2000.f, sr)) <= .5f);
    assert(fabsf(m4 - CascadeMagDb(c0, 4000.f, sr)) <= .5f);
    /* and the measured roll-off is the expected ~11.5 dB (justified
     * interval: clearly stronger than the old single pole's ~6 dB,
     * clearly below an idealized exact-12 Butterworth promise) */
    const float rolloff_db = m2 - m4;
    assert(rolloff_db >= 10.f && rolloff_db <= 13.f);
}

static void test_reinit_clears_state()
{
    /* deterministic reinitialization: after a warmup that charges BOTH
     * filter histories (and the leveling followers), Init() restores
     * the exact boot state - a re-initialized instance matches a fresh
     * one bit-for-bit */
    Distortion d;
    d.Init(48000.f);
    d.SetTonePercent(20);
    for(int b = 0; b < 50; b++)
    {
        d.PrepareBlock(1.f);
        for(int i = 0; i < 48; i++)
            d.Process(.4f * sinf(2.f * kPi * 300.f * (b * 48 + i) / 48000.f),
                      1.f);
    }
    d.Init(48000.f);
    Distortion fresh;
    fresh.Init(48000.f);
    for(int b = 0; b < 50; b++)
    {
        const float drv = static_cast<float>(b % 33) * (1.f / 32.f);
        d.PrepareBlock(drv);
        fresh.PrepareBlock(drv);
        for(int i = 0; i < 48; i++)
        {
            const float in = .4f * sinf(2.f * kPi * 300.f * (b * 48 + i)
                                        / 48000.f);
            assert(d.Process(in, drv) == fresh.Process(in, drv));
        }
    }
}

int main()
{
    test_init_and_clamp();
    test_tone_mapping(48000.f);
    test_tone_mapping(32000.f);
    test_reference_equality(48000.f);
    test_reference_equality(32000.f);
    test_zero_bypass_exact();
    test_transition_to_zero_bypass();
    test_gain_ramp_smooths_ticks();
    test_silence_settles();
    test_tone_darkening();
    test_filter_response(48000.f);
    test_filter_response(32000.f);
    test_reinit_clears_state();
    test_tone_ramp(48000.f);
    test_tone_ramp(32000.f); /* ramps scale with the sample rate */
    test_fuzz_voicing();
    test_bounded_output();
    test_output_gain();
    printf("distortion_test: all checks passed\n");
    return 0;
}

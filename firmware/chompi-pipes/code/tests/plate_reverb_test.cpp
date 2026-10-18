/** @file plate_reverb_test.cpp
 *  @brief Native assert-based numerical tests for PlateReverb.h.
 *
 *  Pure host C++: no MCU/Daisy dependencies (PlateReverb.h only needs
 *  WAVE's vendored fx_engine.h via a relative include). Covers:
 *   - silent initialization with a nonzero-prefilled delay buffer
 *   - repeated initialization determinism (re-Init fully resets state)
 *   - finite, bounded, decaying stereo impulse response (no runaway)
 *   - mix clamping
 *   - ~10 ms ramp convergence and mid-ramp retargeting (bit-exact proofs)
 *   - stereo dry preservation at 0% (dry path is bit-exact)
 *   - exact settled endpoints: 0% == dry, 100% == the wet-only reference
 *   - long silence after a tail (bounded, no self-oscillation)
 *   - decay clamping/default (0.5 s at boot) and decay-time ordering
 *     (a 0.5 s tail is gone where a 20 s tail is still strong)
 *   - stereo tank paths: a left-only impulse takes a distinguishable
 *     path through the tank (right output exactly silent until the
 *     cross-feedback tap reaches it, well separated after) with
 *     decay-scaled cross-coupling (inaudible at 0.2 s, clearly audible
 *     at 4 s)
 *   - stereo decorrelation: identical L/R content still produces
 *     meaningfully different L/R wet tails
 *   - approximate L/R symmetry of the two tank paths
 *   - strongly anti-phase stereo input at a long decay stays finite,
 *     bounded and decaying (no runaway feedback)
 *
 *  Run: make -C firmware/chompi-pipes/code/tests test
 */
#include <cassert>
#include <cmath>
#include <cstdio>
#include <stdint.h>
#include "PlateReverb.h"

using chompi::PlateReverb;

static const float kSampleRate = 48000.f;

/** Per-test delay storage (the MCU build places this in SDRAM). */
static PlateReverb::T buf_a[PlateReverb::kDelayBufferSize];
static PlateReverb::T buf_b[PlateReverb::kDelayBufferSize];

static void prefill(PlateReverb::T *buf, PlateReverb::T v)
{
    for(size_t i = 0; i < PlateReverb::kDelayBufferSize; i++)
        buf[i] = v;
}

/** Process n samples of constant stereo input through r. */
static void run(PlateReverb &r, float in_l, float in_r, int n)
{
    float l, r_;
    for(int i = 0; i < n; i++)
        r.Process(in_l, in_r, l, r_);
}

/** Settle the ~10 ms mix ramp (480 samples at 48 kHz; use margin). */
static void settle_mix(PlateReverb &r) { run(r, 0.f, 0.f, 600); }

/** Impulse response helper with independent L/R impulse levels:
 *  settles at the given mix, fires one impulse of (imp_l, imp_r),
 *  records `n` samples of both outputs. */
static void impulse_response_lr(PlateReverb &r, float mix, float imp_l,
                                float imp_r, float *out_l, float *out_r,
                                int n)
{
    r.SetMix(mix);
    settle_mix(r);
    float l, rr;
    r.Process(imp_l, imp_r, l, rr); /* t = 0: the impulse */
    out_l[0] = l;
    out_r[0] = rr;
    for(int i = 1; i < n; i++)
    {
        r.Process(0.f, 0.f, l, rr);
        out_l[i] = l;
        out_r[i] = rr;
    }
}

static void test_silent_init_prefilled_buffer()
{
    prefill(buf_a, 1e30f); /* garbage must not survive Init */
    PlateReverb r;
    r.Init(kSampleRate, buf_a);

    /* silence in -> exact silence out, at any mix */
    r.SetMix(1.f);
    float l = 1.f, rr = 1.f;
    for(int i = 0; i < 48000; i++)
    {
        r.Process(0.f, 0.f, l, rr);
        assert(l == 0.f);
        assert(rr == 0.f);
    }
    /* the buffer really was cleared */
    for(size_t i = 0; i < PlateReverb::kDelayBufferSize; i++)
        assert(buf_a[i] == 0);
    printf("ok: silent init with nonzero-prefilled buffer\n");
}

/** Impulse response helper: settles at the given mix, fires one unit
 *  impulse on both channels, records `n` samples of the left output. */
static void impulse_response(PlateReverb &r, float mix, float *out_l,
                             float *out_r, int n)
{
    impulse_response_lr(r, mix, 1.f, 1.f, out_l, out_r, n);
}

static void test_repeated_init_determinism()
{
    static float a_l[2400], a_r[2400], b_l[2400], b_r[2400];
    PlateReverb r;
    r.Init(kSampleRate, buf_a);
    impulse_response(r, 1.f, a_l, a_r, 2400);

    /* re-Init on the dirty buffer must restore the exact initial state */
    r.Init(kSampleRate, buf_a);
    impulse_response(r, 1.f, b_l, b_r, 2400);

    for(int i = 0; i < 2400; i++)
    {
        assert(a_l[i] == b_l[i]);
        assert(a_r[i] == b_r[i]);
    }
    printf("ok: repeated init determinism\n");
}

static void test_impulse_bounded_and_decaying()
{
    const int n = 3 * 48000; /* 3 s */
    static float ir_l[3 * 48000], ir_r[3 * 48000];
    PlateReverb r;
    r.Init(kSampleRate, buf_a);
    impulse_response(r, 1.f, ir_l, ir_r, n);

    float peak_first = 0.f, peak_last = 0.f;
    for(int i = 0; i < n; i++)
    {
        assert(isfinite(ir_l[i]));
        assert(isfinite(ir_r[i]));
        const float al = fabsf(ir_l[i]), ar = fabsf(ir_r[i]);
        /* bounded: conservative gains keep the tank far from clipping */
        assert(al <= 1.5f);
        assert(ar <= 1.5f);
        if(i < 4800) /* first 100 ms */
        {
            if(al > peak_first)
                peak_first = al;
            if(ar > peak_first)
                peak_first = ar;
        }
        if(i >= n - 24000) /* last 500 ms */
        {
            if(al > peak_last)
                peak_last = al;
            if(ar > peak_last)
                peak_last = ar;
        }
    }
    /* an audible tail exists, and it decays */
    assert(peak_first > 0.01f);
    assert(peak_last < 0.25f * peak_first);
    printf("ok: impulse bounded and decaying (peak %.3f -> %.4f)\n",
           (double)peak_first, (double)peak_last);
}

static void test_mix_clamping()
{
    PlateReverb r;
    r.Init(kSampleRate, buf_a);
    r.SetMix(-0.5f);
    assert(r.GetMix() == 0.f);
    r.SetMix(1.7f);
    assert(r.GetMix() == 1.f);
    r.SetMix(.42f);
    assert(r.GetMix() == .42f);
    printf("ok: mix clamping\n");
}

static void test_stereo_dry_preservation()
{
    PlateReverb r;
    r.Init(kSampleRate, buf_a);
    r.SetMix(0.f);
    settle_mix(r);
    /* distinct L/R content passes through bit-exactly at 0% */
    for(int i = 0; i < 4096; i++)
    {
        const float in_l = .3f * sinf(i * .013f);
        const float in_r = -.2f * sinf(i * .007f);
        float       l, rr;
        r.Process(in_l, in_r, l, rr);
        assert(l == in_l);
        assert(rr == in_r);
    }
    printf("ok: stereo dry preservation\n");
}

static void test_exact_endpoints_and_ramp()
{
    PlateReverb wet_ref, r;
    wet_ref.Init(kSampleRate, buf_a);
    r.Init(kSampleRate, buf_b);

    /* wet-only reference: mix 1 settled before any input */
    wet_ref.SetMix(1.f);
    settle_mix(wet_ref);
    settle_mix(r); /* identical silent history, so the two tanks and
                    * LFOs stay sample-aligned for the comparisons
                    * below (the mix does not touch the tank) */

    /* r: ramp to 1 while processing the same signal; after the ~10 ms
     * ramp settles it must produce the wet-only reference BIT-EXACTLY
     * (any residual dry term would change the bits). */
    r.SetMix(1.f);
    float l1, r1, l2, r2;
    for(int i = 0; i < 600; i++)
    {
        const float in_l = .2f * sinf(i * .011f);
        const float in_r = .15f * cosf(i * .017f);
        wet_ref.Process(in_l, in_r, l1, r1);
        r.Process(in_l, in_r, l2, r2);
        if(i >= 600 - 64) /* settled tail of the ramp */
        {
            assert(l1 == l2);
            assert(r1 == r2);
        }
    }
    /* keep comparing: settled 100% stays on the wet-only reference */
    for(int i = 0; i < 4096; i++)
    {
        const float in_l = .2f * sinf(i * .011f);
        const float in_r = .15f * cosf(i * .017f);
        wet_ref.Process(in_l, in_r, l1, r1);
        r.Process(in_l, in_r, l2, r2);
        assert(l1 == l2);
        assert(r1 == r2);
    }

    /* mid-ramp retarget: halfway up, reverse to 0; it must land back on
     * bit-exact dry (exact endpoint, no residue). */
    PlateReverb s;
    s.Init(kSampleRate, buf_a);
    s.SetMix(1.f);
    run(s, .1f, -.1f, 240); /* half ramp */
    s.SetMix(0.f);
    for(int i = 0; i < 600; i++)
    {
        const float in_l = .25f * sinf(i * .019f);
        const float in_r = -.1f * cosf(i * .023f);
        s.Process(in_l, in_r, l1, r1);
        if(i >= 600 - 64)
        {
            assert(l1 == in_l);
            assert(r1 == in_r);
        }
    }

    /* 100% wet has NO dry passthrough: the first sample after an impulse
     * carries essentially none of the unit impulse (the dry spike would
     * be 1.0). */
    static float ir_l[8], ir_r[8];
    PlateReverb t;
    t.Init(kSampleRate, buf_a);
    impulse_response(t, 1.f, ir_l, ir_r, 8);
    assert(fabsf(ir_l[0]) < .05f);
    assert(fabsf(ir_r[0]) < .05f);
    printf("ok: exact endpoints and ramp\n");
}

static void test_long_silence_no_runaway()
{
    PlateReverb r;
    r.Init(kSampleRate, buf_a);
    r.SetMix(1.f);
    settle_mix(r);
    float l, rr;
    r.Process(1.f, 1.f, l, rr); /* impulse */

    /* 8 s of decay + silence; the tail must shrink toward the noise
     * floor and never blow up */
    float peak_tail = 0.f;
    for(int i = 0; i < 8 * 48000; i++)
    {
        r.Process(0.f, 0.f, l, rr);
        assert(isfinite(l));
        assert(isfinite(rr));
        if(i >= 7 * 48000) /* final second */
        {
            if(fabsf(l) > peak_tail)
                peak_tail = fabsf(l);
            if(fabsf(rr) > peak_tail)
                peak_tail = fabsf(rr);
        }
    }
    assert(peak_tail < 2e-3f);
    printf("ok: long silence, tail floor %.5f\n", (double)peak_tail);
}

static void test_decay_clamping_and_default()
{
    PlateReverb r;
    r.Init(kSampleRate, buf_a);
    /* boots at the minimum decay */
    assert(r.GetDecaySeconds() == PlateReverb::kMinDecaySeconds);
    r.SetDecaySeconds(0.01f);
    assert(r.GetDecaySeconds() == .2f);
    r.SetDecaySeconds(100.f);
    assert(r.GetDecaySeconds() == 20.f);
    r.SetDecaySeconds(3.2f);
    assert(fabsf(r.GetDecaySeconds() - 3.2f) < 1e-6f);
    printf("ok: decay clamping and default\n");
}

/** Peak |out| of the 100%-wet tail inside [t0, t1] seconds after a 1 s
 *  sustained stereo burst. A unit impulse is NOT a fair probe here:
 *  the input gain is normalized by (1 - feedback) for a constant
 *  SUSTAINED-tone wet level, so a long decay's impulse tail is tiny
 *  in comparison; organ tones are sustained. */
static float burst_tail_peak(float decay_s, float t0_s, float t1_s)
{
    PlateReverb r;
    r.Init(kSampleRate, buf_a);
    r.SetDecaySeconds(decay_s);
    r.SetMix(1.f);
    settle_mix(r);
    float l, rr;
    for(int i = 0; i < 48000; i++) /* 1 s sustained 220 Hz burst */
    {
        const float s = .4f * sinf(i * .0288f);
        r.Process(s, s, l, rr);
    }
    const int n0 = static_cast<int>(t0_s * kSampleRate);
    const int n1 = static_cast<int>(t1_s * kSampleRate);
    float     peak = 0.f;
    for(int i = 1; i < n1; i++)
    {
        r.Process(0.f, 0.f, l, rr);
        if(i >= n0)
        {
            const float m = fabsf(l) > fabsf(rr) ? fabsf(l) : fabsf(rr);
            if(m > peak)
                peak = m;
        }
    }
    return peak;
}

static void test_decay_time_ordering()
{
    /* 2-2.5 s window after the burst: a 0.5 s T60 tail is long gone,
     * a 3 s tail is faint, a 20 s tail is still present. Bounds are
     * loose on purpose - they separate the orders of magnitude. */
    const float short_peak = burst_tail_peak(.5f, 2.f, 2.5f);
    const float long_peak  = burst_tail_peak(20.f, 2.f, 2.5f);
    assert(short_peak < 2e-4f);
    assert(long_peak > 3e-3f);
    /* monotonic in between */
    const float mid_peak = burst_tail_peak(3.f, 2.f, 2.5f);
    assert(mid_peak > short_peak);
    assert(mid_peak < long_peak);
    printf("ok: decay ordering (0.5s tail %.6f, 3s %.5f, 20s %.5f)\n",
           (double)short_peak, (double)mid_peak, (double)long_peak);
}

static void test_stereo_separation_and_coupling()
{
    /* A left-only impulse must take a distinguishable path through the
     * tank: it excites the left wet output directly and reaches the
     * right only through the tank's cross-feedback. */
    const int n = 48000; /* 1 s */
    static float ir_l[48000], ir_r[48000];

    /* Boot decay (0.2 s): tiny feedback = strong separation. Until the
     * cross-feedback tap delay elapses (~93 ms) the right output is
     * EXACTLY silent (no path to it exists yet); afterwards it stays
     * at the noise floor (measured: peak_l ~0.023, peak_r ~3e-6). */
    PlateReverb r;
    r.Init(kSampleRate, buf_a);
    impulse_response_lr(r, 1.f, 1.f, 0.f, ir_l, ir_r, n);
    float peak_l = 0.f, peak_r = 0.f;
    for(int i = 0; i < n; i++)
    {
        assert(isfinite(ir_l[i]));
        assert(isfinite(ir_r[i]));
        if(i < 4000) /* ~83 ms: before any cross-feedback can arrive */
            assert(ir_r[i] == 0.f);
        if(fabsf(ir_l[i]) > peak_l)
            peak_l = fabsf(ir_l[i]);
        if(fabsf(ir_r[i]) > peak_r)
            peak_r = fabsf(ir_r[i]);
    }
    assert(peak_l > .01f);
    assert(peak_r < 1e-3f);          /* right stays at the noise floor */
    assert(peak_l > 50.f * peak_r); /* clearly separated */

    /* Longer decay (4 s): the cross-feedback now carries clearly
     * audible energy to the unexcited side (controlled coupling).
     * Measured in the first 200 ms: peak_l ~0.0072, peak_r ~0.0021. */
    PlateReverb s;
    s.Init(kSampleRate, buf_b);
    s.SetDecaySeconds(4.f);
    impulse_response_lr(s, 1.f, 1.f, 0.f, ir_l, ir_r, n);
    peak_l = peak_r = 0.f;
    for(int i = 0; i < n / 5; i++) /* first 200 ms */
    {
        if(fabsf(ir_l[i]) > peak_l)
            peak_l = fabsf(ir_l[i]);
        if(fabsf(ir_r[i]) > peak_r)
            peak_r = fabsf(ir_r[i]);
    }
    assert(peak_r > 1e-3f);  /* coupling is audible at this decay */
    assert(peak_l > peak_r); /* the excited side still dominates early */
    printf("ok: stereo separation and cross-coupling\n");
}

static void test_stereo_decorrelation()
{
    /* Identical L/R content must still produce meaningfully DIFFERENT
     * L/R wet tails: the diffuser chains use different lengths per
     * channel and the two tank sides/LFOs differ, so a centered signal
     * does not collapse to a mono-sounding wet field. */
    const int n = 48000;
    static float ir_l[48000], ir_r[48000];
    PlateReverb r;
    r.Init(kSampleRate, buf_a);
    r.SetDecaySeconds(4.f);
    impulse_response(r, 1.f, ir_l, ir_r, n);

    float max_diff = 0.f, diff_energy = 0.f, ref_energy = 0.f;
    for(int i = 0; i < n; i++)
    {
        const float d = ir_l[i] - ir_r[i];
        if(fabsf(d) > max_diff)
            max_diff = fabsf(d);
        diff_energy += d * d;
        ref_energy += ir_l[i] * ir_l[i];
    }
    assert(max_diff > 1e-3f);
    assert(diff_energy > .01f * ref_energy);
    printf("ok: stereo decorrelation (max diff %.4f, diff/ref energy "
           "%.3f)\n",
           (double)max_diff, (double)(diff_energy / ref_energy));
}

static void test_channel_symmetry()
{
    /* The two tank paths are voiced slightly differently (on purpose,
     * for decorrelation) but must be balanced: a left-only impulse's
     * left-tail energy and a right-only impulse's right-tail energy
     * agree within a wide tolerance. */
    const int n = 48000;
    static float ir_l[48000], ir_r[48000];

    PlateReverb r;
    r.Init(kSampleRate, buf_a);
    impulse_response_lr(r, 1.f, 1.f, 0.f, ir_l, ir_r, n);
    float e_l = 0.f;
    for(int i = 0; i < n; i++)
        e_l += ir_l[i] * ir_l[i];

    PlateReverb s;
    s.Init(kSampleRate, buf_b);
    impulse_response_lr(s, 1.f, 0.f, 1.f, ir_l, ir_r, n);
    float e_r = 0.f;
    for(int i = 0; i < n; i++)
        e_r += ir_r[i] * ir_r[i];

    const float ratio = e_l / e_r;
    assert(ratio > .25f);
    assert(ratio < 4.f);
    printf("ok: channel symmetry (L/R tail energy ratio %.3f)\n",
           (double)ratio);
}

static void test_antiphase_stereo_bounded()
{
    /* Strongly anti-correlated stereo input (opposite-phase sines) at
     * a long decay: everything stays finite, bounded and decaying -
     * no runaway feedback or non-finite samples. */
    PlateReverb r;
    r.Init(kSampleRate, buf_a);
    r.SetDecaySeconds(2.f);
    r.SetMix(1.f);
    settle_mix(r);

    float peak_first = 0.f, peak_last = 0.f;
    float l = 0.f, rr = 0.f;
    for(int i = 0; i < 2 * 48000; i++) /* 2 s of anti-phase input */
    {
        const float s = .4f * sinf(i * .03f);
        r.Process(s, -s, l, rr);
        assert(isfinite(l));
        assert(isfinite(rr));
        assert(fabsf(l) <= 1.5f);
        assert(fabsf(rr) <= 1.5f);
        if(i >= 2 * 48000 - 4800) /* last 100 ms of the driven part */
        {
            if(fabsf(l) > peak_first)
                peak_first = fabsf(l);
            if(fabsf(rr) > peak_first)
                peak_first = fabsf(rr);
        }
    }
    for(int i = 0; i < 3 * 48000; i++) /* 3 s of tail */
    {
        r.Process(0.f, 0.f, l, rr);
        assert(isfinite(l));
        assert(isfinite(rr));
        if(i >= 3 * 48000 - 24000) /* final 500 ms */
        {
            if(fabsf(l) > peak_last)
                peak_last = fabsf(l);
            if(fabsf(rr) > peak_last)
                peak_last = fabsf(rr);
        }
    }
    assert(peak_first > .01f);              /* it actually reverberated */
    assert(peak_last < .25f * peak_first);  /* and it decays */
    printf("ok: antiphase stereo bounded and decaying\n");
}

int main()
{
    test_silent_init_prefilled_buffer();
    test_repeated_init_determinism();
    test_impulse_bounded_and_decaying();
    test_mix_clamping();
    test_stereo_dry_preservation();
    test_exact_endpoints_and_ramp();
    test_long_silence_no_runaway();
    test_decay_clamping_and_default();
    test_decay_time_ordering();
    test_stereo_separation_and_coupling();
    test_stereo_decorrelation();
    test_channel_symmetry();
    test_antiphase_stereo_bounded();
    printf("ALL PLATE REVERB TESTS PASSED\n");
    return 0;
}

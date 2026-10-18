/** @file stereo_bbd_delay_test.cpp
 *  @brief Native tests for StereoBbdDelay.h - the ORGAN stereo
 *  BBD-inspired delay: buffer clearing/deterministic init, mix/time
 *  clamping and defaults, bit-exact dry bypass, wet-only behavior,
 *  mix-ramp and time-slew convergence/retargeting, echo arrival,
 *  repeat decay and progressive darkening, stereo (not ping-pong)
 *  paths, feedback defaults/clamping, sustained full feedback,
 *  feedback-ramp smoothness, boundedness, history preservation and
 *  in-place processing.
 *
 *  Build & run: make -C firmware/chompi-pipes/code/tests test
 */
#include "StereoBbdDelay.h"
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

using chompi::StereoBbdDelay;

static constexpr float kSr = 48000.f;

struct Rig
{
    StereoBbdDelay d;
    std::vector<float> mem_l;
    std::vector<float> mem_r;

    Rig()
        : mem_l(StereoBbdDelay::kBufferSize, 0.f),
          mem_r(StereoBbdDelay::kBufferSize, 0.f)
    {
        d.Init(kSr, mem_l.data(), mem_r.data());
    }

    void Prefill(float v) /* garbage-fill to prove Init clears */
    {
        for(int i = 0; i < StereoBbdDelay::kBufferSize; i++)
            mem_l[i] = mem_r[i] = v;
    }

    /** Run n samples of the given constant-ish input pair. */
    void Run(int n, float in_l, float in_r, float *peak_out = nullptr)
    {
        float peak = 0.f;
        for(int i = 0; i < n; i++)
        {
            float l, r;
            d.Process(in_l, in_r, l, r);
            const float a = fabsf(l) > fabsf(r) ? fabsf(l) : fabsf(r);
            if(a > peak)
                peak = a;
        }
        if(peak_out)
            *peak_out = peak;
    }
};

static void test_init_clears_and_silent()
{
    Rig r;
    r.Prefill(.777f); /* nonzero history must not survive Init */
    r.d.Init(kSr, r.mem_l.data(), r.mem_r.data());
    r.d.SetMix(1.f);
    /* settle the mix ramp */
    r.Run(600, 0.f, 0.f);
    float peak = 1.f;
    r.Run(4800, 0.f, 0.f, &peak);
    assert(peak == 0.f); /* silent init: no stale echoes */
}

static void test_deterministic_reinit()
{
    /* impulse response capture */
    Rig r1;
    r1.d.SetMix(1.f);
    r1.Run(600, 0.f, 0.f);
    std::vector<float> cap1;
    {
        float l, rr;
        r1.d.Process(1.f, .8f, l, rr);
        cap1.push_back(l);
        for(int i = 0; i < 20000; i++)
        {
            r1.d.Process(0.f, 0.f, l, rr);
            cap1.push_back(l);
        }
    }
    /* re-init the SAME rig: buffers dirty, must clear deterministically */
    r1.d.Init(kSr, r1.mem_l.data(), r1.mem_r.data());
    r1.d.SetMix(1.f);
    r1.Run(600, 0.f, 0.f);
    std::vector<float> cap2;
    {
        float l, rr;
        r1.d.Process(1.f, .8f, l, rr);
        cap2.push_back(l);
        for(int i = 0; i < 20000; i++)
        {
            r1.d.Process(0.f, 0.f, l, rr);
            cap2.push_back(l);
        }
    }
    assert(cap1.size() == cap2.size());
    for(size_t i = 0; i < cap1.size(); i++)
        assert(cap1[i] == cap2[i]); /* bit-identical reinitialization */
}

static void test_defaults_and_clamps()
{
    Rig r;
    assert(r.d.GetMix() == 0.f);
    assert(r.d.GetDelayMs() == 250.f);
    r.d.SetMix(-1.f);
    assert(r.d.GetMix() == 0.f);
    r.d.SetMix(2.f);
    assert(r.d.GetMix() == 1.f);
    r.d.SetDelayMs(1.f);
    assert(r.d.GetDelayMs() == 40.f);  /* exact low endpoint */
    r.d.SetDelayMs(9999.f);
    assert(r.d.GetDelayMs() == 500.f); /* exact high endpoint */
    r.d.SetDelayMs(250.f);
    assert(r.d.GetDelayMs() == 250.f);
}

static void test_bit_exact_dry_bypass()
{
    Rig r;
    /* settled mix 0: output IS the input, bit-exact, while the buffers
     * keep running (history preserved) */
    unsigned x = 12345u;
    for(int i = 0; i < 4800; i++)
    {
        x = x * 1664525u + 1013904223u;
        const float in_l = (x >> 8) * (1.f / 16777216.f) - .5f;
        x = x * 1664525u + 1013904223u;
        const float in_r = (x >> 8) * (1.f / 16777216.f) - .5f;
        float l, rr;
        r.d.Process(in_l, in_r, l, rr);
        assert(l == in_l && rr == in_r);
    }
}

static void test_wet_only_no_dry_feedthrough()
{
    Rig r;
    r.d.SetMix(1.f);
    r.Run(600, 0.f, 0.f); /* settle the ramp */
    r.d.SetDelayMs(100.f);
    r.Run(static_cast<int>(1.f * kSr), 0.f, 0.f); /* settle the slew */
    /* impulse: exactly nothing until just before the echo */
    float l, rr;
    r.d.Process(1.f, 1.f, l, rr);
    assert(l == 0.f && rr == 0.f);
    const int echo = static_cast<int>(.100f * kSr);
    for(int i = 1; i < echo - 480; i++) /* up to 10 ms before */
    {
        r.d.Process(0.f, 0.f, l, rr);
        assert(l == 0.f && rr == 0.f);
    }
}

/** Find the first sample index where |x| exceeds `thresh`. */
static int FirstPeak(const std::vector<float> &x, float thresh)
{
    for(size_t i = 0; i < x.size(); i++)
        if(fabsf(x[i]) > thresh)
            return static_cast<int>(i);
    return -1;
}

static void test_echo_arrival_and_stereo()
{
    Rig r;
    r.d.SetMix(1.f);
    r.Run(600, 0.f, 0.f);
    r.d.SetDelayMs(250.f);
    r.Run(static_cast<int>(1.2f * kSr), 0.f, 0.f); /* settle the slew */

    /* impulse both channels: echo arrives near 250 ms on each, with a
     * small L/R timing difference (stereo spread, not ping-pong) */
    std::vector<float> cl, cr;
    float l, rr;
    r.d.Process(1.f, 1.f, l, rr);
    cl.push_back(l);
    cr.push_back(rr);
    for(int i = 0; i < static_cast<int>(.5f * kSr); i++)
    {
        r.d.Process(0.f, 0.f, l, rr);
        cl.push_back(l);
        cr.push_back(rr);
    }
    const int pl = FirstPeak(cl, .05f);
    const int pr = FirstPeak(cr, .05f);
    const int expect = static_cast<int>(.250f * kSr);
    assert(pl > 0 && pr > 0);
    assert(abs(pl - expect) < static_cast<int>(.004f * kSr)); /* +-4 ms */
    assert(abs(pr - expect) < static_cast<int>(.004f * kSr));
    const int spread = abs(pr - pl);
    /* +/-0.5% spread => ~2.5 ms apart at 250 ms; allow LFO wobble */
    assert(spread > static_cast<int>(.0005f * kSr)
           && spread < static_cast<int>(.006f * kSr));

    /* left-only impulse: the echo stays left - no ping-pong
     * cross-feed */
    Rig r2;
    r2.d.SetMix(1.f);
    r2.Run(600, 0.f, 0.f);
    r2.d.SetDelayMs(100.f);
    r2.Run(static_cast<int>(1.f * kSr), 0.f, 0.f);
    float peak_l = 0.f, peak_r = 0.f;
    r2.d.Process(1.f, 0.f, l, rr);
    for(int i = 0; i < static_cast<int>(.4f * kSr); i++)
    {
        r2.d.Process(0.f, 0.f, l, rr);
        if(fabsf(l) > peak_l)
            peak_l = fabsf(l);
        if(fabsf(rr) > peak_r)
            peak_r = fabsf(rr);
    }
    assert(peak_l > .1f);
    assert(peak_r < .01f * peak_l);
}

static void test_repeat_decay_and_darkening()
{
    /* Probe with short tone bursts (5 ms) so the darkening low-pass
     * barely touches the probe level: echo-train peaks then track the
     * fixed ~35% loop feedback directly. */
    const int echo80 = static_cast<int>(.080f * kSr);

    auto burst_peaks = [&](float freq) {
        Rig r;
        r.d.SetMix(1.f);
        r.Run(600, 0.f, 0.f);
        r.d.SetDelayMs(80.f);
        r.Run(static_cast<int>(1.f * kSr), 0.f, 0.f); /* settle slew */
        /* 5 ms burst */
        const int blen = static_cast<int>(.005f * kSr);
        float l, rr;
        for(int i = 0; i < blen; i++)
        {
            const float in
                = .8f * sinf(2.f * 3.14159265358979f * freq * i / kSr);
            r.d.Process(in, in, l, rr);
        }
        std::vector<float> cap;
        for(int i = 0; i < static_cast<int>(.5f * kSr); i++)
        {
            r.d.Process(0.f, 0.f, l, rr);
            cap.push_back(l);
        }
        float peaks[4] = {0.f, 0.f, 0.f, 0.f};
        for(int n = 0; n < 4; n++)
        {
            const int lo = echo80 - blen + n * echo80;
            const int hi = echo80 + 2 * blen + n * echo80;
            for(int i = lo; i < hi && i < static_cast<int>(cap.size()); i++)
                if(fabsf(cap[i]) > peaks[n])
                    peaks[n] = fabsf(cap[i]);
        }
        return std::vector<float>(peaks, peaks + 4);
    };

    const std::vector<float> p400 = burst_peaks(400.f);
    /* measurable decay: each repeat clearly smaller, ratio ~0.35 */
    assert(p400[0] > .3f);
    assert(p400[1] < p400[0] && p400[2] < p400[1]
           && p400[3] < p400[2]);
    assert(p400[1] > .1f * p400[0]);
    const float ratio = p400[2] / p400[1];
    assert(ratio > .15f && ratio < .55f);

    /* progressive darkening: the same burst at 5 kHz decays much
     * faster per repeat (the loop low-pass applies once per repeat) */
    const std::vector<float> p5k = burst_peaks(5000.f);
    const float ratio_5k = p5k[1] / p5k[0];
    const float ratio_400 = p400[1] / p400[0];
    assert(ratio_5k < .7f * ratio_400);
}

static void test_mix_ramp_and_retarget()
{
    /* convergence + exact endpoints via the crossfade: with a silent
     * history and a constant input, out = in*(1-mix) until the first
     * echo arrives (40 ms min), so the ramp is directly observable */
    Rig r;
    r.d.SetDelayMs(40.f);
    r.d.SetMix(1.f);
    const int ramp = static_cast<int>(.010f * kSr);
    float l, rr;
    int zero_at = -1;
    for(int i = 0; i < ramp + 480; i++)
    {
        r.d.Process(.5f, .5f, l, rr);
        if(l == 0.f && zero_at < 0)
            zero_at = i;
    }
    /* exact wet-only (out = wet = 0 on silent history) within the 10 ms
     * ramp plus a small margin */
    assert(zero_at > 0 && zero_at <= ramp + 24);

    /* mid-ramp retarget: back to dry before the ramp finishes, settle,
     * then bit-exact dry */
    Rig r2;
    r2.d.SetMix(1.f);
    for(int i = 0; i < ramp / 2; i++)
        r2.d.Process(.5f, .5f, l, rr);
    r2.d.SetMix(0.f);
    for(int i = 0; i < ramp + 480; i++)
        r2.d.Process(.5f, .5f, l, rr);
    assert(l == .5f && rr == .5f); /* exact dry, mid-value proof */

    /* click-free: no per-sample jump during a retarget */
    Rig r3;
    r3.d.SetMix(1.f);
    float prev, max_step = 0.f;
    r3.d.Process(.5f, .5f, prev, rr); /* first sample: not a "jump" */
    for(int i = 0; i < ramp * 2; i++)
    {
        r3.d.Process(.5f, .5f, l, rr);
        const float step = fabsf(l - prev);
        if(step > max_step)
            max_step = step;
        prev = l;
    }
    assert(max_step < .002f); /* ~0.5/480 per sample */
}

static void test_time_slew_continuity_and_retarget()
{
    Rig r;
    r.d.SetMix(1.f);
    r.Run(600, 0.f, 0.f);
    r.d.SetDelayMs(250.f);
    r.Run(static_cast<int>(1.f * kSr), 0.f, 0.f);

    /* retarget 250 -> 500 ms under a sustained tone: the read position
     * slews (pitch glide) - output stays continuous, no clicks */
    r.d.SetDelayMs(500.f);
    float prev = 0.f, max_step = 0.f;
    int   phase = 0;
    for(int i = 0; i < static_cast<int>(.5f * kSr); i++)
    {
        const float in = sinf(2.f * 3.14159265358979f * 220.f * phase++
                              / kSr);
        float l, rr;
        r.d.Process(in * .4f, in * .4f, l, rr);
        const float step = fabsf(l - prev);
        if(step > max_step)
            max_step = step;
        prev = l;
    }
    assert(max_step < .05f); /* smooth glide, no sample steps */

    /* after settling, the echo arrives at the new time */
    r.Run(static_cast<int>(1.f * kSr), 0.f, 0.f);
    std::vector<float> cap;
    float l, rr;
    r.d.Process(1.f, 1.f, l, rr);
    cap.push_back(l);
    for(int i = 0; i < static_cast<int>(.7f * kSr); i++)
    {
        r.d.Process(0.f, 0.f, l, rr);
        cap.push_back(l);
    }
    const int p = FirstPeak(cap, .05f);
    const int expect = static_cast<int>(.500f * kSr);
    assert(p > 0 && abs(p - expect) < static_cast<int>(.005f * kSr));

    /* mid-slew retarget back to 40 ms converges to the exact low
     * endpoint behavior */
    r.d.SetDelayMs(200.f);
    r.Run(static_cast<int>(.2f * kSr), 0.f, 0.f); /* mid-glide */
    r.d.SetDelayMs(40.f);
    r.Run(static_cast<int>(1.5f * kSr), 0.f, 0.f);
    cap.clear();
    r.d.Process(1.f, 1.f, l, rr);
    cap.push_back(l);
    for(int i = 0; i < static_cast<int>(.2f * kSr); i++)
    {
        r.d.Process(0.f, 0.f, l, rr);
        cap.push_back(l);
    }
    const int p40 = FirstPeak(cap, .05f);
    const int e40 = static_cast<int>(.040f * kSr);
    assert(p40 > 0 && abs(p40 - e40) < static_cast<int>(.004f * kSr));

    /* endpoint-safe modulation: run the modulation extremes at both
     * time endpoints with sustained input - always finite/bounded */
    r.d.SetDelayMs(40.f);
    r.Run(static_cast<int>(2.f * kSr), .8f, -.8f, nullptr);
    float peak = 0.f;
    r.d.SetDelayMs(500.f);
    r.Run(static_cast<int>(2.f * kSr), .8f, -.8f, &peak);
    assert(std::isfinite(peak) && peak < 2.f);
}

static void test_history_survives_bypass_and_changes()
{
    Rig r;
    r.d.SetDelayMs(250.f);
    r.Run(static_cast<int>(1.f * kSr), 0.f, 0.f); /* settle, dry mix */

    /* impulse lands in the buffer while dry; re-open the mix before
     * the echo arrives: the history was never cleared */
    float l, rr;
    r.d.Process(1.f, 1.f, l, rr);
    assert(l == 1.f); /* dry: impulse passes straight through */
    r.Run(static_cast<int>(.100f * kSr), 0.f, 0.f);
    r.d.SetMix(1.f);
    r.Run(static_cast<int>(.050f * kSr), 0.f, 0.f);
    std::vector<float> cap;
    for(int i = 0; i < static_cast<int>(.200f * kSr); i++)
    {
        r.d.Process(0.f, 0.f, l, rr);
        cap.push_back(l);
    }
    assert(FirstPeak(cap, .05f) > 0); /* the echo survived the bypass */

    /* time changes never clear the buffers either: echoes keep
     * repeating (at the new spacing) after a retarget */
    Rig r2;
    r2.d.SetMix(1.f);
    r2.Run(600, 0.f, 0.f);
    r2.d.SetDelayMs(60.f);
    r2.Run(static_cast<int>(1.f * kSr), 0.f, 0.f);
    r2.d.Process(1.f, 1.f, l, rr);
    r2.Run(static_cast<int>(.070f * kSr), 0.f, 0.f); /* echo 1 just past */
    r2.d.SetDelayMs(150.f); /* mid-tail retarget */
    float peak = 0.f;
    r2.Run(static_cast<int>(.400f * kSr), 0.f, 0.f, &peak);
    assert(peak > .02f); /* tail still ringing at the new spacing */
}

static void test_feedback_defaults_and_clamps()
{
    Rig r;
    /* boots at the classic 35% voicing: bit-exact the old fixed
     * constant (all other tests run this default path) */
    assert(r.d.GetFeedback() == StereoBbdDelay::kDefaultFeedback);
    assert(r.d.GetFeedback() == .35f);
    r.d.SetFeedback(-1.f);
    assert(r.d.GetFeedback() == 0.f);  /* exact low endpoint */
    r.d.SetFeedback(2.f);
    assert(r.d.GetFeedback() == StereoBbdDelay::kMaxFeedback);
    assert(r.d.GetFeedback() == 1.2f); /* exact high endpoint */
    r.d.SetFeedback(.5f);
    assert(r.d.GetFeedback() == .5f);
}

static void test_full_feedback_sustains_bounded()
{
    /* feedback 1.0 vs the 35% default: the same impulse rings FAR
     * longer (measured 2026-10-18: ~0.005 at 4-5 s vs exact 0), and
     * the sustained tail stays bounded by the soft-clipper */
    auto tail_peak = [](float fb) {
        Rig r;
        r.d.SetMix(1.f);
        r.d.SetFeedback(fb);
        r.Run(600, 0.f, 0.f); /* settle both ramps */
        r.d.SetDelayMs(80.f);
        r.Run(static_cast<int>(1.f * kSr), 0.f, 0.f);
        float l, rr;
        r.d.Process(1.f, 1.f, l, rr);
        r.Run(static_cast<int>(4.f * kSr), 0.f, 0.f);
        float peak = 0.f;
        r.Run(static_cast<int>(1.f * kSr), 0.f, 0.f, &peak);
        return peak;
    };
    const float full = tail_peak(1.f);
    const float def  = tail_peak(.35f);
    assert(std::isfinite(full) && std::isfinite(def));
    assert(full < 1.f);        /* bounded by the soft-clipper */
    assert(full > .002f);      /* still ringing seconds later */
    assert(def < 1e-3f);       /* the 35% default has decayed away */
    assert(full > 20.f * def); /* dramatically longer tail */
}

static void test_self_oscillation()
{
    /* feedback 1.2: a seeded impulse does not decay - the tail grows
     * into a steady limit cycle bounded by the soft-clipper (measured
     * 2026-10-18: ~0.62 at 4-5 s, settling at ~0.66 = the |2/3|
     * clipper bound) */
    Rig r;
    r.d.SetMix(1.f);
    r.d.SetFeedback(StereoBbdDelay::kMaxFeedback);
    r.Run(600, 0.f, 0.f); /* settle both ramps */
    r.d.SetDelayMs(80.f);
    r.Run(static_cast<int>(1.f * kSr), 0.f, 0.f); /* settle the slew */
    float l, rr;
    r.d.Process(1.f, 1.f, l, rr); /* seed impulse */
    r.Run(static_cast<int>(4.f * kSr), 0.f, 0.f);
    float peak = 0.f;
    r.Run(static_cast<int>(1.f * kSr), 0.f, 0.f, &peak);
    assert(peak > .5f);  /* the drone is fully established */
    assert(peak < .7f);  /* bounded by the soft-clipper */

    /* loud playing on top of the drone stays finite and bounded */
    peak = 0.f;
    int phase = 0;
    for(int i = 0; i < static_cast<int>(2.f * kSr); i++)
    {
        const float in = phase++ % 240 < 120 ? .9f : -.9f;
        r.d.Process(in, -in, l, rr);
        assert(std::isfinite(l) && std::isfinite(rr));
        if(fabsf(l) > peak)
            peak = fabsf(l);
    }
    assert(peak < .7f);
}

static void test_feedback_ramp_click_free()
{
    /* with a steady DC tail ringing, retarget the feedback 0.35 -> 1.0:
     * the loop gain moves on the ~10 ms ramp, so the output grows
     * smoothly with no per-sample clicks */
    Rig r;
    r.d.SetMix(1.f);
    r.Run(600, 0.f, 0.f); /* settle the mix ramp */
    r.d.SetDelayMs(60.f);
    r.Run(static_cast<int>(1.f * kSr), 0.f, 0.f); /* settle the slew */
    float l, rr;
    for(int i = 0; i < static_cast<int>(.5f * kSr); i++)
        r.d.Process(.5f, .5f, l, rr); /* build the steady DC tail */
    r.d.SetFeedback(1.f); /* mid-tail retarget */
    float prev = l, max_step = 0.f;
    for(int i = 0; i < 2 * static_cast<int>(.010f * kSr); i++)
    {
        r.d.Process(.5f, .5f, l, rr);
        const float step = fabsf(l - prev);
        if(step > max_step)
            max_step = step;
        prev = l;
    }
    assert(max_step < .005f); /* smooth sweep, no zipper clicks */
    assert(r.d.GetFeedback() == 1.f);
}

static void test_in_place_and_bounded()
{
    /* in-place processing (outputs aliasing inputs) matches the
     * non-aliased render exactly */
    Rig a, b;
    a.d.SetMix(.7f);
    b.d.SetMix(.7f);
    for(int i = 0; i < 4800; i++)
    {
        const float in_l = sinf(i * .13f) * .5f;
        const float in_r = cosf(i * .11f) * .5f;
        float al, ar;
        a.d.Process(in_l, in_r, al, ar);
        float bl = in_l, br = in_r;
        b.d.Process(bl, br, bl, br); /* aliased */
        assert(al == bl && ar == br);
    }

    /* sustained full-scale anti-phase and long-silence behavior: finite,
     * bounded, no self-oscillation */
    Rig r;
    r.d.SetMix(1.f);
    r.Run(600, 0.f, 0.f);
    r.d.SetDelayMs(100.f);
    r.Run(static_cast<int>(1.f * kSr), 0.f, 0.f);
    float peak = 0.f;
    int  phase = 0;
    for(int i = 0; i < static_cast<int>(2.f * kSr); i++)
    {
        const float in = phase++ % 240 < 120 ? .9f : -.9f; /* 100 Hz sq */
        float l, rr;
        r.d.Process(in, -in, l, rr);
        assert(std::isfinite(l) && std::isfinite(rr));
        if(fabsf(l) > peak)
            peak = fabsf(l);
    }
    assert(peak < 2.f);
    /* silence decays away (fixed 35% feedback cannot self-oscillate) -
     * measured over the LAST 0.5 s of 5 s of silence, after the loud
     * content has repeated out */
    r.Run(static_cast<int>(4.5f * kSr), 0.f, 0.f);
    r.Run(static_cast<int>(.5f * kSr), 0.f, 0.f, &peak);
    assert(peak < 1e-3f);

    /* ring wrap: run the write pointer past the 32768-sample buffer
     * end (with silence, so no history lingers); the 500 ms echo must
     * still arrive correctly across the wrap */
    Rig w;
    w.d.SetMix(1.f);
    w.Run(600, 0.f, 0.f);
    w.d.SetDelayMs(500.f);
    w.Run(static_cast<int>(1.f * kSr), 0.f, 0.f);
    w.Run(StereoBbdDelay::kBufferSize + 1000, 0.f, 0.f); /* force wrap */
    std::vector<float> cap;
    float l, rr;
    w.d.Process(1.f, 1.f, l, rr);
    cap.push_back(l);
    for(int i = 0; i < static_cast<int>(.6f * kSr); i++)
    {
        w.d.Process(0.f, 0.f, l, rr);
        cap.push_back(l);
    }
    const int p = FirstPeak(cap, .05f);
    assert(p > 0);
    assert(abs(p - static_cast<int>(.5f * kSr))
           < static_cast<int>(.006f * kSr));
}

int main()
{
    test_init_clears_and_silent();
    test_deterministic_reinit();
    test_defaults_and_clamps();
    test_bit_exact_dry_bypass();
    test_wet_only_no_dry_feedthrough();
    test_echo_arrival_and_stereo();
    test_repeat_decay_and_darkening();
    test_mix_ramp_and_retarget();
    test_time_slew_continuity_and_retarget();
    test_history_survives_bypass_and_changes();
    test_feedback_defaults_and_clamps();
    test_full_feedback_sustains_bounded();
    test_self_oscillation();
    test_feedback_ramp_click_free();
    test_in_place_and_bounded();
    printf("stereo_bbd_delay_test: all tests passed\n");
    return 0;
}

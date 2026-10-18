/** @file reverb_noise_probe.cpp
 *  @brief TEMPORARY diagnostic: quantify the plate reverb's 16-bit tank
 *  quantization noise by running a FORMAT_16_BIT and a FORMAT_32_BIT
 *  tank on identical input and diffing the outputs.
 *
 *  Scenarios:
 *   1. steady 440 Hz sine, mix 0.5, short decay (0.2 s)
 *   2. steady 440 Hz sine, mix 0.5, long decay (4 s) - recirculating
 *      quantization error
 *   3. burst + silence at 4 s decay: tail granularity / limit-cycle
 *      check on the 16-bit tank (does the tail reach exact 0, and what
 *      does the diff look like deep in the tail?)
 *
 *  Not part of `make test`; build manually:
 *    g++ -std=c++14 -O2 -I../src \
 *        -I../../../chompi-wave/code/libs/DaisySP/Source \
 *        -o reverb_noise_probe reverb_noise_probe.cpp && ./reverb_noise_probe
 */
#include <cmath>
#include <cstdio>
#include <stdint.h>
#include "PlateReverb.h"

static const float kSampleRate = 48000.f;

typedef chompi::PlateReverbT<daisysp::FORMAT_16_BIT> R16;
typedef chompi::PlateReverbT<daisysp::FORMAT_32_BIT> R32;

static uint16_t buf16[R16::kDelayBufferSize];
static float    buf32[R32::kDelayBufferSize];

static float db(double x) { return 20.f * log10(x + 1e-30); }

/** Steady-sine diff at the given decay. Prints noise RMS and SNR. */
static void steady_sine(float decay)
{
    R16 r16;
    R32 r32;
    r16.Init(kSampleRate, buf16);
    r32.Init(kSampleRate, buf32);
    r16.SetDecaySeconds(decay);
    r32.SetDecaySeconds(decay);
    r16.SetMix(.5f);
    r32.SetMix(.5f);

    const int kSettle = 3 * 48000;
    const int kMeas   = 2 * 48000;
    double   noise_sq = 0., sig_sq = 0.;
    float    peak_diff = 0.f;
    for(int i = 0; i < kSettle + kMeas; i++)
    {
        const float in
            = .5f * sinf(2.f * 3.14159265f * 440.f * i / kSampleRate);
        float l16, r16o, l32, r32o;
        r16.Process(in, in, l16, r16o);
        r32.Process(in, in, l32, r32o);
        if(i >= kSettle)
        {
            const double d = l16 - l32;
            noise_sq += d * d;
            sig_sq += (double)l32 * l32;
            if(fabs(d) > peak_diff)
                peak_diff = fabs(d);
        }
    }
    const double noise_rms = sqrt(noise_sq / kMeas);
    const double sig_rms   = sqrt(sig_sq / kMeas);
    printf("sine 0.5pk, mix 0.5, decay %4.1f s:  noise RMS %.3e "
           "(%.1f dBFS, %.1f dB vs signal), peak diff %.3e\n",
           decay, noise_rms, db(noise_rms), db(noise_rms / sig_rms),
           peak_diff);
}

/** 1 s burst, then silence: watch the 16-bit tail's absolute level and
 *  the 16-vs-32 diff deep in the tail. `freeze_lfo` sets both LFO rates
 *  to 0 (static interpolated taps) to isolate time-variation pumping. */
static void tail_probe(float decay, bool freeze_lfo)
{
    R16 r16;
    R32 r32;
    r16.Init(kSampleRate, buf16);
    r32.Init(kSampleRate, buf32);
    if(freeze_lfo)
    {
        r16.SetLfoRatesForTesting(0.f, 0.f);
        r32.SetLfoRatesForTesting(0.f, 0.f);
    }
    r16.SetDecaySeconds(decay);
    r32.SetDecaySeconds(decay);
    r16.SetMix(1.f); /* wet only, so the tail is measured directly */
    r32.SetMix(1.f);

    float l, r;
    for(int i = 0; i < 48000; i++) /* 1 s burst */
    {
        const float in
            = .5f * sinf(2.f * 3.14159265f * 440.f * i / kSampleRate);
        r16.Process(in, in, l, r);
        r32.Process(in, in, l, r);
    }

    printf("tail after 1 s burst, decay %.1f s (wet-only out)%s "
           "[windowed: last 1 s]:\n",
           decay, freeze_lfo ? ", LFO frozen" : "");
    const int kCheckpoints[] = {48000, 96000, 192000, 480000};
    int       next_cp        = 0;
    double    acc16 = 0., acc32 = 0., accdiff = 0.;
    for(int i = 0; i < 480000; i++) /* 10 s of silence */
    {
        float l16, r16o, l32, r32o;
        r16.Process(0.f, 0.f, l16, r16o);
        r32.Process(0.f, 0.f, l32, r32o);
        acc16 += (double)l16 * l16;
        acc32 += (double)l32 * l32;
        const double d = l16 - l32;
        accdiff += d * d;
        if(next_cp < 4 && i + 1 == kCheckpoints[next_cp])
        {
            /* RMS over the LAST 1 s window only */
            const double w  = 48000.;
            const double w16 = sqrt(acc16 / w), w32 = sqrt(acc32 / w),
                         wd = sqrt(accdiff / w);
            printf("  t=%4.1f s: |wet16| %.3e (%5.1f dBFS), |wet32| %.3e "
                   "(%5.1f dBFS), diff %.3e (%5.1f dBFS)\n",
                   kCheckpoints[next_cp] / 48000., w16, db(w16), w32,
                   db(w32), wd, db(wd));
            acc16 = acc32 = accdiff = 0.;
            next_cp++;
        }
    }
}

/** After a burst + 10 s of silence, characterize the remaining floor:
 *  level, DC content, zero-crossing rate (tone vs noise), dominant
 *  periodicity via a small autocorrelation scan. */
static void tail_analysis(float decay, bool freeze_lfo)
{
    R32 r;
    r.Init(kSampleRate, buf32);
    if(freeze_lfo)
        r.SetLfoRatesForTesting(0.f, 0.f);
    r.SetDecaySeconds(decay);
    r.SetMix(1.f);

    float l, rr;
    for(int i = 0; i < 48000; i++)
    {
        const float in
            = .5f * sinf(2.f * 3.14159265f * 440.f * i / kSampleRate);
        r.Process(in, in, l, rr);
    }
    for(int i = 0; i < 10 * 48000; i++)
        r.Process(0.f, 0.f, l, rr);

    static float x[4800];
    double mean = 0., sq = 0.;
    float  peak = 0.f;
    for(int i = 0; i < 4800; i++)
    {
        r.Process(0.f, 0.f, l, rr);
        x[i] = l;
        mean += l;
        sq += (double)l * l;
        if(fabsf(l) > peak)
            peak = fabsf(l);
    }
    mean /= 4800;

    int zc = 0;
    for(int i = 1; i < 4800; i++)
        if((x[i - 1] < 0.f) != (x[i] < 0.f))
            zc++;

    /* autocorrelation peak over lags 4..2400 (20 Hz..6 kHz) */
    double best = 0.;
    int    best_lag = 0;
    for(int lag = 4; lag < 2400; lag++)
    {
        double a = 0.;
        for(int i = 0; i + lag < 4800; i++)
            a += (double)x[i] * x[i + lag];
        if(a > best)
        {
            best     = a;
            best_lag = lag;
        }
    }
    const double e0 = sq; /* energy at lag 0 */
    printf("floor, decay %4.1f s%s: RMS %.3e (%.1f dBFS), peak %.3e, "
           "DC %.2e, zc/s %.0f (~%.0f Hz), best acorr lag %d "
           "(%.1f Hz, norm %.2f)\n",
           decay, freeze_lfo ? " LFO-frozen" : "", sqrt(sq / 4800),
           db(sqrt(sq / 4800)), peak, mean, zc / 0.1 / 2., zc / 0.1 / 2.,
           best_lag, kSampleRate / best_lag, e0 > 0. ? best / e0 : 0.);

    printf("  first 16 floor samples:");
    for(int i = 0; i < 16; i++)
        printf(" %+.4f", x[i]);
    printf("\n");
}

int main()
{
    steady_sine(0.2f);
    steady_sine(2.f);
    steady_sine(4.f);
    steady_sine(20.f);
    tail_probe(4.f, false);
    tail_probe(4.f, true);
    tail_probe(20.f, false);
    tail_probe(20.f, true);
    tail_analysis(4.f, false);
    tail_analysis(4.f, true);
    tail_analysis(20.f, false);
    tail_analysis(0.2f, false); /* sanity: short decay must die */
    return 0;
}

/** @file EnvelopeMap.h
 *  @brief Shared step->seconds mapping for the per-pipe attack/release
 *  envelopes: 32 log-spaced steps, 5 ms to 10 s.
 *
 *  Pure math with no Daisy dependencies so the native tests can include
 *  it directly (see code/tests/envelope_map_test.cpp).
 */
#pragma once
#include <math.h>

namespace chompi
{

struct EnvelopeMap
{
    static constexpr int   kMaxSteps   = 32;    /* 0..32 encoder clicks */
    static constexpr float kMinSeconds = .005f; /* step 0: the original
                                                 * fixed 5 ms gate */
    static constexpr float kMaxSeconds = 10.f;  /* step 32 */

    /** Log-spaced envelope time: 5 ms (step 0) to 10 s (step 32).
     *  Log spacing keeps the short end usable - a linear
     *  seconds-per-click mapping would make the first clicks huge
     *  relative jumps (same rationale as the plate-reverb decay). */
    static float StepToSeconds(int step)
    {
        step = step < 0 ? 0 : (step > kMaxSteps ? kMaxSteps : step);
        return kMinSeconds
               * powf(kMaxSeconds / kMinSeconds,
                      static_cast<float>(step) * (1.f / kMaxSteps));
    }

    /** Per-sample linear ramp increment for a 0->1 (or 1->0) envelope. */
    static float StepToInc(int step, float sample_rate)
    {
        return 1.f / (StepToSeconds(step) * sample_rate);
    }
};

} // namespace chompi

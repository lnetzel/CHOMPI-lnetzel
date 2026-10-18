/** @file envelope_map_test.cpp
 *  @brief Native tests for EnvelopeMap.h - the step->seconds mapping
 *  behind the per-pipe attack/release pages (pg.3/pg.4): 32 log-spaced
 *  steps from 5 ms (step 0, the original fixed gate timing) to 10 s
 *  (step 32).
 *
 *  Build & run: make -C firmware/chompi-pipes/code/tests test
 */
#include "EnvelopeMap.h"
#include <cassert>
#include <cmath>
#include <cstdio>

using chompi::EnvelopeMap;

static void test_endpoints()
{
    /* step 0 = exactly the original 5 ms fixed gate (no boot regression) */
    assert(std::fabs(EnvelopeMap::StepToSeconds(0) - .005f) < 1e-7f);
    /* step 32 = 10 s */
    assert(std::fabs(EnvelopeMap::StepToSeconds(EnvelopeMap::kMaxSteps)
                   - 10.f)
           < 1e-4f);
}

static void test_monotonic()
{
    float prev = EnvelopeMap::StepToSeconds(0);
    for(int s = 1; s <= EnvelopeMap::kMaxSteps; s++)
    {
        const float t = EnvelopeMap::StepToSeconds(s);
        assert(t > prev);
        prev = t;
    }
}

static void test_log_midpoint()
{
    /* step 16 sits at the geometric midpoint: 5 ms * sqrt(2000) ~ 224 ms */
    const float expect = .005f * std::sqrt(2000.f);
    assert(std::fabs(EnvelopeMap::StepToSeconds(16) - expect)
           < 1e-3f * expect);
}

static void test_clamping()
{
    /* out-of-range steps clamp to the endpoints */
    assert(EnvelopeMap::StepToSeconds(-1) == EnvelopeMap::StepToSeconds(0));
    assert(EnvelopeMap::StepToSeconds(33)
           == EnvelopeMap::StepToSeconds(EnvelopeMap::kMaxSteps));
}

static void test_increment()
{
    /* inc * samples-per-envelope = full 0..1 swing */
    const float srate = 48000.f;
    const float inc0  = EnvelopeMap::StepToInc(0, srate);
    assert(std::fabs(inc0 - 1.f / (.005f * srate)) < 1e-9f);
    const float inc32 = EnvelopeMap::StepToInc(EnvelopeMap::kMaxSteps, srate);
    assert(std::fabs(inc32 * 10.f * srate - 1.f) < 1e-5f);
    /* longer time = smaller increment */
    assert(inc32 < inc0);
}

int main()
{
    test_endpoints();
    test_monotonic();
    test_log_midpoint();
    test_clamping();
    test_increment();
    std::puts("envelope_map_test: all assertions passed");
    return 0;
}

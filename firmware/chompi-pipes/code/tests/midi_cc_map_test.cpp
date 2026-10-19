/** @file midi_cc_map_test.cpp
 *  @brief Native tests for MidiCcMap.h - the CC number map and the
 *  0-127 -> engine-value conversions behind the PIPES MIDI
 *  implementation (firmware/chompi-pipes/midi.md).
 *
 *  Build & run: make -C firmware/chompi-pipes/code/tests test
 */
#include "MidiCcMap.h"
#include <cassert>
#include <cstdio>

using namespace chompi::MidiCcMap;

static void test_cc_map_layout()
{
    /* the original blocks are untouched */
    assert(kVibratoCc == 1);
    assert(kLevelCcBase == 70);
    assert(kCharacterCcBase == 74);

    /* the per-pipe blocks are 4 consecutive CCs each, contiguous
     * 78..105 */
    const uint8_t bases[] = {kAttackCcBase, kReleaseCcBase,  kMuteCcBase,
                             kLengthCcBase, kVolModCcBase,   kCharModCcBase,
                             kLengthModCcBase};
    assert(kAttackCcBase == 78);
    for(size_t i = 1; i < sizeof(bases); i++)
        assert(bases[i] == bases[i - 1] + 4);

    /* the globals are contiguous 106..116 and stay clear of the
     * reserved channel-mode CCs 120-127 */
    const uint8_t globals[] = {kVibratoDelayCc, kRotaryCc,     kDriveCc,
                               kDriveToneCc,    kReverbMixCc,  kReverbDecayCc,
                               kPercussionCc,   kDelayMixCc,   kDelayTimeCc,
                               kDriveModCc,     kDelayModCc};
    assert(kVibratoDelayCc == 106);
    for(size_t i = 1; i < sizeof(globals); i++)
        assert(globals[i] == globals[i - 1] + 1);
    assert(kDriveModCc == 115);
    assert(kDelayModCc == 116);
}

static void test_steps_endpoints()
{
    /* 0 and 127 hit the exact range ends for every step range used */
    const int maxes[] = {32, 50, 64, 100};
    for(size_t m = 0; m < sizeof(maxes) / sizeof(maxes[0]); m++)
    {
        assert(CcToSteps(0, maxes[m]) == 0);
        assert(CcToSteps(127, maxes[m]) == maxes[m]);
    }
}

static void test_steps_rounding()
{
    /* CC 64 lands on the half step for the even ranges (CC 64 = the
     * boot defaults: level step 16, Character step 32) */
    assert(CcToSteps(64, 32) == 16);
    assert(CcToSteps(64, 64) == 32);
    assert(CcToSteps(64, 50) == 25);
    /* no value ever leaves the range */
    for(int v = 0; v < 128; v++)
    {
        assert(CcToSteps(v, 32) >= 0 && CcToSteps(v, 32) <= 32);
        assert(CcToSteps(v, 100) >= 0 && CcToSteps(v, 100) <= 100);
    }
}

static void test_toggle()
{
    assert(!CcToToggle(0));
    assert(!CcToToggle(63));
    assert(CcToToggle(64));
    assert(CcToToggle(127));
}

static void test_length_anchors()
{
    /* the five anchors are exact */
    assert(CcToLengthOctave(0) == -2);
    assert(CcToLengthOctave(32) == -1);
    assert(CcToLengthOctave(64) == 0);
    assert(CcToLengthOctave(96) == 1);
    assert(CcToLengthOctave(127) == 2);
    /* in-between values snap to the nearest anchor (ties snap up) */
    assert(CcToLengthOctave(33) == -1);
    assert(CcToLengthOctave(95) == 1);
    assert(CcToLengthOctave(79) == 0);
    assert(CcToLengthOctave(80) == 1);
    /* every value lands inside -2..+2 */
    for(int v = 0; v < 128; v++)
    {
        const int oct = CcToLengthOctave(v);
        assert(oct >= -2 && oct <= 2);
    }
}

static void test_delay_time()
{
    assert(CcToDelayTimeMs(0) == 40);
    assert(CcToDelayTimeMs(127) == 500);
    /* monotonic and in range over the whole span */
    int prev = CcToDelayTimeMs(0);
    for(int v = 1; v < 128; v++)
    {
        const int ms = CcToDelayTimeMs(v);
        assert(ms >= prev);
        assert(ms >= 40 && ms <= 500);
        prev = ms;
    }
}

static void test_program_slots()
{
    /* PC numbers 0-14 (documented as slots 1-15) map to slots 0-14 */
    assert(ProgramToPresetSlot(0) == 0);
    assert(ProgramToPresetSlot(14) == 14);
    /* everything else is not a slot */
    assert(ProgramToPresetSlot(15) == -1);
    assert(ProgramToPresetSlot(127) == -1);
}

int main()
{
    test_cc_map_layout();
    test_steps_endpoints();
    test_steps_rounding();
    test_toggle();
    test_length_anchors();
    test_delay_time();
    test_program_slots();
    std::puts("midi_cc_map_test: all assertions passed");
    return 0;
}

/** @file MidiCcMap.h
 *  @brief CC number map and 0-127 -> engine-value conversions for the
 *  CHOMPI-PIPES MIDI implementation (see firmware/chompi-pipes/midi.md).
 *  Header-only with no Daisy dependencies, so the host tests can include
 *  it (same convention as OrganPreset.h).
 *
 *  Every parameter that can be saved in an SD preset has a CC. The map
 *  continues the original blocks (CC 1 vibrato depth, CC 70-73 pipe
 *  levels, CC 74-77 Character) upward and ends at CC 116, staying clear
 *  of the reserved channel-mode CCs 120-127. All incoming CCs set their
 *  values silently (no white-key LED value bars) and nothing is sent
 *  back out for the new CCs (receive-only).
 */
#pragma once
#include <cstdint>

namespace chompi
{
namespace MidiCcMap
{
    /* --- original blocks --- */
    constexpr uint8_t kVibratoCc       = 1;  /* modulation wheel: 0..50 cents */
    constexpr uint8_t kLevelCcBase     = 70; /* 70-73:  0..32 steps */
    constexpr uint8_t kCharacterCcBase = 74; /* 74-77:  0..64 steps */

    /* --- per-pipe blocks, 4 CCs each (pipe = cc - base, 0..3) --- */
    constexpr uint8_t kAttackCcBase    = 78;  /* 78-81:   0..32 steps */
    constexpr uint8_t kReleaseCcBase   = 82;  /* 82-85:   0..32 steps */
    constexpr uint8_t kMuteCcBase      = 86;  /* 86-89:   0-63 off, 64-127 on */
    constexpr uint8_t kLengthCcBase    = 90;  /* 90-93:   -2..+2 octaves, snapped */
    constexpr uint8_t kVolModCcBase    = 94;  /* 94-97:   0..32 steps */
    constexpr uint8_t kCharModCcBase   = 98;  /* 98-101:  0..32 steps */
    constexpr uint8_t kLengthModCcBase = 102; /* 102-105: 0..32 steps */

    /* --- globals --- */
    constexpr uint8_t kVibratoDelayCc = 106; /* 0..50 steps of 100 ms */
    constexpr uint8_t kRotaryCc       = 107; /* 0..32 steps */
    constexpr uint8_t kDriveCc        = 108; /* 0..32 steps */
    constexpr uint8_t kDriveToneCc    = 109; /* 0..100 % */
    constexpr uint8_t kReverbMixCc    = 110; /* 0..32 steps */
    constexpr uint8_t kReverbDecayCc  = 111; /* 0..100 steps */
    constexpr uint8_t kPercussionCc   = 112; /* 0-63 off, 64-127 on */
    constexpr uint8_t kDelayMixCc     = 113; /* 0..100 % */
    constexpr uint8_t kDelayTimeCc    = 114; /* 40..500 ms */
    constexpr uint8_t kDriveModCc     = 115; /* 0..32 steps (global) */
    constexpr uint8_t kDelayModCc     = 116; /* 0..32 steps (global) */

    /** 0-127 -> 0..max, rounded (the formula the original pipe CCs
     *  used: CC 64 lands on the half step for the even ranges). */
    inline int CcToSteps(uint8_t v, int max)
    {
        return (static_cast<int>(v) * max + 63) / 127;
    }

    /** 0-63 = off, 64-127 = on (standard switch convention). */
    inline bool CcToToggle(uint8_t v) { return v >= 64; }

    /** 0-127 -> -2..+2 octaves with snapped anchors: 0/32/64/96/127 hit
     *  -2/-1/0/+1/+2 exactly, every other value snaps to the nearest
     *  anchor (ties snap upward). */
    inline int CcToLengthOctave(uint8_t v)
    {
        return (static_cast<int>(v) * 4 + 63) / 127 - 2;
    }

    /** 0-127 -> 40..500 ms, rounded (the stereo delay's time range). */
    inline int CcToDelayTimeMs(uint8_t v)
    {
        return 40 + (static_cast<int>(v) * 460 + 63) / 127;
    }

    /** Program Change number -> preset slot index 0..14 (documented as
     *  slots 1-15), or -1 when the program number is not a slot. */
    inline int ProgramToPresetSlot(uint8_t program)
    {
        return program < 15 ? static_cast<int>(program) : -1;
    }

} /* namespace MidiCcMap */
} /* namespace chompi */

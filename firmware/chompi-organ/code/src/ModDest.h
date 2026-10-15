/** @file ModDest.h
 *  @brief CHOMPI Organ Modulation-mode destination selection and the
 *  pipe-encoder UI rules shared by both modes.
 *
 *  Modulation Mode has ONE shared destination for all four pipe
 *  encoders, selected with SHIFT + a physical black key (the switch is
 *  UP). Black keys are numbered physically from left to right: key 1
 *  (C#3) selects the actual pipe attack time, key 2 (D#3) the actual
 *  pipe release time, key 3 (F#3) the volume-modulation amount and key
 *  4 (G#3) the Character-modulation amount; the fifth and later black
 *  keys select nothing. The selection boots on volume modulation, is
 *  remembered across mode changes until reboot and is never stored in
 *  the SD-card presets.
 *
 *  Hardware-independent (no Daisy includes) so the native tests exercise
 *  the exact production logic; chompi_main.cpp static_asserts the key
 *  table against Hardware::SwId.
 */
#pragma once

namespace chompi
{

/** The four shared Modulation-mode destinations. */
enum ModDest
{
    kModDestAttack = 0, /**< actual pipe attack time (purple) */
    kModDestRelease,    /**< actual pipe release time (yellow) */
    kModDestVolMod,     /**< volume-modulation amount (white) */
    kModDestCharMod,    /**< Character-modulation amount (blue) */
    kNumModDests
};

/** Boot destination: volume modulation. */
constexpr int kModDestDefault = kModDestVolMod;

/** Destination-key LED intensities: the selected key shows 100% of the
 *  destination color, the other three exactly 75%. */
constexpr float kModDestSelected   = 1.f;
constexpr float kModDestUnselected = .75f;

/** SwId values of the four destination keys, in destination order:
 *  KEY_16 (C#3), KEY_17 (D#3), KEY_18 (F#3), KEY_19 (G#3) - the first
 *  four physical black keys from the left. */
constexpr int kModDestKeySw[kNumModDests] = {7, 12, 13, 14};

/** SHIFT + key destination selection: returns the pressed key's
 *  destination, or `cur` unchanged when the key selects nothing (white
 *  keys, the fifth and later black keys). Nothing else writes the
 *  destination, so the selection persists across mode changes until
 *  reboot. */
constexpr int ModDestAfterKey(int cur, int swid)
{
    return swid == kModDestKeySw[0]   ? kModDestAttack
           : swid == kModDestKeySw[1] ? kModDestRelease
           : swid == kModDestKeySw[2] ? kModDestVolMod
           : swid == kModDestKeySw[3] ? kModDestCharMod
                                      : cur;
}

/** Full-intensity destination color, matching the white-key value bars:
 *  attack purple, release yellow, volume modulation white, Character
 *  modulation blue. */
inline void ModDestColor(int dest, float &r, float &g, float &b)
{
    switch(dest)
    {
        case kModDestAttack:
            r = .55f;
            g = 0.f;
            b = 1.f; /* purple */
            break;
        case kModDestRelease:
            r = 1.f;
            g = 1.f;
            b = 0.f; /* yellow */
            break;
        case kModDestVolMod:
            r = 1.f;
            g = 1.f;
            b = 1.f; /* white */
            break;
        default: /* kModDestCharMod */
            r = 0.f;
            g = 0.f;
            b = 1.f; /* blue */
            break;
    }
}

/** What a pipe-encoder press does. Modulation Mode: unshifted presses
 *  do nothing (the destination is key-selected); Normal Mode: unshifted
 *  presses cycle the pipe's page. SHIFT + press toggles the pipe's mute
 *  in both modes. */
enum PipePressAction
{
    kPipePressNothing = 0,
    kPipePressMute,
    kPipePressCyclePage
};

constexpr PipePressAction PipeEncoderPress(bool mod_mode, bool shift)
{
    return shift ? kPipePressMute
                 : (mod_mode ? kPipePressNothing : kPipePressCyclePage);
}

/** Normal Mode keeps two independent pages per pipe: page 0 (Volume)
 *  <-> page 1 (Character). Attack and release are edited in Modulation
 *  Mode. The page is preserved across mode changes. */
constexpr int NextNormalPipePage(int page) { return (page + 1) % 2; }

/** One detent step clamped to [0, max]: the pipe setters clamp too, but
 *  the SHIFT "absolute broadcast" needs the clamped result BEFORE
 *  writing it to all four pipes. */
constexpr int ClampStep(int value, int max)
{
    return value < 0 ? 0 : (value > max ? max : value);
}

/** SHIFT + turn broadcast value: the turned pipe's current value plus
 *  the delta, clamped; the result is written to the turned pipe and
 *  broadcast to the other three. */
constexpr int BroadcastStep(int cur, int delta, int max)
{
    return ClampStep(cur + delta, max);
}

} // namespace chompi

/** @file ModDest.h
 *  @brief CHOMPI-PIPES Modulation-mode destination selection and the
 *  pipe-encoder UI rules shared by both modes.
 *
 *  Modulation Mode has ONE shared destination for the pipe encoders,
 *  selected with SHIFT + a physical black key (the switch is UP). Black
 *  keys are numbered physically from left to right: key 1 (C#3) selects
 *  the actual pipe attack time, key 2 (D#3) the actual pipe release
 *  time, key 3 (F#3) the volume-modulation amount, key 4 (G#3) the
 *  Character-modulation amount and key 5 (A#3) the pipe-length
 *  modulation amount; key 8 (F#4) selects the distortion modulation
 *  amount - ONE global value (the drive is shared by all four pipes),
 *  adjusted by the leftmost pipe encoder only; key 9 (G#4) selects the
 *  delay modulation amount - likewise ONE global value (the stereo
 *  delay's combined mix/feedback control is shared), adjusted by the
 *  leftmost pipe encoder only. Keys 6, 7 and 10 select nothing. The
 *  selection boots on volume modulation, is remembered across mode
 *  changes until reboot and is never stored in the SD-card presets
 *  (the distortion- and delay-modulation amounts themselves ARE, as
 *  the global "drive_mod" and "delay_mod" fields).
 *
 *  Hardware-independent (no Daisy includes) so the native tests exercise
 *  the exact production logic; chompi_main.cpp static_asserts the key
 *  table against Hardware::SwId.
 */
#pragma once

namespace chompi
{

/** The seven shared Modulation-mode destinations. */
enum ModDest
{
    kModDestAttack = 0, /**< actual pipe attack time (purple) */
    kModDestRelease,    /**< actual pipe release time (yellow) */
    kModDestVolMod,     /**< volume-modulation amount (white) */
    kModDestCharMod,    /**< Character-modulation amount (blue) */
    kModDestLengthMod,  /**< pipe-length modulation amount (yellow) */
    kModDestDriveMod,   /**< distortion modulation, global (red) */
    kModDestDelayMod,   /**< delay modulation, global (yellow) */
    kNumModDests
};

/** Boot destination: volume modulation. */
constexpr int kModDestDefault = kModDestVolMod;

/** Destination-key LED intensities: the selected key shows 100% of the
 *  destination color, the other four exactly 75%. */
constexpr float kModDestSelected   = 1.f;
constexpr float kModDestUnselected = .75f;

/** SwId values of the seven destination keys, in destination order:
 *  KEY_16 (C#3), KEY_17 (D#3), KEY_18 (F#3), KEY_19 (G#3), KEY_20 (A#3),
 *  KEY_23 (F#4), KEY_24 (G#4) - the first five physical black keys from
 *  the left, then the eighth and ninth (keys 6 and 7 are skipped, 10
 *  stays inert). */
constexpr int kModDestKeySw[kNumModDests] = {7, 12, 13, 14, 21, 29, 30};

/** SHIFT + key destination selection: returns the pressed key's
 *  destination, or `cur` unchanged when the key selects nothing (white
 *  keys, black keys 6, 7 and 10). Nothing else writes the
 *  destination, so the selection persists across mode changes until
 *  reboot. */
constexpr int ModDestAfterKey(int cur, int swid)
{
    return swid == kModDestKeySw[0]   ? kModDestAttack
           : swid == kModDestKeySw[1] ? kModDestRelease
           : swid == kModDestKeySw[2] ? kModDestVolMod
           : swid == kModDestKeySw[3] ? kModDestCharMod
           : swid == kModDestKeySw[4] ? kModDestLengthMod
           : swid == kModDestKeySw[5] ? kModDestDriveMod
           : swid == kModDestKeySw[6] ? kModDestDelayMod
                                      : cur;
}

/** Full-intensity destination color, matching the white-key value bars:
 *  attack purple, release yellow, volume modulation white, Character
 *  modulation blue, pipe-length modulation yellow (the pipe-length
 *  color), distortion modulation red (the overdrive color), delay
 *  modulation yellow (the stereo-delay color). */
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
        case kModDestCharMod:
            r = 0.f;
            g = 0.f;
            b = 1.f; /* blue */
            break;
        case kModDestLengthMod:
            r = 1.f;
            g = 1.f;
            b = 0.f; /* yellow: the pipe-length color */
            break;
        case kModDestDelayMod:
            r = 1.f;
            g = 1.f;
            b = 0.f; /* yellow: the stereo-delay color */
            break;
        default: /* kModDestDriveMod */
            r = 1.f;
            g = 0.f;
            b = 0.f; /* red: the overdrive color */
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

/** Normal Mode keeps three independent pages per pipe: page 0 (Volume)
 *  -> page 1 (Character) -> page 2 (pipe length, the +/-2-octave
 *  offset). Attack and release are edited in Modulation Mode. The page
 *  is preserved across mode changes. */
constexpr int NextNormalPipePage(int page) { return (page + 1) % 3; }

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

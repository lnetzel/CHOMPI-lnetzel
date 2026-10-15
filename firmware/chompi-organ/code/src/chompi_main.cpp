/** @file chompi_main.cpp
 *  @brief CHOMPI Organ Emulator firmware entry point.
 *
 *  Runs the spec in firmware/chompi-organ/chompi-organ- specification.md.
 *  Three execution contexts, like the other CHOMPI firmwares:
 *   1. AudioCallback() - audio ISR: MIDI in, control polling, audio render.
 *   2. MainLoop()      - MIDI out, LEDs, battery checks.
 *   3. Boot            - control-surface settle before audio starts.
 */
#include "hardware.h"
#include "temp_led_stuff.h"
#include "daisysp.h"
#include "OrganEngine.h"
#include "MidiHandler.h"
#include "OrganPreset.h"
#include "PresetStorage.h"
#ifdef ORGAN_CPU_METER
#include "util/CpuLoadMeter.h"
#endif

using namespace daisy;
using namespace chompi;

Hardware    hw;
OrganEngine engine;
OrganMidi   midi;

/* Plate-reverb delay memory: 64 KiB in external SDRAM, kept out of the
 * tight SRAM regions. Never written from a global constructor: it is
 * cleared by PlateReverb::Init via engine.Init(), after hw.Init() has
 * configured the SDRAM controller. */
static uint16_t DSY_SDRAM_BSS plate_delay_mem[PlateReverb::kDelayBufferSize];

/* B4: debug-build CPU meter (build with ORGAN_CPU_METER=1); peak load
 * shows on PTH LED 0 (green < 70%, amber 70-85%, red > 85%). */
#ifdef ORGAN_CPU_METER
static daisy::CpuLoadMeter cpu_meter;
#endif

/** SwId index -> MIDI note, copied from chompi-wave's NormalPage.h.
 *  The 25 playable keys are SwId 7..31 (KEY_16, KEY_2..KEY_5, KEY_17..KEY_19,
 *  KEY_1, KEY_6..KEY_10, KEY_20..KEY_22, KEY_11..KEY_15, KEY_23..KEY_25). */
static const uint8_t key_map[40] = {
    0x01, /**< ENC_1_SW */
    0x02, /**< ENC_2_SW */
    0x03, /**< ENC_3_SW */
    0x00, /**< ENC_4_SW */
    0x04, /**< ENC_5_SW */
    0x15, /**< KEY_26 chompi (shift) */
    0x00, /**< SW_TOG skip */
    0x31, /**< KEY_16 */
    0x32, /**< KEY_2 */
    0x34, /**< KEY_3 */
    0x35, /**< KEY_4 */
    0x37, /**< KEY_5 */
    0x33, /**< KEY_17 */
    0x36, /**< KEY_18 */
    0x38, /**< KEY_19 */
    0x30, /**< KEY_1 (C3, lowest) */
    0x39, /**< KEY_6 */
    0x3b, /**< KEY_7 */
    0x3c, /**< KEY_8 */
    0x3e, /**< KEY_9 */
    0x40, /**< KEY_10 */
    0x3a, /**< KEY_20 */
    0x3d, /**< KEY_21 */
    0x3f, /**< KEY_22 */
    0x41, /**< KEY_11 */
    0x43, /**< KEY_12 */
    0x45, /**< KEY_13 */
    0x47, /**< KEY_14 */
    0x48, /**< KEY_15 */
    0x42, /**< KEY_23 */
    0x44, /**< KEY_24 */
    0x46, /**< KEY_25 */
    0x05, /**< ENC_6_SW */
    0x17, /**< KEY_27 play */
    0x18, /**< KEY_28 loop */
    0x00, /**< NC_1 skip */
    0x00, /**< NC_2 skip */
    0x00, /**< NC_3 skip */
    0x00, /**< NC_4 skip */
    0x00, /**< NC_5 skip */
};

/** SwId index -> keybed (SMT) LED index, copied from chompi-wave's NormalPage.h */
static const uint8_t led_map[40] = {
    2,    /**< ENC_1_SW */
    3,    /**< ENC_2_SW */
    4,    /**< ENC_3_SW */
    1,    /**< ENC_4_SW */
    0,    /**< ENC_5_SW */
    0,    /**< KEY_26 chompi */
    0x00, /**< SW_TOG skip */
    0,    /**< KEY_16 */
    23,   /**< KEY_2 */
    22,   /**< KEY_3 */
    21,   /**< KEY_4 */
    20,   /**< KEY_5 */
    1,    /**< KEY_17 */
    2,    /**< KEY_18 */
    3,    /**< KEY_19 */
    24,   /**< KEY_1 */
    19,   /**< KEY_6 */
    18,   /**< KEY_7 */
    17,   /**< KEY_8 */
    16,   /**< KEY_9 */
    15,   /**< KEY_10 */
    4,    /**< KEY_20 */
    5,    /**< KEY_21 */
    6,    /**< KEY_22 */
    14,   /**< KEY_11 */
    13,   /**< KEY_12 */
    12,   /**< KEY_13 */
    11,   /**< KEY_14 */
    10,   /**< KEY_15 */
    7,    /**< KEY_23 */
    8,    /**< KEY_24 */
    9,    /**< KEY_25 */
    9,    /**< ENC_6_SW */
    7,    /**< KEY_27 play */
    8,    /**< KEY_28 loop */
    0x00, /**< NC_1 skip */
    0x00, /**< NC_2 skip */
    0x00, /**< NC_3 skip */
    0x00, /**< NC_4 skip */
    0x00, /**< NC_5 skip */
};

/** Pipe registration -> hardware encoder index.
 *  Physical knobs left-to-right are enc[3], enc[0], enc[1], enc[2]
 *  (PCB net names ENC_4, ENC_1, ENC_2, ENC_3); enc[4] is the transport
 *  wheel (ENC_5, rotary-speaker amount) and enc[5] (ENC_6) is master
 *  volume. */
static const int pipe_encoder[OrganEngine::kNumPipes] = {3, 0, 1, 2};
static const int kVolEnc = 5; /* ENC_6 */

/** SMT LED indices of the 15 white keys, left (C3) to right (C5). */
static constexpr int     kNumWhiteKeys = 15;
static const uint8_t white_key_led[kNumWhiteKeys] = {
    24, 23, 22, 21, 20, 19, 18, 17, 16, 15, 14, 13, 12, 11, 10
};

/** Key-bar color modes. */
enum BarKind
{
    kBarVolume,   /**< green/orange/red position gradient */
    kBarCharacter, /**< uniformly blue: pipe Character bar */
    kBarDrive,    /**< uniformly red: overdrive bar */
    kBarRotary,   /**< uniformly purple: rotary-speaker bar */
    kBarReverb,   /**< uniformly blue: plate-reverb mix bar */
    kBarVibrato,  /**< uniformly pink: vibrato-depth bar */
    kBarAttack,   /**< uniformly purple: pipe attack-time bar */
    kBarRelease,  /**< uniformly yellow: pipe release-time bar */
    kBarModVolume,   /**< uniformly white: pipe volume-modulation bar */
    kBarModCharacter /**< uniformly blue: pipe Character-modulation bar */
};

/** Key-bar color by white-key position w (0 = leftmost): volume bars
 *  (page-1 pipes + master volume) run green for the first 50% of keys,
 *  orange 50-75%, red above 75%. The page-2 pipe Character bar
 *  (kBarCharacter) is uniformly blue; the page-3 pipe attack bar
 *  (kBarAttack) is uniformly purple and the page-4 pipe release bar
 *  (kBarRelease) is uniformly yellow; the overdrive bar (kBarDrive) is
 *  uniformly red; the rotary bar (kBarRotary) is uniformly purple; the
 *  plate-reverb mix bar (kBarReverb) is uniformly blue; the vibrato bar
 *  (kBarVibrato) is uniformly pink. */
static void BarKeyColor(int w, BarKind kind, float &r, float &g, float &b)
{
    if(kind == kBarAttack)
    {
        r = .55f;
        g = 0.f;
        b = 1.f; /* purple: the whole attack-time bar */
        return;
    }
    if(kind == kBarRelease)
    {
        r = 1.f;
        g = 1.f;
        b = 0.f; /* yellow: the whole release-time bar */
        return;
    }
    if(kind == kBarVibrato)
    {
        r = 1.f;
        g = 0.f;
        b = .5f; /* pink: the whole vibrato-depth bar */
        return;
    }
    if(kind == kBarCharacter)
    {
        r = 0.f;
        g = 0.f;
        b = 1.f; /* blue: the whole Character bar */
        return;
    }
    if(kind == kBarReverb)
    {
        r = 0.f;
        g = 0.f;
        b = 1.f; /* blue: the whole plate-reverb mix bar */
        return;
    }
    if(kind == kBarDrive)
    {
        r = 1.f;
        g = 0.f;
        b = 0.f; /* red: the whole overdrive bar, no gradient */
        return;
    }
    if(kind == kBarRotary)
    {
        r = .55f;
        g = 0.f;
        b = 1.f; /* purple: the whole rotary bar */
        return;
    }
    if(kind == kBarModVolume)
    {
        r = 1.f;
        g = 1.f;
        b = 1.f; /* white: the whole volume-modulation bar */
        return;
    }
    if(kind == kBarModCharacter)
    {
        r = 0.f;
        g = 0.f;
        b = 1.f; /* blue: the whole Character-modulation bar */
        return;
    }
    r = 0.f;
    g = 1.f;
    b = 0.f; /* green: first 50% of keys */
    if(w >= 11) /* keys 11-14: above 75% */
    {
        r = 1.f;
        g = 0.f; /* red */
    }
    else if(w >= 7) /* keys 7-10: 50-75% */
    {
        r = 1.f;
        g = .6f; /* orange */
    }
}

/* ---- cross-context (ISR <-> main loop) UI state ---- */
static volatile int      bar_pipe_  = -1;     /* pipe/volume bar showing */
static volatile int      bar_page_  = 0;      /* pipes: 0 = level,
                                               * 1 = Character, 2 = attack,
                                               * 3 = release,
                                               * 4/5 = mod-mode volume/
                                               * Character bars; volume bar:
                                               * 0 = master, 1 = drive,
                                               * 2 = reverb mix, 3 = decay,
                                               * 4 = drive tone */
static volatile uint32_t bar_until_ = 0;      /* ms timestamp for bar timeout */
static const int         kVolBar    = OrganEngine::kNumPipes; /* bar_pipe_: master volume */
static const int         kRotaryBar = OrganEngine::kNumPipes + 1; /* bar_pipe_: wheel (rotary/vibrato) */
static const int         kWheelEnc  = 4; /* enc[4]: transport wheel (ENC_5) */
/** bar_page_ sub-state for kRotaryBar: 0 = rotary amount, 1 = vibrato depth */
static const int         kWheelBarVibrato = 1;
/** Transport-wheel selection: false = rotary (boot default), true =
 *  vibrato. Toggled by the debounced wheel-press edge. Volatile: written
 *  by PollControls (audio ISR), read by UpdateLeds (main loop). */
static volatile bool wheel_vibrato_mode_ = false;
/** Per-pipe encoder pages: 0 = level, 1 = Character, 2 = attack time,
 *  3 = release time; cycled per pipe by that pipe's encoder push-switch. */
static volatile int pipe_page_[OrganEngine::kNumPipes];

/** Modulation Mode: the hardware toggle switch next to SHIFT/CHOMPI
 *  selects Normal Mode (DOWN) or Modulation Mode (UP). Polarity
 *  verified against CHOMPI-Tape: Tape's recording/input monitoring is
 *  active when switch_state == false (NormalPage.h
 *  SetInputMonitor(!switch_state)) and Tape records only with the
 *  switch UP, so UP == hw.GetToggleState() == false. Uses the debounced
 *  GetToggleState() (tog_state hysteresis in ProcessAllControls). */
static bool ModModeSwitchUp() { return !hw.GetToggleState(); }

/** mod_mode_: sampled from the toggle in PollControls (volatile:
 *  ISR-written, main-loop-read for the knob LEDs and bars).
 *  mod_page_[pipe]: Modulation Mode page, 0 = volume-modulation amount
 *  (white knob LED), 1 = Character-modulation amount (blue knob LED) -
 *  separate state from the Normal Mode pipe_page_, which is preserved. */
static volatile bool mod_mode_ = false;
static bool          prev_mod_mode_ = false; /* ISR-only edge tracking */
static volatile int  mod_page_[OrganEngine::kNumPipes];
/** bar_page_ sub-states for pipe bars in Modulation Mode. */
static const int kModBarVolume    = 4;
static const int kModBarCharacter = 5;

/** ENC_6 (volume encoder) pages: master volume -> overdrive -> plate
 *  reverb mix, cycled by the encoder push-switch. */
static const int kVolPageMaster = 0;
static const int kVolPageDrive  = 1;
static const int kVolPageReverb = 2;
static volatile int vol_page_ = kVolPageMaster; /* ENC_6 page */
/** bar_page_ sub-state (not a vol_page_ value): on the reverb page a
 *  SHIFT+turn edits the decay, and the bar shows it instead of the mix. */
static const int kVolBarDecay = 3;
/** bar_page_ sub-state (not a vol_page_ value): on the drive page a
 *  SHIFT+turn edits the distortion tone, and the bar shows the tone
 *  instead of the amount (same 500 ms timeout as the decay bar). */
static const int kVolBarTone = 4;

/* ---- key/encoder edge tracking (audio ISR only) ---- */
static volatile bool prev_key_[40];
static uint8_t       active_note_[40];      /* note used at note-on, +1 (0 = none) */
static volatile int  octave_ = 0;           /* -2..+2, SHIFT+KEY_16/KEY_17 */
static int      prev_enc_[6];
static bool     prev_enc_sw_[4];            /* ENC_1..4_SW push-switch states */
static uint32_t last_click_t_[6];           /* chatter filter timestamps */
static bool     shift_held_ = false;
static bool     latch_on_   = false;        /* KEY_28 (LOOP) toggles key latch */
static bool     latched_[40];               /* key released while latch on: note
                                             * keeps sounding until unlatch */

/** MIDI note on/off goes through this queue so DMA transmits only happen
 *  in the main loop, never inside the audio ISR. B2: notes only - a
 *  dropped note-off leaves external gear hanging, so the queue is deep
 *  and CCs never enter it (they coalesce through the dirty flags below:
 *  only the latest step per pipe is sent). */
struct OutMsg
{
    uint8_t kind; /* 0 = note on, 1 = note off */
    uint8_t a;    /* note */
    uint8_t b;    /* unused */
};
static FIFO<OutMsg, 256> out_q;
static volatile uint8_t cc_pipe_step_[OrganEngine::kNumPipes];
static volatile uint8_t cc_character_step_[OrganEngine::kNumPipes];
static volatile bool    cc_pipe_dirty_[OrganEngine::kNumPipes];
static volatile bool    cc_character_dirty_[OrganEngine::kNumPipes];

/* ---- SD-card presets: one bank of 15 slots (ORGAN/P01..P15.json) ---- */
static OrganPresetBank preset_bank_;
static PresetStorage   preset_storage_;

/** Preset UI modes (Tape's gestures): slot-selection modes plus the
 *  >=1 s SAVING/COPYING/ERASING animations. Volatile: written by
 *  PollControls (audio ISR), read by UpdateLeds (main loop); the main
 *  loop only ends the animations and cancels a selection when the card
 *  is removed. */
enum PresetMode
{
    kPresetNone = 0,
    kPresetSaveSel,
    kPresetSaving,
    kPresetEraseSel,
    kPresetErasing,
    kPresetCopySrc,
    kPresetCopyDest,
    kPresetCopying
};
static volatile int      preset_mode_     = kPresetNone;
static volatile int      preset_sel_slot_ = -1; /* chosen slot */
static volatile int      preset_copy_src_ = -1; /* copy source slot */
static volatile int      preset_current_  = -1; /* last loaded/saved slot */
static volatile uint32_t preset_anim_t_   = 0;  /* animation start (ms) */

/** White-key SwIds, slot 0 (C3, leftmost) .. slot 14 (C5, rightmost). */
static const int kWhiteKeySw[kNumWhiteKeys] = {
    static_cast<int>(Hardware::SwId::KEY_1),
    static_cast<int>(Hardware::SwId::KEY_2),
    static_cast<int>(Hardware::SwId::KEY_3),
    static_cast<int>(Hardware::SwId::KEY_4),
    static_cast<int>(Hardware::SwId::KEY_5),
    static_cast<int>(Hardware::SwId::KEY_6),
    static_cast<int>(Hardware::SwId::KEY_7),
    static_cast<int>(Hardware::SwId::KEY_8),
    static_cast<int>(Hardware::SwId::KEY_9),
    static_cast<int>(Hardware::SwId::KEY_10),
    static_cast<int>(Hardware::SwId::KEY_11),
    static_cast<int>(Hardware::SwId::KEY_12),
    static_cast<int>(Hardware::SwId::KEY_13),
    static_cast<int>(Hardware::SwId::KEY_14),
    static_cast<int>(Hardware::SwId::KEY_15),
};

/** A4: incremented by the audio ISR; the IWDG is only refreshed while this
 *  keeps changing (see MainLoop), so any lockup resets the device. */
volatile uint32_t audio_ticks = 0;

static void CcLevelCallback(int pipe, int page)
{
    bar_pipe_  = pipe;
    bar_page_  = page;
    bar_until_ = System::GetNow() + 500;
}

/* ---- preset gesture helpers (audio ISR context) ---- */

static int WhiteKeySlot(int swid)
{
    for(int i = 0; i < kNumWhiteKeys; i++)
        if(kWhiteKeySw[i] == swid)
            return i;
    return -1;
}

/** Load a slot: apply the RAM-cached preset to the engine through the
 *  regular (ramped/phase-continuous) setters, so held and latched notes
 *  keep sounding click-free. Empty slots do nothing. */
static void PresetLoadSlot(int slot)
{
    if(!preset_bank_.IsValid(slot))
        return;
    ApplyToEngine(preset_bank_.Get(slot), engine);
    preset_current_ = slot;
    /* resync MIDI controllers with the loaded values (the coalesced CCs
     * go out in MainLoop) */
    for(int p = 0; p < OrganEngine::kNumPipes; p++)
    {
        cc_pipe_step_[p]       = engine.GetPipeStep(p);
        cc_pipe_dirty_[p]      = true;
        cc_character_step_[p]  = engine.GetPipeCharacterStep(p);
        cc_character_dirty_[p] = true;
    }
    bar_pipe_  = -1; /* cancel any active value bar */
    bar_until_ = 0;
}

static void PresetEnterMode(int mode)
{
    preset_mode_     = mode;
    preset_sel_slot_ = -1;
    preset_copy_src_ = -1;
    bar_pipe_        = -1; /* the preset view takes over the white keys */
    bar_until_       = 0;
}

/** A FRESH SHIFT press confirms the pending selection. Entering a mode
 *  needs SHIFT already held, so the entering hold can never double as
 *  the confirm; the user releases and re-presses SHIFT to confirm. */
static void PresetShiftConfirm()
{
    switch(preset_mode_)
    {
        case kPresetSaveSel:
            if(preset_sel_slot_ >= 0)
            {
                OrganPreset p;
                CaptureFromEngine(p, engine);
                preset_bank_.Save(preset_sel_slot_, p);
                preset_current_ = preset_sel_slot_;
                preset_mode_    = kPresetSaving;
                preset_anim_t_  = System::GetNow();
            }
            break;
        case kPresetEraseSel:
            if(preset_sel_slot_ >= 0)
            {
                preset_bank_.Erase(preset_sel_slot_);
                if(preset_current_ == preset_sel_slot_)
                    preset_current_ = -1; /* deleting the current slot
                                           * clears "current" */
                preset_mode_   = kPresetErasing;
                preset_anim_t_ = System::GetNow();
            }
            break;
        case kPresetCopyDest:
            if(preset_sel_slot_ >= 0 && preset_copy_src_ >= 0)
            {
                preset_bank_.Copy(preset_copy_src_, preset_sel_slot_);
                preset_mode_   = kPresetCopying;
                preset_anim_t_ = System::GetNow();
            }
            break;
        default: break; /* COPY_SRC: a destination is still needed */
    }
}

/** Preset gestures. Returns true when the key event is consumed (it never
 *  reaches the note/latch logic). Releases of notes started before a
 *  mode was entered fall through, so held notes cannot hang. */
static bool PresetKeyEvent(int swid, bool down)
{
    if(preset_mode_ != kPresetNone)
    {
        if(!down)
            return active_note_[swid] == 0; /* live note: fall through */
        const int slot = WhiteKeySlot(swid);
        if(slot >= 0)
        {
            switch(preset_mode_)
            {
                case kPresetSaveSel:
                    preset_sel_slot_ = slot; /* any slot */
                    break;
                case kPresetEraseSel:
                    if(preset_bank_.IsValid(slot)) /* filled slots only */
                        preset_sel_slot_ = slot;
                    break;
                case kPresetCopySrc:
                    if(preset_bank_.IsValid(slot))
                    {
                        preset_copy_src_ = slot;
                        preset_mode_     = kPresetCopyDest;
                    }
                    break;
                case kPresetCopyDest:
                    if(slot != preset_copy_src_) /* a different slot */
                        preset_sel_slot_ = slot;
                    break;
                default: break; /* animations: consume everything */
            }
            return true;
        }
        /* the matching operation key cancels; the others are inert */
        if(swid == static_cast<int>(Hardware::SwId::KEY_23))
        {
            if(preset_mode_ == kPresetEraseSel)
                preset_mode_ = kPresetNone;
            return true;
        }
        if(swid == static_cast<int>(Hardware::SwId::KEY_24))
        {
            if(preset_mode_ == kPresetCopySrc
               || preset_mode_ == kPresetCopyDest)
                preset_mode_ = kPresetNone;
            return true;
        }
        if(swid == static_cast<int>(Hardware::SwId::KEY_25))
        {
            if(preset_mode_ == kPresetSaveSel)
                preset_mode_ = kPresetNone;
            return true;
        }
        return true; /* every other key is consumed while a mode is active */
    }

    /* NONE mode: SHIFT + key preset gestures (unshifted keys, and SHIFT
     * with the octave keys, fall through to the normal handling) */
    if(!down || !shift_held_)
        return false;
    const bool oct = swid == static_cast<int>(Hardware::SwId::KEY_16)
                     || swid == static_cast<int>(Hardware::SwId::KEY_17);
    if(preset_storage_.NoSdCard())
        return !oct; /* red-key view: gestures do nothing, octave works */
    if(swid == static_cast<int>(Hardware::SwId::KEY_23))
    {
        PresetEnterMode(kPresetEraseSel);
        return true;
    }
    if(swid == static_cast<int>(Hardware::SwId::KEY_24))
    {
        PresetEnterMode(kPresetCopySrc);
        return true;
    }
    if(swid == static_cast<int>(Hardware::SwId::KEY_25))
    {
        PresetEnterMode(kPresetSaveSel);
        return true;
    }
    const int slot = WhiteKeySlot(swid);
    if(slot >= 0)
    {
        PresetLoadSlot(slot);
        return true;
    }
    return false;
}

/** Key + encoder polling, called from the audio ISR once per block (2 kHz). */
static void PollControls()
{
    /* shift key (CHOMPI key) */
    shift_held_ = hw.button_sr.State(static_cast<int>(Hardware::SwId::KEY_26));

    /* a fresh SHIFT press confirms a pending preset selection */
    {
        static bool prev_shift_held_ = false;
        if(shift_held_ && !prev_shift_held_)
            PresetShiftConfirm();
        prev_shift_held_ = shift_held_;
    }

    /* Modulation Mode toggle (UP = modulation). Flipping the switch
     * mid-bar cancels the old mode's bar; the knob LEDs follow
     * mod_mode_ directly on the next UpdateLeds pass. */
    mod_mode_ = ModModeSwitchUp();
    if(mod_mode_ != prev_mod_mode_)
    {
        prev_mod_mode_ = mod_mode_;
        bar_pipe_      = -1; /* cancel the other mode's bar */
        bar_until_     = 0;
    }

    /* pipe encoder push-switches (ENC_1..4_SW = SwId 0..3). Normal Mode:
     * press cycles that pipe's encoder through page 1 (level) -> 2
     * (Character) -> 3 (attack) -> 4 (release); Modulation Mode: press
     * toggles that pipe between mod page 1 (volume modulation) and mod
     * page 2 (Character modulation); either way the newly active page's
     * value bar shows as confirmation. SHIFT+press instead toggles that
     * pipe's mute in both modes (the press is fully consumed: no page
     * cycle). */
    for(int i = 0; i < 4; i++)
    {
        const bool down = hw.button_sr.State(i);
        if(down && !prev_enc_sw_[i])
        {
            for(int p = 0; p < OrganEngine::kNumPipes; p++)
            {
                if(pipe_encoder[p] == i)
                {
                    if(shift_held_)
                    {
                        /* mute is a state, not a value: the persistent
                         * red knob LED is the feedback, so no value bar
                         * is re-armed here */
                        engine.TogglePipeMuted(p);
                    }
                    else if(mod_mode_)
                    {
                        mod_page_[p] = mod_page_[p] == 0 ? 1 : 0;
                        bar_pipe_    = p;
                        bar_page_    = mod_page_[p] == 0
                                           ? kModBarVolume
                                           : kModBarCharacter;
                        bar_until_   = System::GetNow() + 500;
                    }
                    else
                    {
                        pipe_page_[p] = (pipe_page_[p] + 1) % 4;
                        bar_pipe_     = p;
                        bar_page_     = pipe_page_[p];
                        bar_until_    = System::GetNow() + 500;
                    }
                }
            }
        }
        prev_enc_sw_[i] = down;
    }

    /* ENC_6_SW (volume-knob push, SwId 32): press cycles the volume
     * encoder through master volume -> overdrive -> plate-reverb mix;
     * show the newly active page's value bar as confirmation */
    {
        const int  vsw  = static_cast<int>(Hardware::SwId::ENC_6_SW);
        const bool down = hw.button_sr.State(vsw);
        if(down && !prev_key_[vsw])
        {
            vol_page_  = (vol_page_ + 1) % 3;
            bar_pipe_  = kVolBar;
            bar_page_  = vol_page_;
            bar_until_ = System::GetNow() + 500;
        }
        prev_key_[vsw] = down;
    }

    /* KEY_28 (LOOP) toggles the key latch. While latched, releasing a key
     * keeps its note sounding; re-pressing a key whose note is latched
     * releases that note (toggle-off) instead of retriggering it;
     * unlatching releases every latched note whose key is not physically
     * held down again (those keep sounding and release normally on key
     * release). Applies to physical keys only, never MIDI. */
    {
        const int  loop_sw = static_cast<int>(Hardware::SwId::KEY_28);
        const bool down    = hw.button_sr.State(loop_sw);
        if(down && !prev_key_[loop_sw])
        {
            latch_on_ = !latch_on_;
            if(!latch_on_)
            {
                for(int swid = 7; swid < 32; swid++)
                {
                    if(latched_[swid] && active_note_[swid] != 0
                       && !hw.button_sr.State(swid))
                    {
                        const uint8_t note = active_note_[swid] - 1;
                        engine.NoteOff(note);
                        out_q.PushBack({1, note, 0});
                    }
                    latched_[swid] = false;
                }
            }
        }
        prev_key_[loop_sw] = down;
    }

    /* playable keys: SwId 7..31 (SHIFT+KEY_16/KEY_17 = octave down/up) */
    for(int swid = 7; swid < 32; swid++)
    {
        const bool down = hw.button_sr.State(swid);
        if(down == prev_key_[swid])
            continue;

        /* Presets: while a selection mode (or its animation) is active,
         * key presses select/confirm/cancel and never play notes or
         * touch the latch; in NONE mode SHIFT+white-key loads a slot and
         * SHIFT+KEY_23/24/25 enters erase/copy/save select. Releases of
         * notes started before the mode fall through, so nothing hangs. */
        if(PresetKeyEvent(swid, down))
        {
            prev_key_[swid] = down;
            continue;
        }
        prev_key_[swid] = down;

        const bool oct_key = (swid == static_cast<int>(Hardware::SwId::KEY_16))
                             || (swid == static_cast<int>(Hardware::SwId::KEY_17));
        if(down)
        {
            if(shift_held_ && oct_key)
            {
                int oct = octave_
                          + (swid == static_cast<int>(Hardware::SwId::KEY_16)
                                 ? -1
                                 : 1);
                octave_ = oct < -2 ? -2 : (oct > 2 ? 2 : oct);
                continue;
            }
            /* Re-pressing a key whose note is currently latched releases
             * that note from the latch (toggle-off) instead of retriggering
             * it. latched_ is only set while active_note_ != 0, so the old
             * note is always valid; the later key-up edge finds
             * active_note_ == 0 and does nothing. */
            if(latch_on_ && latched_[swid])
            {
                const uint8_t old  = active_note_[swid] - 1;
                active_note_[swid] = 0;
                latched_[swid]     = false;
                engine.NoteOff(old);
                out_q.PushBack({1, old, 0});
                continue;
            }
            int n = key_map[swid] + 12 * octave_;
            n              = n < 0 ? 0 : (n > 127 ? 127 : n);
            const uint8_t note = static_cast<uint8_t>(n);
            /* B3: re-pressing a latched key after an octave change used to
             * overwrite active_note_ and leak the old note (no NoteOff, no
             * MIDI note-off): release the old note first. With the
             * toggle-off branch above this only covers non-latch edge
             * cases, kept as a safety net. */
            if(active_note_[swid] != 0 && active_note_[swid] - 1 != note)
            {
                const uint8_t old = active_note_[swid] - 1;
                engine.NoteOff(old);
                out_q.PushBack({1, old, 0});
            }
            active_note_[swid] = note + 1;
            latched_[swid]     = false;
            engine.NoteOn(note, 1.f);
            out_q.PushBack({0, note, 0});
        }
        else if(active_note_[swid] != 0)
        {
            /* note-off uses the note that was started (octave may have
             * changed, or shift may now be held, while the key was down) */
            if(latch_on_)
            {
                /* latched: keep sounding; note-off is deferred to unlatch */
                latched_[swid] = true;
            }
            else
            {
                const uint8_t note   = active_note_[swid] - 1;
                active_note_[swid]   = 0;
                engine.NoteOff(note);
                out_q.PushBack({1, note, 0});
            }
        }
    }

    /* wheel press (ENC_5 push-switch via the ChompiEncoder click pin D10):
     * toggle the wheel between rotary-speaker and vibrato control and
     * show the newly selected control's value bar as confirmation.
     * RisingEdge is a const read of the debounced switch state, so this
     * coexists with the tracker reset inside the encoder loop below. */
    if(hw.enc[kWheelEnc].RisingEdge())
    {
        wheel_vibrato_mode_ = !wheel_vibrato_mode_;
        bar_pipe_           = kRotaryBar;
        bar_page_           = wheel_vibrato_mode_ ? kWheelBarVibrato : 0;
        bar_until_          = System::GetNow() + 500;
    }

    /* encoders: enc_trackers accumulate clicks and reset to 0 on press */
    for(int i = 0; i < 6; i++)
    {
        if(hw.enc[i].RisingEdge())
            prev_enc_[i] = 0;

        const int delta = hw.enc_trackers[i] - prev_enc_[i];
        prev_enc_[i]    = hw.enc_trackers[i];
        if(delta == 0)
            continue;

        /* Chatter filter: the detent-less volume encoder can rest on a
         * quadrature transition and emit increment bursts at ~1 ms spacing,
         * which would re-arm the level bar forever. Accept at most one click
         * per 8 ms per encoder (fast hand turns are >10 ms per detent). */
        const uint32_t now_ms = System::GetNow();
        if(now_ms - last_click_t_[i] < 8)
            continue; /* dropped for good: prev_enc_ already synced */
        last_click_t_[i] = now_ms;

        if(i == kVolEnc)
        {
            if(shift_held_)
            {
                if(vol_page_ == kVolPageDrive)
                {
                    /* SHIFT+turn on page 2: distortion tone, 1
                     * percentage point per click, clamped 0..100
                     * without wrapping (FR-4b): 0% = 500 Hz per stage
                     * (dark), 100% = 10 kHz per stage (two cascaded
                     * one-pole stages, ~12 dB/octave). The tone shows
                     * as a red value bar for 500 ms like page 3's
                     * decay bar; the amount is not touched. */
                    engine.SetDriveTonePercent(engine.GetDriveTonePercent()
                                               + delta);
                    bar_pipe_  = kVolBar;
                    bar_page_  = kVolBarTone;
                    bar_until_ = System::GetNow() + 500;
                }
                else if(vol_page_ == kVolPageReverb)
                {
                    /* SHIFT+turn on page 3: plate-reverb decay, 100
                     * log-spaced steps from 0.2 s to 20 s (fine tune
                     * stays on page 1) */
                    engine.SetReverbDecayStep(engine.GetReverbDecayStep()
                                              + delta);
                    bar_pipe_  = kVolBar;
                    bar_page_  = kVolBarDecay;
                    bar_until_ = System::GetNow() + 500;
                }
                else
                {
                    /* SHIFT+ENC_6 on page 1: fine tune */
                    engine.AddFineTuneCents(delta);
                }
            }
            else if(vol_page_ == kVolPageDrive)
            {
                /* volume-encoder page 2: overdrive amount, 32 steps */
                engine.SetDriveStep(engine.GetDriveStep() + delta);
                bar_pipe_  = kVolBar;
                bar_page_  = kVolPageDrive;
                bar_until_ = System::GetNow() + 500;
            }
            else if(vol_page_ == kVolPageReverb)
            {
                /* volume-encoder page 3: plate-reverb mix, 1% per click,
                 * clamped 0..100 */
                engine.SetReverbMixPercent(engine.GetReverbMixPercent()
                                           + delta);
                bar_pipe_  = kVolBar;
                bar_page_  = kVolPageReverb;
                bar_until_ = System::GetNow() + 500;
            }
            else
            {
                /* existing CHOMPI volume-knob behaviour: 100 clicks, 0..1 */
                float g = engine.GetMasterGain() + delta * .01f;
                engine.SetMasterGain(g);
                bar_pipe_  = kVolBar;
                bar_page_  = kVolPageMaster;
                bar_until_ = System::GetNow() + 500;
            }
        }
        else if(i == kWheelEnc)
        {
            if(wheel_vibrato_mode_)
            {
                /* wheel in vibrato mode: depth 0..50 cents peak */
                engine.SetVibratoDepth(engine.GetVibratoDepth() + delta);
                bar_pipe_  = kRotaryBar;
                bar_page_  = kWheelBarVibrato;
                bar_until_ = System::GetNow() + 500;
            }
            else
            {
                /* transport wheel (ENC_5): rotary-speaker amount, 32 steps */
                engine.SetRotaryStep(engine.GetRotaryStep() + delta);
                bar_pipe_  = kRotaryBar;
                bar_page_  = 0;
                bar_until_ = System::GetNow() + 500;
            }
        }
        else
        {
            for(int p = 0; p < OrganEngine::kNumPipes; p++)
            {
                if(pipe_encoder[p] != i)
                    continue;
                if(mod_mode_)
                {
                    /* Modulation Mode turn: set this pipe's modulation
                     * amount for its mod page (32 steps, 0..100%,
                     * clamped, no wrap). SHIFT+turn applies the delta
                     * to the turned pipe, reads back its clamped value
                     * and broadcasts it to all 4 pipes (the same
                     * absolute-broadcast pattern as Normal Mode). No
                     * MIDI CCs for the modulation amounts. */
                    if(mod_page_[p] == 0)
                    {
                        engine.SetPipeVolModStep(
                            p, engine.GetPipeVolModStep(p) + delta);
                        if(shift_held_)
                        {
                            const int v = engine.GetPipeVolModStep(p);
                            for(int q = 0; q < OrganEngine::kNumPipes; q++)
                                engine.SetPipeVolModStep(q, v);
                        }
                    }
                    else
                    {
                        engine.SetPipeCharModStep(
                            p, engine.GetPipeCharModStep(p) + delta);
                        if(shift_held_)
                        {
                            const int v = engine.GetPipeCharModStep(p);
                            for(int q = 0; q < OrganEngine::kNumPipes; q++)
                                engine.SetPipeCharModStep(q, v);
                        }
                    }
                    bar_pipe_  = p;
                    bar_page_  = mod_page_[p] == 0 ? kModBarVolume
                                                   : kModBarCharacter;
                    bar_until_ = System::GetNow() + 500;
                    continue;
                }
                if(shift_held_)
                {
                    /* SHIFT+turn: apply the delta to the turned pipe,
                     * read back its clamped value, then write that same
                     * value to ALL 4 pipes (absolute broadcast, per the
                     * turned pipe's current page). The bar shows the
                     * value all four pipes now share. */
                    switch(pipe_page_[p])
                    {
                        case 1: /* Character */
                        {
                            engine.SetPipeCharacterStep(
                                p, engine.GetPipeCharacterStep(p) + delta);
                            const int v = engine.GetPipeCharacterStep(p);
                            for(int q = 0; q < OrganEngine::kNumPipes; q++)
                            {
                                engine.SetPipeCharacterStep(q, v);
                                cc_character_step_[q]  = v;
                                cc_character_dirty_[q]
                                    = true; /* coalesced, sent in MainLoop */
                            }
                            break;
                        }
                        case 2: /* attack time (no MIDI CC) */
                        {
                            engine.SetPipeAttackStep(
                                p, engine.GetPipeAttackStep(p) + delta);
                            const int v = engine.GetPipeAttackStep(p);
                            for(int q = 0; q < OrganEngine::kNumPipes; q++)
                                engine.SetPipeAttackStep(q, v);
                            break;
                        }
                        case 3: /* release time (no MIDI CC) */
                        {
                            engine.SetPipeReleaseStep(
                                p, engine.GetPipeReleaseStep(p) + delta);
                            const int v = engine.GetPipeReleaseStep(p);
                            for(int q = 0; q < OrganEngine::kNumPipes; q++)
                                engine.SetPipeReleaseStep(q, v);
                            break;
                        }
                        default: /* level */
                        {
                            engine.SetPipeStep(
                                p, engine.GetPipeStep(p) + delta);
                            const int v = engine.GetPipeStep(p);
                            for(int q = 0; q < OrganEngine::kNumPipes; q++)
                            {
                                engine.SetPipeStep(q, v);
                                cc_pipe_step_[q]  = v;
                                cc_pipe_dirty_[q]
                                    = true; /* coalesced, sent in MainLoop */
                            }
                            break;
                        }
                    }
                }
                else
                {
                    switch(pipe_page_[p])
                    {
                        case 1: /* Character */
                            engine.SetPipeCharacterStep(
                                p, engine.GetPipeCharacterStep(p) + delta);
                            cc_character_step_[p]
                                = engine.GetPipeCharacterStep(p);
                            cc_character_dirty_[p]
                                = true; /* coalesced, sent in MainLoop */
                            break;
                        case 2: /* attack time (no MIDI CC) */
                            engine.SetPipeAttackStep(
                                p, engine.GetPipeAttackStep(p) + delta);
                            break;
                        case 3: /* release time (no MIDI CC) */
                            engine.SetPipeReleaseStep(
                                p, engine.GetPipeReleaseStep(p) + delta);
                            break;
                        default: /* level */
                            engine.SetPipeStep(p, engine.GetPipeStep(p) + delta);
                            cc_pipe_step_[p]  = engine.GetPipeStep(p);
                            cc_pipe_dirty_[p] = true; /* coalesced, sent in MainLoop */
                            break;
                    }
                }
                bar_pipe_  = p;
                bar_page_  = pipe_page_[p];
                bar_until_ = System::GetNow() + 500;
            }
        }
    }
}

void AudioCallback(AudioHandle::InputBuffer in,
                   AudioHandle::OutputBuffer out,
                   size_t size)
{
    audio_ticks++; /* A4: IWDG liveness proof */
#ifdef ORGAN_CPU_METER
    cpu_meter.OnBlockStart();
#endif
    midi.ProcessMidiIn();
    hw.ProcessAllControls();
    PollControls();
    engine.Prepare(size);
    engine.Process(out, size); /* audio input ignored */
#ifdef ORGAN_CPU_METER
    cpu_meter.OnBlockEnd();
#endif
}

/** Key LEDs: preset views/modes while active, level bar while a pipe
 *  knob turns, the SHIFT slot overview, else held notes. */
static void UpdateLeds()
{
    const uint32_t now = System::GetNow();

    /* 250 ms blink for the preset UI */
    static bool     pre_blink   = false;
    static uint32_t pre_blink_t = 0;
    if(now - pre_blink_t > 250)
    {
        pre_blink   = !pre_blink;
        pre_blink_t = now;
    }

    if(preset_mode_ != kPresetNone)
    {
        /* A preset selection mode (or animation) takes over the white
         * keys; first match wins: during the SAVING/COPYING/ERASING
         * animations the slots show their new state steadily; in
         * copy-destination select the copy source is green; the
         * selected slot is blue (save / copy destination) or red
         * (erase target); the current slot (last loaded/saved) is
         * white when filled. The rest depends on the mode: in SAVE
         * select / copy-destination select filled slots are solid
         * purple, the current slot solid white and empty slots blink
         * dim grey; in ERASE select / copy-source select filled slots
         * blink dim purple, the current slot blinks white and empty
         * slots stay off. */
        for(int i = 0; i < 25; i++)
            SetSmtLedFloat(i, 0.f, 0.f, 0.f);
        const bool anim = preset_mode_ == kPresetSaving
                          || preset_mode_ == kPresetCopying
                          || preset_mode_ == kPresetErasing;
        const bool save_like = preset_mode_ == kPresetSaveSel
                               || preset_mode_ == kPresetCopyDest;
        for(int w = 0; w < kNumWhiteKeys; w++)
        {
            float      r = 0.f, g = 0.f, b = 0.f;
            const bool filled = preset_bank_.IsValid(w);
            if(anim)
            {
                if(filled)
                {
                    r = .55f;
                    b = 1.f; /* steady purple */
                }
            }
            else if(preset_mode_ == kPresetCopyDest && w == preset_copy_src_)
            {
                g = 1.f; /* copy source: green */
            }
            else if(w == preset_sel_slot_)
            {
                if(preset_mode_ == kPresetEraseSel)
                    r = 1.f; /* erase target: red */
                else
                    b = 1.f; /* save / copy destination: blue */
            }
            else if(filled && w == preset_current_)
            {
                if(save_like || pre_blink)
                    r = g = b = 1.f; /* current: solid white (save /
                                      * copy dest), blinking white
                                      * (erase / copy source) */
            }
            else if(filled)
            {
                if(save_like)
                {
                    r = .55f;
                    b = 1.f; /* solid purple */
                }
                else if(pre_blink)
                {
                    r = .22f;
                    b = .4f; /* 0.4x purple, blinking */
                }
            }
            else if(save_like && pre_blink)
            {
                r = g = b = .4f; /* selectable empty slots: blinking grey */
            }
            SetSmtLedFloat(white_key_led[w], r, g, b);
        }
    }
    else if(bar_pipe_ >= 0 && now < bar_until_)
    {
        const int p = bar_pipe_;
        /* value bar spans the 15 white keys only: leftmost white = 0%,
         * rightmost white = 100%. Volume bars (page-1 pipes + master) use
         * the green/orange/red position gradient; the page-2 pipe
         * Character bar is uniformly blue, the page-3 attack bar purple,
         * the page-4 release bar yellow, the overdrive bar uniformly
         * red and the rotary bar uniformly purple. */
        int    lit;
        BarKind kind = kBarVolume;
        if(p == kVolBar)
        {
            if(bar_page_ == kVolBarTone)
            {
                /* volume-knob page 2, SHIFT+turn: distortion tone
                 * percent across the 15 white keys, same red as the
                 * overdrive bar */
                lit  = (engine.GetDriveTonePercent() * kNumWhiteKeys + 50)
                       / 100;
                kind = kBarDrive;
            }
            else if(bar_page_ == kVolBarDecay)
            {
                /* volume-knob page 3, SHIFT+turn: reverb decay step
                 * (log-spaced 0.5..20 s) */
                lit  = (engine.GetReverbDecayStep() * kNumWhiteKeys + 50)
                       / 100;
                kind = kBarReverb;
            }
            else if(bar_page_ == kVolPageReverb)
            {
                /* volume-knob page 3: plate-reverb mix percent */
                lit  = (engine.GetReverbMixPercent() * kNumWhiteKeys + 50)
                       / 100;
                kind = kBarReverb;
            }
            else if(bar_page_ == kVolPageDrive)
            {
                /* volume-knob page 2: overdrive amount */
                lit  = (engine.GetDriveStep() * kNumWhiteKeys + 16)
                       / OrganEngine::kMaxSteps;
                kind = kBarDrive;
            }
            else
            {
                lit = static_cast<int>(engine.GetMasterGain() * kNumWhiteKeys
                                       + .5f);
            }
        }
        else if(p == kRotaryBar)
        {
            if(bar_page_ == kWheelBarVibrato)
            {
                /* wheel in vibrato mode: depth 0..50 cents mapped across
                 * the 15 white keys (not 50 display segments) */
                lit  = (engine.GetVibratoDepth() * kNumWhiteKeys + 25)
                       / OrganEngine::kMaxVibratoDepthCents;
                kind = kBarVibrato;
            }
            else
            {
                /* transport wheel: rotary-speaker amount */
                lit  = (engine.GetRotaryStep() * kNumWhiteKeys + 16)
                       / OrganEngine::kMaxSteps;
                kind = kBarRotary;
            }
        }
        else if(bar_page_ == kModBarVolume)
        {
            /* Modulation Mode mod-page 1: pipe volume-modulation amount */
            lit  = (engine.GetPipeVolModStep(p) * kNumWhiteKeys + 16)
                   / OrganEngine::kMaxModSteps;
            kind = kBarModVolume;
        }
        else if(bar_page_ == kModBarCharacter)
        {
            /* Modulation Mode mod-page 2: pipe Character-modulation amt */
            lit  = (engine.GetPipeCharModStep(p) * kNumWhiteKeys + 16)
                   / OrganEngine::kMaxModSteps;
            kind = kBarModCharacter;
        }
        else if(bar_page_ == 1)
        {
            lit  = (engine.GetPipeCharacterStep(p) * kNumWhiteKeys + 32)
                   / OrganEngine::kMaxCharacterSteps;
            kind = kBarCharacter;
        }
        else if(bar_page_ == 2)
        {
            /* pipe page 3: attack time, 32 log-spaced steps 5 ms..10 s */
            lit  = (engine.GetPipeAttackStep(p) * kNumWhiteKeys + 16)
                   / OrganEngine::kMaxEnvSteps;
            kind = kBarAttack;
        }
        else if(bar_page_ == 3)
        {
            /* pipe page 4: release time, 32 log-spaced steps 5 ms..10 s */
            lit  = (engine.GetPipeReleaseStep(p) * kNumWhiteKeys + 16)
                   / OrganEngine::kMaxEnvSteps;
            kind = kBarRelease;
        }
        else
        {
            lit = (engine.GetPipeStep(p) * kNumWhiteKeys + 16)
                  / OrganEngine::kMaxSteps;
        }
        for(int i = 0; i < 25; i++)
            SetSmtLedFloat(i, 0.f, 0.f, 0.f);
        for(int w = 0; w < lit && w < kNumWhiteKeys; w++)
        {
            float r, g, b;
            BarKeyColor(w, kind, r, g, b);
            SetSmtLedFloat(white_key_led[w], r, g, b);
        }
    }
    else if(shift_held_)
    {
        /* SHIFT preset view (NONE mode): the 15 white keys are the 15
         * preset slots - filled purple, the current slot white, empty
         * off; all red with no SD card. The two octave-key hints stay
         * lit; the erase/copy/save operation LEDs are added below. */
        for(int i = 0; i < 25; i++)
            SetSmtLedFloat(i, 0.f, 0.f, 0.f);
        if(preset_storage_.NoSdCard())
        {
            for(int w = 0; w < kNumWhiteKeys; w++)
                SetSmtLedFloat(white_key_led[w], 1.f, 0.f, 0.f);
        }
        else
        {
            for(int w = 0; w < kNumWhiteKeys; w++)
            {
                if(w == preset_current_ && preset_bank_.IsValid(w))
                    SetSmtLedFloat(white_key_led[w], 1.f, 1.f, 1.f);
                else if(preset_bank_.IsValid(w))
                    SetSmtLedFloat(white_key_led[w], .55f, 0.f, 1.f);
            }
        }
        /* the two octave keys (C#3/D#3) are always lit dim white so the
         * control is discoverable; the active direction turns orange
         * (dim = 1 octave, bright = 2) */
        const float dn = octave_ < 0 ? (octave_ < -1 ? 1.f : .45f) : 0.f;
        const float up = octave_ > 0 ? (octave_ > 1 ? 1.f : .45f) : 0.f;
        SetSmtLedFloat(0, dn > 0.f ? dn : .10f,
                       dn > 0.f ? .5f * dn : .10f, 0.f);
        SetSmtLedFloat(1, up > 0.f ? up : .10f,
                       up > 0.f ? .5f * up : .10f, 0.f);
    }
    else
    {
        bar_pipe_ = -1;
        for(int swid = 7; swid < 32; swid++)
        {
            /* physical key held (octave-shifted note) or MIDI-held note
             * (C3: compare against the octave-shifted note) */
            int held_note = key_map[swid] + 12 * octave_;
            held_note = held_note < 0 ? 0 : (held_note > 127 ? 127 : held_note);
            const bool on
                = prev_key_[swid]
                  || engine.IsNoteHeld(static_cast<uint8_t>(held_note));
            SetSmtLedFloat(led_map[swid], on ? 1.f : 0.f, on ? 1.f : 0.f,
                           on ? 1.f : 0.f);
        }
    }

    /* preset operation LEDs on the three top black keys (SMT 7/8/9):
     * erase = red, copy = green, save = blue, shown while SHIFT is held
     * or a preset mode is active; during an operation only that
     * operation's LED is lit; all off with no SD card */
    if(shift_held_ || preset_mode_ != kPresetNone)
    {
        float er = 0.f, cp = 0.f, sv = 0.f;
        if(!preset_storage_.NoSdCard())
        {
            switch(preset_mode_)
            {
                case kPresetNone: er = cp = sv = 1.f; break;
                case kPresetEraseSel:
                case kPresetErasing: er = 1.f; break;
                case kPresetCopySrc:
                case kPresetCopyDest:
                case kPresetCopying: cp = 1.f; break;
                default: sv = 1.f; break; /* save select / saving */
            }
        }
        SetSmtLedFloat(7, er, 0.f, 0.f);
        SetSmtLedFloat(8, 0.f, cp, 0.f);
        SetSmtLedFloat(9, 0.f, 0.f, sv);
    }

    /* knob LEDs: pipe values on PTH 1-4, master volume on PTH 9.
     * Normal Mode: page 2 pipes glow blue, page 3 (attack) purple and
     * page 4 (release) yellow - each with a brightness floor so the
     * page stays visible at step 0; page 1 is white. Modulation Mode:
     * mod page 1 (volume modulation) is white and mod page 2
     * (Character modulation) is blue, brightness = 0.2 + 0.8 x amount.
     * A muted pipe's knob LED is solid red in both modes, overriding
     * the page color. */
    for(int p = 0; p < OrganEngine::kNumPipes; p++)
    {
        if(engine.GetPipeMuted(p))
        {
            SetPthLedFloat(p + 1, 1.f, 0.f, 0.f); /* solid red: muted */
            continue;
        }
        if(mod_mode_)
        {
            if(mod_page_[p] == 0)
            {
                const float s = .2f
                                + .8f * engine.GetPipeVolModStep(p)
                                      * (1.f / OrganEngine::kMaxModSteps);
                SetPthLedFloat(p + 1, s, s, s); /* white: volume mod */
            }
            else
            {
                const float s = .2f
                                + .8f * engine.GetPipeCharModStep(p)
                                      * (1.f / OrganEngine::kMaxModSteps);
                SetPthLedFloat(p + 1, 0.f, 0.f, s); /* blue: Character mod */
            }
            continue;
        }
        switch(pipe_page_[p])
        {
            case 1:
            {
                const float s = .2f
                                + .8f * engine.GetPipeCharacterStep(p)
                                      * (1.f / OrganEngine::kMaxCharacterSteps);
                SetPthLedFloat(p + 1, 0.f, 0.f, s); /* blue: Character */
                break;
            }
            case 2:
            {
                const float s = .2f
                                + .8f * engine.GetPipeAttackStep(p)
                                      * (1.f / OrganEngine::kMaxEnvSteps);
                SetPthLedFloat(p + 1, .55f * s, 0.f, s); /* purple: attack */
                break;
            }
            case 3:
            {
                const float s = .2f
                                + .8f * engine.GetPipeReleaseStep(p)
                                      * (1.f / OrganEngine::kMaxEnvSteps);
                SetPthLedFloat(p + 1, s, s, 0.f); /* yellow: release */
                break;
            }
            default:
            {
                const float s = engine.GetPipeStep(p)
                                * (1.f / OrganEngine::kMaxSteps);
                SetPthLedFloat(p + 1, s, s, s); /* white: level */
                break;
            }
        }
    }
    /* master-volume knob LED (PTH 9): volume-encoder page 3 tracks the
     * plate-reverb mix in blue and page 2 the overdrive amount in red
     * (both with a brightness floor so the page stays visible at 0%). On
     * page 1 brightness tracks the level and the color follows the same
     * position gradient as the white-key volume bar (green up to 50%,
     * orange 50-75%, red above 75%) */
    if(vol_page_ == kVolPageReverb)
    {
        const float s = .2f
                        + .8f * engine.GetReverbMixPercent() * .01f;
        SetPthLedFloat(9, 0.f, 0.f, s);
    }
    else if(vol_page_ == kVolPageDrive)
    {
        const float s = .2f
                        + .8f * engine.GetDriveStep()
                                  * (1.f / OrganEngine::kMaxSteps);
        SetPthLedFloat(9, s, 0.f, 0.f);
    }
    else
    {
        const float v = engine.GetMasterGain();
        if(v > .75f)
            SetPthLedFloat(9, v, 0.f, 0.f); /* red */
        else if(v > .5f)
            SetPthLedFloat(9, v, .6f * v, 0.f); /* orange */
        else
            SetPthLedFloat(9, 0.f, v, 0.f); /* green */
    }

    /* wheel-mode indicators on the PTH pair at the wheel column (Rev4
     * PCB: the PTH chain runs LED1..LED10 = indices 0..9; LED6 = PTH 5
     * at x=200.1 mm and LED7 = PTH 6 at x=220.2 mm flank the wheel SW5
     * at x=210.1 mm). Left LED purple = rotary selected, right LED pink
     * = vibrato selected; the selected mode's LED stays lit until the
     * next toggle, the other is off. Boots on rotary. */
    if(wheel_vibrato_mode_)
    {
        SetPthLedFloat(5, 0.f, 0.f, 0.f);
        SetPthLedFloat(6, 1.f, 0.f, .5f); /* pink */
    }
    else
    {
        SetPthLedFloat(5, .55f, 0.f, 1.f); /* purple */
        SetPthLedFloat(6, 0.f, 0.f, 0.f);
    }

    /* CHOMPI/SHIFT key LED: purple while SHIFT is held with no preset
     * mode active; blinking red once a slot is selected in SAVE_SEL,
     * ERASE_SEL or COPY_DEST; blinking white during the SAVING, COPYING
     * and ERASING animations; off otherwise (copy-source select has no
     * slot yet) */
    if(preset_mode_ == kPresetNone)
    {
        if(shift_held_)
            SetPthLedFloat(0, .67f, 0.f, 1.f);
        else
            SetPthLedFloat(0, 0.f, 0.f, 0.f);
    }
    else if(preset_mode_ == kPresetSaving || preset_mode_ == kPresetCopying
            || preset_mode_ == kPresetErasing)
    {
        const float w = pre_blink ? 1.f : 0.f;
        SetPthLedFloat(0, w, w, w);
    }
    else if(preset_sel_slot_ >= 0
            && (preset_mode_ == kPresetSaveSel
                || preset_mode_ == kPresetEraseSel
                || preset_mode_ == kPresetCopyDest))
    {
        SetPthLedFloat(0, pre_blink ? 1.f : 0.f, 0.f, 0.f);
    }
    else
    {
        SetPthLedFloat(0, 0.f, 0.f, 0.f);
    }

    /* latch indicator on the LOOP key LED (PTH 8): solid red while latched */
    SetPthLedFloat(8, latch_on_ ? 1.f : 0.f, 0.f, 0.f);

#ifdef ORGAN_CPU_METER
    /* B4: peak audio CPU load on PTH LED 0 (overrides shift indicator) */
    const float peak = cpu_meter.GetMaxCpuLoad();
    if(peak > .85f)
        SetPthLedFloat(0, 1.f, 0.f, 0.f);
    else if(peak > .70f)
        SetPthLedFloat(0, 1.f, .6f, 0.f);
    else
        SetPthLedFloat(0, 0.f, 1.f, 0.f);
#endif

    fill_led_data();
}

uint32_t ledt = 0, batt = 0;
static uint32_t midi_svc = 0;

void MainLoop(void *data)
{
    /* A4: refresh the IWDG only while the audio ISR keeps running - a
     * stuck ISR, a stuck main loop, or IRQs left disabled all reset the
     * device within ~2 s instead of hanging until a power cycle. */
    static uint32_t wd_audio_ticks = 0;
    if(audio_ticks != wd_audio_ticks)
    {
        wd_audio_ticks = audio_ticks;
        IWDG1->KR      = 0x0000AAAA;
    }

    /* staged, non-blocking preset SD writer: one small FatFS step per
     * pass, at thread level only (never in the audio ISR) */
    preset_storage_.Service(preset_bank_);

    /* drain queued MIDI note on/off messages (B2: notes only; CCs below) */
    while(!out_q.IsEmpty())
    {
        const OutMsg msg = out_q.PopFront();
        if(msg.kind == 0)
            midi.SendNoteOn(msg.a);
        else
            midi.SendNoteOff(msg.a);
    }

    /* B2: coalesced CCs - only the latest step per pipe goes out */
    for(int p = 0; p < OrganEngine::kNumPipes; p++)
    {
        if(cc_pipe_dirty_[p])
        {
            cc_pipe_dirty_[p] = false;
            midi.SendPipeCC(p, cc_pipe_step_[p]);
        }
        if(cc_character_dirty_[p])
        {
            cc_character_dirty_[p] = false;
            midi.SendCharacterCC(p, cc_character_step_[p]);
        }
    }

    /* A2: kick the DMA queue after draining so a message queued above
     * leaves in this same loop pass */
    midi.ProcessMidiOut();

    const uint32_t now = System::GetNow();

    /* the SAVING/COPYING/ERASING animations show for at least 1 s; a
     * removed SD card cancels any pending preset selection */
    if((preset_mode_ == kPresetSaving || preset_mode_ == kPresetCopying
        || preset_mode_ == kPresetErasing)
       && now - preset_anim_t_ >= 1000)
        preset_mode_ = kPresetNone;
    else if(preset_mode_ != kPresetNone && preset_storage_.NoSdCard())
        preset_mode_ = kPresetNone;

    if(now - ledt > 16)
    {
        ledt = now;
        UpdateLeds();
    }
    LedChainWatchdog();

    /* B1: service the MIDI-in listener (UART error recovery) */
    if(now - midi_svc > 50)
    {
        midi_svc = now;
        midi.ServiceMidiIn();
    }

#if !NO_BATT
    /* B6: 250 ms is plenty for a battery gauge; the I2C calls in here can
     * block the main loop, so don't run them at 50 Hz */
    if(now - batt > 250)
    {
        batt = now;
        hw.LowBatteryLockoutCheck();
    }
#endif

    System::DelayUs(10);
}

int main(void)
{
    hw.Init();
    midi.Init(&engine);
    midi.SetCcLevelCallback(CcLevelCallback);

    hw.MpWrite(0x0c, 0B01010001); /* set BATT_LOW to 3V */
    hw.MpReadAll();
    for(size_t i = 0; i < 10; i++)
    {
        hw.LowBatteryLockoutCheck();
        System::Delay(10);
    }

    engine.Init(hw.seed.AudioSampleRate(), plate_delay_mem);

    /* SD-card presets: mount, create ORGAN/ if missing, and read all 15
     * slots into the RAM cache before audio starts. With no card (or no
     * files) the cache stays empty and the organ runs on its factory
     * defaults; a cached preset is only applied when a slot is loaded. */
    preset_bank_.Init();
    preset_storage_.Init();
    preset_storage_.LoadAll(preset_bank_);

    LedSetup();

    /* Flush the shift registers for 0.5 s BEFORE starting audio;
     * KEY_26+27+28 held = shipping mode. The 4021 debounce counters start
     * at 0, which State() reads as "pressed", so polling controls before
     * they settle would fire bogus key/LOOP presses at boot. */
    uint32_t sleep_state = 0;
    for(int i = 0; i < 5000; i++)
    {
        hw.ProcessAllControls();
        sleep_state += hw.button_sr.State(int(Hardware::SwId::KEY_26))
                       && hw.button_sr.State(int(Hardware::SwId::KEY_27))
                       && hw.button_sr.State(int(Hardware::SwId::KEY_28));
        System::DelayUs(100);
    }
    if(sleep_state > 4000)
        hw.MpWrite(0x08, 0B10111111); /* SHIPPING MODE */

    /* Capture the settled control states as the edge baseline: nothing held
     * at power-on fires a note-on or toggles the latch, so the organ always
     * boots silent with the latch off. */
    for(int i = 0; i < 40; i++)
    {
        prev_key_[i]    = hw.button_sr.State(i);
        active_note_[i] = 0;
        latched_[i]     = false;
    }
    latch_on_ = false;
    for(int i = 0; i < 6; i++)
    {
        prev_enc_[i]     = hw.enc_trackers[i];
        last_click_t_[i] = 0;
    }
    for(int p = 0; p < OrganEngine::kNumPipes; p++)
    {
        pipe_page_[p]     = 0;
        mod_page_[p]      = 0; /* boot on mod page 1 (volume modulation) */
        prev_enc_sw_[p]   = hw.button_sr.State(p);
    }
    vol_page_            = kVolPageMaster;
    wheel_vibrato_mode_  = false; /* boot with the wheel on rotary */
    mod_mode_            = ModModeSwitchUp(); /* settled toggle baseline */
    prev_mod_mode_       = mod_mode_;

    hw.StartAudio(AudioCallback);

#ifdef ORGAN_CPU_METER
    cpu_meter.Init(hw.seed.AudioSampleRate(), 24); /* block size, hardware.h */
#endif

    /* A4: start the independent watchdog (~2 s): LSI ~32 kHz, /64 -> 500 Hz,
     * 1000 ticks ~ 2 s. Refreshed from MainLoop only while AudioCallback
     * keeps incrementing audio_ticks, so a stuck ISR, a stuck main loop or
     * IRQs-left-disabled all reset the device instead of hanging until a
     * power cycle. The bootloader is unaffected (IWDG is off until here). */
    IWDG1->KR  = 0x0000CCCC; /* start */
    IWDG1->KR  = 0x00005555; /* unlock PR/RLR */
    IWDG1->PR  = 4;          /* prescaler /64 */
    IWDG1->RLR = 1000;
    while(IWDG1->SR != 0) {} /* wait for PR/RLR to latch */
    IWDG1->KR  = 0x0000AAAA; /* first refresh */

    /* give USB control, enable AutoDPDM, take USB control (as chompi-wave) */
    hw.usb_sw.Write(false);
    System::Delay(1);
    hw.MpWrite(0x0a, 0B00100100);
    System::Delay(1);
    hw.usb_sw.Write(true);

    while(1)
    {
        MainLoop(nullptr);
    }
}

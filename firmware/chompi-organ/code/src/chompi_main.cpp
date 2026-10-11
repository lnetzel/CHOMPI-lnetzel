/** @file chompi_main.cpp
 *  @brief CHOMPI Organ Emulator firmware entry point.
 *
 *  Runs the spec in firmware/chompi-organ/chompi-organ- specification.md.
 *  Three execution contexts, like the other CHOMPI firmwares:
 *   1. AudioCallback() - audio ISR: MIDI in, control polling, audio render.
 *   2. MainLoop()      - MIDI out, SD auto-save, LEDs, battery checks.
 *   3. Boot            - SD mount + settings load before audio starts.
 */
#include "hardware.h"
#include "temp_led_stuff.h"
#include "daisysp.h"
#include "fatfs.h"
#include "OrganEngine.h"
#include "MidiHandler.h"
#include "SettingsManager.h"

using namespace daisy;
using namespace chompi;

Hardware        hw;
OrganEngine     engine;
OrganMidi       midi;
SettingsManager settings;

SdmmcHandler   sdmmc;
FatFSInterface fsi;

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
 *  wheel (ENC_5, unused) and enc[5] (ENC_6) is master volume. */
static const int pipe_encoder[OrganEngine::kNumPipes] = {3, 0, 1, 2};
static const int kVolEnc = 5; /* ENC_6 */

/** Per-pipe bar colors (teal, green, orange, red) */
static const float pipe_colors[OrganEngine::kNumPipes][3] = {
    {.14f, 1.f, .92f},
    {0.f, 1.f, 0.f},
    {1.f, .6f, .24f},
    {1.f, 0.f, 0.f},
};

/** SMT LED indices of the 15 white keys, left (C3) to right (C5). */
static constexpr int     kNumWhiteKeys = 15;
static const uint8_t white_key_led[kNumWhiteKeys] = {
    24, 23, 22, 21, 20, 19, 18, 17, 16, 15, 14, 13, 12, 11, 10
};

/* ---- cross-context (ISR <-> main loop) UI state ---- */
static volatile int      bar_pipe_  = -1;     /* pipe/volume bar showing */
static volatile uint32_t bar_until_ = 0;      /* ms timestamp for bar timeout */
static const int         kVolBar    = OrganEngine::kNumPipes; /* bar_pipe_: master volume */

/* ---- key/encoder edge tracking (audio ISR only) ---- */
static volatile bool prev_key_[40];
static uint8_t       active_note_[40];      /* note used at note-on, +1 (0 = none) */
static volatile int  octave_ = 0;           /* -2..+2, SHIFT+KEY_16/KEY_17 */
static int      prev_enc_[6];
static uint32_t last_click_t_[6];           /* chatter filter timestamps */
static bool     shift_held_ = false;

/** MIDI out goes through this queue so DMA transmits only happen in the
 *  main loop, never inside the audio ISR. */
struct OutMsg
{
    uint8_t kind; /* 0 = note on, 1 = note off, 2 = pipe CC */
    uint8_t a;    /* note or pipe index */
    uint8_t b;    /* step (CC only) */
};
static FIFO<OutMsg, 64> out_q;

static void CcLevelCallback(int pipe)
{
    bar_pipe_  = pipe;
    bar_until_ = System::GetNow() + 500;
}

/** Key + encoder polling, called from the audio ISR once per block (2 kHz). */
static void PollControls()
{
    /* shift key (CHOMPI key) */
    shift_held_ = hw.button_sr.State(static_cast<int>(Hardware::SwId::KEY_26));

    /* playable keys: SwId 7..31 (SHIFT+KEY_16/KEY_17 = octave down/up) */
    for(int swid = 7; swid < 32; swid++)
    {
        const bool down = hw.button_sr.State(swid);
        if(down == prev_key_[swid])
            continue;
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
            int n = key_map[swid] + 12 * octave_;
            n              = n < 0 ? 0 : (n > 127 ? 127 : n);
            const uint8_t note = static_cast<uint8_t>(n);
            active_note_[swid] = note + 1;
            engine.NoteOn(note, 1.f);
            out_q.PushBack({0, note, 0});
        }
        else if(active_note_[swid] != 0)
        {
            /* note-off uses the note that was started (octave may have
             * changed, or shift may now be held, while the key was down) */
            const uint8_t note   = active_note_[swid] - 1;
            active_note_[swid]   = 0;
            engine.NoteOff(note);
            out_q.PushBack({1, note, 0});
        }
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
                engine.AddFineTuneCents(delta); /* SHIFT+ENC_6: fine tune */
            }
            else
            {
                /* existing CHOMPI volume-knob behaviour: 100 clicks, 0..1 */
                float g = engine.GetMasterGain() + delta * .01f;
                engine.SetMasterGain(g);
                bar_pipe_  = kVolBar;
                bar_until_ = System::GetNow() + 500;
            }
        }
        else if(i != 4) /* enc[4] (transport wheel, ENC_5) has no function */
        {
            for(int p = 0; p < OrganEngine::kNumPipes; p++)
            {
                if(pipe_encoder[p] != i)
                    continue;
                engine.SetPipeStep(p, engine.GetPipeStep(p) + delta);
                out_q.PushBack({2, static_cast<uint8_t>(p),
                                static_cast<uint8_t>(engine.GetPipeStep(p))});
                settings.NotifyChanged();
                bar_pipe_  = p;
                bar_until_ = System::GetNow() + 500;
            }
        }
    }
}

void AudioCallback(AudioHandle::InputBuffer in,
                   AudioHandle::OutputBuffer out,
                   size_t size)
{
    midi.ProcessMidiIn();
    hw.ProcessAllControls();
    PollControls();
    engine.Prepare();
    engine.Process(out, size); /* audio input ignored */
}

/** Key LEDs: level bar while a pipe knob turns, else held notes. */
static void UpdateLeds()
{
    const uint32_t now = System::GetNow();

    if(bar_pipe_ >= 0 && now < bar_until_)
    {
        const int p = bar_pipe_;
        /* level bar spans the 15 white keys only: leftmost white = 0%,
         * rightmost white = 100%; pipes use their colors, volume is white */
        float r = 1.f, g = 1.f, b = 1.f;
        int   lit;
        if(p == kVolBar)
        {
            lit = static_cast<int>(engine.GetMasterGain() * kNumWhiteKeys
                                   + .5f);
        }
        else
        {
            lit = (engine.GetPipeStep(p) * kNumWhiteKeys + 16)
                  / OrganEngine::kMaxSteps;
            r = pipe_colors[p][0];
            g = pipe_colors[p][1];
            b = pipe_colors[p][2];
        }
        for(int i = 0; i < 25; i++)
            SetSmtLedFloat(i, 0.f, 0.f, 0.f);
        for(int w = 0; w < lit && w < kNumWhiteKeys; w++)
            SetSmtLedFloat(white_key_led[w], r, g, b);
    }
    else
    {
        bar_pipe_ = -1;
        for(int swid = 7; swid < 32; swid++)
        {
            /* physical key held (octave-shifted note) or MIDI-held note */
            const bool on = prev_key_[swid] || engine.IsNoteHeld(key_map[swid]);
            SetSmtLedFloat(led_map[swid], on ? 1.f : 0.f, on ? 1.f : 0.f,
                           on ? 1.f : 0.f);
        }
        /* SHIFT held: the two octave keys (C#3/D#3) are always lit dim
         * white so the control is discoverable; the active direction turns
         * orange (dim = 1 octave, bright = 2) */
        if(shift_held_)
        {
            const float dn = octave_ < 0 ? (octave_ < -1 ? 1.f : .45f) : 0.f;
            const float up = octave_ > 0 ? (octave_ > 1 ? 1.f : .45f) : 0.f;
            SetSmtLedFloat(0, dn > 0.f ? dn : .10f,
                           dn > 0.f ? .5f * dn : .10f, 0.f);
            SetSmtLedFloat(1, up > 0.f ? up : .10f,
                           up > 0.f ? .5f * up : .10f, 0.f);
        }
    }

    /* knob LEDs: pipe levels on PTH 1-4, master volume on PTH 9 */
    for(int p = 0; p < OrganEngine::kNumPipes; p++)
    {
        const float s = engine.GetPipeStep(p) * (1.f / OrganEngine::kMaxSteps);
        SetPthLedFloat(p + 1, s, s, s);
    }
    const float v = engine.GetMasterGain();
    SetPthLedFloat(9, v, v, v);

    /* shift indicator on the CHOMPI key LED */
    if(shift_held_)
        SetPthLedFloat(0, .67f, 0.f, 1.f);
    else
        SetPthLedFloat(0, 0.f, 0.f, 0.f);

    fill_led_data();
}

uint32_t ledt = 0, batt = 0;

void MainLoop(void *data)
{
    midi.ProcessMidiOut();

    /* drain queued MIDI out messages */
    while(!out_q.IsEmpty())
    {
        const OutMsg msg = out_q.PopFront();
        if(msg.kind == 0)
            midi.SendNoteOn(msg.a);
        else if(msg.kind == 1)
            midi.SendNoteOff(msg.a);
        else
            midi.SendPipeCC(msg.a, msg.b);
    }

    settings.Poll();

    const uint32_t now = System::GetNow();
    if(now - ledt > 16)
    {
        ledt = now;
        UpdateLeds();
    }
    LedChainWatchdog();

#if !NO_BATT
    if(now - batt > 20)
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

    /* SD card: mount, then load saved pipe levels (defaults if absent) */
    System::Delay(100);
    SdmmcHandler::Config sd_cfg;
    sd_cfg.speed = SdmmcHandler::Speed::FAST;
    sd_cfg.width = SdmmcHandler::BusWidth::BITS_4;
    sdmmc.Init(sd_cfg);
    System::Delay(100);
    fsi.Init(FatFSInterface::Config::MEDIA_SD);
    System::Delay(100);
    settings.Init(&engine);
    if(f_mount(&fsi.GetSDFileSystem(), fsi.GetSDPath(), 1) == FR_OK)
        settings.Load();

    engine.Init(hw.seed.AudioSampleRate());

    LedSetup();

    hw.StartAudio(AudioCallback);

    /* flush the shift registers for 0.5 s; KEY_26+27+28 held = shipping mode */
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

    for(int i = 0; i < 40; i++)
    {
        prev_key_[i]    = false;
        active_note_[i] = 0;
    }
    for(int i = 0; i < 6; i++)
        prev_enc_[i] = 0;

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

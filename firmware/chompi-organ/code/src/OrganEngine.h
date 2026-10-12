/** @file OrganEngine.h
 *  @brief Polyphonic organ voice engine: 6 voices x 4 pipe registrations.
 *
 *  Implements FR-1/FR-6 and DSP-1..DSP-3 of chompi-organ- specification.md:
 *  - Pipe ratios 1x / 2x / 2^(19/12)x / 4x with 20 kHz octave foldback
 *  - Per-pipe Character: tonewheel-inspired harmonic coloration, 64
 *    steps, one independent amount per pipe shared across all voices
 *    (TonewheelCharacter.h). Step 0 is the exact clean sine; step 64 is
 *    clearly aged but still organ-like. One sine oscillator per pipe;
 *    the 2nd/3rd harmonics are derived algebraically from the sine
 *    sample, smoothly eased in and anti-alias-masked per folded note.
 *  - Free-running oscillators (phase never reset at note-on)
 *  - Fixed 5 ms linear attack/release gate envelope; 5 ms steal fade
 *  - Oldest-first voice stealing; same-note retrigger reuses its voice
 *  - Fixed 1/24 output scale so 6 voices x 4 pipes cannot clip
 *  - Pipe levels in 32 steps, gain = (step/32)^2, smoothed per sample
 *  - Overdrive: 32-step overdrive-pedal stage on the mix (pre-gain into a
 *    tanh soft clip with level-anchored post gain), placed before the
 *    master gain so the drive character is level-independent; 0% is an
 *    exact bypass (DSP-5)
 *  - Rotary speaker: 60s Leslie-style mono->stereo stage after the
 *    overdrive (800 Hz crossover, Doppler horn + AM drum, two virtual
 *    mics 180 deg apart); 0% is an exact bypass (DSP-6)
 *  - Plate reverb: compact vintage-1960s-inspired plate after the
 *    rotary (PlateReverb.h; Griesinger/Dattorro tank adapted from
 *    chompi-wave's reverb). Linear complementary dry/wet crossfade with
 *    a ~10 ms ramp: 0% mix is bit-exact dry, 100% is wet only. Decay is
 *    adjustable 0.5-20 s in 100 log-spaced steps (boots at 0.5 s). The
 *    tank runs continuously (live tails, no stale state); the 64 KiB
 *    delay buffer is caller-owned SDRAM passed to Init().
 *  - Smoothed master gain (existing CHOMPI volume-knob behaviour)
 */
#pragma once
#include "daisysp.h"
#include "RotarySpeaker.h"
#include "TonewheelCharacter.h"
#include "PlateReverb.h"

namespace chompi
{

class OrganEngine
{
  public:
    static constexpr int kNumVoices         = 6;
    static constexpr int kNumPipes          = 4;
    static constexpr int kMaxSteps          = 32;
    static constexpr int kMaxCharacterSteps = 64;

    /** Pipe frequency ratios: 8', 4', 2 2/3' (equal-tempered), 2' */
    static float PipeRatio(int p)
    {
        switch(p)
        {
            case 1: return 2.f;
            case 2: return 2.9966142f; /* 2^(19/12) */
            case 3: return 4.f;
            default: return 1.f;
        }
    }
    static constexpr float kFoldbackHz     = 20000.f;
    static constexpr float kMixScale       = 1.f / 24.f;
    static constexpr int   kDefaultStep          = 16; /* 50% volume */
    static constexpr int   kDefaultCharacterStep = 0;  /* clean sine */
    static constexpr int   kDefaultDriveStep     = 0;  /* overdrive off */
    static constexpr int   kDefaultRotaryStep    = 0;  /* rotary off */
    static constexpr int   kDefaultReverbMixPercent = 0; /* reverb dry */
    static constexpr int   kDefaultReverbDecayStep  = 0; /* 0.5 s decay */
    static constexpr int   kMaxReverbDecaySteps     = 100;
    static constexpr float kMinReverbDecaySeconds   = .5f;
    static constexpr float kMaxReverbDecaySeconds   = 20.f;

    /** `reverb_delay` must point to PlateReverb::kDelayBufferSize
     *  uint16_t elements in writable memory (ORGAN uses SDRAM); pass it
     *  only after hw.Init() has configured the SDRAM controller. */
    void Init(float sample_rate, uint16_t *reverb_delay)
    {
        sample_rate_ = sample_rate;
        env_inc_     = 1.f / (0.005f * sample_rate); /* 5 ms attack/release/steal */
        age_counter_ = 0;
        fine_cents_  = 0;
        freqs_dirty_ = false;
        character_.Init(sample_rate);

        for(int v = 0; v < kNumVoices; v++)
        {
            Voice &voice = voices_[v];
            voice.state        = IDLE;
            voice.note         = 0;
            voice.pending_note = 0;
            voice.pending_vel  = 0.f;
            voice.velocity     = 0.f;
            voice.env          = 0.f;
            voice.age          = 0;
            for(int p = 0; p < kNumPipes; p++)
            {
                voice.osc[p].Init(sample_rate);
                voice.osc[p].SetAmp(1.f);
                voice.osc[p].SetFreq(100.f);
                voice.osc[p].SetWaveform(daisysp::Oscillator::WAVE_SIN);
            }
        }

        for(int p = 0; p < kNumPipes; p++)
        {
            pipe_steps[p] = kDefaultStep;
            pipe_gain_[p] = pipe_gain_target_[p] = StepToGain(kDefaultStep);
        }

        master_gain_ = master_gain_target_ = kDefaultMasterGain;

        drive_step_ = kDefaultDriveStep;
        drive_ = drive_target_
            = static_cast<float>(kDefaultDriveStep) * (1.f / kMaxSteps);
        drive_g_    = 1.f;
        drive_post_ = 0.33f / tanhf(0.33f);

        rotary_.Init(sample_rate);
        rotary_step_ = kDefaultRotaryStep;

        plate_.Init(sample_rate, reverb_delay);
        reverb_mix_percent_ = kDefaultReverbMixPercent;
        reverb_decay_step_  = kDefaultReverbDecayStep; /* plate_.Init()
                                                        * already set 0.5 s */

        for(int n = 0; n < 128; n++)
            note_held_[n] = false;
    }

    /** Apply one encoder click (or MIDI CC step) to a pipe level. */
    void SetPipeStep(int pipe, int step)
    {
        if(pipe < 0 || pipe >= kNumPipes)
            return;
        step = step < 0 ? 0 : (step > kMaxSteps ? kMaxSteps : step);
        pipe_steps[pipe]            = static_cast<uint8_t>(step);
        pipe_gain_target_[pipe]     = StepToGain(step);
    }

    int GetPipeStep(int pipe) const { return pipe_steps[pipe]; }

    /** Apply one encoder click (or MIDI CC step) to a pipe's Character
 *      amount. Step 0 = exact clean sine, 64 = fully aged (tonewheel-
 *      inspired harmonic color, not square). Ramped ~10 ms, click-free. */
    void SetPipeCharacterStep(int pipe, int step)
    {
        character_.SetStep(pipe, step);
    }

    int GetPipeCharacterStep(int pipe) const
    {
        return character_.GetStep(pipe);
    }

    /** Apply one encoder click to the overdrive amount (0..kMaxSteps).
 *      Step 0 is an exact bypass; higher steps increase the pre-gain. */
    void SetDriveStep(int step)
    {
        step = step < 0 ? 0 : (step > kMaxSteps ? kMaxSteps : step);
        drive_step_   = static_cast<uint8_t>(step);
        drive_target_ = static_cast<float>(step) * (1.f / kMaxSteps);
    }

    int GetDriveStep() const { return drive_step_; }

    /** Apply one encoder click to the rotary-speaker amount (0..kMaxSteps).
 *      Step 0 is an exact bypass; higher steps deepen the modulation and
 *      spin the virtual rotors faster (DSP-6). */
    void SetRotaryStep(int step)
    {
        step = step < 0 ? 0 : (step > kMaxSteps ? kMaxSteps : step);
        rotary_step_ = static_cast<uint8_t>(step);
        rotary_.SetAmount(static_cast<float>(step) * (1.f / kMaxSteps));
    }

    int GetRotaryStep() const { return rotary_step_; }

    /** Apply one encoder click to the plate-reverb mix (0..100 percent,
 *      clamped). 0% is bit-exact dry, 100% is wet only; the ~10 ms
 *      crossfade ramp retargets from the current mix, click-free. */
    void SetReverbMixPercent(int percent)
    {
        percent = percent < 0 ? 0 : (percent > 100 ? 100 : percent);
        reverb_mix_percent_ = static_cast<uint8_t>(percent);
        plate_.SetMix(static_cast<float>(percent) * .01f);
    }

    int GetReverbMixPercent() const { return reverb_mix_percent_; }

    /** Apply one encoder click to the plate-reverb decay (0..100 steps,
 *      clamped), mapped exponentially from 0.5 s (step 0, boot default)
 *      to 20 s (step 100): seconds = 0.5 * 40^(step/100). Log spacing
 *      keeps the short end usable - a linear seconds-per-click mapping
 *      would make the first clicks huge relative jumps. */
    void SetReverbDecayStep(int step)
    {
        step = step < 0
                   ? 0
                   : (step > kMaxReverbDecaySteps ? kMaxReverbDecaySteps
                                                  : step);
        reverb_decay_step_ = static_cast<uint8_t>(step);
        plate_.SetDecaySeconds(
            kMinReverbDecaySeconds
            * powf(kMaxReverbDecaySeconds / kMinReverbDecaySeconds,
                   static_cast<float>(step) * (1.f / kMaxReverbDecaySteps)));
    }

    int GetReverbDecayStep() const { return reverb_decay_step_; }

    void SetMasterGain(float g)
    {
        master_gain_target_ = g < 0.f ? 0.f : (g > 1.f ? 1.f : g);
    }

    float GetMasterGain() const { return master_gain_target_; }

    void AddFineTuneCents(int delta)
    {
        int c = fine_cents_ + delta;
        c = c < -50 ? -50 : (c > 50 ? 50 : c);
        if(c != fine_cents_)
        {
            fine_cents_  = c;
            freqs_dirty_ = true;
        }
    }

    int GetFineTuneCents() const { return fine_cents_; }

    void NoteOn(uint8_t note, float velocity)
    {
        note_held_[note & 0x7f] = true;

        /* Retrigger: reuse a voice already playing/releasing this note.
         * C1: a voice mid-STEAL whose pending note matches is also a
         * retrigger (it starts this note when its fade ends) - without
         * this, two voices could end up on the same note. */
        for(int v = 0; v < kNumVoices; v++)
        {
            Voice &voice = voices_[v];
            if((voice.state == ATTACK || voice.state == SUSTAIN
                || voice.state == RELEASE)
               && voice.note == note)
            {
                StartVoice(v, note, velocity);
                return;
            }
            if(voice.state == STEAL && voice.pending_note == note)
            {
                voice.pending_vel = velocity;
                return;
            }
        }

        /* Free voice */
        for(int v = 0; v < kNumVoices; v++)
        {
            if(voices_[v].state == IDLE)
            {
                StartVoice(v, note, velocity);
                return;
            }
        }

        /* Steal the oldest voice (5 ms fade, then the new note starts).
         * C1: prefer voices not already mid-STEAL - stealing one would
         * silently drop its pending note (that key stays held but
         * sounds nothing). */
        int oldest = -1;
        for(int v = 0; v < kNumVoices; v++)
        {
            if(voices_[v].state != STEAL
               && (oldest < 0 || voices_[v].age < voices_[oldest].age))
                oldest = v;
        }
        if(oldest < 0) /* all six mid-steal: fall back to oldest overall */
        {
            for(int v = 0; v < kNumVoices; v++)
            {
                if(oldest < 0 || voices_[v].age < voices_[oldest].age)
                    oldest = v;
            }
        }
        Voice &voice      = voices_[oldest];
        voice.pending_note = note;
        voice.pending_vel  = velocity;
        voice.state        = STEAL;
    }

    void NoteOff(uint8_t note)
    {
        note &= 0x7f;
        note_held_[note] = false;

        for(int v = 0; v < kNumVoices; v++)
        {
            Voice &voice = voices_[v];
            if((voice.state == ATTACK || voice.state == SUSTAIN)
               && voice.note == note)
            {
                voice.state = RELEASE;
            }
            else if(voice.state == STEAL && voice.pending_note == note)
            {
                /* stolen voice's pending note was cancelled: just fade out */
                voice.state = RELEASE;
            }
        }
    }

    bool IsNoteHeld(uint8_t note) const { return note_held_[note & 0x7f]; }

    /** Called once per audio block, before Process(). */
    void Prepare()
    {
        /* B4: overdrive pre/post gain only depend on the drive target, so
         * compute them once per block instead of a tanhf + divide per
         * sample. drive_ still slews per sample for a click-free bypass. */
        drive_g_    = 1.f + drive_target_ * drive_target_ * 63.f;
        drive_post_ = 0.33f / tanhf(0.33f * drive_g_);

        if(freqs_dirty_)
        {
            freqs_dirty_ = false;
            for(int v = 0; v < kNumVoices; v++)
            {
                if(voices_[v].state != IDLE && voices_[v].state != STEAL)
                    UpdateFreqs(v);
            }
        }
    }

    void Process(float **out, size_t size)
    {
        for(size_t i = 0; i < size; i++)
        {
            /* Smooth pipe gains and master gain (~1 ms slew). Advance the
             * four Character amounts once per sample (not once per voice),
             * even while voices are idle, so the ramps always settle. */
            float ca[kNumPipes];
            character_.Advance();
            for(int p = 0; p < kNumPipes; p++)
            {
                daisysp::fonepole(pipe_gain_[p], pipe_gain_target_[p], .001f);
                ca[p] = character_.EasedAmount(p);
            }
            daisysp::fonepole(master_gain_, master_gain_target_, .001f);
            daisysp::fonepole(drive_, drive_target_, .001f);

            float mix = 0.f;

            for(int v = 0; v < kNumVoices; v++)
            {
                Voice &voice = voices_[v];
                if(voice.state == IDLE)
                    continue;

                float sig = 0.f;
                for(int p = 0; p < kNumPipes; p++)
                {
                    /* One sine per pipe. Character shapes the raw sine
                     * sample BEFORE pipe gain/envelope/velocity, so muted
                     * pipes and completed envelopes stay silent; settled
                     * Character zero returns the sine exactly. */
                    float psig = voice.osc[p].Process();
                    if(ca[p] > 0.f)
                        psig = TonewheelCharacter::ShapeSample(
                            psig, character_.Coeffs(v, p), ca[p]);
                    sig += psig * pipe_gain_[p];
                }

                switch(voice.state)
                {
                    case ATTACK:
                        voice.env += env_inc_;
                        if(voice.env >= 1.f)
                        {
                            voice.env   = 1.f;
                            voice.state = SUSTAIN;
                        }
                        break;
                    case SUSTAIN: break;
                    case RELEASE:
                        voice.env -= env_inc_;
                        if(voice.env <= 0.f)
                        {
                            voice.env   = 0.f;
                            voice.state = IDLE;
                        }
                        break;
                    case STEAL:
                        voice.env -= env_inc_;
                        if(voice.env <= 0.f)
                        {
                            voice.env = 0.f;
                            StartVoice(v, voice.pending_note, voice.pending_vel);
                        }
                        break;
                    default: break;
                }

                mix += sig * voice.env * voice.velocity;
            }

            /* Overdrive (DSP-5): overdrive-pedal style — quadratic-mapped
             * pre-gain (1..64x) into a tanh soft clipper, fully wet (no dry
             * blend, so harmonics are always audible above 0%). Post gain
             * anchors a 1/3-full-scale input to the same output level, so
             * turning the knob up adds crunch/compression instead of just
             * loudness; step 0 is an exact bypass. Runs before the master
             * gain so the drive character does not depend on the listening
             * level. */
            float out_sig = mix * kMixScale;
            if(drive_ > 0.f)
            {
                /* B4: drive_g_/drive_post_ come from Prepare() (per block) */
                out_sig = tanhf(drive_g_ * out_sig) * drive_post_;
            }
            /* Rotary speaker (DSP-6): mono mix -> stereo Leslie, placed
             * after the overdrive (the drive is the preamp, the Leslie is
             * the speaker cabinet) and before the master gain so the
             * volume knob keeps scaling both channels equally. Step 0 is
             * an exact bypass (both channels = out_sig). */
            float out_l, out_r;
            rotary_.Process(out_sig, out_l, out_r);
            /* Plate reverb: stereo in/out between the rotary (speaker
             * cabinet) and the master gain, so the volume knob scales
             * dry and wet equally and 0% master mutes the tail. The tank
             * runs even with idle voices; 0% mix is bit-exact dry. */
            plate_.Process(out_l, out_r, out_l, out_r);
            out_l *= master_gain_;
            out_r *= master_gain_;
            out[0][i] = out_l; /* headphone L */
            out[1][i] = out_r; /* headphone R */
            out[2][i] = out_l; /* master L */
            out[3][i] = out_r; /* master R */
        }
    }

  private:
    enum VoiceState : uint8_t
    {
        IDLE,
        ATTACK,
        SUSTAIN,
        RELEASE,
        STEAL
    };

    struct Voice
    {
        daisysp::Oscillator osc[kNumPipes]; /* one sine per pipe */
        VoiceState          state;
        uint8_t             note;
        uint8_t             pending_note;
        float               pending_vel;
        float               velocity;
        float               env;
        uint32_t            age;
    };

    static float StepToGain(int step)
    {
        const float x = static_cast<float>(step) / kMaxSteps;
        return x * x;
    }

    float NoteToHz(uint8_t note) const
    {
        return 440.f
               * powf(2.f,
                      (static_cast<float>(note) - 69.f
                       + static_cast<float>(fine_cents_) * .01f)
                          / 12.f);
    }

    void UpdateFreqs(int v)
    {
        Voice      &voice = voices_[v];
        const float base  = NoteToHz(voice.note);
        for(int p = 0; p < kNumPipes; p++)
        {
            float f = base * PipeRatio(p);
            while(f > kFoldbackHz)
                f *= .5f; /* organ foldback: drop whole octaves */
            voice.osc[p].SetFreq(f);
            /* Character voicing + anti-alias masks track the FINAL folded
             * frequency (and fine tuning); identical for every voice slot
             * playing this note/pipe. */
            character_.UpdateVoiceCoeffs(v, voice.note, p, f);
        }
    }

    void StartVoice(int v, uint8_t note, float velocity)
    {
        Voice &voice   = voices_[v];
        voice.note     = note;
        voice.velocity = velocity;
        voice.age      = ++age_counter_;
        voice.state    = ATTACK;
        UpdateFreqs(v);
    }

    static constexpr float kDefaultMasterGain = .25f; /* 25% at boot */

    float    sample_rate_;
    float    env_inc_;
    Voice    voices_[kNumVoices];
    uint32_t age_counter_;
    int      fine_cents_;
    bool     freqs_dirty_;
    float    pipe_gain_[kNumPipes];
    float    pipe_gain_target_[kNumPipes];
    TonewheelCharacter character_;
    float    master_gain_;
    float    master_gain_target_;
    float    drive_;
    float    drive_target_;
    float    drive_g_, drive_post_; /* per-block overdrive gains (B4) */
    uint8_t  drive_step_;
    RotarySpeaker rotary_;
    uint8_t  rotary_step_;
    PlateReverb plate_;
    uint8_t  reverb_mix_percent_;
    uint8_t  reverb_decay_step_;
    bool     note_held_[128];

  public:
    uint8_t pipe_steps[kNumPipes];
};

} // namespace chompi

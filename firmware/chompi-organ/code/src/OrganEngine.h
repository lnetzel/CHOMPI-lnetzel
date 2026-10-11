/** @file OrganEngine.h
 *  @brief Polyphonic organ voice engine: 6 voices x 4 sine pipe registrations.
 *
 *  Implements FR-1/FR-6 and DSP-1..DSP-3 of chompi-organ- specification.md:
 *  - Pipe ratios 1x / 2x / 2^(19/12)x / 4x with 20 kHz octave foldback
 *  - Free-running oscillators (phase never reset at note-on)
 *  - Fixed 5 ms linear attack/release gate envelope; 5 ms steal fade
 *  - Oldest-first voice stealing; same-note retrigger reuses its voice
 *  - Fixed 1/24 output scale so 6 voices x 4 pipes cannot clip
 *  - Pipe levels in 32 steps, gain = (step/32)^2, smoothed per sample
 *  - Smoothed master gain (existing CHOMPI volume-knob behaviour)
 */
#pragma once
#include "daisysp.h"

namespace chompi
{

class OrganEngine
{
  public:
    static constexpr int kNumVoices = 6;
    static constexpr int kNumPipes  = 4;
    static constexpr int kMaxSteps  = 32;

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
    static constexpr float kFoldbackHz  = 20000.f;
    static constexpr float kMixScale    = 1.f / 24.f;
    static constexpr int   kDefaultStep = 24;

    void Init(float sample_rate)
    {
        sample_rate_ = sample_rate;
        env_inc_     = 1.f / (0.005f * sample_rate); /* 5 ms attack/release/steal */
        age_counter_ = 0;
        fine_cents_  = 0;
        freqs_dirty_ = false;

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
                voice.osc[p].SetWaveform(daisysp::Oscillator::WAVE_SIN);
                voice.osc[p].SetAmp(1.f);
                voice.osc[p].SetFreq(100.f);
            }
        }

        for(int p = 0; p < kNumPipes; p++)
        {
            pipe_steps[p] = kDefaultStep;
            pipe_gain_[p] = pipe_gain_target_[p] = StepToGain(kDefaultStep);
        }

        master_gain_ = master_gain_target_ = kDefaultMasterGain;

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

        /* Retrigger: reuse a voice already playing/releasing this note */
        for(int v = 0; v < kNumVoices; v++)
        {
            Voice &voice = voices_[v];
            if((voice.state == ATTACK || voice.state == SUSTAIN
                || voice.state == RELEASE)
               && voice.note == note)
            {
                StartVoice(voice, note, velocity);
                return;
            }
        }

        /* Free voice */
        for(int v = 0; v < kNumVoices; v++)
        {
            if(voices_[v].state == IDLE)
            {
                StartVoice(voices_[v], note, velocity);
                return;
            }
        }

        /* Steal the oldest voice (5 ms fade, then the new note starts) */
        int oldest = 0;
        for(int v = 1; v < kNumVoices; v++)
        {
            if(voices_[v].age < voices_[oldest].age)
                oldest = v;
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
        if(freqs_dirty_)
        {
            freqs_dirty_ = false;
            for(int v = 0; v < kNumVoices; v++)
            {
                if(voices_[v].state != IDLE && voices_[v].state != STEAL)
                    UpdateFreqs(voices_[v]);
            }
        }
    }

    void Process(float **out, size_t size)
    {
        for(size_t i = 0; i < size; i++)
        {
            /* Smooth pipe gains and master gain (~1 ms slew, no zipper noise) */
            for(int p = 0; p < kNumPipes; p++)
                daisysp::fonepole(pipe_gain_[p], pipe_gain_target_[p], .001f);
            daisysp::fonepole(master_gain_, master_gain_target_, .001f);

            float mix = 0.f;

            for(int v = 0; v < kNumVoices; v++)
            {
                Voice &voice = voices_[v];
                if(voice.state == IDLE)
                    continue;

                float sig = 0.f;
                for(int p = 0; p < kNumPipes; p++)
                    sig += voice.osc[p].Process() * pipe_gain_[p];

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
                            StartVoice(voice, voice.pending_note, voice.pending_vel);
                        }
                        break;
                    default: break;
                }

                mix += sig * voice.env * voice.velocity;
            }

            const float out_sig = mix * kMixScale * master_gain_;
            out[0][i] = out_sig; /* headphone L */
            out[1][i] = out_sig; /* headphone R */
            out[2][i] = out_sig; /* master L */
            out[3][i] = out_sig; /* master R */
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
        daisysp::Oscillator osc[kNumPipes];
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

    void UpdateFreqs(Voice &voice)
    {
        const float base = NoteToHz(voice.note);
        for(int p = 0; p < kNumPipes; p++)
        {
            float f = base * PipeRatio(p);
            while(f > kFoldbackHz)
                f *= .5f; /* organ foldback: drop whole octaves */
            voice.osc[p].SetFreq(f);
        }
    }

    void StartVoice(Voice &voice, uint8_t note, float velocity)
    {
        voice.note     = note;
        voice.velocity = velocity;
        voice.age      = ++age_counter_;
        voice.state    = ATTACK;
        UpdateFreqs(voice);
    }

    static constexpr float kDefaultMasterGain = .84f;

    float    sample_rate_;
    float    env_inc_;
    Voice    voices_[kNumVoices];
    uint32_t age_counter_;
    int      fine_cents_;
    bool     freqs_dirty_;
    float    pipe_gain_[kNumPipes];
    float    pipe_gain_target_[kNumPipes];
    float    master_gain_;
    float    master_gain_target_;
    bool     note_held_[128];

  public:
    uint8_t pipe_steps[kNumPipes];
};

} // namespace chompi

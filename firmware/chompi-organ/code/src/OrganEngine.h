/** @file OrganEngine.h
 *  @brief Polyphonic organ voice engine: 6 voices x 4 pipe registrations.
 *
 *  Implements FR-1/FR-6 and DSP-1..DSP-3 of chompi-organ- specification.md:
 *  - Pipe ratios 1x / 2x / 2^(19/12)x / 4x with 20 kHz octave foldback
 *  - Per-pipe waveform morph: sine -> square, 64 steps, smoothstep-eased
 *    crossfade of two oscillators per pipe (polyBLEP square keeps the
 *    high registrations alias-free); the easing keeps the blend
 *    lingering near the pure waveforms, so the first clicks stay in
 *    sine territory
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
 *  - Smoothed master gain (existing CHOMPI volume-knob behaviour)
 */
#pragma once
#include "daisysp.h"
#include "RotarySpeaker.h"

namespace chompi
{

class OrganEngine
{
  public:
    static constexpr int kNumVoices    = 6;
    static constexpr int kNumPipes     = 4;
    static constexpr int kMaxSteps     = 32;
    static constexpr int kMaxWaveSteps = 64;
    static constexpr int kNumWaveforms = 2;

    /** Oscillator slot order within each pipe's morph chain. */
    enum Waveform : uint8_t
    {
        kWaveSin = 0,
        kWaveSqr
    };

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
    static constexpr int   kDefaultStep       = 16; /* 50% volume */
    static constexpr int   kDefaultWaveStep   = 0; /* sine */
    static constexpr int   kDefaultDriveStep  = 0; /* overdrive off */
    static constexpr int   kDefaultRotaryStep = 0; /* rotary off */

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
                for(int w = 0; w < kNumWaveforms; w++)
                {
                    voice.osc[p][w].Init(sample_rate);
                    voice.osc[p][w].SetAmp(1.f);
                    voice.osc[p][w].SetFreq(100.f);
                }
                voice.osc[p][kWaveSin].SetWaveform(daisysp::Oscillator::WAVE_SIN);
                voice.osc[p][kWaveSqr].SetWaveform(daisysp::Oscillator::WAVE_POLYBLEP_SQUARE);
            }
        }

        for(int p = 0; p < kNumPipes; p++)
        {
            pipe_steps[p]      = kDefaultStep;
            pipe_gain_[p]      = pipe_gain_target_[p] = StepToGain(kDefaultStep);
            pipe_wave_steps[p] = kDefaultWaveStep;
            pipe_wave_[p]      = pipe_wave_target_[p]
                = StepToMorph(kDefaultWaveStep);
        }

        master_gain_ = master_gain_target_ = kDefaultMasterGain;

        drive_step_ = kDefaultDriveStep;
        drive_ = drive_target_
            = static_cast<float>(kDefaultDriveStep) * (1.f / kMaxSteps);

        rotary_.Init(sample_rate);
        rotary_step_ = kDefaultRotaryStep;

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

    /** Apply one encoder click (or MIDI CC step) to a pipe's waveform morph.
 *      Step 0 = sine, 64 = square; in-between steps crossfade.
 */
    void SetPipeWaveStep(int pipe, int step)
    {
        if(pipe < 0 || pipe >= kNumPipes)
            return;
        step = step < 0 ? 0 : (step > kMaxWaveSteps ? kMaxWaveSteps : step);
        pipe_wave_steps[pipe]    = static_cast<uint8_t>(step);
        pipe_wave_target_[pipe]  = StepToMorph(step);
    }

    int GetPipeWaveStep(int pipe) const { return pipe_wave_steps[pipe]; }

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
            /* Smooth pipe gains, morphs and master gain (~1 ms slew) */
            float wv[kNumPipes][kNumWaveforms];
            for(int p = 0; p < kNumPipes; p++)
            {
                daisysp::fonepole(pipe_gain_[p], pipe_gain_target_[p], .001f);
                daisysp::fonepole(pipe_wave_[p], pipe_wave_target_[p], .001f);

                /* Morph weights over morph x in 0..1: sine fades out as
                 * square fades in. The coordinate runs through MorphEase
                 * (smoothstep) first, so the blend lingers near the pure
                 * waveforms: the first clicks stay in sine territory and
                 * each pipe keeps more playable sweet spots. The weights
                 * sum to 1, so loudness stays flat across the sweep. */
                const float ease = MorphEase(pipe_wave_[p]);
                wv[p][kWaveSin] = 1.f - ease;
                wv[p][kWaveSqr] = ease;
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
                    float psig = 0.f;
                    for(int w = 0; w < kNumWaveforms; w++)
                    {
                        /* Skip silent morph legs (phase stalls while the
                         * weight is 0, which is inaudible behind the
                         * crossfade) to keep CPU near the sine-only budget */
                        if(wv[p][w] > 0.f)
                            psig += voice.osc[p][w].Process() * wv[p][w];
                    }
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
                            StartVoice(voice, voice.pending_note, voice.pending_vel);
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
                const float g    = 1.f + drive_ * drive_ * 63.f;
                const float post = 0.33f / tanhf(0.33f * g);
                out_sig          = tanhf(g * out_sig) * post;
            }
            /* Rotary speaker (DSP-6): mono mix -> stereo Leslie, placed
             * after the overdrive (the drive is the preamp, the Leslie is
             * the speaker cabinet) and before the master gain so the
             * volume knob keeps scaling both channels equally. Step 0 is
             * an exact bypass (both channels = out_sig). */
            float out_l, out_r;
            rotary_.Process(out_sig, out_l, out_r);
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
        daisysp::Oscillator osc[kNumPipes][kNumWaveforms];
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

    static float StepToMorph(int step)
    {
        return static_cast<float>(step) / kMaxWaveSteps; /* 0..1 */
    }

    /** Easing applied across the sine->square morph. Smoothstep: flat
     *  ends make the blend linger near the pure waveforms. For an even
     *  longer sine-land use t*t; for less easing, blend toward linear:
     *  .5f*(t + t*t*(3.f - 2.f*t)). */
    static float MorphEase(float t)
    {
        return t * t * (3.f - 2.f * t);
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
            for(int w = 0; w < kNumWaveforms; w++)
                voice.osc[p][w].SetFreq(f);
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

    static constexpr float kDefaultMasterGain = .25f; /* 25% at boot */

    float    sample_rate_;
    float    env_inc_;
    Voice    voices_[kNumVoices];
    uint32_t age_counter_;
    int      fine_cents_;
    bool     freqs_dirty_;
    float    pipe_gain_[kNumPipes];
    float    pipe_gain_target_[kNumPipes];
    float    pipe_wave_[kNumPipes];
    float    pipe_wave_target_[kNumPipes];
    float    master_gain_;
    float    master_gain_target_;
    float    drive_;
    float    drive_target_;
    uint8_t  drive_step_;
    RotarySpeaker rotary_;
    uint8_t  rotary_step_;
    bool     note_held_[128];

  public:
    uint8_t pipe_steps[kNumPipes];
    uint8_t pipe_wave_steps[kNumPipes];
};

} // namespace chompi

/** DSPEngine
 *  Core DSP for sampling engine, looping engine, and additional DSP
 */
#pragma once
#include "Sampler.h"
#include "RamBuffer.h"
using namespace daisy;

static const uint32_t kRecordClearTimeout = 2000;
static const uint32_t kButtonTimeout = 10;
namespace daisy
{

    /** @brief Logic for main looper engine */
    class LooperEngine
    {
    public:
        LooperEngine() {}
        ~LooperEngine() {}

        void Init(float sr, RamBufferMemory* loop_buff, bool tape_slew,
                  int16_t* undo_audio = nullptr, uint32_t* undo_tags = nullptr)
        {
            looper.Init(sr, loop_buff, tape_slew, undo_audio, undo_tags);

            record = false;
            first_record = true;
            record_target = false;
            record_arm = false;
            playing = false;

            undo_request_pending_ = false;
            undo_commit_requested_ = false;
            undo_request_generation_ = 0;
            undo_fade_ = 1.f;

            char name_buffer[32];
            sprintf(name_buffer, "TAPE/looper.wav");
            looper.Reset();

            fx_env_ = fx_env_target_ = 1.f;
            playback_gain_ = playback_gain_target_ = 1.f;
        }

        void FXEnvelope()
        {
            if(IsFirstRecording() && !IsRecording() && !looper.IsResetting())
                return;

            fx_env_target_ = 0.f;
        }

        void Process(float* out_l, float* out_r, size_t size)
        {
            ServiceUndoRequests(); // block-boundary undo request/commit

            if(IsFirstRecording() && !IsRecording() && !looper.IsResetting())
                return;

            // looper.PerBlock(playing);

            if(fx_env_ < .01f)
                fx_env_target_ = 1.f;

            const bool undo_fading = looper.GetUndoState() == LooperUndoState::FadeOutForUndo;
            const bool undo_suspended = looper.GetUndoState() == LooperUndoState::RestoreSuspended;

            for(size_t i = 0; i < size; i++)
            {
                daisysp::fonepole(fx_env_, fx_env_target_, .001f);
                daisysp::fonepole(playback_gain_, playback_gain_target_, .001f);

                if(!first_record || looper.IsResetting())
                {
                    int16_t aol = 0;
                    int16_t aor = 0;

                    // no loop DSP reads/writes while suspended for restore
                    if(!undo_suspended)
                        looper.PopStereoSamps(f2s16(out_l[i] * fx_env_), f2s16(out_r[i] * fx_env_), &aol, &aor, record, playing);

                    float loop_gain = fx_env_ * playback_gain_;
                    if(undo_fading) // fade only the looper contribution
                    {
                        loop_gain *= undo_fade_;
                        undo_fade_ = undo_fade_ > kUndoFadeDec ? undo_fade_ - kUndoFadeDec : 0.f;
                    }

                    out_l[i] += s162f(aol) * loop_gain;
                    out_r[i] += s162f(aor) * loop_gain;
                }
                else if(!reset)
                {
                    looper.PushStereoSamps(f2s16(out_l[i] * fx_env_), f2s16(out_r[i] * fx_env_));
                }

                if(looper.WriteFullLength())
                {
                    ToggleRecord(); // overdub mode
                    ToggleRecord(); // play mode
                }
            }

            if(undo_fading && undo_fade_ <= 0.f)
                looper.SuspendUndoDsp(); // -> RestoreSuspended
        }

        // ====================  last-overdub undo  ====================

        /** UI/foreground posts an undo request carrying the expected
         *  generation; validated at the next block boundary.
         */
        void RequestUndo(uint32_t generation)
        {
            undo_request_generation_ = generation;
            undo_request_pending_ = true;
        }

        bool UndoBusy()
        {
            const LooperUndoState s = looper.GetUndoState();
            return undo_request_pending_ || undo_commit_requested_
                || s == LooperUndoState::FadeOutForUndo
                || s == LooperUndoState::RestoreSuspended;
        }

        bool CanUndoLastOverdub()
        {
            return looper.GetUndoState() == LooperUndoState::Available
                && !UndoBusy();
        }

        inline uint32_t GetUndoAvailableGeneration() { return looper.GetUndoAvailableGeneration(); }

        /** Drop history (clear/load/new base recording paths). */
        inline void InvalidateUndo() { looper.InvalidateUndo(); }

        /** Foreground: tag-table maintenance + bounded restore chunks. */
        void ServiceUndoRestore()
        {
            looper.ServiceUndoMaintenance();

            if(looper.GetUndoState() == LooperUndoState::RestoreSuspended)
            {
                if(!looper.RestoreUndoChunk())
                    undo_commit_requested_ = true; // commit at next block boundary
            }
        }

    private:
        /** Accept a valid undo request / commit a finished restore,
         *  only at audio block boundaries.
         */
        void ServiceUndoRequests()
        {
            if(undo_request_pending_)
            {
                undo_request_pending_ = false;

                if(undo_request_generation_ == looper.GetUndoAvailableGeneration()
                   && looper.GetUndoState() == LooperUndoState::Available)
                {
                    // clear record/play targets and arms, then fade the
                    // looper contribution down before suspending loop DSP
                    record = false;
                    record_target = false;
                    play_target = false;
                    record_arm = false;

                    undo_fade_ = 1.f;
                    looper.BeginUndoFadeOut();
                }
                // stale/invalid generations are rejected silently
            }

            if(undo_commit_requested_)
            {
                undo_commit_requested_ = false;

                if(looper.GetUndoState() == LooperUndoState::RestoreSuspended)
                {
                    looper.ResetAfterRestore();

                    record = false;
                    record_target = false;
                    play_target = false;
                    record_arm = false;
                    playing = false;
                    // first_record stays false; pitch/direction/feedback/
                    // playback volume preserved by ResetAfterRestore
                    undo_fade_ = 1.f;
                }
            }
        }

    public:

        inline bool IsPlaying() { return playing && !first_record; }
        inline bool IsFirstRecording() { return first_record; }

        void IncrementDubGain(float gain) { looper.IncrementDubGain(gain, first_record && !record); }
        inline float GetDubGain() { return looper.GetDubGain(); }

        bool CheckReset()
        {
            if(!looper.IsResetting() && !record_btn && !play_btn)
                reset = false;

            uint32_t elapsed = System::GetNow() - last_button_press;            
            if(elapsed >= kButtonTimeout)
            {
                if(play_btn && !record_btn && !reset && !toggle_play && (record || IsPlaying())) // rising edge and recording or playing
                    TogglePlaying();
                if(record_btn && !play_btn && !reset && !toggle_record)
                    ToggleRecord();            
            }
            if(elapsed >= kRecordClearTimeout)
            {
                if(!record_btn && play_btn && !jump_to_start && !IsPlaying())
                {
                    looper.JumpToStart();
                    jump_to_start = true;
                    return false;
                }
                else if(record_btn && play_btn && !reset)
                {
                    Reset();
                    return true;
                }
            }

            return false;
        }

        void CheckRecordReady()
        {
            if(record_target)
            {
                record = true;
                record_target = false;
                looper.ReadyToRecord();

                if(IsFirstRecording())
                {
                    looper.InvalidateUndo(); // new base recording drops history
                    looper.ResetRamBuff();
                }
                else
                {
                    looper.BeginUndoSession(); // genuine overdub start
                }
            }

            if(play_target)
            {
                playing = true;
                play_target = false;
            }
        }

        bool IsRecording()
        {
            return record;
        }

        inline void SetPitch(float val) { looper.SetVarispeed(val); }
        inline void SetPlaybackGain(float gain)
        {
            playback_gain_target_ = gain < 0.f ? 0.f : (gain > 1.f ? 1.f : gain);
        }
        inline void SetReverse(bool rev) { looper.SetReverse(rev); }
        inline void SetScrub(float scrub) { looper.SetScrub(scrub); }
        inline float GetScrub() { return looper.GetScrub(); }
        inline bool GetReverse() { return looper.GetReverse(); }

        void RecordButton(bool rising)
        {

            record_btn = rising;
            toggle_record = false;
            last_button_press = System::GetNow();

            if(looper.IsResetting() || reset || record_arm)
                return; // short circuit situations
            else if(record_btn && play_btn && first_record && !record)
                record_arm = true;
        }

        void PlayButton(bool rising)
        {
            // falling edge and not recording
            if(play_btn && !rising && !jump_to_start && !record && !toggle_play)
                TogglePlaying();

            toggle_play = false;

            jump_to_start = false;

            play_btn = rising;
            last_button_press = System::GetNow();
         
            if(looper.IsResetting() || reset || record_arm)
                return; // short circuit situations
            else if(record_btn && play_btn && first_record && !record)
                record_arm = true;
        }

        void ToggleRecord(bool play_pressed = false)
        {
            toggle_record = true;
            record_arm = false;
            if(record)
            {
                // don't go into overdub from new record if play is pressed
                if(play_pressed || !first_record)
                {
                    record_target = record = false;
                }
                else
                {
                    // automatic transition from initial recording into overdub:
                    // establish the undo session before its first destructive write
                    looper.BeginUndoSession();
                }

                if(first_record) // reload, then overdub
                {
                    looper.JumpToStart();
                }

                first_record = false;
                play_target = true;
                playing = false;
            }
            else
            {
                if(first_record)
                {
                    looper.WriteJump(sizeof(WAV_FormatTypeDef));
                }
                if (!first_record)
                {
                    playing = true;
                }
                record_target = true;
            }
        }

        void TogglePlaying()
        {
            toggle_play = true;

            if(first_record && !record)
                return; // do nothing on empty looper and not recording

            if(record)
            {
                ToggleRecord(true);
            }
            else
            {
                playing = !playing;
            }
        }

        inline bool GetRecordArm() { return record_arm; }
        inline bool GetIsEmpty() { return !record && first_record; }
        
        float GetPosition() { 
            float tell = static_cast<float>(looper.GetReadTell());
            float size = static_cast<float>(looper.GetReadSize());
            return tell / size;
        }

        float GetPitch()
        {
            return looper.GetVarispeed();
        }

        inline bool GetReset() { return reset; }

        void Reset()
        {
            looper.InvalidateUndo(); // clear drops history
            looper.Reset();
            reset = true;

            record = false;
            first_record = true;
            record_target = false;
            record_arm = false;
            playing = false;
        }

        void OpenFile()
        {
            looper.InvalidateUndo(); // load replaces loop audio

            first_record = false;
            playing = false;
            record = false;
            record_arm = false;
            record_target = false;

            looper.SetVarispeed(1.f, true);
            looper.JumpTo(0);
            looper.ForceSetScrub(0.f);
        }

    private:
        FileSampler looper;
        bool record, first_record, record_target;
        bool playing, play_target;
        uint32_t loop_end;
        uint32_t last_button_press;
        bool record_btn, play_btn;
        bool toggle_record, toggle_play;
        bool record_arm;
        bool reset, jump_to_start;

        float fx_env_, fx_env_target_;
        float playback_gain_, playback_gain_target_;

        /** last-overdub undo request/commit state */
        volatile bool undo_request_pending_;
        volatile bool undo_commit_requested_;
        uint32_t undo_request_generation_;
        float undo_fade_;
        const float kUndoFadeDec = .0005f; // ~42 ms fade at 48 kHz
    };
}
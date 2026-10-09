#pragma once
#include "daisy_seed.h"
#include "daisysp.h"
#include "fatfs.h"
#include "RamBuffer.h"
#include "Limiter.h"
#include <cstring>

namespace daisy
{
    /** Last-overdub undo session states (audio-owned).
     *  Empty/Available describe history; Capturing/DrainingTail describe an
     *  active overdub's snapshot capture; FadeOutForUndo/RestoreSuspended
     *  describe a confirmed undo in flight.
     */
    enum class LooperUndoState : uint8_t
    {
        Empty = 0,
        Capturing,
        DrainingTail,
        Available,
        FadeOutForUndo,
        RestoreSuspended,
    };

    class FileSampler
    {
    public:
        void Init(float sr, RamBufferMemory* buff, bool tape_slew,
                  int16_t* undo_audio = nullptr, uint32_t* undo_tags = nullptr)
        {
            ram_buff.Init(buff);
            sr_ = sr;
            varispeed_factor = 1.f;
            scrub_ =  1.f;
            reverse_ = false;
            
            tape_slew_ = tape_slew;

            lim_l_.Init();
            lim_r_.Init();

            input_env = 0.f;

            undo_audio_ = undo_audio;
            undo_tags_ = undo_tags;
            undo_state_ = LooperUndoState::Empty;
            undo_generation_ = 0; // generation 0 is reserved (never captured)
            undo_available_generation_ = 0;
            undo_pages_touched_ = 0;
            undo_restore_page_ = 0;
            undo_tag_reset_pending_ = false;
        }

        void ReadJumpStart()
        {
            ReadJump(reverse_ ? ram_buff.GetSize() : 0);
        }

        void WriteJumpStart()
        {
            if(reverse_)
                WriteJump(ram_buff.GetSize());
            else
                WriteJump(0);
        }

        inline void ReadJump(uint32_t pos = 0) { ram_buff.SetReadHead(pos); }

        void WriteJump(size_t pos)
        {
            ram_buff.SetWriteHead(pos);
        }

        /** Marks current buffer as invalid, and requests new data at beginning of file */
        void JumpTo(uint32_t pos = 0, bool clear_read = true)
        {
            ram_buff.SetWriteHead(pos);
            ram_buff.SetReadHead(pos);
        }

        void JumpToStart()
        {
            ReadJumpStart();
            WriteJumpStart();
        }

        inline void Reset() { reset = true; }    
        inline bool IsResetting() { return reset; }

        uint16_t write_left_a, write_right_a;
        void ReadyToRecord()
        {
            write_left_a = write_right_a = 0;
        }

        int16_t last_l, last_r;

        /** returns true if a sample was popped */
        bool old_rec = false;
        void PopStereoSamps(int16_t inl, int16_t inr, int16_t *out_l, int16_t *out_r, bool recording, bool playing)
        {
            if(!old_rec && recording) // rising edge
            {
                input_env_dec = .001f;
                old_samps_l.Clear();
                old_samps_r.Clear();
                rev_pushback = true; // poorly named var imo
                ram_buff.SetWriteHead(ram_buff.GetReadHead());
            }
            else if(!recording) // falling edge
            {
                input_env_dec = -.001f;
            }

            old_rec = recording;

            // undo session: record release keeps capturing through the fade tail
            if(undo_state_ == LooperUndoState::Capturing && !recording)
                undo_state_ = LooperUndoState::DrainingTail;
            else if(undo_state_ == LooperUndoState::DrainingTail && !recording
                    && input_env <= .01f)
                FinalizeUndoSession();

            {
                float target = playing ? varispeed_factor : scrub_target_;
                if(reset)
                    target = 0.f;

                
                daisysp::fonepole(scrub_, target, tape_slew_ ? .0001f : .01f);
                scrub_ = daisysp::fclamp(scrub_, -2.f, 2.f);

                if((scrub_ < 0.f && target < 0.f) || (scrub_ > 0.f && target > 0.f))
                {
                    // rev_check = 512;
                    if(SetReverse(scrub_ < 0.f))
                    {
                        *out_l = last_l;
                        *out_r = last_r;
                        return;
                    }
                }

                if(ram_buff.GetSize() == 0)
                {
                    reset = false;
                }

                const float abs_scrub = fabsf(scrub_);
                if(reset && abs_scrub < 0.05f)
                {
                    *out_l = last_l * reset_env;
                    *out_r = last_r * reset_env;
                    reset_env -= .0001f;
                    if(reset_env <= 0.f)
                    {
                        loop_env = 0.f;
                        loop_env_dec = loop_env_dec_val; // ~5ms
                        scrub_ = scrub_target_ = 0.f;
                        SetVarispeed(1.f, true);
                        ram_buff.Reset();
                        ForceSetScrub(1.f);
                        old_samps_l.Clear();
                        old_samps_r.Clear();
                        *out_l = *out_r = 0;
                        reset = false;
                        reset_env = 1.f;
                    }

                    return;
                }
                else if(!reset)
                {
                    reset_env += .005f;

                    if(reset_env > 1.f)
                        reset_env = 1.f;
                }

                old_rpos_frac = rpos_frac_;
                rpos_frac_ += abs_scrub;
                if(turn_period_count_ > kTurnPeriodTimeout)
                {
                    if(!tape_slew_)
                    {
                        if(turn_count_ > 0.f)
                            turn_count_ = 5.f * varispeed_factor;
                        else if(turn_count_ < 0.f)
                            turn_count_ = -5.f * varispeed_factor;
                    }
                    
                    scrub_target_ = turn_count_ * .2f;
                    turn_count_ = 0.f;
                    turn_period_count_ = 0;
                }
                else
                    turn_period_count_++;

                if (rpos_frac_ >= 1.f)
                {
                    /** increment that shouldn't be susceptible to f32 precision loss w/ long files*/
                    const uint32_t samp_stride_ = (uint32_t)rpos_frac_;

                    rpos_frac_ -= samp_stride_;

                    for (uint32_t i = 0; i < samp_stride_; i++)
                    {
                        al = bl;
                        ar = br;
                        ram_buff.StereoRead(&bl, &br, reverse_);

                        if(ram_buff.ReadLoop(reverse_))
                        {
                            ReadJumpStart();
                        }
                        else if(ram_buff.GetRemainingRead(reverse_) <= 480 && loop_env_dec > 0.f)
                        {
                            loop_env_dec = -1.f * loop_env_dec_val;
                        }                    

                        // stop a junk sample from being pushed after changing directions
                        if(!rev_pushback)
                        {
                            old_samps_l.PushBack(al);
                            old_samps_r.PushBack(ar);
                        }
                        rev_pushback = false;

                        loop_env += loop_env_dec;
                        const bool ote = (reverse_ && ram_buff.GetReadHead() > (ram_buff.GetSize() - 50))
                                         || (!reverse_ && ram_buff.GetReadHead() < 50);
                        if(loop_env < 0.f && ote)
                        {
                            loop_env = 0.f;
                            loop_env_dec = loop_env_dec_val;
                        }
                    }

                    const float num_samps = old_samps_l.GetNumElements() - 1.f;
                    float pos = (1.f - old_rpos_frac);
                    const float delta = 1.f / (num_samps + rpos_frac_ - old_rpos_frac);
                    while(old_samps_l.GetNumElements() > 1) // leave one sample on the stack
                    {
                        int32_t write_left = old_samps_l.PopFront() * dub_gain_;
                        int32_t write_right = old_samps_r.PopFront() * dub_gain_;

                        input_env += input_env_dec;
                        input_env = daisysp::fclamp(input_env, 0.f, 1.f);

                        if(recording || input_env > .01f)
                        {
                            daisysp::fonepole(dub_gain_, dub_gain_target_, .0001f);

                            const float lerp_val = pos * delta;
                            write_left += (old_inl + (inl - old_inl) * lerp_val) * input_env;
                            write_right += (old_inr + (inr - old_inr) * lerp_val) * input_env;

                            write_left  = f2s16(lim_l_.ProcessHard(s162f(write_left)));
                            write_right = f2s16(lim_r_.ProcessHard(s162f(write_right)));

                            pos += 1.f;
                            // lazy page snapshot: preserve the original page
                            // immediately before its first destructive write
                            // this session
                            CaptureUndoPage();
                            ram_buff.StereoWrite(write_left, write_right, reverse_, false);

                            if(ram_buff.WriteLoop(reverse_))
                            {
                                WriteJumpStart();
                            }
                        }
                        else if(ram_buff.GetSize())
                            ram_buff.AdvanceWrite(reverse_);
                    }
                }

                loop_env = daisysp::fclamp(loop_env, 0.f, 1.f);

                rev_env += rev_env_dec; // rev_env_dec should be positive at this point
                rev_env = daisysp::fclamp(rev_env, 0.f, 1.f);

                /** linear interpolation */
                const float tl = al + (bl - al) * rpos_frac_;
                const float tr = ar + (br - ar) * rpos_frac_;

                old_inl = inl;
                old_inr = inr;

                // Amp env
                *out_l = static_cast<int16_t>(tl * loop_env * rev_env * reset_env);
                last_l = *out_l;
                *out_r = static_cast<int16_t>(tr * loop_env * rev_env * reset_env);
                last_r = *out_r;
            }
        }

        // only used for first recording. no varispeed
        inline void PushStereoSamps(int16_t l, int16_t r) { ram_buff.StereoWrite(l, r, reverse_, true); }

        // ====================  last-overdub undo  ====================

        inline LooperUndoState GetUndoState() const { return undo_state_; }
        inline uint32_t GetUndoAvailableGeneration() const { return undo_available_generation_; }
        inline bool UndoTagResetPending() const { return undo_tag_reset_pending_; }

        /** Begin snapshot capture for a new overdub session. Any unfinished
         *  session is finalized first so rapid off/on never merges sessions.
         *  Never starts during a confirmed undo or while a generation-wrap
         *  tag reset is pending.
         */
        void BeginUndoSession()
        {
            if(undo_audio_ == nullptr || undo_tags_ == nullptr)
                return;
            if(undo_state_ == LooperUndoState::FadeOutForUndo
               || undo_state_ == LooperUndoState::RestoreSuspended)
                return;

            FinalizeUndoSession(); // close out Capturing/DrainingTail, if any

            if(undo_tag_reset_pending_)
            {
                // generation wrapped: stay Empty until the foreground clears
                // the tag table (gated reset, never in a live callback)
                undo_state_ = LooperUndoState::Empty;
                return;
            }

            uint32_t gen = undo_generation_ + 1;
            if(gen == 0) // generation 0 is reserved
            {
                undo_tag_reset_pending_ = true;
                undo_state_ = LooperUndoState::Empty;
                return;
            }

            undo_generation_ = gen;
            undo_pages_touched_ = 0;
            undo_state_ = LooperUndoState::Capturing;
        }

        /** Publish history once the fade tail has fully drained. */
        void FinalizeUndoSession()
        {
            if(undo_state_ == LooperUndoState::Capturing
               || undo_state_ == LooperUndoState::DrainingTail)
            {
                if(undo_pages_touched_ > 0)
                {
                    undo_available_generation_ = undo_generation_;
                    undo_state_ = LooperUndoState::Available;
                }
                else
                {
                    undo_state_ = LooperUndoState::Empty;
                }
            }
        }

        /** Drop all undo history (before clear/load/replace/append mutations). */
        void InvalidateUndo()
        {
            if(undo_state_ == LooperUndoState::FadeOutForUndo
               || undo_state_ == LooperUndoState::RestoreSuspended)
                return; // a confirmed undo cannot be interrupted

            undo_state_ = LooperUndoState::Empty;
            undo_available_generation_ = 0;
            undo_pages_touched_ = 0;
        }

        /** Available -> FadeOutForUndo (request validated by LooperEngine). */
        void BeginUndoFadeOut()
        {
            if(undo_state_ == LooperUndoState::Available)
                undo_state_ = LooperUndoState::FadeOutForUndo;
        }

        /** FadeOutForUndo -> RestoreSuspended; arms the chunked restore scan. */
        void SuspendUndoDsp()
        {
            if(undo_state_ == LooperUndoState::FadeOutForUndo)
            {
                undo_restore_page_ = 0;
                undo_state_ = LooperUndoState::RestoreSuspended;
            }
        }

        /** Restore up to kUndoRestorePagesPerChunk tagged pages from the
         *  snapshot back into live memory. Foreground only; loop DSP must be
         *  suspended. Returns true while work remains.
         */
        bool RestoreUndoChunk()
        {
            if(undo_state_ != LooperUndoState::RestoreSuspended)
                return false;

            const uint32_t gen = undo_available_generation_;
            size_t scanned = 0;
            while(scanned < kUndoRestorePagesPerChunk
                  && undo_restore_page_ < kLooperUndoNumPages)
            {
                const size_t page = undo_restore_page_++;
                scanned++;
                if(undo_tags_[page] == gen)
                {
                    const size_t base = page * kLooperUndoPageSize;
                    std::memcpy(&ram_buff.RawMem()[base],
                                &undo_audio_[base],
                                kLooperUndoPageSize * sizeof(int16_t));
                    undo_tags_[page] = 0; // consume as we restore
                }
            }

            return undo_restore_page_ < kLooperUndoNumPages;
        }

        /** Foreground maintenance: gated generation-wrap tag table reset.
         *  Only runs when no capture/restore is active.
         */
        void ServiceUndoMaintenance()
        {
            if(undo_tag_reset_pending_
               && undo_state_ == LooperUndoState::Empty)
            {
                std::memset(undo_tags_, 0, kLooperUndoNumPages * sizeof(uint32_t));
                undo_generation_ = 0;
                undo_tag_reset_pending_ = false;
            }
        }

        /** After a completed restore: clear FIFO/interpolation/old-input/
         *  recording-edge/envelope caches and park the heads at the logical
         *  loop start (end when reversed). Pitch, direction, feedback, and
         *  playback volume are intentionally preserved.
         */
        void ResetAfterRestore()
        {
            old_samps_l.Clear();
            old_samps_r.Clear();
            old_inl = old_inr = 0;
            old_rpos_frac = 0.f;
            rpos_frac_ = 0.f;
            al = bl = ar = br = 0;
            last_l = last_r = 0;
            old_rec = false;

            input_env = 0.f;
            input_env_dec = .001f;
            loop_env = 0.f;
            loop_env_dec = loop_env_dec_val;
            rev_env = 1.f;
            rev_env_dec = .001f;
            rev_pushback = true;

            scrub_ = scrub_target_ = 0.f;
            turn_count_ = 0.f;
            turn_period_count_ = 0;

            reset = false;
            reset_env = 1.f;

            ReadJumpStart();
            WriteJumpStart();

            // consume history; InvalidateUndo() refuses while RestoreSuspended
            undo_state_ = LooperUndoState::Empty;
            undo_available_generation_ = 0;
            undo_pages_touched_ = 0;
        }

        /** Capture the live page about to be overwritten, once per session. */
        inline void CaptureUndoPage()
        {
            if(undo_state_ != LooperUndoState::Capturing
               && undo_state_ != LooperUndoState::DrainingTail)
                return;

            // reverse writes target write_head - 2 int16 elements
            const size_t head = ram_buff.GetWriteHead();
            const size_t idx = (reverse_ && head >= 2) ? head - 2 : head;
            size_t page = idx / kLooperUndoPageSize;

            // clamp to the last page covering valid loop data
            const size_t len = ram_buff.GetSize();
            const size_t last_valid = len > 0 ? (len - 1) / kLooperUndoPageSize : 0;
            if(page > last_valid)
                page = last_valid;

            if(undo_tags_[page] != undo_generation_)
            {
                undo_tags_[page] = undo_generation_;
                undo_pages_touched_++;
                const size_t base = page * kLooperUndoPageSize;
                std::memcpy(&undo_audio_[base],
                            &ram_buff.RawMem()[base],
                            kLooperUndoPageSize * sizeof(int16_t));
            }
        }

        /** @brief sets the rate at which to read from the file
         *  @param speed a value relative to original speed
         *      (e.g. 1.0 for original, 2.0 for double, 0.5 for half)
         *      TODO: negative values will be reverse (unless we setup a diff. mechanism for that).
         */
        void SetVarispeed(float speed, bool force = false)
        {
            varispeed_factor = speed;

            if(force)
            {
                SetReverse(speed < 0.f);
            }
        }

        inline float GetVarispeed() { return varispeed_factor; }

        // reverse functions only used internally (except getter)
        bool SetReverse(bool rev) 
        { 
            const bool change = rev != reverse_;
            if(change)
            {
                old_samps_l.Clear();
                old_samps_r.Clear();
                
                rev_pushback = true;
                WriteJump(ram_buff.GetReadHead());
    
                reverse_ = rev;
            }

            return change;
        }
        inline bool GetReverse() { return reverse_; }


        inline void ForceSetScrub(float s) { scrub_ = s; }
        inline void SetScrub(float addl) { turn_count_ += addl; }
        inline float GetScrub() { return scrub_; }

        inline uint32_t GetReadTell() { return ram_buff.GetReadHead(); }
        inline uint32_t GetReadSize() { return ram_buff.GetSize(); }

        inline bool WriteFullLength() { return ram_buff.WriteFullLength(); }

        void IncrementDubGain(float increment, bool force)
        {
            dub_gain_target_ += increment;
            dub_gain_target_ = daisysp::fclamp(dub_gain_target_, 0.f, 1.f);

            if(force)
                dub_gain_ = dub_gain_target_;
        }
        inline float GetDubGain() { return dub_gain_target_; }

        void ResetRamBuff() { ram_buff.Reset(); }

        RamBuffer ram_buff;
        volatile bool reset;
        float reset_env = 1.f;

        /** Compression for overdub feedback loop */
        chompi::Limiter lim_l_;
        chompi::Limiter lim_r_;

        /** Varispeed handling */
        float varispeed_factor;
        float rpos_frac_;

        /** reverse handling */
        bool reverse_;

        /** scrubbing */
        float scrub_, scrub_target_;
        uint32_t turn_period_count_;
        const size_t kTurnPeriodTimeout = 6000; // 1/8 second
        float turn_count_ = 0.f;

        /** cached varispeed data */
        int16_t old_inl = 0;
        int16_t old_inr = 0;
        float old_rpos_frac = 0.f;

        FIFO<int16_t, 32> old_samps_l;
        FIFO<int16_t, 32> old_samps_r;

        int16_t al, bl = 0;
        int16_t ar, br = 0;

        /** Envelope to avoid clicking over the end */
        float loop_env = 0.f;
        const float loop_env_dec_val = .00464f;
        float loop_env_dec = loop_env_dec_val;

        float input_env;
        float input_env_dec = .001f;

        float rev_env = 1.f;
        float rev_env_dec = .001f;
        float rev_pushback = true;

        float dub_gain_ = 1.f;
        float dub_gain_target_ = 1.f;

        /** last-overdub undo snapshot state */
        int16_t* undo_audio_ = nullptr;   // kMaxLooperRamBuffSize int16 in SDRAM
        uint32_t* undo_tags_ = nullptr;   // kLooperUndoNumPages generation tags in SDRAM
        LooperUndoState undo_state_ = LooperUndoState::Empty;
        uint32_t undo_generation_ = 0;
        uint32_t undo_available_generation_ = 0;
        uint32_t undo_pages_touched_ = 0;
        size_t undo_restore_page_ = 0;
        volatile bool undo_tag_reset_pending_ = false;
        static const size_t kUndoRestorePagesPerChunk = 16; // 16 KiB per foreground pass

        float sr_;

        bool tape_slew_;
    };

} // namespace daisy
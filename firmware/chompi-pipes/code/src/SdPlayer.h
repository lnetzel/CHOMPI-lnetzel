/** @file SdPlayer.h
 *  @brief CHOMPI-PIPES playback mode: plays the 48 kHz / 16-bit /
 *  stereo WAV takes in PIPES/RECORDINGS/ (Recording NN.wav, written by
 *  SdRecorder.h) straight from the SD card, one at a time. The newest
 *  15 takes form a window on the 15 white keys (slot 0 = newest).
 *
 *  Same constraint class as SdRecorder.h:
 *   1. The audio ISR only pops the lock-free SPSC ring and converts;
 *      every FatFS call runs at thread level in Service(), staged in
 *      small steps so the main loop keeps feeding the ~2 s IWDG.
 *   2. The instance must be a GLOBAL (like sd_recorder_): its FIL,
 *      FILINFO and bounce buffers then live in D1 .bss. The main stack
 *      is in DTCMRAM, which the SDMMC IDMA cannot reach - a stack FIL
 *      wedges the card access (see PresetStorage.h).
 *   3. The playback ring is caller-owned SDRAM; 1 MiB ~ 5.5 s of
 *      audio, which trivially covers the tens of ms a take close/open
 *      takes - the advance between takes is sample-gapless, and the
 *      loop mode never leaves kSPlay (just an f_lseek), so looping is
 *      gapless too. An empty ring while playing pops silence and sets
 *      the underrun flag - never garbage.
 *
 *  Lifecycle: RequestRescan() -> kSScan (staged f_stat scan DOWNWARD
 *  from 999, collecting up to 15 existing take numbers newest-first,
 *  early exit once 15 are found) -> kSIdle with the window filled.
 *  RequestPlay(slot) -> kSOpen (1 ms ISR-drain wait, ring reset,
 *  f_open, 44-byte header read, SdpWavOk validation) -> kSPlay (fill
 *  the ring in <=4x 16 KiB chunks per pass; EOF -> loop ? f_lseek to
 *  the data start : close + open the SdpNextSlot take with the ring
 *  untouched). A take whose open/read fails or whose header is not a
 *  canonical PIPES WAV is skipped (the UI flashes its slot red); if
 *  every take in the window fails, or any FatFS call hits a card-level
 *  error, the state machine lands in kSError for ~2 s.
 *
 *  Take switch (RequestPlay) resets the ring so the new take starts at
 *  0:00; the transport (paused/playing) is preserved across a switch.
 *  The ring reset is race-safe: playing_ (the ISR gate for PopBlock)
 *  is cleared first and the reset happens >=1 ms later, after any
 *  in-flight pop has finished - the same gate-then-reset-later pattern
 *  the recorder uses (capturing_ cleared in kSClose, ring reset on the
 *  next take's start).
 *
 *  The pure pieces (int16->float, WAV header validation, the take
 *  window, slot mapping) have no Daisy/FatFS dependency so the native
 *  tests exercise the exact production logic; the FatFS-facing class
 *  is compiled out for host tests (CHOMPI_SD_PLAYER_HOST).
 */
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "SdRecorder.h" /* SdrRing, SdrRecPath, kSdRecMaxNumber, header size */

namespace chompi
{

/* ---------------- pure, host-testable core ---------------- */

/** Take-window size: the newest 15 takes sit on the 15 white keys. */
constexpr int kSdpMaxTakes = 15;

/** signed 16-bit -> float: the exact inverse of SdrF2S16
 *  (scale by 1/32767). */
inline float SdpS16ToF(int16_t s)
{
    return static_cast<float>(s) * (1.f / 32767.f);
}

inline uint16_t SdpGetU16LE(const uint8_t *h)
{
    return static_cast<uint16_t>(h[0] | (h[1] << 8));
}

inline uint32_t SdpGetU32LE(const uint8_t *h)
{
    return static_cast<uint32_t>(h[0])
           | (static_cast<uint32_t>(h[1]) << 8)
           | (static_cast<uint32_t>(h[2]) << 16)
           | (static_cast<uint32_t>(h[3]) << 24);
}

/** Validate the canonical 44-byte PCM WAV header that SdrWavHeader
 *  writes: RIFF/WAVE/fmt /data magics, PCM format, 2 channels, 16-bit,
 *  the firmware's sample rate and a non-empty data chunk. Foreign
 *  files dropped into PIPES/RECORDINGS/ fail this check (a fmt chunk
 *  longer than 16 bytes moves the data magic, a 24-bit file fails the
 *  bit depth, etc.). */
inline bool SdpWavOk(const uint8_t *h, uint32_t sample_rate)
{
    return h[0] == 'R' && h[1] == 'I' && h[2] == 'F' && h[3] == 'F'
           && h[8] == 'W' && h[9] == 'A' && h[10] == 'V' && h[11] == 'E'
           && h[12] == 'f' && h[13] == 'm' && h[14] == 't'
           && h[15] == ' '
           && h[36] == 'd' && h[37] == 'a' && h[38] == 't'
           && h[39] == 'a'
           && SdpGetU16LE(h + 20) == 1u    /* PCM */
           && SdpGetU16LE(h + 22) == 2u    /* stereo */
           && SdpGetU32LE(h + 24) == sample_rate
           && SdpGetU16LE(h + 34) == 16u   /* bits per sample */
           && SdpGetU32LE(h + 40) > 0u;    /* data size */
}

/** Deinterleave + convert `frames` int16 stereo frames into float
 *  l/r (SdpS16ToF per sample). */
inline void SdpDeinterleave(const int16_t *src, float *l, float *r,
                            uint32_t frames)
{
    for(uint32_t i = 0; i < frames; i++)
    {
        l[i] = SdpS16ToF(src[2u * i]);
        r[i] = SdpS16ToF(src[2u * i + 1u]);
    }
}

/** The take window: up to kSdpMaxTakes take numbers sorted newest
 *  first (a descending 999 -> 1 scan produces this order naturally).
 *  Slot 0 = newest = the highest take number. */
struct SdpTakeWindow
{
    void Clear() { count = 0; }

    /** Append during a descending scan; the window keeps at most
     *  kSdpMaxTakes entries (extra older takes are dropped). */
    void PushNewestFirst(int take)
    {
        if(count < kSdpMaxTakes)
            takes[count++] = take;
    }

    int takes[kSdpMaxTakes];
    int count;
};

/** White-key slot 0..14 -> take number (slot 0 = newest); -1 for a
 *  slot outside the window. */
inline int SdpSlotToTake(const SdpTakeWindow &w, int slot)
{
    if(slot < 0 || slot >= w.count)
        return -1;
    return w.takes[slot];
}

/** Slot advance when a take finishes with loop off: toward the
 *  next-newer take (slot - 1); the newest take (slot 0) wraps to the
 *  oldest take in the window (slot count - 1). -1 when the window is
 *  empty or the slot is outside it. */
inline int SdpNextSlot(int slot, int count)
{
    if(count <= 0 || slot < 0 || slot >= count)
        return -1;
    return slot > 0 ? slot - 1 : count - 1;
}

} /* namespace chompi */

/* ---------------- FatFS-facing player (target only) --------------- */

#ifndef CHOMPI_SD_PLAYER_HOST

#include "fatfs.h"

namespace chompi
{

class SdPlayer
{
  public:
    enum State : uint8_t
    {
        kSIdle = 0,
        kSScan, /**< staged descending f_stat scan of the takes */
        kSOpen, /**< ISR-drain wait, ring reset, f_open, header check */
        kSPlay, /**< filling the ring from the open take */
        kSError /**< ~2 s, then back to idle */
    };

    static constexpr int kBounceSamples   = 8192; /**< 16 KiB of int16 */
    static constexpr int kScanPerPass     = 32;   /**< f_stat per pass */
    static constexpr int kIsrBounceFrames = 64;   /**< PopBlock chunk */

    /** ring: caller-owned SDRAM, ring_frames stereo frames (frames*2
     *  must be a power of two); sample_rate for header validation. */
    void Init(int16_t *ring, uint32_t ring_frames, float sample_rate)
    {
        ring_.Init(ring, ring_frames * 2u);
        sample_rate_       = static_cast<uint32_t>(sample_rate);
        state_             = kSIdle;
        play_req_          = false;
        req_slot_          = 0;
        pause_req_         = false;
        resume_req_        = false;
        stop_req_          = false;
        scan_req_          = false;
        loop_              = false;
        playing_           = false;
        transport_playing_ = false;
        underrun_          = false;
        skip_slot_         = -1;
        window_.Clear();
        current_slot_      = -1;
        scan_num_          = 0;
        scan_gen_          = 0;
        open_stage_        = 0;
        open_fails_        = 0;
        file_open_         = false;
        data_bytes_        = 0;
        data_left_         = 0;
        gate_off_t_        = 0;
        err_t_             = 0;
    }

    /* ---- audio-ISR side: flags + ring pop only, never FatFS ---- */

    /** Switch to the window slot's take, from 0:00; the transport
     *  (paused/playing) is preserved. The ring is reset. */
    void RequestPlay(int slot)
    {
        req_slot_ = slot;
        play_req_ = true;
    }
    void RequestPause() { pause_req_ = true; }
    void RequestResume() { resume_req_ = true; }
    /** Close the take and go idle; the transport resets to paused. */
    void RequestStop() { stop_req_ = true; }
    /** Rescan the take window (entry, and a refresh after takes were
     *  added/removed outside the mode). Stops any playback. */
    void RequestRescan() { scan_req_ = true; }
    void SetLoop(bool on) { loop_ = on; }

    /** Drain one audio block from the ring into the deinterleaved
     *  float buffers (the engine injects them after the record tap).
     *  Silence - and the underrun flag - when paused/stopped or when
     *  the ring underflows. */
    void PopBlock(float *l, float *r, size_t frames)
    {
        size_t done = 0;
        while(done < frames)
        {
            uint32_t chunk = static_cast<uint32_t>(frames - done);
            if(chunk > static_cast<uint32_t>(kIsrBounceFrames))
                chunk = kIsrBounceFrames;
            uint32_t got = 0;
            if(playing_)
            {
                const uint32_t avail = ring_.Count() / 2u;
                got = chunk < avail ? chunk : avail;
                if(got > 0)
                {
                    ring_.Pop(isr_bounce_, got * 2u);
                    SdpDeinterleave(isr_bounce_, l + done, r + done, got);
                }
                if(got < chunk)
                    underrun_ = true;
            }
            for(uint32_t i = got; i < chunk; i++)
            {
                l[done + i] = 0.f;
                r[done + i] = 0.f;
            }
            done += chunk;
        }
    }

    /* ---- main-loop side ---- */

    State GetState() const { return state_; }
    /** The user's transport (stays true across a take switch's brief
     *  ring reset; use this for the play/pause LEDs). */
    bool IsPlaying() const { return transport_playing_; }
    bool GetLoop() const { return loop_; }
    int  TakeCount() const { return window_.count; }
    int  SlotTake(int slot) const { return SdpSlotToTake(window_, slot); }
    int  CurrentSlot() const { return current_slot_; }
    /** Scan epoch: incremented every time a scan completes, so the
     *  caller can tell "scan done" apart from "scan not started yet". */
    uint32_t ScanGen() const { return scan_gen_; }
    /** Slot whose file failed to open or failed validation, once
     *  (for the red slot flash); -1 when none. */
    int ConsumeSkipSlot()
    {
        const int s = skip_slot_;
        skip_slot_  = -1;
        return s;
    }
    bool Underrun() const { return underrun_; }

    /** One staged FatFS step per main-loop pass. */
    void Service(uint32_t now_ms)
    {
        /* Requests are consumed every pass, whatever the state; kSError
         * drops them (the ~2 s error dwell must finish first). */
        if(pause_req_)
        {
            pause_req_         = false;
            transport_playing_ = false;
            playing_           = false;
        }
        if(resume_req_)
        {
            resume_req_        = false;
            transport_playing_ = true;
            if(state_ == kSPlay)
                playing_ = true;
        }
        if(stop_req_)
        {
            stop_req_          = false;
            play_req_          = false;
            playing_           = false;
            transport_playing_ = false;
            if(state_ != kSError)
            {
                if(file_open_)
                {
                    f_close(&fil_);
                    file_open_ = false;
                }
                state_ = kSIdle;
            }
        }
        if(scan_req_)
        {
            scan_req_ = false;
            play_req_ = false;
            if(state_ != kSError)
            {
                playing_           = false;
                transport_playing_ = false;
                if(file_open_)
                {
                    f_close(&fil_);
                    file_open_ = false;
                }
                window_.Clear();
                scan_num_ = kSdRecMaxNumber;
                state_    = kSScan;
            }
        }
        if(play_req_)
        {
            play_req_     = false;
            const int slot = req_slot_;
            if(state_ != kSError && slot >= 0 && slot < window_.count)
            {
                playing_ = false; /* ISR gate closed before the reset */
                if(file_open_)
                {
                    f_close(&fil_);
                    file_open_ = false;
                }
                current_slot_ = slot;
                open_fails_   = 0;
                gate_off_t_   = now_ms;
                open_stage_   = 0;
                state_        = kSOpen;
            }
        }

        switch(state_)
        {
            case kSIdle: break;

            case kSScan:
            {
                /* downward from 999: the takes land newest-first; the
                 * scan early-exits once the 15-slot window is full */
                for(int i = 0; i < kScanPerPass && scan_num_ >= 1; i++)
                {
                    SdrRecPath(scan_num_, path_);
                    const FRESULT r = f_stat(path_, &fno_);
                    if(r == FR_OK)
                        window_.PushNewestFirst(scan_num_);
                    else if(r != FR_NO_FILE)
                    {
                        AbortPlay(now_ms);
                        return;
                    }
                    scan_num_--;
                    if(window_.count >= kSdpMaxTakes)
                        scan_num_ = 0; /* window full: stop scanning */
                }
                if(scan_num_ < 1)
                {
                    scan_gen_++;
                    state_ = kSIdle;
                }
                break;
            }

            case kSOpen:
                switch(open_stage_)
                {
                    case 0: /* ring-reset wait: >= 1 ms with the ISR
                             * gate closed, so no PopBlock can be
                             * mid-pop when the ring is reset */
                        if(now_ms - gate_off_t_ < 1u)
                            break;
                        ring_.Reset();
                        underrun_   = false;
                        open_stage_ = 1;
                        break; /* open on the next pass */
                    case 1: /* open the current slot's take */
                    {
                        const int take = SdpSlotToTake(window_,
                                                       current_slot_);
                        if(take < 0)
                        {
                            AbortPlay(now_ms);
                            break;
                        }
                        SdrRecPath(take, path_);
                        const FRESULT r = f_open(&fil_, path_, FA_READ);
                        if(r == FR_OK)
                        {
                            file_open_  = true;
                            open_stage_ = 2;
                        }
                        else if(r == FR_NO_FILE)
                            SkipTake(now_ms); /* stale window entry */
                        else
                            AbortPlay(now_ms); /* card-level failure */
                        break;
                    }
                    case 2: /* read + validate the 44-byte header */
                    default:
                    {
                        UINT          br = 0;
                        const FRESULT r  = f_read(&fil_, header_,
                                                  kSdRecWavHeaderSize,
                                                  &br);
                        if(r == FR_OK && br == kSdRecWavHeaderSize
                           && SdpWavOk(header_, sample_rate_))
                        {
                            data_bytes_ = SdpGetU32LE(header_ + 40);
                            data_left_  = data_bytes_;
                            open_fails_ = 0;
                            state_      = kSPlay;
                            playing_    = transport_playing_;
                        }
                        else if(r != FR_OK)
                            AbortPlay(now_ms);
                        else
                            SkipTake(now_ms); /* foreign/invalid file */
                        break;
                    }
                }
                break;

            case kSPlay:
            {
                /* fill the ring: <=4x 16 KiB per pass, never more than
                 * the free space (single producer: the ISR's pops only
                 * grow it, so the push below can never be refused) */
                for(int c = 0; c < 4 && data_left_ > 0; c++)
                {
                    uint32_t free_samples = ring_.cap_ - ring_.Count();
                    free_samples &= ~1u; /* whole frames */
                    if(free_samples < 2u)
                        break;
                    uint32_t bytes = free_samples > kBounceSamples
                                         ? kBounceSamples * 2u
                                         : free_samples * 2u;
                    if(bytes > data_left_)
                        bytes = data_left_;
                    UINT br = 0;
                    if(f_read(&fil_, bounce_, bytes, &br) != FR_OK)
                    {
                        AbortPlay(now_ms);
                        break;
                    }
                    if(br == 0) /* truncated file: treat as EOF */
                    {
                        data_left_ = 0;
                        break;
                    }
                    ring_.Push(bounce_, br / 2u);
                    data_left_ -= br;
                    if(br < bytes) /* short read: EOF */
                        data_left_ = 0;
                }
                if(state_ == kSPlay && data_left_ == 0)
                {
                    if(loop_)
                    {
                        /* loop the take: back to the data start,
                         * never leaving kSPlay - sample-gapless */
                        if(f_lseek(&fil_, kSdRecWavHeaderSize) == FR_OK)
                            data_left_ = data_bytes_;
                        else
                            AbortPlay(now_ms);
                    }
                    else
                    {
                        /* gapless advance to the next-newer take
                         * (wrapping from the newest to the oldest):
                         * the ISR keeps popping the ring (playing_ and
                         * the content untouched) while the next take
                         * opens; the ~5.5 s ring covers the switch */
                        f_close(&fil_);
                        file_open_    = false;
                        current_slot_ = SdpNextSlot(current_slot_,
                                                    window_.count);
                        open_stage_   = 1; /* keep the ring: gapless */
                        state_        = kSOpen;
                    }
                }
                break;
            }

            case kSError:
            default:
                if(now_ms - err_t_ >= 2000u)
                    state_ = kSIdle;
                break;
        }
    }

  private:
    /** The current slot's file is gone or is not a PIPES take: flag
     *  its slot for the UI flash and try the next take; if every take
     *  in the window failed, give up with kSError. */
    void SkipTake(uint32_t now_ms)
    {
        if(file_open_)
        {
            f_close(&fil_);
            file_open_ = false;
        }
        skip_slot_ = current_slot_;
        open_fails_++;
        if(open_fails_ >= window_.count)
        {
            AbortPlay(now_ms);
            return;
        }
        current_slot_ = SdpNextSlot(current_slot_, window_.count);
        open_stage_   = 1; /* the ring state the entry chose stands */
    }

    /** Fatal playback error (card pulled, read/open failure on every
     *  take): stop, close, ~2 s kSError. Never retries. */
    void AbortPlay(uint32_t now_ms)
    {
        playing_           = false;
        transport_playing_ = false;
        play_req_          = false;
        if(file_open_)
        {
            f_close(&fil_);
            file_open_ = false;
        }
        err_t_ = now_ms;
        state_ = kSError;
    }

    SdrRing  ring_;
    uint32_t sample_rate_;

    volatile State    state_;
    volatile bool     play_req_;
    volatile int      req_slot_;
    volatile bool     pause_req_;
    volatile bool     resume_req_;
    volatile bool     stop_req_;
    volatile bool     scan_req_;
    volatile bool     loop_;
    volatile bool     playing_;           /**< ISR gate for PopBlock */
    volatile bool     transport_playing_; /**< the user's transport */
    volatile bool     underrun_;
    volatile int      skip_slot_;

    SdpTakeWindow window_;
    int           current_slot_;
    int           scan_num_; /**< descending: 999 -> 1 */
    volatile uint32_t scan_gen_;
    int      open_stage_;  /**< 0 = ring-reset wait, 1 = open, 2 = hdr */
    int      open_fails_;  /**< consecutive open/validation failures */
    bool     file_open_;
    uint32_t data_bytes_;  /**< PCM bytes in the open take */
    uint32_t data_left_;   /**< PCM bytes not yet read into the ring */
    uint32_t gate_off_t_;
    uint32_t err_t_;

    /* All FatFS-facing storage is member storage (D1 .bss for a global
     * instance): the SDMMC IDMA cannot reach the DTCMRAM stack. */
    FIL      fil_;
    FILINFO  fno_;
    uint8_t  header_[kSdRecWavHeaderSize];
    int16_t  bounce_[kBounceSamples];
    int16_t  isr_bounce_[kIsrBounceFrames * 2];
    char     path_[40];
};

} /* namespace chompi */

#endif /* CHOMPI_SD_PLAYER_HOST */

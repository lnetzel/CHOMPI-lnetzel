/** @file SdRecorder.h
 *  @brief CHOMPI-PIPES record-to-SD: captures the full audio chain
 *  (the pre-master-gain stereo bus at a fixed -6 dB level, so takes
 *  are independent of the volume knob) to 48 kHz / 16-bit / stereo
 *  WAV files at PIPES/RECORDINGS/Recording NN.wav.
 *
 *  Same constraint class as PresetStorage.h:
 *   1. The audio ISR only converts + appends to a lock-free SPSC ring;
 *      every FatFS call runs at thread level in Service(), staged in
 *      small steps so the main loop keeps feeding the ~2 s IWDG.
 *   2. The instance must be a GLOBAL (like preset_storage_): its FIL,
 *      FILINFO and 16 KiB bounce buffer then live in D1 .bss. The main
 *      stack is in DTCMRAM, which the SDMMC IDMA cannot reach - a
 *      stack FIL wedges the card access (boot hang, see
 *      PresetStorage.h).
 *   3. The audio ring is caller-owned SDRAM; 1 MiB ~ 5.5 s of slack
 *      for cheap-card stalls. A full ring aborts the take - the ISR
 *      never overwrites unconsumed audio.
 *
 *  Take lifecycle: RequestStart() (ISR) -> kSOpen (mkdir, next free
 *  take number, open, placeholder header) -> kSRecord (drain the ring
 *  in <=4 chunks of 16 KiB per pass, f_sync once per second, auto-stop
 *  at kSdRecMaxBytes) -> kSClose (drain the tail, patch the header
 *  sizes, close). Any write error or a full card aborts the take
 *  (kSError, ~2 s): with a working card the partial take's header is
 *  still patched so it stays playable; a removed card just drops it.
 *  Nothing ever overwrites an old take.
 *
 *  The pure pieces (ring math, float->int16, take naming, WAV header
 *  layout, byte-limit check) have no Daisy/FatFS dependency so the
 *  native tests exercise the exact production logic; the FatFS-facing
 *  class is compiled out for host tests (CHOMPI_SD_RECORDER_HOST).
 */
#pragma once
#include <stddef.h>
#include <stdint.h>

namespace chompi
{

/* ---------------- pure, host-testable core ---------------- */

/** Highest take number: Recording 01.wav .. Recording 999.wav. */
constexpr int kSdRecMaxNumber = 999;

/** Per-take data-byte ceiling: 6 h at 48 kHz / 16-bit / stereo
 *  (~3.9 GiB), comfortably under the 4 GiB FAT32 / 32-bit WAV limit
 *  even with the header and one final 16 KiB chunk on top. */
constexpr uint32_t kSdRecMaxBytes = 4147200000u; /* 48000 * 4 * 21600 */

/** Canonical PCM WAV header size in bytes. */
constexpr uint32_t kSdRecWavHeaderSize = 44;

/** float -> signed 16-bit: clamp to [-1, 1], scale by 32767, truncate
 *  (same recipe as libDaisy's f2s16). */
inline int16_t SdrF2S16(float x)
{
    if(x > 1.f)
        x = 1.f;
    else if(x < -1.f)
        x = -1.f;
    return static_cast<int16_t>(x * 32767.f);
}

/** SPSC 16-bit sample ring: the audio ISR (producer) converts and
 *  appends, the main loop (consumer) drains to the card. Head/tail are
 *  monotonically increasing uint32_t counters (wrap-safe count),
 *  masked into the power-of-two buffer. A ring without room for the
 *  WHOLE push refuses it and sets the overflow flag - never a partial
 *  write, never an overwrite. Reset() runs per take, consumer side
 *  only, while the producer is paused (SdRecorder clears capturing_
 *  first). */
struct SdrRing
{
    /** buf: caller-owned storage; capacity: samples, a power of two. */
    void Init(int16_t *buf, uint32_t capacity)
    {
        buf_  = buf;
        cap_  = capacity;
        mask_ = capacity - 1u;
        Reset();
    }

    void Reset()
    {
        head_     = 0;
        tail_     = 0;
        overflow_ = false;
    }

    /** Samples waiting to be drained. */
    uint32_t Count() const { return head_ - tail_; }

    /** A push was refused since the last Reset(). */
    bool Overflow() const { return overflow_; }

    /** Producer: append n raw samples; 0 and overflow when full. */
    uint32_t Push(const int16_t *src, uint32_t n)
    {
        const uint32_t h = head_;
        if(cap_ - (h - tail_) < n)
        {
            overflow_ = true;
            return 0;
        }
        for(uint32_t i = 0; i < n; i++)
            buf_[(h + i) & mask_] = src[i];
        head_ = h + n;
        return n;
    }

    /** Producer: convert + append `frames` interleaved stereo frames;
     *  0 and overflow when full. */
    uint32_t PushStereo(const float *l, const float *r, uint32_t frames)
    {
        const uint32_t h = head_;
        if(cap_ - (h - tail_) < frames * 2u)
        {
            overflow_ = true;
            return 0;
        }
        for(uint32_t i = 0; i < frames; i++)
        {
            buf_[(h + 2u * i) & mask_]      = SdrF2S16(l[i]);
            buf_[(h + 2u * i + 1u) & mask_] = SdrF2S16(r[i]);
        }
        head_ = h + frames * 2u;
        return frames;
    }

    /** Consumer: drain up to `max` samples; returns the count. */
    uint32_t Pop(int16_t *dst, uint32_t max)
    {
        const uint32_t t = tail_;
        uint32_t       n = head_ - t;
        if(n > max)
            n = max;
        for(uint32_t i = 0; i < n; i++)
            dst[i] = buf_[(t + i) & mask_];
        tail_ = t + n;
        return n;
    }

    int16_t *         buf_;
    uint32_t          cap_;
    uint32_t          mask_;
    volatile uint32_t head_; /**< producer advances */
    volatile uint32_t tail_; /**< consumer advances */
    volatile bool     overflow_;
};

/** First take number in 1..max whose exists[] entry is false; -1 when
 *  every number is taken. exists[i] is take number i + 1. */
inline int SdrNextFreeNumber(const bool exists[], int max)
{
    for(int i = 0; i < max; i++)
        if(!exists[i])
            return i + 1;
    return -1;
}

/** Take file name: "Recording 01.wav" (zero-padded) for 1..99, plain
 *  "Recording 100.wav" .. "Recording 999.wav" above. `out` holds
 *  >= 24 bytes; n must be 1..999. */
inline void SdrRecFileName(int n, char *out)
{
    static const char kPrefix[] = "Recording ";
    int               i         = 0;
    for(const char *p = kPrefix; *p != '\0'; p++)
        out[i++] = *p;
    if(n < 100)
    {
        out[i++] = static_cast<char>('0' + (n / 10) % 10);
        out[i++] = static_cast<char>('0' + n % 10);
    }
    else
    {
        out[i++] = static_cast<char>('0' + (n / 100) % 10);
        out[i++] = static_cast<char>('0' + (n / 10) % 10);
        out[i++] = static_cast<char>('0' + n % 10);
    }
    out[i++] = '.';
    out[i++] = 'w';
    out[i++] = 'a';
    out[i++] = 'v';
    out[i]   = '\0';
}

/** Full take path "PIPES/RECORDINGS/Recording NN.wav". `out` holds
 *  >= 40 bytes; n must be 1..999. */
inline void SdrRecPath(int n, char *out)
{
    static const char kDir[] = "PIPES/RECORDINGS/";
    int               i      = 0;
    for(const char *p = kDir; *p != '\0'; p++)
        out[i++] = *p;
    SdrRecFileName(n, out + i);
}

inline void SdrPutU16LE(uint8_t *h, uint16_t v)
{
    h[0] = static_cast<uint8_t>(v & 0xffu);
    h[1] = static_cast<uint8_t>(v >> 8);
}

inline void SdrPutU32LE(uint8_t *h, uint32_t v)
{
    h[0] = static_cast<uint8_t>(v & 0xffu);
    h[1] = static_cast<uint8_t>((v >> 8) & 0xffu);
    h[2] = static_cast<uint8_t>((v >> 16) & 0xffu);
    h[3] = static_cast<uint8_t>(v >> 24);
}

/** Canonical 44-byte PCM WAV header (the same byte layout as
 *  libDaisy's WAV_FormatTypeDef in util/wav_format.h, written
 *  byte-wise so it is endian-safe): RIFF/size/WAVE/fmt /16/PCM/
 *  2ch/rate/byterate/align/16/data/datasize. Stereo, 16-bit. Called
 *  with data_bytes = 0 for the placeholder at open, then again with
 *  the final length to patch the sizes on close. */
inline void SdrWavHeader(uint8_t *h, uint32_t sample_rate,
                         uint32_t data_bytes)
{
    h[0] = 'R';
    h[1] = 'I';
    h[2] = 'F';
    h[3] = 'F';
    SdrPutU32LE(h + 4, 36u + data_bytes); /* RIFF chunk size */
    h[8]  = 'W';
    h[9]  = 'A';
    h[10] = 'V';
    h[11] = 'E';
    h[12] = 'f';
    h[13] = 'm';
    h[14] = 't';
    h[15] = ' ';
    SdrPutU32LE(h + 16, 16u);          /* fmt chunk size */
    SdrPutU16LE(h + 20, 1u);           /* PCM */
    SdrPutU16LE(h + 22, 2u);           /* stereo */
    SdrPutU32LE(h + 24, sample_rate);
    SdrPutU32LE(h + 28, sample_rate * 4u); /* byte rate */
    SdrPutU16LE(h + 32, 4u);           /* block align */
    SdrPutU16LE(h + 34, 16u);          /* bits per sample */
    h[36] = 'd';
    h[37] = 'a';
    h[38] = 't';
    h[39] = 'a';
    SdrPutU32LE(h + 40, data_bytes);
}

/** Per-take byte-limit check (auto-stop). */
inline bool SdrRecOverLimit(uint32_t data_bytes)
{
    return data_bytes >= kSdRecMaxBytes;
}

} /* namespace chompi */

/* ---------------- FatFS-facing recorder (target only) ------------- */

#ifndef CHOMPI_SD_RECORDER_HOST

#include "fatfs.h"

namespace chompi
{

class SdRecorder
{
  public:
    enum State : uint8_t
    {
        kSIdle = 0,
        kSOpen,   /**< mkdir, next-free-number scan, open, header */
        kSRecord, /**< draining the ring to the card */
        kSClose,  /**< drain the tail, patch the header, close */
        kSError   /**< ~2 s, then back to idle */
    };

    static constexpr int kBounceSamples = 8192; /**< 16 KiB of int16 */
    static constexpr int kScanPerPass   = 32;   /**< f_stat per pass */

    /** ring: caller-owned SDRAM, ring_frames stereo frames (frames*2
     *  must be a power of two); sample_rate for the WAV header. */
    void Init(int16_t *ring, uint32_t ring_frames, float sample_rate)
    {
        ring_.Init(ring, ring_frames * 2u);
        sample_rate_ = static_cast<uint32_t>(sample_rate);
        state_       = kSIdle;
        start_req_   = false;
        stop_req_    = false;
        capturing_   = false;
        file_open_   = false;
        open_stage_  = 0;
        scanned_     = 0;
        take_num_    = -1;
        data_bytes_  = 0;
        sync_t_      = 0;
        err_t_       = 0;
    }

    /* ---- audio-ISR side: flags + ring append only, never FatFS ---- */

    void RequestStart() { start_req_ = true; }
    void RequestStop() { stop_req_ = true; }

    /** Append one block of the rendered chain (deinterleaved float
     *  stereo) to the ring. No-op unless a take is capturing. */
    void PushBlock(const float *l, const float *r, size_t frames)
    {
        if(capturing_)
            ring_.PushStereo(l, r, static_cast<uint32_t>(frames));
    }

    /* ---- main-loop side ---- */

    State GetState() const { return state_; }
    bool  IsRecording() const { return state_ == kSRecord; }

    /** One staged FatFS step per main-loop pass. */
    void Service(uint32_t now_ms)
    {
        switch(state_)
        {
            case kSIdle:
                stop_req_ = false;
                if(start_req_)
                {
                    start_req_  = false;
                    ring_.Reset(); /* producer paused: capturing_ false */
                    capturing_  = true;
                    file_open_  = false;
                    open_stage_ = 0;
                    scanned_    = 0;
                    take_num_   = -1;
                    data_bytes_ = 0;
                    state_      = kSOpen;
                }
                break;

            case kSOpen:
                if(ring_.Overflow())
                {
                    AbortTake(now_ms);
                    break;
                }
                switch(open_stage_)
                {
                    case 0: /* directories (FR_EXIST is fine) */
                    {
                        const FRESULT r1 = f_mkdir("PIPES");
                        const FRESULT r2 = f_mkdir("PIPES/RECORDINGS");
                        if((r1 == FR_OK || r1 == FR_EXIST)
                           && (r2 == FR_OK || r2 == FR_EXIST))
                            open_stage_ = 1;
                        else
                            AbortTake(now_ms);
                        break;
                    }
                    case 1: /* next-free-number scan, staged */
                    {
                        for(int i = 0;
                            i < kScanPerPass && scanned_ < kSdRecMaxNumber;
                            i++)
                        {
                            SdrRecPath(scanned_ + 1, path_);
                            const FRESULT r = f_stat(path_, &fno_);
                            if(r == FR_OK)
                                exists_[scanned_] = true;
                            else if(r == FR_NO_FILE)
                                exists_[scanned_] = false;
                            else
                            {
                                AbortTake(now_ms);
                                return;
                            }
                            scanned_++;
                        }
                        take_num_ = SdrNextFreeNumber(exists_, scanned_);
                        if(take_num_ > 0)
                            open_stage_ = 2;
                        else if(scanned_ >= kSdRecMaxNumber)
                            AbortTake(now_ms); /* every name taken */
                        break;
                    }
                    case 2: /* open the take file */
                        SdrRecPath(take_num_, path_);
                        if(f_open(&fil_, path_, FA_WRITE | FA_CREATE_ALWAYS)
                           == FR_OK)
                        {
                            file_open_  = true;
                            open_stage_ = 3;
                        }
                        else
                            AbortTake(now_ms);
                        break;
                    case 3: /* placeholder header, then record */
                    default:
                    {
                        SdrWavHeader(header_, sample_rate_, 0);
                        UINT bw = 0;
                        if(f_write(&fil_, header_, kSdRecWavHeaderSize,
                                   &bw)
                               == FR_OK
                           && bw == kSdRecWavHeaderSize)
                        {
                            data_bytes_ = 0;
                            sync_t_     = now_ms;
                            /* a stop requested while opening still
                             * closes the take cleanly */
                            state_     = stop_req_ ? kSClose : kSRecord;
                            stop_req_  = false;
                            capturing_ = state_ == kSRecord;
                        }
                        else
                            AbortTake(now_ms);
                        break;
                    }
                }
                break;

            case kSRecord:
            {
                start_req_ = false; /* a start while recording is inert */
                if(ring_.Overflow())
                {
                    AbortTake(now_ms);
                    break;
                }
                if(stop_req_ || SdrRecOverLimit(data_bytes_))
                {
                    stop_req_  = false;
                    capturing_ = false; /* the ring holds the tail */
                    state_     = kSClose;
                    break;
                }
                for(int c = 0; c < 4; c++)
                {
                    const uint32_t got
                        = ring_.Pop(reinterpret_cast<int16_t *>(bounce_),
                                    kBounceSamples);
                    if(got == 0)
                        break;
                    UINT           bw    = 0;
                    const uint32_t bytes = got * 2u;
                    if(f_write(&fil_, bounce_, bytes, &bw) != FR_OK
                       || bw != bytes)
                    {
                        AbortTake(now_ms);
                        break;
                    }
                    data_bytes_ += bytes;
                }
                if(state_ == kSRecord && now_ms - sync_t_ >= 1000u)
                {
                    sync_t_ = now_ms;
                    if(f_sync(&fil_) != FR_OK)
                        AbortTake(now_ms);
                }
                break;
            }

            case kSClose:
            {
                if(ring_.Count() > 0)
                {
                    for(int c = 0; c < 4 && ring_.Count() > 0; c++)
                    {
                        const uint32_t got = ring_.Pop(
                            reinterpret_cast<int16_t *>(bounce_),
                            kBounceSamples);
                        UINT           bw    = 0;
                        const uint32_t bytes = got * 2u;
                        if(f_write(&fil_, bounce_, bytes, &bw) != FR_OK
                           || bw != bytes)
                        {
                            AbortTake(now_ms);
                            return;
                        }
                        data_bytes_ += bytes;
                    }
                    break; /* more next pass */
                }
                /* patch the header sizes, then close */
                SdrWavHeader(header_, sample_rate_, data_bytes_);
                UINT    bw = 0;
                FRESULT r  = f_lseek(&fil_, 0);
                if(r == FR_OK)
                    r = f_write(&fil_, header_, kSdRecWavHeaderSize, &bw);
                f_close(&fil_);
                file_open_ = false;
                if(r == FR_OK && bw == kSdRecWavHeaderSize)
                    state_ = kSIdle;
                else
                {
                    err_t_ = now_ms;
                    state_ = kSError;
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
    /** Take abort: stop capturing; a working card still gets its
     *  header patched so the partial take stays playable (card gone:
     *  every call fails and the file is dropped). Never retries. */
    void AbortTake(uint32_t now_ms)
    {
        capturing_ = false;
        start_req_ = false;
        stop_req_  = false;
        ring_.Reset();
        if(file_open_)
        {
            SdrWavHeader(header_, sample_rate_, data_bytes_);
            UINT bw = 0;
            if(f_lseek(&fil_, 0) == FR_OK)
                f_write(&fil_, header_, kSdRecWavHeaderSize, &bw);
            f_close(&fil_);
            file_open_ = false;
        }
        err_t_ = now_ms;
        state_ = kSError;
    }

    SdrRing  ring_;
    uint32_t sample_rate_;

    volatile State state_;
    volatile bool  start_req_;
    volatile bool  stop_req_;
    volatile bool  capturing_; /**< ISR gate for PushBlock */

    bool     file_open_;
    int      open_stage_;
    int      scanned_;
    int      take_num_;
    bool     exists_[kSdRecMaxNumber];
    uint32_t data_bytes_; /**< PCM bytes written (header excluded) */
    uint32_t sync_t_;
    uint32_t err_t_;

    /* All FatFS-facing storage is member storage (D1 .bss for a global
     * instance): the SDMMC IDMA cannot reach the DTCMRAM stack. */
    FIL     fil_;
    FILINFO fno_;
    uint8_t header_[kSdRecWavHeaderSize];
    uint8_t bounce_[kBounceSamples * 2];
    char    path_[40];
};

} /* namespace chompi */

#endif /* CHOMPI_SD_RECORDER_HOST */

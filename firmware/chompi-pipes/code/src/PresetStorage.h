/** @file PresetStorage.h
 *  @brief SD-card storage for organ presets: mount + boot load, and a
 *  staged, non-blocking writer that runs ONLY from the main loop.
 *
 *  One small file per slot: PIPES/P01.json .. P15.json (~300 B). A dirty
 *  slot is written as Pxx.tmp, synced, then renamed over the old file, so
 *  a power loss can corrupt at most the slot being written and the
 *  previous file survives. A dirty-but-invalid slot is a pending delete
 *  (single f_unlink). Stray .tmp files are never opened.
 *
 *  Each Service() call performs at most one small FatFS stage, so the
 *  watchdog keeps being fed and the (higher-priority) audio ISR is never
 *  stalled. The RAM-bank snapshot is taken with IRQs briefly off; if the
 *  ISR dirties the slot again mid-write the bit is set again and the
 *  newer content is written on a later pass - nothing is lost.
 *
 *  Card presence is re-checked once per second via disk_status(0);
 *  without a card every gesture is inert and the organ just runs on its
 *  RAM cache. Re-inserting a card re-mounts and gap-fills slots that are
 *  still empty and untouched in RAM.
 */
#pragma once
#include "daisy.h"
#include "fatfs.h"
#include "diskio.h"
#include "OrganPreset.h"

namespace chompi
{

class PresetStorage
{
  public:
    static constexpr int    kNumSlots    = OrganPresetBank::kNumSlots;
    static constexpr size_t kJsonSize    = 768; /* serialized ~300 B */
    static constexpr int    kMaxRetries  = 3;
    static constexpr uint32_t kBackoffMs = 250;

    void Init()
    {
        no_sd_card_      = true;
        stage_           = kIdle;
        slot_            = -1;
        retries_         = 0;
        last_disk_check_ = 0;
        retry_at_        = 0;

        /* same SDMMC/FatFS init as chompi-tape; with no card inserted
         * this configures the peripheral without touching a card and the
         * disk_status check below leaves the presets disabled instead of
         * hanging the boot */
        daisy::SdmmcHandler::Config sd_cfg;
        sd_cfg.speed = daisy::SdmmcHandler::Speed::FAST;
        sd_cfg.width = daisy::SdmmcHandler::BusWidth::BITS_4;
        sdmmc_.Init(sd_cfg);
        fsi_.Init(daisy::FatFSInterface::Config::MEDIA_SD);
        f_mount(&fsi_.GetSDFileSystem(), fsi_.GetSDPath(), 1);
        f_mkdir("PIPES"); /* FR_EXIST is fine */
        no_sd_card_ = disk_status(0) != RES_OK;
    }

    /** Read all 15 slot files into the RAM bank. Call at boot, before
     *  audio starts. Missing/corrupt files leave their slot invalid;
     *  the organ then boots on factory defaults as usual. */
    void LoadAll(OrganPresetBank &bank)
    {
        if(no_sd_card_)
            return;
        for(int s = 0; s < kNumSlots; s++)
            LoadSlotFromCard(s, bank, false);
    }

    /** One storage stage per main-loop pass. */
    void Service(OrganPresetBank &bank)
    {
        const uint32_t now = daisy::System::GetNow();

        /* 1 s card-presence check */
        if(now - last_disk_check_ >= 1000)
        {
            last_disk_check_ = now;
            const bool gone  = disk_status(0) != RES_OK;
            if(gone)
            {
                Abort(); /* drop any in-flight write */
                no_sd_card_ = true;
            }
            else
            {
                if(no_sd_card_)
                {
                    /* card (re-)inserted: re-mount and fill slots that
                     * are empty AND untouched in RAM from the card;
                     * RAM-resident and dirty slots keep their content */
                    f_mount(&fsi_.GetSDFileSystem(), fsi_.GetSDPath(), 1);
                    f_mkdir("PIPES");
                    for(int s = 0; s < kNumSlots; s++)
                        LoadSlotFromCard(s, bank, true);
                }
                no_sd_card_ = false;
            }
        }

        if(no_sd_card_)
            return;

        switch(stage_)
        {
            case kIdle:
            {
                const int s = bank.NextDirtySlot();
                if(s < 0)
                    break;
                __disable_irq();
                slot_           = s;
                valid_snapshot_ = bank.IsValid(s);
                preset_snapshot_ = bank.Get(s);
                bank.ClearDirty(s);
                __enable_irq();
                retries_ = 0;
                if(valid_snapshot_)
                {
                    json_len_ = Serialize(preset_snapshot_, json_,
                                          kJsonSize);
                    stage_    = json_len_ > 0 ? kOpenTmp : kIdle;
                }
                else
                {
                    stage_ = kDelete; /* erased slot: remove its file */
                }
                break;
            }

            case kOpenTmp:
            {
                char name[24];
                SlotName(slot_, name, true);
                if(f_open(&fil_, name,
                          (FA_OPEN_ALWAYS | FA_WRITE | FA_READ))
                   == FR_OK)
                {
                    stage_ = kWrite;
                }
                else
                {
                    Retry(now, kOpenTmp);
                }
                break;
            }

            case kWrite:
            {
                UINT bw = 0;
                const FRESULT r = f_write(&fil_, json_, json_len_, &bw);
                if(r == FR_OK && bw == json_len_)
                    stage_ = kSyncClose;
                else
                    Retry(now, kOpenTmp);
                break;
            }

            case kSyncClose:
            {
                /* truncate at the write pointer: the tmp file may hold
                 * a longer stale body from a previous attempt, and
                 * trailing garbage would fail JSON_Validate on load */
                FRESULT r = f_truncate(&fil_);
                if(r == FR_OK)
                    r = f_sync(&fil_);
                f_close(&fil_);
                if(r == FR_OK)
                    stage_ = kUnlinkOld;
                else
                    Retry(now, kOpenTmp);
                break;
            }

            case kUnlinkOld:
            {
                char name[24];
                SlotName(slot_, name, false);
                const FRESULT r = f_unlink(name);
                if(r == FR_OK || r == FR_NO_FILE)
                    stage_ = kRename;
                else
                    Retry(now, kOpenTmp);
                break;
            }

            case kRename:
            {
                char from[24], to[24];
                SlotName(slot_, from, true);
                SlotName(slot_, to, false);
                f_rename(from, to); /* done either way: a failed rename
                                 * is not retried (the .tmp stays behind
                                 * and is ignored by LoadAll) */
                stage_ = kIdle;
                break;
            }

            case kDelete:
            {
                char name[24];
                SlotName(slot_, name, false);
                const FRESULT r = f_unlink(name);
                if(r == FR_OK || r == FR_NO_FILE)
                    stage_ = kIdle; /* delete is idempotent */
                else
                    Retry(now, kDelete);
                break;
            }

            case kBackoff:
            default:
                if(static_cast<int32_t>(now - retry_at_) >= 0)
                    stage_ = retry_stage_;
                break;
        }
    }

    /** Card state, polled by the UI (red keys / gestures inert). */
    bool NoSdCard() const { return no_sd_card_; }

    /** A write/delete cycle is in flight. */
    bool Busy() const { return stage_ != kIdle; }

  private:
    enum Stage : uint8_t
    {
        kIdle = 0,
        kOpenTmp,
        kWrite,
        kSyncClose,
        kUnlinkOld,
        kRename,
        kDelete,
        kBackoff
    };

    static void SlotName(int slot, char *out, bool tmp)
    {
        snprintf(out, 24, "PIPES/P%02d.%s", slot + 1,
                 tmp ? "tmp" : "json");
    }

    void LoadSlotFromCard(int s, OrganPresetBank &bank, bool gaps_only)
    {
        if(gaps_only && (bank.IsValid(s) || bank.IsDirty(s)))
            return; /* RAM content wins for touched slots */
        char name[24];
        SlotName(s, name, false);
        /* read_fil_ and read_buf_ MUST be members (SRAM, .bss), never
         * stack locals: with _FS_TINY=0 every FIL carries its own
         * 512 B sector buffer and f_read DMAs the data sector into it
         * (and larger reads DMA straight into the user buffer). The
         * main stack is in DTCMRAM, which the SDMMC IDMA cannot reach,
         * so a stack FIL stalls the read forever. Same rule as Tape. */
        if(f_open(&read_fil_, name, FA_READ) != FR_OK)
            return;
        UINT    br = 0;
        FRESULT r  = f_read(&read_fil_, read_buf_, kJsonSize - 1, &br);
        f_close(&read_fil_);
        if(r != FR_OK)
            return;
        read_buf_[br] = '\0';
        OrganPreset p;
        if(Parse(read_buf_, br, p))
            bank.LoadSlot(s, p);
    }

    /** Failed stage: retry a few times with a short backoff, then drop
     *  the write (the RAM cache keeps the data; the next gesture
     *  re-dirties the slot). */
    void Retry(uint32_t now, Stage retry_stage)
    {
        if(stage_ == kOpenTmp || stage_ == kWrite || stage_ == kSyncClose
           || stage_ == kUnlinkOld)
            f_close(&fil_); /* harmless if not open */
        if(++retries_ >= kMaxRetries)
        {
            stage_ = kIdle;
            return;
        }
        retry_stage_ = retry_stage;
        retry_at_    = now + kBackoffMs;
        stage_       = kBackoff;
    }

    /** Card vanished mid-write: close and drop back to idle. The dirty
     *  bit was cleared at snapshot time; the RAM cache keeps the data
     *  and the user can re-save after re-inserting the card. */
    void Abort()
    {
        if(stage_ == kOpenTmp || stage_ == kWrite || stage_ == kSyncClose
           || stage_ == kUnlinkOld)
            f_close(&fil_);
        stage_ = kIdle;
    }

    daisy::SdmmcHandler   sdmmc_;
    daisy::FatFSInterface fsi_;
    FIL                   fil_;
    FIL                   read_fil_; /* own FIL for the reader: the write
                                      * path's fil_ may hold an open tmp
                                      * file; both must stay off the
                                      * DTCMRAM stack (IDMA, see above) */

    volatile bool no_sd_card_;
    Stage         stage_;
    Stage         retry_stage_;
    int           slot_;
    int           retries_;
    uint32_t      last_disk_check_;
    uint32_t      retry_at_;

    OrganPreset preset_snapshot_;
    bool        valid_snapshot_;
    char        json_[kJsonSize];
    size_t      json_len_;
    char        read_buf_[kJsonSize]; /* boot/gap-fill read buffer (SRAM,
                                       * DMA-reachable) */
};

} // namespace chompi

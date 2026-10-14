/** @file OrganPreset.h
 *  @brief SD-card preset model for the CHOMPI Organ: one bank of 15 slots,
 *  versioned JSON, host-testable (no Daisy dependencies).
 *
 *  One small file per slot (ORGAN/P01.json .. P15.json on the SD card,
 *  ~300 B each). All 15 slots are cached in RAM; load/save/copy/erase act
 *  on the cache instantly and PresetStorage writes dirty slots out from
 *  the main loop, so the audio path never waits for the card.
 *
 *  Field ranges and defaults mirror the OrganEngine setters. Parsing
 *  clamps to those ranges and fills missing keys with the boot defaults,
 *  so older and newer files are tolerated; unknown keys are ignored.
 */
#pragma once
#include <cstdint>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "core_json.h"

namespace chompi
{

struct OrganPreset
{
    static constexpr int kNumPipes = 4;
    static constexpr int kVersion  = 1;

    /* field ranges (mirror the OrganEngine setters) */
    static constexpr int kMaxLevel       = 32;
    static constexpr int kMaxCharacter   = 64;
    static constexpr int kMaxEnv         = 32;
    static constexpr int kMaxVibrato     = 50;
    static constexpr int kMaxRotary      = 32;
    static constexpr int kMaxDrive       = 32;
    static constexpr int kMaxTone        = 100;
    static constexpr int kMaxReverbMix   = 100;
    static constexpr int kMaxReverbDecay = 100;
    static constexpr int kMaxMod         = 32;

    /* boot defaults (mirror OrganEngine) */
    static constexpr int kDefaultLevel       = 16; /* 50% */
    static constexpr int kDefaultCharacter   = 32; /* 50% */
    static constexpr int kDefaultEnv         = 0;  /* 5 ms */
    static constexpr int kDefaultVibrato     = 0;
    static constexpr int kDefaultRotary      = 10; /* ~30% */
    static constexpr int kDefaultDrive       = 0;
    static constexpr int kDefaultTone        = 100;
    static constexpr int kDefaultReverbMix   = 0;
    static constexpr int kDefaultReverbDecay = 0;  /* 0.2 s */
    static constexpr int kDefaultMod         = 0;

    uint8_t level[kNumPipes];     /**< 0..32 pipe levels */
    uint8_t character[kNumPipes]; /**< 0..64 Character amounts */
    uint8_t attack[kNumPipes];    /**< 0..32 attack steps */
    uint8_t release[kNumPipes];   /**< 0..32 release steps */
    uint8_t mute[kNumPipes];      /**< 0/1 pipe mutes */
    uint8_t vibrato;              /**< 0..50 cents peak */
    uint8_t rotary;               /**< 0..32 rotary amount */
    uint8_t drive;                /**< 0..32 fuzz amount */
    uint8_t drive_tone;           /**< 0..100 fuzz tone percent */
    uint8_t reverb_mix;           /**< 0..100 plate mix percent */
    uint8_t reverb_decay;         /**< 0..100 plate decay steps */
    uint8_t vol_mod[kNumPipes];   /**< 0..32 volume-modulation amounts */
    uint8_t char_mod[kNumPipes];  /**< 0..32 Character-modulation amounts */

    void SetDefaults()
    {
        for(int p = 0; p < kNumPipes; p++)
        {
            level[p]     = kDefaultLevel;
            character[p] = kDefaultCharacter;
            attack[p]    = kDefaultEnv;
            release[p]   = kDefaultEnv;
            mute[p]      = 0;
            vol_mod[p]   = kDefaultMod;
            char_mod[p]  = kDefaultMod;
        }
        vibrato      = kDefaultVibrato;
        rotary       = kDefaultRotary;
        drive        = kDefaultDrive;
        drive_tone   = kDefaultTone;
        reverb_mix   = kDefaultReverbMix;
        reverb_decay = kDefaultReverbDecay;
    }
};

/** Serialize as compact versioned JSON. Returns the number of bytes
 *  written (excluding the terminator), or 0 when the buffer is too
 *  small (never happens with a >= 384 B buffer). */
inline size_t Serialize(const OrganPreset &p, char *buf, size_t size)
{
    const int n = snprintf(
        buf,
        size,
        "{\"version\":%d,"
        "\"level\":[%u,%u,%u,%u],"
        "\"character\":[%u,%u,%u,%u],"
        "\"attack\":[%u,%u,%u,%u],"
        "\"release\":[%u,%u,%u,%u],"
        "\"mute\":[%u,%u,%u,%u],"
        "\"vibrato\":%u,\"rotary\":%u,\"drive\":%u,"
        "\"drive_tone\":%u,\"reverb_mix\":%u,\"reverb_decay\":%u,"
        "\"vol_mod\":[%u,%u,%u,%u],"
        "\"char_mod\":[%u,%u,%u,%u]}",
        OrganPreset::kVersion,
        (unsigned)p.level[0], (unsigned)p.level[1], (unsigned)p.level[2],
        (unsigned)p.level[3],
        (unsigned)p.character[0], (unsigned)p.character[1],
        (unsigned)p.character[2], (unsigned)p.character[3],
        (unsigned)p.attack[0], (unsigned)p.attack[1], (unsigned)p.attack[2],
        (unsigned)p.attack[3],
        (unsigned)p.release[0], (unsigned)p.release[1],
        (unsigned)p.release[2], (unsigned)p.release[3],
        (unsigned)p.mute[0], (unsigned)p.mute[1], (unsigned)p.mute[2],
        (unsigned)p.mute[3],
        (unsigned)p.vibrato,
        (unsigned)p.rotary,
        (unsigned)p.drive,
        (unsigned)p.drive_tone,
        (unsigned)p.reverb_mix,
        (unsigned)p.reverb_decay,
        (unsigned)p.vol_mod[0], (unsigned)p.vol_mod[1],
        (unsigned)p.vol_mod[2], (unsigned)p.vol_mod[3],
        (unsigned)p.char_mod[0], (unsigned)p.char_mod[1],
        (unsigned)p.char_mod[2], (unsigned)p.char_mod[3]);
    if(n < 0 || static_cast<size_t>(n) >= size)
        return 0;
    return static_cast<size_t>(n);
}

/** Read one integer value for `query`, clamped to [lo, hi]. Missing
 *  keys and non-numeric values yield `fallback`. The buffer is non-const
 *  because coreJSON's JSON_Search takes a char* (it never writes). */
inline int QueryInt(char *      buf,
                    size_t      len,
                    const char *query,
                    int         fallback,
                    int         lo,
                    int         hi)
{
    char * value      = nullptr;
    size_t value_len  = 0;
    if(JSON_Search(buf, len, query, strlen(query), &value, &value_len)
       != JSONSuccess)
        return fallback;
    /* the value is not null-terminated: copy it out for strtol */
    char tmp[12];
    if(value_len >= sizeof(tmp))
        value_len = sizeof(tmp) - 1;
    memcpy(tmp, value, value_len);
    tmp[value_len] = '\0';
    char *end = tmp;
    const long x = strtol(tmp, &end, 10);
    if(end == tmp)
        return fallback; /* not a number (true, "str", ...) */
    if(x < lo)
        return lo;
    if(x > hi)
        return hi;
    return static_cast<int>(x);
}

/** Parse one preset file into `p`. Invalid JSON returns false and `p`
 *  is untouched; valid JSON always succeeds: every key is optional
 *  (missing = boot default), clamped to its range, and unknown keys are
 *  ignored. The "version" key is informational - any version parses, so
 *  newer files degrade gracefully. */
inline bool Parse(char *buf, size_t len, OrganPreset &p)
{
    if(JSON_Validate(buf, len) != JSONSuccess)
        return false;

    OrganPreset out;
    out.SetDefaults();

    /* version is currently informational only (all versions parse) */
    (void)QueryInt(buf, len, "version", OrganPreset::kVersion, 0, 255);

    char query[16];
    for(int i = 0; i < OrganPreset::kNumPipes; i++)
    {
        snprintf(query, sizeof(query), "level[%d]", i);
        out.level[i] = (uint8_t)QueryInt(buf, len, query,
                                         OrganPreset::kDefaultLevel, 0,
                                         OrganPreset::kMaxLevel);
        snprintf(query, sizeof(query), "character[%d]", i);
        out.character[i] = (uint8_t)QueryInt(buf, len, query,
                                             OrganPreset::kDefaultCharacter,
                                             0, OrganPreset::kMaxCharacter);
        snprintf(query, sizeof(query), "attack[%d]", i);
        out.attack[i] = (uint8_t)QueryInt(buf, len, query,
                                          OrganPreset::kDefaultEnv, 0,
                                          OrganPreset::kMaxEnv);
        snprintf(query, sizeof(query), "release[%d]", i);
        out.release[i] = (uint8_t)QueryInt(buf, len, query,
                                           OrganPreset::kDefaultEnv, 0,
                                           OrganPreset::kMaxEnv);
        snprintf(query, sizeof(query), "mute[%d]", i);
        out.mute[i] = (uint8_t)QueryInt(buf, len, query, 0, 0, 1);
        snprintf(query, sizeof(query), "vol_mod[%d]", i);
        out.vol_mod[i] = (uint8_t)QueryInt(buf, len, query,
                                           OrganPreset::kDefaultMod, 0,
                                           OrganPreset::kMaxMod);
        snprintf(query, sizeof(query), "char_mod[%d]", i);
        out.char_mod[i] = (uint8_t)QueryInt(buf, len, query,
                                            OrganPreset::kDefaultMod, 0,
                                            OrganPreset::kMaxMod);
    }
    out.vibrato = (uint8_t)QueryInt(buf, len, "vibrato",
                                    OrganPreset::kDefaultVibrato, 0,
                                    OrganPreset::kMaxVibrato);
    out.rotary  = (uint8_t)QueryInt(buf, len, "rotary",
                                    OrganPreset::kDefaultRotary, 0,
                                    OrganPreset::kMaxRotary);
    out.drive   = (uint8_t)QueryInt(buf, len, "drive",
                                    OrganPreset::kDefaultDrive, 0,
                                    OrganPreset::kMaxDrive);
    out.drive_tone = (uint8_t)QueryInt(buf, len, "drive_tone",
                                       OrganPreset::kDefaultTone, 0,
                                       OrganPreset::kMaxTone);
    out.reverb_mix = (uint8_t)QueryInt(buf, len, "reverb_mix",
                                       OrganPreset::kDefaultReverbMix, 0,
                                       OrganPreset::kMaxReverbMix);
    out.reverb_decay = (uint8_t)QueryInt(buf, len, "reverb_decay",
                                         OrganPreset::kDefaultReverbDecay, 0,
                                         OrganPreset::kMaxReverbDecay);
    p = out;
    return true;
}

/** Snapshot the engine state into a preset. Templated so host tests can
 *  pass a fake engine with the same getters. */
template <typename Engine>
void CaptureFromEngine(OrganPreset &p, const Engine &e)
{
    for(int i = 0; i < OrganPreset::kNumPipes; i++)
    {
        p.level[i]     = (uint8_t)e.GetPipeStep(i);
        p.character[i] = (uint8_t)e.GetPipeCharacterStep(i);
        p.attack[i]    = (uint8_t)e.GetPipeAttackStep(i);
        p.release[i]   = (uint8_t)e.GetPipeReleaseStep(i);
        p.mute[i]      = e.GetPipeMuted(i) ? 1 : 0;
        p.vol_mod[i]   = (uint8_t)e.GetPipeVolModStep(i);
        p.char_mod[i]  = (uint8_t)e.GetPipeCharModStep(i);
    }
    p.vibrato      = (uint8_t)e.GetVibratoDepth();
    p.rotary       = (uint8_t)e.GetRotaryStep();
    p.drive        = (uint8_t)e.GetDriveStep();
    p.drive_tone   = (uint8_t)e.GetDriveTonePercent();
    p.reverb_mix   = (uint8_t)e.GetReverbMixPercent();
    p.reverb_decay = (uint8_t)e.GetReverbDecayStep();
}

/** Push a preset into the engine through the regular setters (all ramped
 *  or phase-continuous, so held and latched notes keep sounding
 *  click-free). The modulation amounts go first so the level/Character
 *  setters derive their final targets with the loaded amounts. */
template <typename Engine>
void ApplyToEngine(const OrganPreset &p, Engine &e)
{
    for(int i = 0; i < OrganPreset::kNumPipes; i++)
    {
        e.SetPipeVolModStep(i, p.vol_mod[i]);
        e.SetPipeCharModStep(i, p.char_mod[i]);
        e.SetPipeStep(i, p.level[i]);
        e.SetPipeCharacterStep(i, p.character[i]);
        e.SetPipeAttackStep(i, p.attack[i]);
        e.SetPipeReleaseStep(i, p.release[i]);
        e.SetPipeMuted(i, p.mute[i] != 0);
    }
    e.SetVibratoDepth(p.vibrato);
    e.SetRotaryStep(p.rotary);
    e.SetDriveStep(p.drive);
    e.SetDriveTonePercent(p.drive_tone);
    e.SetReverbMixPercent(p.reverb_mix);
    e.SetReverbDecayStep(p.reverb_decay);
}

/** The RAM cache of all 15 slots. The audio ISR mutates it (user
 *  gestures); the main-loop storage reads/snapshots it. A dirty bit per
 *  slot marks content that still needs to hit the SD card: set for
 *  save/copy/erase, cleared by the storage once the slot's file write
 *  (or delete) has finished. A dirty but invalid slot means "delete the
 *  file". */
class OrganPresetBank
{
  public:
    static constexpr int kNumSlots = 15;

    void Init()
    {
        for(int i = 0; i < kNumSlots; i++)
        {
            slots_[i].SetDefaults();
            valid_[i] = false;
        }
        dirty_mask_ = 0;
    }

    bool IsValid(int slot) const
    {
        return slot >= 0 && slot < kNumSlots && valid_[slot];
    }

    const OrganPreset &Get(int slot) const { return slots_[slot]; }

    void Save(int slot, const OrganPreset &p)
    {
        if(slot < 0 || slot >= kNumSlots)
            return;
        slots_[slot] = p;
        valid_[slot] = true;
        dirty_mask_ |= Bit(slot);
    }

    void Copy(int src, int dst)
    {
        if(src < 0 || src >= kNumSlots || dst < 0 || dst >= kNumSlots
           || src == dst || !valid_[src])
            return;
        slots_[dst] = slots_[src];
        valid_[dst] = true;
        dirty_mask_ |= Bit(dst);
    }

    void Erase(int slot)
    {
        if(slot < 0 || slot >= kNumSlots)
            return;
        valid_[slot] = false;
        dirty_mask_ |= Bit(slot); /* pending file delete */
    }

    /** Boot/remount load from the card: fills a slot WITHOUT dirtying it
     *  (the card already holds this content). */
    void LoadSlot(int slot, const OrganPreset &p)
    {
        if(slot < 0 || slot >= kNumSlots)
            return;
        slots_[slot] = p;
        valid_[slot] = true;
    }

    int NextDirtySlot() const
    {
        for(int i = 0; i < kNumSlots; i++)
            if(dirty_mask_ & Bit(i))
                return i;
        return -1;
    }

    bool IsDirty(int slot) const
    {
        return slot >= 0 && slot < kNumSlots && (dirty_mask_ & Bit(slot));
    }

    /** Dirty-bit maintenance for the storage: main loop only, with IRQs
     *  off (the ISR's |= is safe on its own - the main loop can never
     *  interrupt it). */
    void MarkDirty(int slot) { dirty_mask_ |= Bit(slot); }
    void ClearDirty(int slot) { dirty_mask_ &= ~Bit(slot); }

  private:
    static constexpr uint32_t Bit(int slot) { return 1u << slot; }

    OrganPreset slots_[kNumSlots];
    bool        valid_[kNumSlots];
    uint32_t    dirty_mask_;
};

} // namespace chompi

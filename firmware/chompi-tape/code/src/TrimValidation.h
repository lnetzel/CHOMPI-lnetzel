#pragma once
#include <stddef.h>
#include <stdint.h>

namespace daisy
{

/** Result of validating a proposed trim boundary change.
 *  Shared by FileSampleReader validation/commit and the engine preflight.
 */
enum class TrimCheck
{
    kUnchanged,    /**< matches the current boundary: success, no state changes */
    kAccept,       /**< valid change: safe to commit */
    kRejectOrder,  /**< boundary ordering or minimum length violated */
    kRejectActive, /**< collides with the active playback read region, or the
                        region cannot be bounded due to pending I/O */
};

/** WAV header bytes excluded from SD payload math.
 *  Must equal sizeof(WAV_FormatTypeDef); static_assert'ed at the use site
 *  (SampleReader.h), where the real definition is available.
 */
static constexpr uint32_t kTrimWavHeaderBytes = 44;
static constexpr uint32_t kTrimMinDurationMs = 5;

constexpr uint32_t TrimMinFramesForRate(uint32_t sample_rate)
{
    return (sample_rate * kTrimMinDurationMs + 999u) / 1000u;
}

/** Side-effect-free snapshot of everything trim validation needs for one voice.
 *  Stereo frames are the common unit for boundary math: 4 bytes of SD payload,
 *  or 2 int16 RAM elements, per frame.
 */
struct TrimVoiceState
{
    uint32_t payload_units;   /**< SD: payload bytes (file size - WAV header).
                                   RAM: int16 element count. */
    uint32_t units_per_frame; /**< SD: 4. RAM: 2. */
    bool     payload_known;   /**< false while file metadata is unavailable
                                   (e.g. an open request is outstanding) */
    bool     using_ram;
    bool     active;          /**< voice is playing or starting */
    bool     reverse;
    uint32_t file_pos_bytes;  /**< absolute SD read position incl. header (f_tell) */
    uint32_t last_read_bytes; /**< size of the most recent read block */
    uint32_t pending_io;      /**< outstanding open + seek + read requests */
    float    start_norm;      /**< current normalized start boundary */
    float    end_norm;        /**< current normalized end boundary */
};

constexpr float TrimClampNorm(float val)
{
    return val < 0.f ? 0.f : (val > 1.f ? 1.f : val);
}

/** normalized [0,1] boundary -> aligned stereo-frame index within the payload.
 *  Mirrors the existing align-then-divide arithmetic of FileSampleReader.
 */
constexpr uint32_t TrimNormToFrames(float val,
                                    uint32_t payload_units,
                                    uint32_t units_per_frame)
{
    const uint32_t raw     = (uint32_t)(TrimClampNorm(val) * (float)payload_units);
    const uint32_t aligned = raw - (raw % units_per_frame);
    return aligned / units_per_frame;
}

/** stereo-frame index -> absolute SD byte position, including the WAV header */
constexpr uint32_t TrimFramesToAbsBytes(uint32_t frames)
{
    return kTrimWavHeaderBytes + frames * 4;
}

constexpr bool TrimCheckPasses(TrimCheck check)
{
    return check == TrimCheck::kUnchanged || check == TrimCheck::kAccept;
}

/** Boundary ordering + minimum loop length, in stereo frames.
 *  Ordering is checked before the subtraction so short/empty files
 *  cannot wrap the unsigned minimum-length comparison.
 */
constexpr TrimCheck TrimCheckOrdering(uint32_t start_frames,
                                      uint32_t end_frames,
                                      uint32_t min_loop_frames)
{
    return start_frames >= end_frames
               ? TrimCheck::kRejectOrder
               : (end_frames - start_frames < min_loop_frames
                      ? TrimCheck::kRejectOrder
                      : TrimCheck::kAccept);
}

/** Validate a proposed normalized START boundary against a voice snapshot.
 *  Pure: the caller commits only on kAccept and treats kUnchanged as success.
 */
constexpr TrimCheck ValidateTrimStart(const TrimVoiceState& s,
                                      float               new_norm,
                                      uint32_t            min_loop_frames,
                                      uint32_t            margin_bytes)
{
    const float clamped = TrimClampNorm(new_norm);

    // no-op: success without invalidation, regardless of playback state
    if(clamped == s.start_norm)
        return TrimCheck::kUnchanged;

    // file metadata unavailable: maintain valid normalized boundaries,
    // without any unsigned size math
    if(!s.payload_known)
        return clamped < s.end_norm ? TrimCheck::kAccept
                                    : TrimCheck::kRejectOrder;

    const uint32_t start_f = TrimNormToFrames(clamped, s.payload_units,
                                              s.units_per_frame);
    const uint32_t end_f   = TrimNormToFrames(s.end_norm, s.payload_units,
                                              s.units_per_frame);

    const TrimCheck order = TrimCheckOrdering(start_f, end_f, min_loop_frames);
    if(order != TrimCheck::kAccept)
        return order;

    // Playback-collision guard: the start boundary is the playback *exit*
    // only in reverse, and only inward (increasing) moves can smash into the
    // read region. Outward moves and edits on inactive voices stay usable.
    if(s.reverse && !s.using_ram && s.active && clamped > s.start_norm)
    {
        // the queued/audible region cannot be bounded while I/O is outstanding
        if(s.pending_io != 0 || s.last_read_bytes > s.file_pos_bytes)
            return TrimCheck::kRejectActive;

        // lower edge of the most recent reverse block in absolute bytes;
        // the margin absorbs the queued FIFO plus head-room
        const uint32_t lower_edge = s.file_pos_bytes - s.last_read_bytes;
        const uint32_t start_abs  = TrimFramesToAbsBytes(start_f);

        if(lower_edge < margin_bytes || start_abs > lower_edge - margin_bytes)
            return TrimCheck::kRejectActive;
    }

    return TrimCheck::kAccept;
}

/** Validate a proposed normalized END boundary against a voice snapshot.
 *  Pure: the caller commits only on kAccept and treats kUnchanged as success.
 */
constexpr TrimCheck ValidateTrimEnd(const TrimVoiceState& s,
                                    float               new_norm,
                                    uint32_t            min_loop_frames,
                                    uint32_t            margin_bytes)
{
    const float clamped = TrimClampNorm(new_norm);

    // no-op: success without invalidation, regardless of playback state
    if(clamped == s.end_norm)
        return TrimCheck::kUnchanged;

    // file metadata unavailable: maintain valid normalized boundaries,
    // without any unsigned size math
    if(!s.payload_known)
        return s.start_norm < clamped ? TrimCheck::kAccept
                                      : TrimCheck::kRejectOrder;

    const uint32_t start_f = TrimNormToFrames(s.start_norm, s.payload_units,
                                              s.units_per_frame);
    const uint32_t end_f   = TrimNormToFrames(clamped, s.payload_units,
                                              s.units_per_frame);

    const TrimCheck order = TrimCheckOrdering(start_f, end_f, min_loop_frames);
    if(order != TrimCheck::kAccept)
        return order;

    // Playback-collision guard: the end boundary is the playback *exit*
    // only in forward playback, and only inward (shrinking) moves can collide
    // with the already-read/queued region below the read position.
    if(!s.reverse && !s.using_ram && s.active && clamped < s.end_norm)
    {
        if(s.pending_io != 0)
            return TrimCheck::kRejectActive;

        const uint32_t end_abs = TrimFramesToAbsBytes(end_f);
        if((uint64_t)s.file_pos_bytes + margin_bytes > end_abs)
            return TrimCheck::kRejectActive;
    }

    return TrimCheck::kAccept;
}

/** All-or-none preflight of a START change across every targeted voice.
 *  Used by the engine transaction: if any target rejects, nothing is mutated.
 */
constexpr bool TrimPreflightStart(const TrimVoiceState* voices,
                                  size_t                count,
                                  float                 new_norm,
                                  uint32_t              min_loop_frames,
                                  uint32_t              margin_bytes)
{
    for(size_t i = 0; i < count; i++)
    {
        if(!TrimCheckPasses(ValidateTrimStart(voices[i], new_norm,
                                              min_loop_frames, margin_bytes)))
            return false;
    }
    return true;
}

/** All-or-none preflight of an END change across every targeted voice. */
constexpr bool TrimPreflightEnd(const TrimVoiceState* voices,
                                size_t                count,
                                float                 new_norm,
                                uint32_t              min_loop_frames,
                                uint32_t              margin_bytes)
{
    for(size_t i = 0; i < count; i++)
    {
        if(!TrimCheckPasses(ValidateTrimEnd(voices[i], new_norm,
                                            min_loop_frames, margin_bytes)))
            return false;
    }
    return true;
}

} // namespace daisy

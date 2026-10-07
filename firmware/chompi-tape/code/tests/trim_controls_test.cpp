/** Host tests for the TAPE trim-boundary validation helper (TrimValidation.h).
 *
 *  These exercise the production validation arithmetic shared by
 *  FileSampleReader::SetStartPoint/SetEndPoint and the DSPEngine atomic
 *  preflight. The helpers are constexpr, so the threshold-sensitive cases are
 *  also checked as static_asserts at compile time (see the bottom of file).
 *
 *  Test constants are chosen for exact float32 behavior: payload 262144 bytes
 *  (2^18) = 65536 stereo frames, so any normalized value k/65536 is exact.
 *
 *  Build & run:  make -C firmware/chompi-tape/code/tests test
 */

#include <cstdio>
#include "TrimValidation.h"

using namespace daisy;

namespace
{

int failures = 0;

#define CHECK(cond)                                     \
    do                                                  \
    {                                                   \
        if(!(cond))                                     \
        {                                               \
            ++failures;                                 \
            std::printf("FAIL %s:%d: %s\n", __FILE__,   \
                        __LINE__, #cond);               \
        }                                               \
    } while(0)

// production constants under test (see SampleReader.h / FileStreamingManager.h)
constexpr uint32_t kHeader  = kTrimWavHeaderBytes; // 44
constexpr uint32_t kMargin  = 16384; // kMaxFileStreamingSamps * sizeof(int16_t)
constexpr uint32_t kMinLoop = TrimMinFramesForRate(48000);

// reference files
constexpr uint32_t kPayload1s   = 192000;  // 1 s stereo 16-bit @ 48 kHz
constexpr uint32_t kPayloadBig  = 262144;  // 65536 frames, float-exact norms
constexpr uint32_t kFilePosMid  = kHeader + 131072; // 131116

TrimVoiceState MakeSd(uint32_t payload_bytes)
{
    TrimVoiceState s    = {};
    s.payload_units     = payload_bytes;
    s.units_per_frame   = 4;
    s.payload_known     = true;
    s.using_ram         = false;
    s.active            = false;
    s.reverse           = false;
    s.file_pos_bytes    = 0;
    s.last_read_bytes   = 0;
    s.pending_io        = 0;
    s.start_norm        = 0.f;
    s.end_norm          = 1.f;
    return s;
}

constexpr TrimVoiceState MakeSdC(uint32_t payload_bytes,
                                 bool     active,
                                 bool     reverse,
                                 uint32_t file_pos,
                                 uint32_t last_read,
                                 uint32_t pending,
                                 float    start_norm,
                                 float    end_norm)
{
    TrimVoiceState s    = {};
    s.payload_units     = payload_bytes;
    s.units_per_frame   = 4;
    s.payload_known     = true;
    s.using_ram         = false;
    s.active            = active;
    s.reverse           = reverse;
    s.file_pos_bytes    = file_pos;
    s.last_read_bytes   = last_read;
    s.pending_io        = pending;
    s.start_norm        = start_norm;
    s.end_norm          = end_norm;
    return s;
}

TrimVoiceState MakeRam(uint32_t elements)
{
    TrimVoiceState s    = {};
    s.payload_units     = elements;
    s.units_per_frame   = 2;
    s.payload_known     = true;
    s.using_ram         = true;
    s.active            = false;
    s.reverse           = false;
    s.file_pos_bytes    = 0;
    s.last_read_bytes   = 0;
    s.pending_io        = 0;
    s.start_norm        = 0.f;
    s.end_norm          = 1.f;
    return s;
}

void TestUnitConversion()
{
    // 1-second 48 kHz stereo sample: 48000 frames, 192000 payload bytes
    CHECK(TrimNormToFrames(1.0f, kPayload1s, 4) == 48000);
    CHECK(TrimNormToFrames(0.5f, kPayload1s, 4) == 24000);
    // WAV header offset: end of that file in absolute bytes
    CHECK(TrimFramesToAbsBytes(48000) == 192000 + kHeader);
    // alignment: 2 extra payload bytes truncate down to the frame grid
    CHECK(TrimNormToFrames(1.0f, kPayload1s + 2, 4) == 48000);
    // RAM: GetSize() counts int16 elements; 96000 elements = 48000 frames
    CHECK(TrimNormToFrames(1.0f, 96000, 2) == 48000);
    CHECK(TrimNormToFrames(0.5f, 96000, 2) == 24000);
    // out-of-range normalized input clamps
    CHECK(TrimNormToFrames(1.5f, kPayload1s, 4) == 48000);
    CHECK(TrimNormToFrames(-0.5f, kPayload1s, 4) == 0);
    CHECK(kHeader == 44);
}

void TestOrderingAndMinLength()
{
    TrimVoiceState s = MakeSd(kPayloadBig);

    // start >= end rejects without unsigned wrap
    s.end_norm = 0.5f;
    CHECK(ValidateTrimStart(s, 0.6f, kMinLoop, kMargin) == TrimCheck::kRejectOrder);
    CHECK(ValidateTrimStart(s, 0.5f, kMinLoop, kMargin) == TrimCheck::kRejectOrder);
    s.end_norm = 0.4f;
    CHECK(ValidateTrimEnd(s, 0.4f, kMinLoop, kMargin) == TrimCheck::kRejectOrder);

    // minimum length: file with exactly kMinLoop frames accepts end=1,
    // one frame less rejects
    TrimVoiceState sh = MakeSd(kMinLoop * 4); // 16384 bytes = 4096 frames
    sh.end_norm       = 0.75f;
    CHECK(ValidateTrimEnd(sh, 1.0f, kMinLoop, kMargin) == TrimCheck::kAccept);
    const float one_less = 4095.f / 4096.f;
    CHECK(ValidateTrimEnd(sh, one_less, kMinLoop, kMargin) == TrimCheck::kRejectOrder);

    // header-only file: payload is zero, every change rejects on ordering
    TrimVoiceState ho = MakeSd(0);
    CHECK(ValidateTrimStart(ho, 0.5f, kMinLoop, kMargin) == TrimCheck::kRejectOrder);
    CHECK(ValidateTrimEnd(ho, 0.5f, kMinLoop, kMargin) == TrimCheck::kRejectOrder);
    // ...but a no-op still succeeds without invalidation
    CHECK(ValidateTrimEnd(ho, 1.0f, kMinLoop, kMargin) == TrimCheck::kUnchanged);
    CHECK(ValidateTrimStart(ho, 0.0f, kMinLoop, kMargin) == TrimCheck::kUnchanged);
}

void TestForwardEndMargins()
{
    // active forward playback; read position at byte kFilePosMid
    TrimVoiceState s = MakeSd(kPayloadBig);
    s.active         = true;
    s.file_pos_bytes = kFilePosMid;

    // exact threshold: end_abs == file_pos + margin -> accept
    const float at_threshold = 36864.f / 65536.f; // abs = 147500
    CHECK(ValidateTrimEnd(s, at_threshold, kMinLoop, kMargin) == TrimCheck::kAccept);
    // one frame inside the margin -> reject
    const float below_threshold = 36863.f / 65536.f;
    CHECK(ValidateTrimEnd(s, below_threshold, kMinLoop, kMargin)
          == TrimCheck::kRejectActive);

    // outward moves are never blocked by the read position
    TrimVoiceState out       = s;
    out.end_norm             = 0.5f;
    out.file_pos_bytes       = 200000;
    CHECK(ValidateTrimEnd(out, 0.75f, kMinLoop, kMargin) == TrimCheck::kAccept);

    // inward move with identical numbers but inactive voice -> accept
    TrimVoiceState inact       = s;
    inact.active             = false;
    inact.file_pos_bytes     = 200000;
    CHECK(ValidateTrimEnd(inact, 0.25f, kMinLoop, kMargin) == TrimCheck::kAccept);

    // outstanding I/O makes the region unbounded -> reject
    TrimVoiceState pend = s;
    pend.pending_io    = 1;
    CHECK(ValidateTrimEnd(pend, at_threshold, kMinLoop, kMargin)
          == TrimCheck::kRejectActive);

    // no-op succeeds even with the read position past the boundary
    TrimVoiceState noop       = s;
    noop.end_norm            = 0.5f;
    noop.file_pos_bytes      = 200000;
    CHECK(ValidateTrimEnd(noop, 0.5f, kMinLoop, kMargin) == TrimCheck::kUnchanged);

    // in reverse the end is the *entry* boundary: no collision guard
    TrimVoiceState rev       = s;
    rev.reverse              = true;
    rev.end_norm             = 0.5f;
    rev.file_pos_bytes       = 200000;
    CHECK(ValidateTrimEnd(rev, 0.25f, kMinLoop, kMargin) == TrimCheck::kAccept);
}

void TestReverseStartMargins()
{
    // active reverse playback; lower edge = file_pos - last_read
    TrimVoiceState s = MakeSd(kPayloadBig);
    s.active          = true;
    s.reverse         = true;
    s.file_pos_bytes  = kFilePosMid;
    s.last_read_bytes = kMargin; // lower edge = 114732

    // exact threshold: start_abs == lower_edge - margin -> accept
    const float at_threshold = 24576.f / 65536.f; // abs = 98348
    CHECK(ValidateTrimStart(s, at_threshold, kMinLoop, kMargin)
          == TrimCheck::kAccept);
    // one frame past the threshold -> reject
    const float past_threshold = 24577.f / 65536.f;
    CHECK(ValidateTrimStart(s, past_threshold, kMinLoop, kMargin)
          == TrimCheck::kRejectActive);

    // outward (decreasing) moves are never blocked
    TrimVoiceState out = s;
    out.start_norm     = 0.5f;
    CHECK(ValidateTrimStart(out, 0.25f, kMinLoop, kMargin) == TrimCheck::kAccept);

    // outstanding I/O makes the region unbounded -> reject
    TrimVoiceState pend = s;
    pend.pending_io     = 1;
    CHECK(ValidateTrimStart(pend, at_threshold, kMinLoop, kMargin)
          == TrimCheck::kRejectActive);

    // last_read_size_ larger than the position: cannot bound -> reject
    TrimVoiceState under = s;
    under.file_pos_bytes = 100;
    under.last_read_bytes = 200;
    CHECK(ValidateTrimStart(under, at_threshold, kMinLoop, kMargin)
          == TrimCheck::kRejectActive);

    // in forward playback the start is the *entry* boundary: no collision
    // guard even when the read position is past the proposed boundary
    TrimVoiceState fwd = MakeSd(kPayloadBig);
    fwd.active          = true;
    fwd.file_pos_bytes  = 250000;
    CHECK(ValidateTrimStart(fwd, 0.9f, kMinLoop, kMargin) == TrimCheck::kAccept);

    // inactive voice: valid edits stay usable
    TrimVoiceState inact = s;
    inact.active         = false;
    CHECK(ValidateTrimStart(inact, past_threshold, kMinLoop, kMargin)
          == TrimCheck::kAccept);

    // no-op succeeds regardless of state
    TrimVoiceState noop = s;
    noop.start_norm     = 0.375f;
    CHECK(ValidateTrimStart(noop, 0.375f, kMinLoop, kMargin)
          == TrimCheck::kUnchanged);
}

void TestMetadataUnavailable()
{
    TrimVoiceState s = MakeSd(kPayloadBig);
    s.payload_known   = false;

    CHECK(ValidateTrimStart(s, 0.5f, kMinLoop, kMargin) == TrimCheck::kAccept);
    CHECK(ValidateTrimEnd(s, 0.8f, kMinLoop, kMargin) == TrimCheck::kAccept);

    // normalized ordering still enforced, with no size math involved
    s.end_norm   = 0.4f;
    CHECK(ValidateTrimStart(s, 0.5f, kMinLoop, kMargin) == TrimCheck::kRejectOrder);
    TrimVoiceState s2 = s;
    s2.start_norm = 0.5f;
    CHECK(ValidateTrimEnd(s2, 0.1f, kMinLoop, kMargin) == TrimCheck::kRejectOrder);

    // no-op short-circuits before metadata handling
    TrimVoiceState s3 = s;
    s3.end_norm       = 1.f;
    CHECK(ValidateTrimEnd(s3, 1.0f, kMinLoop, kMargin) == TrimCheck::kUnchanged);
}

void TestRamPath()
{
    TrimVoiceState r = MakeRam(131072); // elements -> 65536 frames

    // minimum length applies in stereo frames, not elements
        CHECK(ValidateTrimStart(r, (65536.f - kMinLoop) / 65536.f,
                        kMinLoop, kMargin)
          == TrimCheck::kAccept);
        CHECK(ValidateTrimStart(r, (65537.f - kMinLoop) / 65536.f,
                        kMinLoop, kMargin)
          == TrimCheck::kRejectOrder);

    // RAM has no streaming collision guard, even active + reverse + inward
    r.reverse        = true;
    r.active         = true;
    r.file_pos_bytes = 200000; // meaningless for RAM, must be ignored
    CHECK(ValidateTrimStart(r, 0.5f, kMinLoop, kMargin) == TrimCheck::kAccept);
    CHECK(ValidateTrimEnd(r, 0.25f, kMinLoop, kMargin) == TrimCheck::kAccept);
}

void TestPreflightAggregation()
{
    // seven JAMMI voices, all valid -> pass
    TrimVoiceState voices[7];
    for(auto& v : voices)
        v = MakeSd(kPayloadBig);
    CHECK(TrimPreflightStart(voices, 7, 0.25f, kMinLoop, kMargin));
    CHECK(TrimPreflightEnd(voices, 7, 0.75f, kMinLoop, kMargin));

    // one rejecting voice vetoes the whole update (all-or-none)
    voices[3].active         = true;
    voices[3].reverse        = true;
    voices[3].file_pos_bytes = kFilePosMid;
    voices[3].last_read_bytes = kMargin;
    CHECK(!TrimPreflightStart(voices, 7, 24577.f / 65536.f, kMinLoop, kMargin));

    // mixed end preflight: inactive voices pass where active ones reject
    TrimVoiceState ends[7];
    for(auto& v : ends)
    {
        v                = MakeSd(kPayloadBig);
        v.end_norm       = 1.0f;
        v.file_pos_bytes = 200000; // past any proposed end
        v.active         = false;  // stopped/cached: stays usable
    }
    ends[5].active = true; // one playing voice would collide
    CHECK(!TrimPreflightEnd(ends, 7, 0.25f, kMinLoop, kMargin));
    ends[5].active = false;
    CHECK(TrimPreflightEnd(ends, 7, 0.25f, kMinLoop, kMargin));

    // CUBBI targets only the latest voice (count == 1)
    TrimVoiceState cubbi = MakeSd(kPayloadBig);
    CHECK(TrimPreflightStart(&cubbi, 1, 0.25f, kMinLoop, kMargin));
    cubbi.pending_io     = 1;
    cubbi.active         = true;
    cubbi.reverse        = true;
    cubbi.file_pos_bytes = kFilePosMid;
    cubbi.last_read_bytes = kMargin;
    CHECK(!TrimPreflightStart(&cubbi, 1, 24577.f / 65536.f, kMinLoop, kMargin));
}

/** Compile-time verification of the exact margin thresholds and unit math.
 *  A plain compile of this translation unit (any compiler, including
 *  arm-none-eabi-g++ -fsyntax-only) checks these.
 */
namespace compiletime
{
    static_assert(kMinLoop == 240, "5 ms at 48 kHz is 240 frames");
    static_assert(TrimMinFramesForRate(44100) == 221,
                  "minimum frame count rounds up to preserve 5 ms");
    static_assert(TrimNormToFrames(1.0f, kPayload1s, 4) == 48000,
                  "1s @ 48k stereo is 48000 frames");
    static_assert(TrimFramesToAbsBytes(48000) == 192044,
                  "WAV header offset in absolute byte math");

    // forward end threshold: file_pos + margin is inclusive-acceptable
    static_assert(ValidateTrimEnd(MakeSdC(kPayloadBig, true, false, kFilePosMid,
                                          0, 0, 0.f, 1.f),
                                  36864.f / 65536.f, kMinLoop, kMargin)
                      == TrimCheck::kAccept,
                  "end exactly at file_pos + margin must be accepted");
    static_assert(ValidateTrimEnd(MakeSdC(kPayloadBig, true, false, kFilePosMid,
                                          0, 0, 0.f, 1.f),
                                  36863.f / 65536.f, kMinLoop, kMargin)
                      == TrimCheck::kRejectActive,
                  "end one frame inside the margin must be rejected");

    // reverse start threshold: lower_edge - margin is inclusive-acceptable
    static_assert(ValidateTrimStart(MakeSdC(kPayloadBig, true, true, kFilePosMid,
                                            kMargin, 0, 0.f, 1.f),
                                    24576.f / 65536.f, kMinLoop, kMargin)
                      == TrimCheck::kAccept,
                  "start exactly at lower_edge - margin must be accepted");
    static_assert(ValidateTrimStart(MakeSdC(kPayloadBig, true, true, kFilePosMid,
                                            kMargin, 0, 0.f, 1.f),
                                    24577.f / 65536.f, kMinLoop, kMargin)
                      == TrimCheck::kRejectActive,
                  "start one frame past the margin must be rejected");

    // start >= end rejects without unsigned wrap
    static_assert(ValidateTrimStart(MakeSdC(kPayloadBig, false, false, 0, 0, 0,
                                            0.f, 0.5f),
                                    0.6f, kMinLoop, kMargin)
                      == TrimCheck::kRejectOrder,
                  "start >= end must reject");
} // namespace compiletime

} // namespace

int main()
{
    TestUnitConversion();
    TestOrderingAndMinLength();
    TestForwardEndMargins();
    TestReverseStartMargins();
    TestMetadataUnavailable();
    TestRamPath();
    TestPreflightAggregation();

    if(failures == 0)
        std::printf("All trim control tests passed.\n");
    else
        std::printf("%d trim control test(s) FAILED.\n", failures);

    return failures == 0 ? 0 : 1;
}

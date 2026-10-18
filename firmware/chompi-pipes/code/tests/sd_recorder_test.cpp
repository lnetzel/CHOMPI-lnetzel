/** @file sd_recorder_test.cpp
 *  @brief Native tests for the host-testable core of SdRecorder.h (the
 *  CHOMPI-PIPES record-to-SD): the float->int16 conversion with
 *  clamping, the SPSC ring (wrap, full-refusal, ordering, stereo
 *  interleave), the next-free-take-number rule with the 99->100
 *  rollover, the take file name/path formatting, the canonical WAV
 *  header bytes + patched sizes and the per-take byte-limit auto-stop.
 *
 *  Build & run: make -C firmware/chompi-pipes/code/tests test
 */
#define CHOMPI_SD_RECORDER_HOST 1
#include "SdRecorder.h"
#include <cassert>
#include <cstdio>
#include <cstring>

using namespace chompi;

static uint32_t rd32(const uint8_t *h)
{
    return static_cast<uint32_t>(h[0])
           | (static_cast<uint32_t>(h[1]) << 8)
           | (static_cast<uint32_t>(h[2]) << 16)
           | (static_cast<uint32_t>(h[3]) << 24);
}

static uint16_t rd16(const uint8_t *h)
{
    return static_cast<uint16_t>(h[0] | (h[1] << 8));
}

static void test_f2s16_clamping()
{
    assert(SdrF2S16(0.f) == 0);
    assert(SdrF2S16(1.f) == 32767);
    assert(SdrF2S16(-1.f) == -32767);
    assert(SdrF2S16(2.f) == 32767);   /* clamped */
    assert(SdrF2S16(-2.f) == -32767); /* clamped */
    assert(SdrF2S16(100.f) == 32767);
    assert(SdrF2S16(-100.f) == -32767);
    assert(SdrF2S16(.5f) == 16383); /* truncates, never rounds */
    assert(SdrF2S16(-.5f) == -16383);
}

static void test_ring_wrap_and_order()
{
    int16_t buf[128];
    SdrRing ring;
    ring.Init(buf, 128);
    assert(ring.Count() == 0u);
    assert(!ring.Overflow());

    int16_t src[192];
    for(int i = 0; i < 192; i++)
        src[i] = static_cast<int16_t>(i * 5 - 300);

    /* push 96, pop 64, push 96: the head wraps through index 0 */
    int16_t dst[192];
    assert(ring.Push(src, 96) == 96u);
    assert(ring.Pop(dst, 64) == 64u);
    assert(ring.Push(src + 96, 96) == 96u);
    assert(ring.Count() == 128u);

    /* popping everything preserves the exact sample order (SPSC) */
    assert(ring.Pop(dst, 192) == 128u); /* max > count: drains count */
    for(int i = 0; i < 128; i++)
        assert(dst[i] == src[64 + i]);
    assert(ring.Count() == 0u);
}

static void test_ring_full_refusal()
{
    int16_t buf[64];
    SdrRing ring;
    ring.Init(buf, 64);

    int16_t src[64];
    for(int i = 0; i < 64; i++)
        src[i] = static_cast<int16_t>(i + 1);

    /* fill to capacity */
    assert(ring.Push(src, 64) == 64u);
    assert(ring.Count() == 64u);

    /* a full ring refuses the whole push: overflow flag, no overwrite */
    const int16_t extra[8] = {1000, 1001, 1002, 1003, 1004, 1005, 1006,
                              1007};
    assert(ring.Push(extra, 1) == 0u);
    assert(ring.Overflow());
    assert(ring.Count() == 64u);

    /* drain 4, then a push bigger than the 4 free slots: refused whole */
    int16_t dst[64];
    assert(ring.Pop(dst, 4) == 4u);
    assert(ring.Push(extra, 8) == 0u);
    assert(ring.Count() == 60u);

    /* the surviving content is untouched */
    assert(ring.Pop(dst, 64) == 60u);
    for(int i = 0; i < 60; i++)
        assert(dst[i] == src[4 + i]);

    /* Reset clears the counters and the overflow flag */
    ring.Reset();
    assert(ring.Count() == 0u);
    assert(!ring.Overflow());
    assert(ring.Push(src, 64) == 64u);
}

static void test_ring_stereo_interleave()
{
    int16_t buf[16];
    SdrRing ring;
    ring.Init(buf, 16);

    const float l[4] = {0.f, .5f, -.5f, 1.f};
    const float r[4] = {1.f, -1.f, 0.f, 2.f}; /* 2.f must clamp */
    assert(ring.PushStereo(l, r, 4) == 4u);
    assert(ring.Count() == 8u);

    int16_t dst[8];
    assert(ring.Pop(dst, 8) == 8u);
    assert(dst[0] == 0 && dst[1] == 32767);       /* L R */
    assert(dst[2] == 16383 && dst[3] == -32767);  /* L R */
    assert(dst[4] == -16383 && dst[5] == 0);      /* L R */
    assert(dst[6] == 32767 && dst[7] == 32767);   /* L, R clamped */

    /* two more frames fit exactly; a further frame is refused whole */
    assert(ring.PushStereo(l, r, 4) == 4u);
    assert(ring.PushStereo(l, r, 4) == 4u);
    assert(ring.Count() == 16u);
    assert(ring.PushStereo(l, r, 1) == 0u);
    assert(ring.Overflow());
}

static void test_next_free_number()
{
    bool exists[kSdRecMaxNumber];

    /* empty directory -> 1 */
    memset(exists, 0, sizeof(exists));
    assert(SdrNextFreeNumber(exists, kSdRecMaxNumber) == 1);

    /* 1 taken -> 2 */
    exists[0] = true;
    assert(SdrNextFreeNumber(exists, kSdRecMaxNumber) == 2);

    /* a gap wins over every higher free number */
    memset(exists, 0, sizeof(exists));
    exists[0] = true; /* 1 */
    exists[1] = true; /* 2 */
    exists[3] = true; /* 4 */
    assert(SdrNextFreeNumber(exists, kSdRecMaxNumber) == 3);

    /* 99 -> 100 rollover: 1..99 taken -> 100 */
    memset(exists, 0, sizeof(exists));
    for(int i = 0; i < 99; i++)
        exists[i] = true;
    assert(SdrNextFreeNumber(exists, kSdRecMaxNumber) == 100);

    /* every name taken -> -1 (the UI flashes red, nothing is
     * overwritten) */
    memset(exists, 1, sizeof(exists));
    assert(SdrNextFreeNumber(exists, kSdRecMaxNumber) == -1);
}

static void test_name_format()
{
    char name[24];
    SdrRecFileName(1, name);
    assert(strcmp(name, "Recording 01.wav") == 0);
    SdrRecFileName(9, name);
    assert(strcmp(name, "Recording 09.wav") == 0);
    SdrRecFileName(10, name);
    assert(strcmp(name, "Recording 10.wav") == 0);
    SdrRecFileName(99, name);
    assert(strcmp(name, "Recording 99.wav") == 0);
    /* 99 -> 100 rollover: plain 3-digit names above 99 */
    SdrRecFileName(100, name);
    assert(strcmp(name, "Recording 100.wav") == 0);
    SdrRecFileName(101, name);
    assert(strcmp(name, "Recording 101.wav") == 0);
    SdrRecFileName(999, name);
    assert(strcmp(name, "Recording 999.wav") == 0);

    char path[40];
    SdrRecPath(1, path);
    assert(strcmp(path, "PIPES/RECORDINGS/Recording 01.wav") == 0);
    SdrRecPath(100, path);
    assert(strcmp(path, "PIPES/RECORDINGS/Recording 100.wav") == 0);
}

static void test_wav_header()
{
    uint8_t h[kSdRecWavHeaderSize];
    SdrWavHeader(h, 48000, 384000); /* 1 s of 48k/16-bit/stereo */

    assert(memcmp(h, "RIFF", 4) == 0);
    assert(memcmp(h + 8, "WAVE", 4) == 0);
    assert(memcmp(h + 12, "fmt ", 4) == 0);
    assert(memcmp(h + 36, "data", 4) == 0);
    assert(rd32(h + 4) == 36u + 384000u); /* RIFF size */
    assert(rd32(h + 16) == 16u);          /* fmt chunk size */
    assert(rd16(h + 20) == 1u);           /* PCM */
    assert(rd16(h + 22) == 2u);           /* stereo */
    assert(rd32(h + 24) == 48000u);       /* sample rate */
    assert(rd32(h + 28) == 192000u);      /* byte rate */
    assert(rd16(h + 32) == 4u);           /* block align */
    assert(rd16(h + 34) == 16u);          /* bits per sample */
    assert(rd32(h + 40) == 384000u);      /* data size */

    /* the placeholder header written at open has zeroed sizes... */
    uint8_t h0[kSdRecWavHeaderSize];
    SdrWavHeader(h0, 48000, 0);
    assert(rd32(h0 + 4) == 36u);
    assert(rd32(h0 + 40) == 0u);
    /* ...and patching refills the same header with the final sizes */
    SdrWavHeader(h0, 48000, 768000);
    assert(rd32(h0 + 4) == 36u + 768000u);
    assert(rd32(h0 + 40) == 768000u);
}

static void test_byte_limit()
{
    assert(!SdrRecOverLimit(kSdRecMaxBytes - 1u));
    assert(SdrRecOverLimit(kSdRecMaxBytes));
    assert(SdrRecOverLimit(kSdRecMaxBytes + 1u));

    /* 6 h at 48 kHz / 16-bit / stereo; under the 4 GiB FAT32/WAV
     * 32-bit limit even with the header and a final 16 KiB chunk */
    assert(kSdRecMaxBytes == 48000u * 4u * 21600u);
    assert(kSdRecMaxBytes + kSdRecWavHeaderSize + 16384u < 0xFFFFFFFFu);
}

int main()
{
    test_f2s16_clamping();
    test_ring_wrap_and_order();
    test_ring_full_refusal();
    test_ring_stereo_interleave();
    test_next_free_number();
    test_name_format();
    test_wav_header();
    test_byte_limit();
    printf("sd_recorder_test: all tests passed\n");
    return 0;
}

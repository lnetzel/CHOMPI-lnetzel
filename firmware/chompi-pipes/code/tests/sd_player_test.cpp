/** @file sd_player_test.cpp
 *  @brief Native tests for the host-testable core of SdPlayer.h (the
 *  CHOMPI-PIPES playback mode): the int16->float conversion as the
 *  exact inverse of the recorder's SdrF2S16 (round-trip), the stereo
 *  deinterleave, the canonical WAV header validation accept/reject
 *  table (foreign files must fail), the newest-first take window with
 *  gappy numbering and the slot mapping, and the next-slot advance
 *  with the wrap at the newest take.
 *
 *  Build & run: make -C firmware/chompi-pipes/code/tests test
 */
#define CHOMPI_SD_RECORDER_HOST 1
#define CHOMPI_SD_PLAYER_HOST 1
#include "SdRecorder.h"
#include "SdPlayer.h"
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>

using namespace chompi;

static void test_s16tof()
{
    assert(SdpS16ToF(0) == 0.f);
    assert(SdpS16ToF(32767) == 1.f);
    assert(SdpS16ToF(-32767) == -1.f);
    assert(SdpS16ToF(16383) > .49f && SdpS16ToF(16383) < .51f);

    /* exact inverse of the recorder's SdrF2S16: round-trip error is
     * at most one LSB (1/32767) */
    const float xs[] = {-1.f, -.75f, -.5f, -.25f, 0.f, .25f, .5f,
                        .75f, 1.f};
    for(unsigned i = 0; i < sizeof(xs) / sizeof(xs[0]); i++)
    {
        const float rt = SdpS16ToF(SdrF2S16(xs[i]));
        assert(fabsf(rt - xs[i]) <= 1.f / 32767.f + 1e-7f);
    }
}

static void test_deinterleave()
{
    const int16_t src[6] = {0, 32767, -32767, 16383, 8192, -8192};
    float         l[3], r[3];
    SdpDeinterleave(src, l, r, 3);
    assert(l[0] == 0.f && r[0] == 1.f);
    assert(l[1] == -1.f && fabsf(r[1] - .5f) < .01f);
    assert(fabsf(l[2] - .25f) < .01f && fabsf(r[2] + .25f) < .01f);
}

static void test_wav_ok()
{
    uint8_t h[kSdRecWavHeaderSize];
    SdrWavHeader(h, 48000, 384000); /* the recorder's own output */
    assert(SdpWavOk(h, 48000));

    /* data size 0 (a placeholder header) is rejected */
    uint8_t h0[kSdRecWavHeaderSize];
    SdrWavHeader(h0, 48000, 0);
    assert(!SdpWavOk(h0, 48000));

    /* sample-rate mismatch is rejected */
    assert(!SdpWavOk(h, 44100));

    /* patch single fields: each break must fail the check */
    uint8_t b[kSdRecWavHeaderSize];

    memcpy(b, h, sizeof(b));
    b[0] = 'X'; /* bad RIFF magic */
    assert(!SdpWavOk(b, 48000));

    memcpy(b, h, sizeof(b));
    b[8] = 'X'; /* bad WAVE magic */
    assert(!SdpWavOk(b, 48000));

    memcpy(b, h, sizeof(b));
    b[12] = 'X'; /* bad fmt magic */
    assert(!SdpWavOk(b, 48000));

    memcpy(b, h, sizeof(b));
    b[36] = 'X'; /* bad data magic (e.g. a foreign fmt chunk size) */
    assert(!SdpWavOk(b, 48000));

    memcpy(b, h, sizeof(b));
    b[20] = 3; /* IEEE float, not PCM */
    assert(!SdpWavOk(b, 48000));

    memcpy(b, h, sizeof(b));
    b[22] = 1; /* mono */
    assert(!SdpWavOk(b, 48000));

    memcpy(b, h, sizeof(b));
    b[34] = 24; /* 24-bit */
    assert(!SdpWavOk(b, 48000));

    memcpy(b, h, sizeof(b));
    b[34] = 8; /* 8-bit */
    assert(!SdpWavOk(b, 48000));

    /* garbage is rejected */
    uint8_t g[kSdRecWavHeaderSize];
    memset(g, 0xa5, sizeof(g));
    assert(!SdpWavOk(g, 48000));
}

static void test_window_slots()
{
    /* gappy numbering: takes 3, 7, 998 exist; the descending scan
     * pushes newest first */
    SdpTakeWindow w;
    w.Clear();
    assert(w.count == 0);
    w.PushNewestFirst(998);
    w.PushNewestFirst(7);
    w.PushNewestFirst(3);
    assert(w.count == 3);

    /* slot 0 = newest = the highest take number */
    assert(SdpSlotToTake(w, 0) == 998);
    assert(SdpSlotToTake(w, 1) == 7);
    assert(SdpSlotToTake(w, 2) == 3);
    /* outside the window -> -1 */
    assert(SdpSlotToTake(w, 3) == -1);
    assert(SdpSlotToTake(w, 14) == -1);
    assert(SdpSlotToTake(w, -1) == -1);

    /* the window keeps at most 15 entries, dropping older takes */
    SdpTakeWindow full;
    full.Clear();
    for(int n = 999; n > 979; n--) /* 20 pushes, descending */
        full.PushNewestFirst(n);
    assert(full.count == kSdpMaxTakes);
    assert(SdpSlotToTake(full, 0) == 999);  /* newest */
    assert(SdpSlotToTake(full, 14) == 985); /* oldest kept */
    assert(SdpSlotToTake(full, 15) == -1);

    /* Clear empties the window */
    w.Clear();
    assert(SdpSlotToTake(w, 0) == -1);
}

static void test_next_slot()
{
    /* advance goes toward the next-newer take (slot - 1) */
    assert(SdpNextSlot(2, 3) == 1);
    assert(SdpNextSlot(1, 3) == 0);
    /* the newest take wraps to the oldest of the window */
    assert(SdpNextSlot(0, 3) == 2);
    /* a single-take window wraps to itself */
    assert(SdpNextSlot(0, 1) == 0);
    /* a full 15-take window wraps newest -> slot 14 */
    assert(SdpNextSlot(0, 15) == 14);
    assert(SdpNextSlot(14, 15) == 13);
    /* invalid inputs -> -1 */
    assert(SdpNextSlot(0, 0) == -1);
    assert(SdpNextSlot(-1, 3) == -1);
    assert(SdpNextSlot(3, 3) == -1);
}

int main()
{
    test_s16tof();
    test_deinterleave();
    test_wav_ok();
    test_window_slots();
    test_next_slot();
    printf("sd_player_test: all tests passed\n");
    return 0;
}

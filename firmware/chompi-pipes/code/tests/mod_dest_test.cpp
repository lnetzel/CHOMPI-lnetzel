/** @file mod_dest_test.cpp
 *  @brief Native tests for ModDest.h - the CHOMPI-PIPES'
 *  Modulation-mode UI rules: the physical black-key destination
 *  mapping, the boot default, the remembered selection, the exact
 *  destination colors and LED intensities, the three-page Normal-mode
 *  cycling, the encoder press behavior, and the SHIFT absolute
 *  broadcast with clamping.
 *
 *  Build & run: make -C firmware/chompi-pipes/code/tests test
 */
#include "ModDest.h"
#include <cassert>
#include <cstdio>

using namespace chompi;

/* SwId values (hardware.h): the 25 playable keys are 7..31; the first
 * five physical black keys are 7 (C#3), 12 (D#3), 13 (F#3), 14 (G#3),
 * 21 (A#3); the eighth is 29 (F#4), the ninth 30 (G#4); the remaining
 * black keys are 22, 23, 31. chompi_main.cpp static_asserts the
 * destination-key table against Hardware::SwId. */

static void test_physical_destination_mapping()
{
    /* black keys numbered physically left to right select in order */
    assert(ModDestAfterKey(kModDestVolMod, 7) == kModDestAttack);  /* C#3 */
    assert(ModDestAfterKey(kModDestVolMod, 12) == kModDestRelease); /* D#3 */
    assert(ModDestAfterKey(kModDestVolMod, 13) == kModDestVolMod);  /* F#3 */
    assert(ModDestAfterKey(kModDestVolMod, 14) == kModDestCharMod); /* G#3 */
    assert(ModDestAfterKey(kModDestVolMod, 21)
           == kModDestLengthMod); /* A#3 */
    assert(ModDestAfterKey(kModDestVolMod, 29)
           == kModDestDriveMod); /* F#4: black key 8 */
    assert(ModDestAfterKey(kModDestVolMod, 30)
           == kModDestDelayMod); /* G#4: black key 9 */
    assert(kModDestKeySw[0] == 7 && kModDestKeySw[1] == 12
           && kModDestKeySw[2] == 13 && kModDestKeySw[3] == 14
           && kModDestKeySw[4] == 21 && kModDestKeySw[5] == 29
           && kModDestKeySw[6] == 30);

    /* every playable key that is not a destination key selects nothing */
    const int dest_key[] = {7, 12, 13, 14, 21, 29, 30};
    for(int swid = 7; swid < 32; swid++)
    {
        bool is_dest = false;
        for(int d = 0; d < kNumModDests; d++)
            if(swid == dest_key[d])
                is_dest = true;
        if(!is_dest) /* white keys and black keys 6, 7, 10 inert */
            assert(ModDestAfterKey(kModDestAttack, swid) == kModDestAttack);
    }
    /* non-key SwIds (encoders, SHIFT, LOOP, ...) select nothing too */
    for(int swid = 0; swid < 40; swid++)
    {
        bool is_dest = false;
        for(int d = 0; d < kNumModDests; d++)
            if(swid == dest_key[d])
                is_dest = true;
        if(!is_dest)
            assert(ModDestAfterKey(kModDestCharMod, swid)
                   == kModDestCharMod);
    }
}

static void test_boot_default_and_remembered_selection()
{
    /* boots on volume modulation */
    assert(kModDestDefault == kModDestVolMod);

    /* the selection is remembered: keys that select nothing leave any
     * current destination untouched, and a selection only changes on a
     * destination key (nothing else - mode changes, page cycles,
     * encoder gestures - ever calls ModDestAfterKey) */
    int dest = kModDestDefault;
    dest     = ModDestAfterKey(dest, 7); /* select attack */
    assert(dest == kModDestAttack);
    dest = ModDestAfterKey(dest, 15); /* C3: white key */
    assert(dest == kModDestAttack);
    dest = ModDestAfterKey(dest, 21); /* A#3: black key 5 */
    assert(dest == kModDestLengthMod);
    dest = ModDestAfterKey(dest, 14); /* select Character modulation */
    assert(dest == kModDestCharMod);
    dest = ModDestAfterKey(dest, 29); /* F#4: distortion modulation */
    assert(dest == kModDestDriveMod);
    dest = ModDestAfterKey(dest, 22); /* C#4: black key 6 */
    assert(dest == kModDestDriveMod);
    dest = ModDestAfterKey(dest, 30); /* G#4: black key 9, delay */
    assert(dest == kModDestDelayMod);
    dest = ModDestAfterKey(dest, 31); /* A#4: black key 10 */
    assert(dest == kModDestDelayMod);
}

static void test_destination_colors_and_intensities()
{
    float r, g, b;
    ModDestColor(kModDestAttack, r, g, b);
    assert(r == .55f && g == 0.f && b == 1.f); /* purple */
    ModDestColor(kModDestRelease, r, g, b);
    assert(r == 1.f && g == 1.f && b == 0.f); /* yellow */
    ModDestColor(kModDestVolMod, r, g, b);
    assert(r == 1.f && g == 1.f && b == 1.f); /* white */
    ModDestColor(kModDestCharMod, r, g, b);
    assert(r == 0.f && g == 0.f && b == 1.f); /* blue */
    ModDestColor(kModDestLengthMod, r, g, b);
    assert(r == 1.f && g == 1.f && b == 0.f); /* yellow: pipe length */
    ModDestColor(kModDestDriveMod, r, g, b);
    assert(r == 1.f && g == 0.f && b == 0.f); /* red: overdrive */
    ModDestColor(kModDestDelayMod, r, g, b);
    assert(r == 1.f && g == 1.f && b == 0.f); /* yellow: stereo delay */

    /* exact LED intensity ratios: selected 100%, unselected 75% */
    assert(kModDestSelected == 1.f);
    assert(kModDestUnselected == .75f);
    ModDestColor(kModDestAttack, r, g, b);
    assert(r * kModDestUnselected == .55f * .75f); /* precisely 75% */
    assert(b * kModDestSelected == 1.f);
}

static void test_normal_mode_three_page_cycling()
{
    /* Normal Mode cycles Volume (0) -> Character (1) -> pipe length
     * (2) -> Volume */
    assert(NextNormalPipePage(0) == 1);
    assert(NextNormalPipePage(1) == 2);
    assert(NextNormalPipePage(2) == 0);
}

static void test_encoder_press_behavior()
{
    /* Modulation Mode: unshifted presses do nothing */
    assert(PipeEncoderPress(true, false) == kPipePressNothing);
    /* SHIFT + press toggles the mute in both modes */
    assert(PipeEncoderPress(true, true) == kPipePressMute);
    assert(PipeEncoderPress(false, true) == kPipePressMute);
    /* Normal Mode: unshifted press cycles the page */
    assert(PipeEncoderPress(false, false) == kPipePressCyclePage);
}

static void test_broadcast_and_clamping()
{
    /* plain detent steps in range */
    assert(BroadcastStep(16, 1, 32) == 17);
    assert(BroadcastStep(16, -1, 32) == 15);
    /* clamped at both ends, no wrap (attack/release/mod: 32 steps) */
    assert(BroadcastStep(30, 5, 32) == 32);
    assert(BroadcastStep(32, 1, 32) == 32);
    assert(BroadcastStep(1, -5, 32) == 0);
    assert(BroadcastStep(0, -1, 32) == 0);
    /* exact bounds stay reachable */
    assert(BroadcastStep(0, 0, 32) == 0);
    assert(BroadcastStep(32, 0, 32) == 32);
    /* Character runs 64 steps in Normal Mode */
    assert(BroadcastStep(60, 10, 64) == 64);
    assert(BroadcastStep(63, 1, 64) == 64);

    assert(ClampStep(-3, 32) == 0);
    assert(ClampStep(35, 32) == 32);
    assert(ClampStep(7, 32) == 7);
}

int main()
{
    test_physical_destination_mapping();
    test_boot_default_and_remembered_selection();
    test_destination_colors_and_intensities();
    test_normal_mode_three_page_cycling();
    test_encoder_press_behavior();
    test_broadcast_and_clamping();
    printf("mod_dest_test: all tests passed\n");
    return 0;
}

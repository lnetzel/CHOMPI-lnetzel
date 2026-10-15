# Plan: fix the preset and distortion-page LEDs in the organ firmware

Both fixes are in `code/src/chompi_main.cpp`.

1. On volume page 2, holding SHIFT will show the preset slots and octave hints, the same as on the other pages.
2. In SAVE select, empty slots will blink and filled slots will stay solid.

## Steps

### Phase A: Page 2 SHIFT view (bug 1)

1. Remove the tone overlay state `tone_overlay_active_` and `prev_tone_overlay_`:
   - their declarations (~L302-307)
   - the end-of-poll block that sets them (~L1014-1027)
   - the overlay branch in `UpdateLeds()` (~L1116-1138)

   This overlay is what currently hides the slots and octave hints whenever SHIFT is held on page 2.
2. Add a bar sub-state `kVolBarTone = 4` next to `kVolBarDecay`. In the SHIFT+turn code for `kVolPageDrive` (~L805), start a 500 ms value bar for the tone. This copies how page 3's SHIFT+turn shows the decay.
3. In the `p == kVolBar` part of the value-bar code, add a `kVolBarTone` case. It shows the tone percentage across the 15 white keys, using the same red as the overdrive bar.
4. Result:
   - Holding SHIFT on page 2 shows the slot overview, octave hints and the three operation LEDs.
   - The red tone bar only appears while SHIFT+turning, and for 500 ms after the last click.
   - Loading presets and shifting octaves already worked on page 2 and stay unchanged.

### Phase B: Slot LEDs in the selection modes (bug 2)

Can be done in parallel with Phase A.

5. Rewrite the slot colours in the `preset_mode_ != kPresetNone` part of `UpdateLeds()` (~L1063-1115). When a slot matches more than one rule, the first match wins:
   1. During the saving, copying or erasing animation: no change from today.
   2. In copy-destination select, the copy source is green.
   3. The selected slot is blue (save or copy destination) or red (erase target).
   4. The current slot (last loaded or saved) is white, if it is filled.
   5. Other filled and empty slots depend on the mode:

| Mode | Filled | Current | Empty |
|---|---|---|---|
| SAVE select, copy destination | solid purple | solid white | blinking dim grey |
| ERASE select, copy source | blinking dim purple (as today) | blinking white | off |

### Phase C: Docs

6. Update `README.md`:
   - Change the tone paragraph (~L146-149): the bar shows while SHIFT+turning, not while SHIFT is held.
   - Rewrite the LEDs list (~L233-245) for the new blink scheme and remove the page-2 exception.
7. Tick off the two Presets items in `todo.md`.

## Verification

1. Build the firmware: `make` in `code/src`. It should compile with no warnings about the removed variables.
2. Run the host tests: `make` in `code/tests`. No changes are expected there, since they don't include `chompi_main.cpp`.
3. Check on the device:
   - **Page 2, hold SHIFT:** slots appear in purple (current one white), plus the octave hints and the three operation LEDs.
   - **Page 2, SHIFT+turn:** the red tone bar shows and disappears 500 ms after you stop turning.
   - **Octave hints:** SHIFT+C#3/D#3 changes the orange hint on page 2.
   - **SAVE select (SHIFT+KEY_25):**
     - Empty slots blink grey, filled slots are solid purple and the current slot is solid white.
     - Picking a slot turns it solid blue.
   - **Copy:** source slots blink in source select. In destination select, the source is green, empty slots blink and filled slots are solid.
   - **Erase:** filled slots blink, empty slots are off, and the target turns red.

## Decisions

- Page 2's SHIFT+turn tone feedback now works like page 3's decay bar (a 500 ms bar), replacing the SHIFT-held overlay.
- While a value bar is showing, it covers the SHIFT slot view. This already happens on every page, so it is left as is.
- Not changing: the animations, the CHOMPI key LED, the operation-LED colours, and the gesture logic.

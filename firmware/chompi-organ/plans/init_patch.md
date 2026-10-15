# Plan: Init patch on SHIFT + OVERDUB held for 4 s

**Gesture:** hold SHIFT, then press OVERDUB (`KEY_28`, the organ's LOOP/latch key) and keep both held for 4 s.
- **Progress:** a red bar fills the 15 white keys while you hold.
- **Cancel:** letting go of either key before 4 s cancels.
- **Done:** at 4 s the init sound is applied and the CHOMPI key LED blinks white for about 1 s.
- **Latch:** with SHIFT held, OVERDUB no longer toggles the latch.
- **Play key:** `KEY_27` stays untouched.

**Init values** (all 4 pipes where per-pipe):

| Parameter | Init value | Notes |
|---|---|---|
| Pipe level | 16 (50%) | |
| Character | 0 | Clean sine, shaping bypassed |
| Attack, release | 0 | 5 ms, the fastest |
| Mute | off | |
| Volume and Character modulation | 0 | |
| Vibrato, rotary, drive | 0 | |
| Distortion tone | 100% | Neutral; only audible once drive is raised |
| Reverb mix | 0 | |
| Reverb decay | 0 | 0.2 s, the shortest |

**Not touched:** master volume, fine tune, octave, latch state, page selections and the wheel mode. These are the same things presets don't save.

## Steps

### Phase A: Init values (testable on the host)
1. In `code/src/OrganPreset.h`, add `OrganPreset::SetInit()`. It calls `SetDefaults()`, then sets all `character[]` to 0 and `rotary` to 0. The other defaults already match the table.
2. In `code/tests/organ_preset_test.cpp`, add a test that checks every `SetInit()` field. *Depends on step 1.*

### Phase B: Gesture and engine

These all change `code/src/chompi_main.cpp`.

3. Move the body of `PresetLoadSlot()` (`ApplyToEngine`, CC resync, cancelling any value bar) into a new `ApplyPresetLive(const OrganPreset&)`. `PresetLoadSlot()` then calls it, so its behaviour doesn't change.
4. Add the state next to the preset state:
   - `volatile bool init_holding_`
   - `volatile uint32_t init_hold_t_` and `init_done_t_`
   - `kInitHoldMs = 4000`
5. Change the `KEY_28` block in `PollControls()` (~L654-680):
   - **On a fresh press with SHIFT held:** don't toggle the latch. If `preset_mode_ == kPresetNone`, start the hold (`init_holding_ = true`, `init_hold_t_ = now`).
   - **On a press without SHIFT:** toggle the latch exactly as today.
   - **While holding:** cancel if `KEY_28` is released, SHIFT is released, or a preset mode becomes active.
   - **At 4000 ms:**
     - Build an `OrganPreset`, call `SetInit()`, then `ApplyPresetLive()`.
     - Set `preset_current_ = -1`, `init_done_t_ = now` and `init_holding_ = false`.
     - A new press is needed to fire again.

### Phase C: LEDs (depends on step 4)

6. In `UpdateLeds()`, add a branch after the preset-mode branch and before the value-bar branch. While `init_holding_`, it clears the keyboard LEDs and lights `min(15, elapsed × 15 / 4000)` white keys red, left to right.
7. For the CHOMPI key LED (PTH 0): while `now - init_done_t_ < 1000`, blink it white using `pre_blink`. This takes priority over the "purple while SHIFT is held" case.

### Phase D: Docs (can be done in parallel with B and C)

8. Update `README.md`:
   - In the Keys section, note that SHIFT + LOOP doesn't toggle the latch.
   - In the Presets section, add an "Init" bullet with the gesture, the value table and what isn't touched.
   - Note that init works without an SD card.

## Verification
1. Host tests: run `make` in `code/tests`. The new `SetInit` test and the existing preset tests should pass.
2. Firmware: run `make` in `code/src` and check it builds.
3. On the device:
   - **Partial hold:** hold SHIFT + OVERDUB for 2 s and let go. The bar stops partway, the sound is unchanged and the latch is unchanged.
   - **Full hold:** after 4 s the bar is full, the CHOMPI LED blinks white and the sound is a plain 4-pipe sine at 50% with no rotary, drive or reverb. Held notes keep sounding with no click.
   - **After init:** in the SHIFT slot view, no slot is white. Master volume, octave and fine tune are unchanged.
   - **Latch:** OVERDUB without SHIFT still toggles it.
   - **Preset mode:** the gesture does nothing while one is active.
   - **No SD card:** init still works.
   - **Shipping mode:** `KEY_26+27+28` held at power-on still enters it, because that check runs at boot, before `PollControls()`.

## Decisions
- Init is kept separate from `preset_mode_`, so it doesn't need the SD card and the main loop's no-card cancel doesn't affect it.
- No undo.
- The engine's boot defaults are unchanged. The organ still powers up with Character at 50% and rotary at about 30%; init is a separate, more stripped-back sound.

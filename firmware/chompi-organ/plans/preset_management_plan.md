# CHOMPI Organ: SD-card presets (Load / Save / Copy / Delete)

## Problem and approach

Organ settings are currently session-only. Add 15 persistent preset slots on
the SD card, using CHOMPI-Tape's gestures, LED colors and blinking, and its
source/destination selection flow. Tape's SD init and temp-file + rename
write pattern is reused. Only one bank is used, with Tape's bank-0 purple.

Design for "quick and smooth, no freeze, no audio glitches":
- **All 15 presets are cached in RAM.** Load, Save, Copy and Delete act on the RAM
  cache and the engine immediately, inside the existing control poll. They
  never wait for the SD card.
- Load applies values through the existing engine setters. These are already
  ramped: pipe gain and mute ~1 ms, Character ~10 ms, drive/rotary/reverb-mix
  slewed. Envelope, vibrato and decay changes are phase-continuous, so held
  or latched notes keep sounding without clicks. No voices are stopped.
- **SD I/O runs only in MainLoop**, never in the audio ISR. It is a staged,
  non-blocking writer that does one small step per loop iteration, so the
  watchdog keeps being fed and the ISR (higher priority) is never stalled.
- **One small file per slot**: `ORGAN/P01.json` … `P15.json`, each about 600 B.
  - Save or copy writes one tiny file, as `Pxx.tmp`, then sync, unlink the
    old file, and rename. Delete is a single `f_unlink`.
  - There is no 8 KB whole-file rewrite as in Tape. A power loss can corrupt
    at most the slot being written, and it falls back to the previous file.
- **Boot always uses factory defaults.** Presets are read into the RAM cache
  at boot, before audio starts, but are only applied when a slot is loaded.

## Preset contents (versioned JSON, `"version": 1`)

| Field | Range | Engine API |
|---|---|---|
| `level[4]` | 0-32 | `Get/SetPipeStep` |
| `character[4]` | 0-64 | `Get/SetPipeCharacterStep` |
| `attack[4]`, `release[4]` | 0-32 | `Get/SetPipeAttackStep/ReleaseStep` |
| `mute[4]` | 0/1 | `Get/SetPipeMuted` |
| `vibrato` | 0-50 | `Get/SetVibratoDepth` |
| `rotary` | 0-32 | `Get/SetRotaryStep` |
| `drive` | 0-32 | `Get/SetDriveStep` |
| `drive_tone` | 0-100, default 100 | reserved: distortion tone plan |
| `reverb_mix` | 0-100 | `Get/SetReverbMixPercent` |
| `reverb_decay` | 0-100 | `Get/SetReverbDecayStep` |
| `vol_mod[4]`, `char_mod[4]` | 0-32, default 0 | reserved: modulation plan |

Excluded: master volume, fine tune, octave, drive type, and the latch/pages.

- Reserved fields are always written and parsed. Today they are stored
  in the cache with no engine hookup. When the tone or modulation plans land,
  they plug their setters/getters into `CaptureFromEngine`/`ApplyToEngine`
  (about one line each).
- Those two plan docs say "no SD persistence". Amend them to say the values
  are saved in presets.
- When parsing, missing keys get defaults and values are clamped, so older
  and newer files are tolerated. Unknown keys are ignored.

## UI (ported from Tape MenuPage; SHIFT = CHOMPI key KEY_26)

- **Load:** SHIFT + white key on a filled slot. The preset is applied instantly
  and becomes the current slot. An empty slot does nothing.
- **Delete:** SHIFT + KEY_23 enters erase select; pressing it again cancels.
  1. Choose a filled slot with a white key; SHIFT may be released.
  2. Confirm with a fresh SHIFT press. The ERASING animation shows for ≥1 s.
- **Copy:** SHIFT + KEY_24 enters copy source select; pressing it again cancels.
  1. Choose a filled source slot, which turns green. The mode becomes
     copy destination.
  2. Choose a different destination slot.
  3. Confirm with SHIFT. The COPYING animation shows for ≥1 s.
- **Save:** SHIFT + KEY_25 enters save select; pressing it again cancels.
  1. Choose any slot.
  2. Confirm with SHIFT. The current engine state is captured, the SAVING
     animation shows for ≥1 s, and the saved slot becomes the current slot.
- While a selection mode is active, keyboard keys do not play notes or change
  latch. Without SHIFT and with no preset mode active, KEY_23–25 and the
  white keys play notes exactly as today.
  - SHIFT + KEY_16/KEY_17 (octave) are black keys, so they are unaffected.
  - Encoders, wheel and MIDI keep working throughout.
  - Note-offs for keys held before entering a mode are still delivered, so
    there are no hung notes.
- Deleting the current slot clears "current". Selection-mode presses are
  ignored when there is no SD card.

LEDs (same as Tape, with UpdateLeds taking priority over bars when a preset
mode or SHIFT preset view is active):
- **CHOMPI/SHIFT PTH LED:**
  - purple while held in NONE mode;
  - blinking red once a slot is selected in SAVE, ERASE or COPY_DEST;
  - blinking white during SAVING, COPYING or ERASING.
- **Black-key LEDs** (shown while SHIFT is held or a mode is active):
  - Delete (SMT 7) red, Copy (SMT 8) green, Save (SMT 9) blue.
  - The LEDs of the other operations are off while an operation is active.
  - All are off with no SD card.
- **White keys** (slot i → `white_key_led[i]`):
  - In NONE mode: filled slots purple, the current slot white, empty slots off.
  - In selection modes: a 250 ms blink of filled slots at 0.4 × purple. Empty
    slots are dim grey (0.4) in SAVE/COPY_DEST, otherwise off.
  - The selected slot is blue for save or copy destination and red for erase.
    The copy source is green.
  - No SD card: white keys red while SHIFT is held, as in Tape.
- Outside preset views the existing LED/bar behavior is unchanged.

## Implementation

1. **`code/src/OrganPreset.h` (new, header-only, host-testable)**
   - `OrganPreset` POD with defaults.
   - `Serialize(const OrganPreset&, char* buf, size_t)` using `snprintf`, and
     `Parse(const char*, size_t, OrganPreset&)` using coreJSON with clamping
     and defaults.
   - `OrganPresetBank`: 15 slots, valid flags, a dirty bitmask, and
     `Save/Copy/Erase/Get`.
   - `CaptureFromEngine`/`ApplyToEngine` are templated on the engine type, so
     tests can use a fake engine.
2. **`code/src/PresetStorage.h` (new)**
   - SD/FatFS mount and `f_mkdir("ORGAN")`.
   - `LoadAll()` at boot reads `Pxx.json` files, ignoring any `.tmp`.
   - `Service()` is a MainLoop state machine. It picks one dirty slot, snapshots
     the RAM preset into a local copy (brief IRQ-off memcpy), then runs
     open tmp → write → sync → close → unlink → rename (or unlink for a
     delete), one stage per call.
   - It retries on error, and `disk_status(0)` is checked every 1 s to set
     `no_sd_card_`.
3. **`code/src/Makefile`**
   - Add `USE_FATFS = 1`.
   - Add `../../../chompi-wave/code/libs/coreJSON/source/core_json.c` and its
     include path.
4. **`code/src/chompi_main.cpp`**
   - SD init and `LoadAll()` in `main()` before `StartAudio` / IWDG.
   - Preset mode state (volatile) and SHIFT / black / white key interception
     at the top of the key loop in `PollControls`.
   - Engine capture and apply happen in the ISR (same context as all other
     setters). After a load, set `cc_pipe_dirty_` / `cc_character_dirty_` so
     MIDI controllers resync. Any active value bar is cancelled.
   - Call the storage `Service()` in MainLoop, and add the preset LED
     rendering to `UpdateLeds`.
5. **Tests (`code/tests/`)**: add `organ_preset_test.cpp` and a Makefile
   target, covering:
   - round-trip of every field;
   - defaults for missing keys, including reserved fields absent;
   - clamping of out-of-range and garbage values;
   - version handling;
   - bank save/copy/erase and dirty flags;
   - apply/capture against a fake engine.
6. **Docs**
   - `README.md`: new Presets section with gestures, LEDs, saved fields,
     files and no-SD behavior. Update the "session-only" statements.
   - `todo.md`: mark presets done.
   - Amend `plans/distortion_tone_control_plan.md` and
     `plans/modulation_mode_plan.md` so the tone and modulation values are
     included in presets.
7. **Validation**
   - Run the new test and the full organ host test suite.
   - Load the `build-firmware` skill and build the organ firmware.
   - Report hardware checks as pending unless verified: SD timing/listening
     while saving with latched notes, LEDs, power-cycle restore and card
     removal.

## Notes / risks

- Tape's `WritePresets` was built at `-O0` "because of clicks" while running
  in a timer IRQ. Running the organ's writes in MainLoop at thread level, with
  tiny files, avoids that path. If clicks still appear on hardware, fall back
  to Tape's -O0 attribute on the writer.
- SDMMC init with no card must not hang boot. Mirror Tape's handling and
  continue with the presets disabled.
- No changes to TAPE, WAVE, TEMPO, the bootloader or the shared libs; coreJSON
  is only referenced from chompi-wave.

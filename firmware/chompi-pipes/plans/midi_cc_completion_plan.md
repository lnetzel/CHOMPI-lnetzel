# Plan: CHOMPI-PIPES complete MIDI CC implementation

Every parameter that can be saved in an SD preset should also be settable
over MIDI. Today only pipe levels (CC 70–73), Character (CC 74–77) and
vibrato depth (CC 1) have CCs; this plan adds CC-in for the remaining 38
values, plus Program Change 1–15 to load preset slots. Per user decision
the new CCs are **receive-only** — MIDI-out stays at the current 9 CCs.

## The gap (checked in the code)

The preset model [OrganPreset.h](../code/src/OrganPreset.h) saves 20
parameter groups (L26–47 for the ranges, L50–78 for the fields):

| Preset field | Range | Engine setter ([OrganEngine.h](../code/src/OrganEngine.h)) | CC today? |
|---|---|---|---|
| `level[4]` | 0–32 | `SetPipeStep` (L327) | ✅ CC 70–73 |
| `character[4]` | 0–64 | `SetPipeCharacterStep` (L367) | ✅ CC 74–77 |
| `vibrato` | 0–50 | `SetVibratoDepth` (L555) | ✅ CC 1 |
| `attack[4]` | 0–32 | `SetPipeAttackStep` (L438) | ❌ |
| `release[4]` | 0–32 | `SetPipeReleaseStep` (L455) | ❌ |
| `mute[4]` | 0/1 | `SetPipeMuted` (L343) | ❌ |
| `length[4]` | −2…+2 | `SetPipeLengthOctave` (L478) | ❌ |
| `vol_mod[4]` | 0–32 | `SetPipeVolModStep` (L383) | ❌ |
| `char_mod[4]` | 0–32 | `SetPipeCharModStep` (L401) | ❌ |
| `length_mod[4]` | 0–32 | `SetPipeLengthModStep` (L421) | ❌ |
| `vibrato_delay` | 0–50 | `SetVibratoDelayStep` (L579) | ❌ |
| `rotary` | 0–32 | `SetRotaryStep` (L540) | ❌ |
| `drive` | 0–32 | `SetDriveStep` (L498) | ❌ |
| `drive_tone` | 0–100 | `SetDriveTonePercent` (L530) | ❌ |
| `reverb_mix` | 0–32 | `SetReverbMixStep` (L596) | ❌ |
| `reverb_decay` | 0–100 | `SetReverbDecayStep` (L610) | ❌ |
| `percussion` | 0/1 | `SetPercussionEnabled` (L680) | ❌ |
| `delay_mix` | 0–100 | `SetDelayMixPercent` (L634) | ❌ |
| `delay_time_ms` | 40–500 | `SetDelayTimeMs` (L663) | ❌ |
| `drive_mod` | 0–32 | `SetDriveModStep` (L516) | ❌ |

More facts checked in the code:

- All CC-in today is handled in the `ControlChange` case of
  `OrganMidi::ProcessMidiIn` in [MidiHandler.h](../code/src/MidiHandler.h),
  which runs **in the audio ISR** and calls engine setters directly. All
  setters above are ramped/clamped and ISR-safe (they are the same ones
  `ApplyToEngine` in [OrganPreset.h](../code/src/OrganPreset.h) uses for
  click-free live preset loads).
- Incoming CCs are deliberately **silent** (no white-key LED value bars);
  see the comment at the top of the `ControlChange` case. The user
  confirmed this rule covers all new CCs.
- The existing value mapping is `(val * max + 63) / 127` (rounded); see
  the CC 1 / CC 70–77 code in `ProcessMidiIn`.
- Program Change is currently ignored (the `default:` branch of
  `ProcessMidiIn`; also documented in [midi.md](../../midi.md)).
- `PresetLoadSlot` / `ApplyPresetLive` live in
  [chompi_main.cpp](../code/src/chompi_main.cpp) (L538–L565) and work on
  the RAM-cached bank, so a MIDI-triggered load never touches the SD card
  and empty slots safely do nothing.
- Not preset-saved, therefore out of scope: master volume, fine tune,
  octave shift, latch state, page selections, wheel mode (listed in the
  `SetInit` comment in [OrganPreset.h](../code/src/OrganPreset.h)).

## Proposed CC map

Continuing the existing blocks (70–77) upward; the map ends at 115, clear
of the reserved channel-mode CCs 120–127. Pipe number = CC − base (0–3).

**Per pipe:**

| Group | CC base | CCs | Range |
|---|---|---|---|
| Attack | 78 | 78–81 | 0–32 steps |
| Release | 82 | 82–85 | 0–32 steps |
| Mute | 86 | 86–89 | 0–63 off, 64–127 on |
| Length | 90 | 90–93 | −2…+2, snapped anchors |
| Volume mod | 94 | 94–97 | 0–32 steps |
| Character mod | 98 | 98–101 | 0–32 steps |
| Length mod | 102 | 102–105 | 0–32 steps |

**Global:**

| Function | CC | Range |
|---|---|---|
| Vibrato note-on delay | 106 | 0–50 steps (100 ms each) |
| Rotary amount | 107 | 0–32 steps |
| Drive amount | 108 | 0–32 steps |
| Drive tone | 109 | 0–100 % |
| Reverb mix | 110 | 0–32 steps |
| Reverb decay | 111 | 0–100 steps |
| Percussion on/off | 112 | 0–63 off, 64–127 on |
| Delay mix | 113 | 0–100 % |
| Delay time | 114 | 40–500 ms |
| Drive mod (global) | 115 | 0–32 steps |

**Value mappings** (incoming 0–127 → engine value, all rounded):

- `0..max` steps: `(v * max + 63) / 127` — same formula as the existing CCs.
- Toggles (mute, percussion): `v >= 64` (standard switch convention).
- Length (per user decision: snapped anchors): `(v * 4 + 63) / 127 − 2`,
  which hits −2/−1/0/+1/+2 exactly at 0/32/64/96/127 and snaps every
  in-between value to the nearest anchor.
- Delay time: `40 + (v * 460 + 63) / 127` ms.

## Steps

### Phase 1 – mapping header + host tests

1. New header `code/src/MidiCcMap.h` (header-only, **no Daisy includes**,
   so host tests can use it — same convention as `OrganPreset.h`):
   - CC number constants for the whole table above (`kAttackCcBase = 78`
     etc.), next to the existing constants it will replace references to.
   - `inline int CcToSteps(uint8_t v, int max)` — the rounded formula.
   - `inline bool CcToToggle(uint8_t v)`.
   - `inline int CcToLengthOctave(uint8_t v)` — snapped-anchor formula.
   - `inline int CcToDelayTimeMs(uint8_t v)`.
2. New host test `code/tests/midi_cc_map_test.cpp` plus a Makefile target
   in `code/tests/Makefile`: endpoints (0 and 127 hit the exact min/max),
   center values, anchor points for length (0/32/64/96/127 exact; 33 snaps
   to −1, 95 snaps to 0…), toggle threshold at 63/64, delay-time endpoints
   40/500. Run via the existing host-test build.

### Phase 2 – firmware CC-in (`code/src/MidiHandler.h`)

3. Extend the `ControlChange` case of `OrganMidi::ProcessMidiIn` to
   dispatch the new ranges using the `MidiCcMap.h` constants and helpers,
   calling the engine setters from the gap table. Keep the chained
   `if/else` style already used there; all new handlers set values
   silently (no LED bars, nothing sent back out — the "CC-in only"
   decision means no new `Send*CC` methods and no new dirty flags in
   `chompi_main.cpp`).

### Phase 3 – Program Change → preset load

4. [MidiHandler.h](../code/src/MidiHandler.h): handle the `ProgramChange`
   event type. Program numbers 0–14 (documented as slots 1–15) store a
   pending slot index; all other program numbers are ignored. Expose
   `int TakePendingPresetSlot()` (returns −1 when empty, clears the flag).
   Rationale: `ProcessMidiIn` runs in the audio ISR and only knows the
   engine; `preset_bank_` and `PresetLoadSlot` are static to
   `chompi_main.cpp`, so the load itself must be done there.
5. [chompi_main.cpp](../code/src/chompi_main.cpp): in the audio-ISR
   control path (next to the existing preset gesture handling, before the
   MIDI drain), poll `midi.TakePendingPresetSlot()` and call
   `PresetLoadSlot(slot)` when ≥ 0. Polling *before* `ProcessMidiIn()`
   keeps PC→CC ordering correct when they arrive in separate audio passes
   (see Risks).

### Phase 4 – documentation (`firmware/chompi-pipes/midi.md`)

6. Extend the **MIDI In** table with all new CCs (same "value span"
   notation as the existing rows), add a Program Change row, and drop
   "program change" from the "no other message types" sentence. The
   **MIDI Out** table stays unchanged (CC-in only). Note that the
   silent-CC rule now covers the whole map.

### Phase 5 – build & verify (implementer)

7. Build the firmware per the repo MSYS2 recipe and flash.
8. Run the new host test; sanity-sweep each CC group on hardware with a
   held note (e.g. reaper/DAW or loopMIDI + a knob): attack/release
   audibly reshape, mute silences one pipe, length jumps octaves at the
   five anchors, delay time sweeps 40–500 ms, PC 1–15 loads slots (and a
   PC for an empty slot does nothing).

## Decisions (from user Q&A, 2026-10-18)

- **CC-in only**: the ~34 new CCs are receive-only; MIDI-out stays at the
  current 9 CCs (level, Character, vibrato).
- **Length mapping**: snapped anchors (0/32/64/96/127 exact), not free
  linear.
- **Program Change included**: PC 1–15 loads preset slots.
- **Silent CCs**: no white-key LED value bars for any incoming CC.

## Risks / open questions

- **PC↔CC ordering in one batch**: the pending-slot bridge means a CC
  that arrives in the *same* MIDI burst *after* a PC is applied to the
  engine first and then overwritten by the deferred preset load. Polling
  the pending slot before each `ProcessMidiIn` drain fixes the common
  case (separate passes); senders wanting "load preset, then tweak"
  should leave ≥ 1 ms between the PC and the CCs. Accepted limitation —
  flag in midi.md if it bites in practice.
- **CC budget**: the map fills 70–115 contiguously. Future per-pipe
  parameters would need a new block below 70 or above 115 (120–127 are
  reserved channel-mode messages) — worth keeping in mind before adding
  preset fields.
- **Test tool**: the planned interactive MIDI regression tool
  ([midi_test_tool_plan.md](midi_test_tool_plan.md)) should gain a test
  section for the new CCs and PC loads when it gets built (add entries in
  its `tests.py`); out of scope for this plan.

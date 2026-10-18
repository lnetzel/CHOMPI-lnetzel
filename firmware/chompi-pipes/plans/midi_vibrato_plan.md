# Plan: CHOMPI-PIPES MIDI modulation (CC 1) → vibrato depth

PIPES currently ignores every incoming CC except 70–77, so a controller's
modulation wheel does nothing. Add **CC 1 (Modulation Wheel)** on the fixed
channel 1, mapped to the global vibrato depth (0–50 cents peak), both
directions: incoming CC 1 sets the depth (and flashes the pink vibrato bar),
and turning the wheel in vibrato mode sends CC 1 out so an external
controller stays in sync.

## What the firmware actually supports (checked in the code)

- **MIDI in** is handled in `OrganMidi::ProcessMidiIn()` in
  [MidiHandler.h](../code/src/MidiHandler.h): the `ControlChange` case only
  accepts `kPipeCcBase` 70–73 (→ `SetPipeStep`, 0–127 mapped to 0–32 with
  `(val*32+63)/127`) and `kCharacterCcBase` 74–77 (→ `SetPipeCharacterStep`,
  mapped to 0–64). Every other CC falls through and is discarded. Both UART
  (DIN) and USB events feed the same switch.
- **The vibrato depth setter already exists**:
  `OrganEngine::SetVibratoDepth(int)` / `GetVibratoDepth()` in
  [OrganEngine.h](../code/src/OrganEngine.h) (~L539), range 0–50 cents
  (`kMaxVibratoDepthCents = Vibrato::kMaxDepthCents` at L166). Reaching 0
  snaps sounding voices back to exact base frequencies (bit-exact bypass).
  The LFO itself is in [Vibrato.h](../code/src/Vibrato.h) (`SetDepth(0..50)`).
  No new DSP is needed — MIDI only drives the existing setter.
- **Incoming CC UI feedback** goes through
  `OrganMidi::SetCcLevelCallback` → `CcLevelCallback(int pipe, int page)` in
  [chompi_main.cpp](../code/src/chompi_main.cpp#L464) (registered at L2030),
  which sets `bar_pipe_` / `bar_page_` / `bar_until_` (+500 ms). `page` is
  currently 0 = volume bar, 1 = Character bar.
- **The vibrato depth bar already exists**: `bar_pipe_ = kRotaryBar`
  ([chompi_main.cpp](../code/src/chompi_main.cpp#L310)) with
  `bar_page_ = kWheelBarVibrato` (= 1, L314) renders a pink bar of
  `(GetVibratoDepth()*15+25)/kMaxVibratoDepthCents` lit white keys
  ([chompi_main.cpp](../code/src/chompi_main.cpp#L1493)).
- **Local vibrato control** is the transport wheel in vibrato mode:
  [chompi_main.cpp](../code/src/chompi_main.cpp#L1075) does
  `SetVibratoDepth(GetVibratoDepth() + delta)` — relative, clamped by the
  engine. An absolute CC value can reuse the same setter safely.
- **MIDI out for CCs** follows a dirty-flag pattern:
  `cc_pipe_step_[]/cc_pipe_dirty_[]` and `cc_character_step_[]/cc_character_dirty_[]`
  ([chompi_main.cpp](../code/src/chompi_main.cpp#L404)) are set by the
  encoders (~L1269–1279) and by `ApplyPresetLive` (L490–495), then consumed
  in MainLoop ([chompi_main.cpp](../code/src/chompi_main.cpp#L1972)) via
  `midi.SendPipeCC` / `midi.SendCharacterCC`, both of which funnel into
  `OrganMidi::SendStepCC(cc, step, max)` = `round(step*127/max)`
  ([MidiHandler.h](../code/src/MidiHandler.h), `SendStepCC`).
- **Vibrato delay stays without MIDI**: `SetVibratoDelayStep` is documented
  "Saved in the SD-card presets; no MIDI CC" in
  [OrganEngine.h](../code/src/OrganEngine.h) — this plan does not change
  that.
- **Docs**: [midi.md](../midi.md) currently states no message types other
  than notes and CC 70–77 are processed or sent — it must be updated.
- **Tests**: there is no native MIDI test (`MidiHandler.h` includes
  `daisy_seed.h`, so it is not native-testable); `code/tests/` covers DSP
  headers only. Verification is on-device (see below).

## Steps

### Phase 1 – MIDI in (in `firmware/chompi-pipes/code/src/`)

1. **[MidiHandler.h](../code/src/MidiHandler.h)**:
   - Add `static constexpr uint8_t kVibratoCc = 1;` next to `kPipeCcBase`.
   - In `ProcessMidiIn()`'s `ControlChange` case, add a branch:
     `cc == kVibratoCc` →
     `step = (val * OrganEngine::kMaxVibratoDepthCents + 63) / 127`
     (same rounded mapping as the pipe CCs) →
     `engine_->SetVibratoDepth(step)`, then fire
     `cc_level_callback_(0, 2)` (new page value 2 = vibrato depth).
   - Update the `@file` comment to mention CC 1.
2. **[chompi_main.cpp](../code/src/chompi_main.cpp#L464)** `CcLevelCallback`:
   handle `page == 2` by showing the existing vibrato bar:
   `bar_pipe_ = kRotaryBar; bar_page_ = kWheelBarVibrato; bar_until_ = now + 500`.
   Pages 0/1 keep their current behavior. (Alternatively pass `kRotaryBar`
   as the pipe argument from MidiHandler.h — pick whichever reads cleaner;
   the bar rendering at L1493 needs no changes either way.)

### Phase 2 – MIDI out (same files)

3. **[MidiHandler.h](../code/src/MidiHandler.h)**: add
   `void SendVibratoCC(int step)` calling
   `SendStepCC(kVibratoCc, step, chompi::OrganEngine::kMaxVibratoDepthCents)`
   (scales to `round(step × 127 / 50)`).
4. **[chompi_main.cpp](../code/src/chompi_main.cpp)**:
   - Add `static volatile int cc_vibrato_step_;` and
     `static volatile bool cc_vibrato_dirty_;` next to the pipe/character
     arrays (L404–405).
   - In the wheel-in-vibrato-mode branch (L1075), after
     `SetVibratoDepth`, set `cc_vibrato_step_ = engine.GetVibratoDepth();
     cc_vibrato_dirty_ = true;` (coalesced, same as pipes).
   - In `ApplyPresetLive` (L490), also resync vibrato:
     `cc_vibrato_step_ = engine.GetVibratoDepth(); cc_vibrato_dirty_ = true;`
     so loading a preset updates an attached controller.
   - In MainLoop next to the pipe/character sends (L1972), consume the
     flag: `if(cc_vibrato_dirty_) { cc_vibrato_dirty_ = false; midi.SendVibratoCC(cc_vibrato_step_); }`.
   - Initialize the new statics at boot (near L2091).

### Phase 3 – Docs and verification

5. **[midi.md](../midi.md)**: add CC 1 rows to the MIDI In table
   ("Modulation → vibrato depth, 0–127 mapped to 0–50 cents, rounded") and
   the MIDI Out table ("CC 1, 0–127, depth scaled round(depth × 127 / 50)"),
   and adjust the "No other message types … are processed" line.
6. **Build** the firmware per the build-pipes recipe and smoke-test on
   hardware: CC 1 sweep 0→127 moves the pink vibrato bar and audibly bends
   pitch on a held note; wheel turns in vibrato mode emit CC 1 on DIN+USB;
   CC 1 on channels 2–16 is ignored.
7. **Extend the MIDI test tool later**: add CC-1 entries to the test list
   from [midi_test_tool_plan.md](midi_test_tool_plan.md) (a "C" section
   sweep test and an "E" section auto-test that wheel turns emit CC 1) when
   that tool is implemented. Not part of this change.

## Relevant files

- Edit:
  - [MidiHandler.h](../code/src/MidiHandler.h): `kVibratoCc`, `ProcessMidiIn` CC branch, `SendVibratoCC`
  - [chompi_main.cpp](../code/src/chompi_main.cpp): `CcLevelCallback` page 2, wheel branch, dirty-flag statics, MainLoop send, `ApplyPresetLive`
  - [midi.md](../midi.md): IN/OUT tables
- Reference only:
  - [OrganEngine.h](../code/src/OrganEngine.h): `SetVibratoDepth`/`GetVibratoDepth`, `kMaxVibratoDepthCents`
  - [Vibrato.h](../code/src/Vibrato.h): depth semantics, exact-0 bypass
  - [midi_test_tool_plan.md](midi_test_tool_plan.md): future test coverage

## Risks / open questions

- **CC number choice**: CC 1 is the standard "Modulation Wheel" and matches
  the request. If a conflict with a specific controller ever matters, only
  the `kVibratoCc` constant changes.
- **Absolute vs relative**: the wheel applies relative deltas while CC 1 is
  absolute; the last writer wins. This matches how the pipe-level CCs
  already behave against their encoders, so it is consistent.
- **Preset persistence**: depth is captured into presets from the engine
  (`CaptureFromEngine`), so a CC-1 change becomes part of a saved preset —
  same behavior as pipe levels today; no preset format change needed.
- **No native unit test possible** for `MidiHandler.h` (Daisy dependency);
  the mapping math is one line and mirrors the two existing CC mappings.
- **Vibrato-delay CC**: deliberately out of scope (engine comment says
  "no MIDI CC"); easy follow-up as e.g. CC 12 if ever wanted.

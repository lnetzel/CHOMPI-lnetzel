# CHOMPI Organ: CPU headroom (latch-off crackling)

## Problem and approach

With 6 voices plus delay, reverb and the full chain active, the organ sits
at the edge of the audio-block CPU budget (24 samples at 48 kHz = 500 µs).
In latch mode with 6 held notes the load is a constant steady state, which
mostly fits. With latch OFF, playing generates note-on/off and voice-steal
events whose handling runs **inside the audio ISR**, and the burst tips a
block over its deadline. The underrun repeats/corrupts a DMA block, which
is the audible "bit-reduction" crackling.

Approach: measure first with the existing CPU meter, then cut both the
steady-state per-sample cost and the per-event burst cost, without changing
polyphony, feature set, or the agreed sounds/ranges.

## Confirmed requirements

- No polyphony reduction; no sound, range, or control changes. All
  exact-bypass guarantees stay (drive 0%, delay/reverb 0% mix, percussion
  settled-off all remain bit-exact).
- Oscillators stay free-running: phase is never reset at note-on, and
  vibrato's per-sample `SetFreq(base * mult)` path stays cheap and
  phase-continuous.
- Target: peak callback load below 85% on `ORGAN_CPU_METER` in the worst
  case: 6 held voices, maximum Character/drift, distortion, rotary, delay
  and long reverb together, while actively playing with latch OFF, plus
  encoder motion and preset SD activity. Host timing is not evidence of
  MCU headroom.
- New DSP primitives get host tests; the existing native suite must stay
  green (the tonewheel_character L273 and vibrato L91 failures are
  pre-existing at HEAD and are out of scope unless a touched file causes
  them).

## Codebase findings

- `chompi_main.cpp` `AudioCallback()` runs everything at ISR priority in
  the audio budget: `midi.ProcessMidiIn()`, `hw.ProcessAllControls()`,
  `PollControls()` (key scan + note logic), `engine.Prepare()`,
  `engine.Process()`.
- Steady-state hot spot: 6 voices x 4 pipes = 24 `daisysp::Oscillator`
  instances, and `Oscillator::Process()` calls libm `sinf()` per sample
  for `WAVE_SIN` (chompi-wave DaisySP `oscillator.cpp`). At ~50-150 cycles
  per `sinf` on the M7 this is roughly a quarter of the whole CPU budget.
  `Oscillator::SetFreq()` only updates `phase_inc_` (one multiply), so
  vibrato's per-sample retune is already cheap.
- Per-event burst: `OrganEngine::NoteOn()` -> `StartVoice()` ->
  `UpdateFreqs()` computes `NoteToHz()` with one `powf()` per pipe (4x per
  note-on), plus `TonewheelCharacter::UpdateVoiceCoeffs()` x4 (cheap
  integer-hash algebra, not a problem). Worse: the STEAL branch inside
  `Process()` calls `StartVoice()` **from the per-sample loop** when the
  5 ms steal fade completes.
- Smaller transcendental costs: `RotarySpeaker` 2 x `sinf`/sample;
  `Vibrato::Process()` 1 x `sinf` + 1 x `exp2f`/sample while depth > 0.
- A sine-LUT pattern already exists in the codebase: `StereoBbdDelay`
  builds `lfo_table_[256]` in `Init()` and never does transcendental work
  per sample. Follow that pattern.
- `TonewheelCharacter::ShapeSample()` shapes the sine *sample*
  algebraically, so replacing the oscillator core is transparent to it.
- The CPU meter is wired: `make ORGAN_CPU_METER=1`; PTH LED 0 shows peak
  load (green < 70%, amber 70-85%, red > 85%).
- Prior hardware lesson: 6 voices is the CPU ceiling at current feature
  load (the 7-voice attempt was reverted). Per-voice CPU savings are the
  correct unlock.

## Implementation todos

### 1. Baseline measurement

Build the meter firmware (`make ORGAN_CPU_METER=1`), run the native suite
for a baseline, and record average/peak load in the worst-case scene above
with latch ON vs OFF. These numbers are the before/after evidence.

### 2. LUT sine oscillator (biggest steady-state win)

Add `code/src/SineLut.h`, host-testable, no Daisy dependencies:

- One shared const 2048-entry full-wave sine table (~8 KiB rodata in
  SRAM_EXEC; drop to 1024 entries if region pressure matters), linear
  interpolation, max error ~1e-6 - far below audibility and below
  Character's own deterministic variation.
- Phase as float cycles in [0,1) with wrap, matching the delay LFO
  pattern; `SetFreq()` computes `phase_inc = hz * sr_recip` (multiply
  only). No amplitude member (the engine scales externally).
- `OrganEngine`: replace `Voice::osc` type `daisysp::Oscillator` ->
  `SineLut`; call sites keep their semantics (Init, SetFreq in
  `UpdateFreqs` and the vibrato path, `Process()` per sample). Phase is
  never reset at note-on.
- Host test `code/tests/sine_lut_test.cpp`: max abs error vs `sinf` over a
  dense phase sweep; phase continuity across `SetFreq` retunes; wrap
  correctness at inc boundaries; frequency accuracy via coherent DFT.
- Re-run the engine-rendering tests (percussion, etc.): output values
  shift by ~1e-6, so tolerance-based asserts must still pass; investigate
  any exact-equality assert that relied on `sinf` values.

### 3. Note-to-Hz table (kill the per-event `powf` burst)

- 128-entry MIDI note -> Hz table filled once in `OrganEngine::Init()`
  with the existing `440 * powf(2, (n-69)/12)` formula - boot-time `powf`
  is fine, and table values stay bit-identical to today's frequencies.
- Fine tune: fold `2^(cents/1200)` into one `fine_mult_` recomputed when
  `fine_cents_` changes (the `freqs_dirty_` block-rate path already
  exists); `UpdateFreqs()` becomes table lookup + one multiply + the
  unchanged foldback loop and Character coeff updates.
- Host test: table monotonic, A4 = 440 exactly, exact octave doubling;
  engine-level `UpdateFreqs` matches the old `powf` formula within 1 ULP
  relative.

### 4. Block-rate steal completion

- When the STEAL fade completes inside `Process()`, do not call
  `StartVoice()` there; mark the voice (e.g. a `STEAL_START` state or a
  pending-start flag) and run `StartVoice()` once per block from
  `Prepare()`. The new note starts <= 0.5 ms later - inaudible - and the
  per-sample loop does zero event work.
- Preserve the C1 invariants: `NoteOn()`/`NoteOff()` branches that match
  a mid-STEAL voice by `pending_note` must also match the new state;
  retrigger-during-steal semantics unchanged.
- Host test: steal still starts the pending note after the fade;
  retrigger and pending-cancel paths behave exactly as before.

### 5. Contingency only - if the meter still peaks over 85%

- Move note/encoder event handling out of the audio ISR into the main
  loop via a small SPSC event queue (key latency <= 1-3 ms, imperceptible
  on an organ); `PollControls` keeps only what must be sample-timed.
- `Vibrato`: reuse `SineLut` for the LFO and approximate `exp2f` over its
  tiny +-50-cent argument range with a short polynomial or small table.
- `RotarySpeaker`: route its 2 `sinf`/sample through `SineLut`.
- Explicitly NOT options here: reducing polyphony, halving effect rates,
  or thinning the reverb/delay - they change the agreed sound.

### 6. Validate behavior, memory, and CPU

- New `sine_lut_test.cpp` + engine note-table asserts registered in the
  existing test/clean targets; run focused tests then the full native
  suite (documented pre-existing failures excluded).
- Clean ARM release build and meter build with the pinned GCC: fresh
  binaries, all linked regions below capacity (watch SRAM_EXEC with the
  new rodata table).
- Hardware acceptance, explicitly separate from host/build verification:
  repeat the section-1 worst-case scene with latch ON/OFF toggling and
  active playing; confirm the crackling is gone and peak load is below
  85%; A/B-listen for any timbral change (there should be none). If no
  board is accessible, report CPU/listening checks as pending; do not
  claim hardware verification.

### 7. Update directly related documentation

- `OrganEngine.h` header comment (oscillator core description), README /
  todo.md if they mention CPU or the 6-voice ceiling, and the repo build
  notes with before/after meter numbers.

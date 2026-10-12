# ORGAN Per-Pipe Tonewheel Character Plan

Status: planned, not implemented.

Replace the four pipe encoders' page-2 sine-to-square morph with independent
tonewheel-inspired Character amounts. Preserve the existing UI and limit
implementation changes to ORGAN.

## Confirmed Decisions

- Exactly four Character controls total: one amount per pipe, not four controls
  per pipe. There is no global amount, linked mode, or additional subcontrol.
- Each pipe's amount applies to that pipe across all six voices. Adjusting one
  pipe must not change the Character settings or isolated spectra of the others.
- Page 1 remains pipe level; page 2 becomes Character. Preserve independent
  page toggles, physical encoder mapping, colors, meters, and press behavior.
- Retain 64 steps: step 0 is the existing clean sine; step 64 is clearly aged
  but still organ-like, not square or deliberately broken. Remove square morph.
- Use inexpensive harmonic coloration and stable note/pipe-dependent variation,
  not FM, random pitch drift, noise, or additional oscillators.
- Keep MIDI CC 74-77, one per pipe, with existing scaling. Their meaning changes
  from waveform morph to Character. CC 70-73 remain pipe levels.
- This is pickup/tonewheel-inspired coloration, not an accurate physical
  Hammond model or authentic neighboring-tonewheel leakage simulation.

## Implementation Defaults

- Boot all four amounts at zero. Retain them across page changes during the
  session; no preset or SD-card persistence.
- Use a finite, sample-rate-derived approximately 10 ms amount ramp, with exact
  settled endpoints and retargeting from the current amount.
- Start maximum voicing around 6% third harmonic and 2.5% second harmonic
  relative to the fundamental before normalization. Calibrate by listening;
  these are implementation constants, not additional user controls.
- Vary harmonic strengths deterministically by up to +/-15% using note and
  pipe identity. Do not vary pitch or depend on voice allocation or randomness
  at note-on.
- Exclude leakage from this version. Muted pipes and completed voice envelopes
  must remain silent.

## Verified Code Anchors

- `OrganEngine::Init`, `Process`, `Voice`, `UpdateFreqs`, `StartVoice`, and
  `Prepare` own oscillator state, rendering, frequency changes, and allocation.
  There are six voices and four pipes, currently with sine and polyBLEP square
  oscillators per pipe.
- The vendored DaisySP sine oscillator calls `sinf` per sample. Morph midpoint
  invokes both waveform paths; zero-weight paths and idle voices stop advancing.
  Existing phases are retained, not continuously free-running tonewheels.
- `PipeRatio`, `NoteToHz`, and `UpdateFreqs` set registration frequencies with
  octave foldback above 20 kHz. `StartVoice` updates frequencies; `Prepare`
  refreshes them when fine tuning changes.
- `chompi_main.cpp` uses `pipe_encoder = {3, 0, 1, 2}`. Preserve this mapping
  rather than assuming physical encoder order matches pipe indices.
- `wave_page_` tracks each pipe's page independently. Page 2 uses a blue encoder
  LED with brightness `0.2 + 0.8 * step / 64`, and a blue white-key bar.
- The existing meter rounds `(step * 15 + 32) / 64`, yielding 0, 8, and 15 keys
  at steps 0, 32, and 64. Reuse `white_key_led`, mapping hardware indices 24
  through 10, and preserve the 500 ms timeout and held-note restoration.
- `PollControls` calls `SetPipeWaveStep`; outgoing queue kind 3 carries its
  pipe and step, and the main-loop dispatch calls `SendWaveCC`.
- `OrganMidi` receives CC 74-77 with `(value * 64 + 63) / 127` and sends values
  with `(step * 127 + 32) / 64`. Preserve channel, DIN/USB paths, and queueing.
- ORGAN runs at 48 kHz with 24-sample blocks, giving the complete callback a
  0.5 ms deadline. Its release build uses `BOOT_SRAM`, `DEBUG=0`, and `OPT=-O3`,
  with WAVE's libDaisy/DaisySP reused read-only.
- No ORGAN native test harness or README was found during planning. Recheck
  before creating either if adjacent features land first. Do not assume the
  specification referenced by older comments exists.

## Implementation Steps

### Phase 1: Small DSP Boundary and Focused Proof

1. Add an ORGAN-owned, header-only `TonewheelCharacter.h` without MCU or Daisy
   dependencies. Keep small amount state per pipe and cached voicing coefficients
   per voice/pipe. Leave sine oscillator ownership in `OrganEngine`.

   Provide initialization, a clamped 0-64 step setter/getter, amount advancement,
   and inexpensive coefficient/shaping helpers. Initialize every state explicitly.
   Do not introduce a global Character scalar.

2. Derive harmonics from the existing normalized sine sample `s`:

   ```cpp
   const float second = 2.f * s * s - 1.f;
   const float third  = s * (3.f - 4.f * s * s);
   ```

   The second term is negative cosine at twice the phase; the third is sine at
   three times the phase. No extra oscillator or trigonometric call is needed.

   This is an oscillator waveform shaper, not a generic zero-preserving audio
   filter. The second term is zero-mean over sine cycles, but is not zero when
   an instantaneous sine sample equals zero. Test generated cycles and apply
   pipe gain/envelopes after shaping; do not incorrectly require `Shape(0) == 0`
   at nonzero Character.

3. Use smoothstep on the normalized Character amount for a gentle lower range.
   Start with the harmonic strengths and deterministic variation specified above.
   Repeated notes on a given pipe must have the same voicing regardless of which
   voice slot renders them.

4. Cache variation and harmonic anti-alias masks when frequencies are initialized
   or updated. Use the final folded pipe frequency and actual sample rate.

   For each added harmonic, smoothly fade its strength from full at harmonic
   frequency `0.35 * sample_rate` to zero at `0.45 * sample_rate`; keep it zero
   above that. Recalculate masks for fine-tune changes. Preserve fundamental
   tuning and foldback.

   Add no per-sample random generation, allocation, coefficient transcendental
   calculation, or extra `sinf`/`powf` calls. The existing fundamental `sinf`
   remains. Keep frequency-dependent coefficient work outside the sample loop.

5. Precompute maximum-aged normalization from
   `1 / (1 + abs(second_gain) + abs(third_gain))` when cached voicing changes.
   This bounds the aged waveform to magnitude 1 and preserves mix headroom.

   Blend original sine toward the bounded aged waveform using the eased current
   amount. This convex blend smoothly changes harmonics and normalization without
   division per sample. Settled Character zero bypasses shaping and returns the
   original sine exactly.

   Advance the finite approximately 10 ms ramp once per pipe per sample, not once
   per voice. Advance controls even when voices are idle. Retarget from the
   current amount and reach exact endpoints without clicks.

6. Immediately add one native assert-based `tonewheel_character_test.cpp` and
   a minimal test Makefile. Reuse an existing ORGAN test Makefile if another
   feature has created one; do not replace its targets or link ARM archives.

   Cover four independent control instances, clamping and invalid-index guards,
   exact clean endpoints, ramp convergence and retargeting at 48 kHz and another
   sample rate, stable note/pipe variation independent of voice slot, cycle-mean
   DC, expected second/third harmonics, amplitude bounds, and high-frequency
   suppression. Use coherent sine cycles and DFT/bin projections with tolerances.
   Sweep folded frequencies and verify zero pipe gain or envelope is silent.

7. Run focused native DSP tests before engine/UI integration. If a host compiler
   is unavailable, document that blocker, use ARM compilation as the first
   available focused check, and explicitly leave numerical tests unverified.

### Phase 2: Engine, Then UI and MIDI

8. After DSP proof, replace `Voice::osc[pipe][waveform]` with one sine oscillator
   per pipe. Remove the square initialization, waveform enum, two-waveform count,
   and morph crossfade.

   Introduce Character-named state and APIs:

   - `kMaxCharacterSteps = 64`, `kDefaultCharacterStep = 0`.
   - `SetPipeCharacterStep(pipe, step)` and `GetPipeCharacterStep(pipe)`.
   - Four independent steps, targets, and ramp states.
   - Cached voicing and anti-alias coefficients per voice/pipe.

   Guard invalid pipe indices without accessing arrays and initialize all state.

9. Update `OrganEngine::Process` to advance four amounts once per sample, shape
   each active voice's corresponding sine, then apply existing pipe gain, voice
   envelope, and velocity in the existing order.

   Preserve note allocation, retriggering, voice stealing, envelopes, tuning,
   `kMixScale`, downstream effects, master gain, and all four output assignments.
   Routing remains Character inside each pipe -> voice mix -> distortion ->
   rotary -> master/output. Include plate after rotary only if the separate
   reverb feature is already implemented.

   Rerun native tests and the ARM build before UI/MIDI changes.

10. Migrate waveform-named consumers in `PollControls`, `UpdateLeds`,
    `CcLevelCallback`, outgoing queue dispatch, boot initialization, and
    `OrganMidi`. Use Character semantics for names such as `character_page_`,
    `kBarCharacter`, `kCharacterCcBase`, and `SendCharacterCC`.

    Preserve independent page toggles, settled boot tracking, press edges,
    encoder chatter filtering, one step per accepted click, and existing
    applicable SHIFT behavior. Page 1 remains unchanged. Page 2 retains its blue
    LED, brightness floor, rounded meter, current-value display on press,
    500 ms timeout, dark black keys during the bar, and held-note restoration.

11. Preserve MIDI receive/send scaling and assignments: CC 74 changes only pipe
    0; CC 75 pipe 1; CC 76 pipe 2; CC 77 pipe 3. Keep CC 70-73 as levels.
    Preserve callback page selection, queue payload routing, channel, and DIN/USB
    transport behavior.

    Document the semantic incompatibility: old automation selecting square now
    selects maximum Character. Do not add CCs, pages, or compatibility modes.

12. Rerun focused tests and the ARM build after this slice. Check all morph
    consumers are migrated without unrelated cleanup. If normalization causes
    perceptible attenuation, adjust voicing locally while maintaining a proven
    peak bound; do not introduce a global level or Character control.

### Phase 3: Documentation and Acceptance

13. Create or update a compact ORGAN README covering four independent amounts,
    their shared-across-voices meaning, endpoints, 64 steps, unchanged UI,
    MIDI assignments, dry boot/session retention, and sine-square removal.
    Avoid claims of real leakage or accurate physical emulation. Leave separate
    reverb/distortion plans unchanged.

14. Run native tests, a clean ARM release build, size/map inspection, and hardware
    UI/audio/CPU acceptance. Do not automatically flash hardware or modify the
    bootloader. Clearly distinguish automated checks from unperformed hardware
    checks.

## Relevant Files

Paths are relative to the ORGAN directory.

| File | Role |
| --- | --- |
| `code/src/OrganEngine.h` | Oscillators, four Character amounts, frequency masks, synthesis. |
| `code/src/chompi_main.cpp` | Independent pages, encoder mapping, meters, LEDs, MIDI queue. |
| `code/src/MidiHandler.h` | Character CC receive/send and documentation. |
| Proposed: `code/src/TonewheelCharacter.h` | Native-testable harmonic/control helper. |
| Proposed: `code/tests/tonewheel_character_test.cpp` | Focused numerical tests. |
| Proposed or existing: `code/tests/Makefile` | Minimal native test recipe; preserve other targets. |
| Proposed or existing: `README.md` | ORGAN usage and changed MIDI semantics. |
| `code/src/Makefile`, `RotarySpeaker.h`, `hardware.h`, `chompi_sram.lds` | Build, style, hardware, and memory references only. |
| `../chompi-wave/code/libs/DaisySP/Source/Synthesis/oscillator.cpp` | Existing sine implementation; read-only. |

## Verification

1. Native DSP: run `make -C firmware/chompi-organ/code/tests test` once the
   target includes the Character test. Use only the host compiler and native
   dependencies. Spectral tests validate harmonic content, not subjective warmth.

2. ARM release: run `make -C firmware/chompi-organ/code/src DEBUG=0 OPT=-O3`
   using GNU Arm Embedded 10.3-2021.10. Use a clean build for final acceptance.
   Inspect `arm-none-eabi-size` and the map for `BOOT_SRAM` region fit. This
   feature requires no new SDRAM allocation.

3. Independence: set amounts 0/16/32/64 and adjust each pipe in turn. Other
   getters, page states, displays, and isolated-pipe harmonic spectra must not
   change. Repeat through CC 74-77 on both transports and across notes, chords,
   retriggers, release, and voice stealing. Downstream nonlinear distortion can
   change total mix timbre when a source changes; it must never mutate another
   pipe's Character setting.

4. UI/MIDI: preserve physical mapping and page-1 levels; verify independent
   toggles, blue page-2 LEDs with visible floor, 0/8/15-key meters, clamped limits,
   500 ms timeout, turn/press display, retained values, held-note restoration,
   and applicable SHIFT behavior. Incoming CC 0/127 reaches exact endpoints;
   outgoing CC reflects only the adjusted pipe.

5. Audio: zero matches the previous sine-only engine with equal downstream
   settings. Maximum is clearly aged but not square/buzzy. Repeated pitches have
   stable voicing; muted pipes and completed envelopes are silent. Verify no
   persistent DC, clipping, clicks, or added high-note aliasing, including rapid
   Character changes and fine tuning. Preserve matching stereo output pairs and
   master mute behavior. Judge warmth on hardware.

6. CPU: measure old sine-only and old morph-midpoint baselines against new
   zero/midpoint/maximum Character. Use six voices, all four pipes at high levels,
   high distortion and rotary, plus reverb if that feature has already landed.
   Time the complete callback, including MIDI and controls. Hard deadline is
   0.5 ms; target at most 0.4 ms. Do not claim savings merely from fewer
   oscillators. If the budget fails, optimize locally or discuss tradeoffs;
   never silently reduce voices or remove existing effects.

## Scope Boundaries and Dependencies

Included: four independent Character amounts replacing four morph amounts,
algebraic harmonic coloration, stable note/pipe variation, frequency-aware
harmonic suppression, smooth endpoints, unchanged UI/MIDI numbering, focused
tests, and documentation.

Excluded: global or linked Character amounts, four controls per pipe, extra
pages, square fallback, physical leakage generators, idle background tones,
key click, FM, pitch drift/flutter, noise, a shared tonewheel bank, oversampling,
persistence, new CCs, unrelated firmware edits, reverb/distortion implementation,
bootloader changes, and automatic flashing.

DSP proof precedes engine integration; engine validation precedes UI/MIDI work.
Documentation and final acceptance follow stable control semantics. Reuse nearby
test/documentation infrastructure if adjacent ORGAN features are implemented
first, without expanding this feature's scope.
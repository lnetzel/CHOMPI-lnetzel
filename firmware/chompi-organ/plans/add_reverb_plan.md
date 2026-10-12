# ORGAN Blue Plate Reverb Plan

Status: planned, not implemented.

Add a third page to CHOMPI ORGAN's Volume encoder. Use a compact,
vintage-1960s-inspired plate reverb after distortion and rotary, before master
volume. Changes are limited to ORGAN.

## Confirmed Decisions

- Volume pages cycle: pg.1 master volume -> pg.2 distortion -> pg.3 reverb mix.
- Pg.3 has a blue encoder LED, with a brightness floor so it remains visible at
  0% mix.
- Mix changes by 1% per click, clamped to 0-100%: 0% is exact dry and 100% is
  wet only. Use a linear complementary crossfade, not an additive send.
- Boot at 0% mix. Retain the setting throughout the session and across page
  changes; no preset or SD-card persistence.
- Approved audio order: voices -> distortion -> rotary -> plate -> master gain
  -> headphone and master stereo output pairs.
- Turning or pressing the encoder shows the current page's keybar for 500 ms.
  The reverb bar uses blue across the 15 white keys: none at 0%, approximately
  half at 50%, all at 100%. Black keys remain dark during the bar.
- Keep the existing SHIFT + Volume fine-tuning behavior. The distortion-type
  selector is a separate planned feature, not part of this change. If that
  feature is implemented first, preserve its page-2-only selector behavior.
- Fixed plate voicing; no additional decay, tone, or predelay pages and no new
  MIDI CC assignments.

## Verified Code Anchors

- `OrganEngine::Init`, `Process`, `SetDriveStep`, `SetRotaryStep`, and
  `SetMasterGain` own DSP state and routing. `Process` currently runs rotary
  before master gain, then duplicates left/right onto output pairs 0/1 and 2/3.
- `PollControls` uses rotary index `kVolEnc = 5` and `ENC_6_SW` (SwId 32).
  `vol_drive_page_` is currently a boolean selecting master or distortion.
- `UpdateLeds` owns the 500 ms value bars and encoder LED colors. The Volume
  encoder LED is PTH 9. `white_key_led` maps the 15 white keys to hardware LED
  indices 24 through 10; reuse this mapping rather than literal indices 0-14.
- ORGAN runs at 48 kHz with 24-sample blocks: the complete callback has a
  0.5 ms deadline and runs 2,000 times per second.
- WAVE's existing `Reverb` uses a compact Griesinger/Dattorro topology: four
  input allpasses and a cross-feedback tank. It is not `ReverbSc`.
  `FxEngine<32768, FORMAT_16_BIT>` needs 32,768 `uint16_t` elements, or 64 KB.
- The old reverb initialization does not explicitly clear its delay memory,
  write pointer, or damping histories. An adaptation must initialize all state.
  `FxEngine::Init` initializes its pointer and LFO state; `Clear` clears the
  buffer and write pointer.
- ORGAN's Makefile reuses WAVE's libDaisy/DaisySP, builds `CHOMPI_ORGAN_1.0`
  with `BOOT_SRAM`, `DEBUG=0`, and `OPT=-O3`. Existing build artifacts identify
  GNU Arm Embedded 10.3-2021.10, but do not validate the proposed feature.
- No ORGAN host-test harness or README/specification file exists at planning
  time. Some source comments and older plans reference a missing specification;
  do not assume that file exists.

## Implementation Steps

### Phase 1: Plate DSP and Focused Proof

1. Add a small ORGAN-owned `PlateReverb.h`, adapting the already-vendored WAVE
   topology rather than inventing a new reverb. Retain its original third-party
   notice. Reuse WAVE's `fx_engine.h` read-only through an explicit relative
   include; do not change WAVE or TAPE. Keep the delay-engine and filter/control
   state in normal SRAM and accept caller-owned `uint16_t[32768]` storage.

   Initialize every state variable after hardware initialization: delay memory,
   write pointer, LFOs, damping histories, mix/ramp state, and any input filter.
   Provide initialization, a clamped mix setter/getter, and stereo processing.

   Initial voicing: diffusion around 0.625, feedback around 0.8, dark damping
   around 4-6 kHz, and subtle slow modulation. Target approximately 1.5-2 seconds
   of decay, calibrated by impulse tests and listening; feedback is not a decay
   time in seconds. Consider a lightweight approximately 120 Hz low-cut on the
   mono tank send if needed during voicing. This is plate-inspired, not a claim
   of accurate physical EMT emulation.

   Normalize the mono send and use conservative wet gain to prevent internal
   16-bit saturation. Preserve stereo dry audio and produce stereo wet audio.
   Process the wet tank independently, then blend `dry * (1 - mix) + wet * mix`.
   Use a finite, sample-rate-derived approximately 10 ms ramp, retargeting from
   the current mix and reaching exact endpoints.

   Keep the tank running at 0% and with idle voices for continuous live tails
   and no stale state. No heap allocation, coefficient transcendental
   recalculation, or bulk clearing inside the callback. Measure CPU cost rather
   than assuming this continuous-processing tradeoff fits.

2. Immediately validate the DSP before UI work. Add one native assert-based
   test source and a minimal test Makefile, without a framework or ARM archives.
   Cover silent/repeated initialization with a nonzero-prefilled buffer,
   finite/bounded impulse response, a decaying stereo tail, mix clamping,
   ramp convergence and retargeting, stereo dry preservation, and exact settled
   dry/wet endpoints. Compare 100% output to the wet-only reference. If a host
   compiler is unavailable, use the ARM build as the first focused check and
   explicitly report the unrun numerical tests.

### Phase 2: Engine and UI

3. Declare a file-scope `DSY_SDRAM_BSS` delay array in `chompi_main.cpp`.
   Do not write SDRAM in global constructors. Pass the buffer to
   `OrganEngine::Init` after `hw.Init`; keep small processor state in the engine.
   Expose `SetReverbMixPercent` and `GetReverbMixPercent`. Process the stereo
   plate after `rotary_.Process` and before master-gain multiplication. Preserve
   all four output assignments and continue processing when voices are idle.
   Rerun focused DSP checks and the ARM application build before UI changes.

4. Replace `vol_drive_page_` with an explicit three-valued page state and named
   constants. Cycle master -> distortion -> reverb -> master using the existing
   press edge. Preserve settled boot-state tracking and encoder chatter filtering.
   Unshifted pg.3 rotation adjusts integer mix percent; shifted rotation retains
   the applicable fine-tuning behavior. Keep each page's value independent.

   Extend `bar_page_` to identify all three pages. Reuse the existing blue color
   rendering, with a semantically named reverb bar kind where appropriate.
   Round `mix_percent * 15 / 100` to the nearest white-key count: 0, 8, and 15
   keys at 0%, 50%, and 100%. Restore normal held-note LEDs after 500 ms.
   PTH 9 is blue on pg.3 with brightness `0.2 + 0.8 * mix`. Leave other page
   colors, MIDI handling, notes, latch, and tuning state unchanged. Rerun focused
   checks and the ARM build after this slice.

### Phase 3: Documentation and Acceptance

5. Add a small ORGAN README documenting page order, colors, 1% increments,
   endpoints, routing, dry boot/session-only state, fixed plate character,
   and SHIFT behavior. Leave the separate distortion-selector plan untouched.

6. Run native tests, a clean ARM release build, ELF size/map inspection, and
   hardware UI/audio/CPU acceptance. Do not flash the bootloader or automatically
   flash hardware. Clearly distinguish automated checks from unperformed
   hardware checks.

## Relevant Files

Paths are relative to the ORGAN directory.

| File | Role |
| --- | --- |
| `code/src/OrganEngine.h` | DSP initialization, plate integration, mix setter/getter. |
| `code/src/chompi_main.cpp` | SDRAM storage, page handling, keybar, encoder LED, boot state. |
| Proposed: `code/src/PlateReverb.h` | Adapted compact plate, explicit state initialization, mix ramp. |
| Proposed: `code/tests/plate_reverb_test.cpp` | One focused native DSP test source. |
| Proposed: `code/tests/Makefile` | Minimal host test recipe. |
| Proposed: `README.md` | Current ORGAN usage documentation. |
| `../chompi-wave/code/src/reverb.h` and `fx_engine.h` | Existing algorithm and delay-engine references; read-only. |
| `code/src/RotarySpeaker.h`, `hardware.h`, `Makefile`, `chompi_sram.lds` | Existing conventions and build/memory configuration; reference only. |

## Verification

1. Native DSP: run `make -C firmware/chompi-organ/code/tests test` once the
   recipe exists. Check endpoints, deterministic initialization, tails,
   clamping, ramps, stereo behavior, and long silence. Never link ARM archives
   into host tests.
2. ARM release: run `make -C firmware/chompi-organ/code/src DEBUG=0 OPT=-O3`
   with GNU Arm Embedded 10.3-2021.10. Use a clean build for final acceptance.
   Inspect `arm-none-eabi-size` and the map: the 65,536-byte buffer must be in
   SDRAM, not stack or SRAM, and all `BOOT_SRAM` regions must fit. TAPE memory
   headroom does not apply to ORGAN.
3. Hardware UI: repeated three-page cycling, visible blue encoder LED at 0%,
   0/50/100% blue keybars, clamped limits, 500 ms timeout, current-value display
   on page clicks, retained values, and unchanged applicable SHIFT tuning.
4. Audio: unchanged settled dry at 0%, wet only at 100%, balanced midpoint,
   tail after note release, master mute silencing dry and wet, correct effect
   order, matching stereo output pairs, and no clicks, DC, or runaway output
   during rapid mix changes. Judge the vintage plate character on hardware.
5. Performance: measure baseline and added CPU with six voices, all four pipes
   at high levels, waveform midpoint activating both oscillators, maximum
   distortion, rotary, and reverb. Use temporary timing instrumentation if
   necessary. The complete callback, including MIDI and controls, must stay
   below 0.5 ms; target at most 0.4 ms for margin. No dropouts. If it fails,
   optimize locally or discuss tradeoffs; do not silently remove existing
   features or change confirmed requirements.

## Scope Boundaries

Included: ORGAN pg.3, blue mix meter and encoder LED, compact fixed plate,
exact dry/full-wet endpoints, smooth control, correct output routing,
focused tests, and documentation.

Excluded: TAPE/WAVE/TEMPO changes, distortion-selector implementation,
SD persistence, new MIDI CCs, additional effect pages, convolution,
oversampling, bootloader changes/flashing, and unrelated fixes.
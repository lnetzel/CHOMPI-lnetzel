# Selectable ORGAN Distortion Plan

Status: superseded by distortion_update_plan.md (2026-10-13). The original
Existing/Warm/Fuzz implementation is replaced with Warm/Fuzz/BrokenSpeaker;
boot default changed from Existing to Warm. Distortion.h, OrganEngine.h,
chompi_main.cpp, distortion_test.cpp, README.md, and specification updated.
ARM build and tests pending.

Previous Status: implemented 2026-10-12. `Distortion.h` (Existing/Warm/Fuzz,
10 ms sum-preserving weight crossfade, wet->dry bypass ramps, primed filter
histories) was integrated; SHIFT + Volume page 2 selected the type with the
five-red-key overlay; native test `code/tests/distortion_test.cpp` was
checked; clean ARM build achieved. Spec FR-4b/FR-5a/DSP-5/UI-1 and README
updated.

Original Task: Add three selectable distortion characters to CHOMPI ORGAN's
existing Volume page 2 using Existing / Warm / Fuzz. Held for revision to
introduce BrokenSpeaker instead of Existing and update boot default.

## Confirmed Decisions

- Target firmware: CHOMPI ORGAN.
- Selection clamps at the ends, without wrapping. Clockwise moves Existing -> Warm -> Fuzz; anticlockwise reverses. Use accumulated signed encoder delta and the existing chatter filter.
- Show the selected type immediately when SHIFT is held on Volume page 2, without requiring rotation, and continuously throughout the hold.
- On release, show the unchanged distortion-amount red bar for 500 ms, then resume existing note LEDs.
- Default to Existing every boot. No persistence or new MIDI CC.
- SHIFT + Volume page 1 remains fine-tuning; page 2 no longer fine-tunes.
- Type selection never changes amount, master volume, tuning, or note state. Amount applies to all three types; zero is dry bypass.

| Type | Sound | Red Keys | Keys Turned Off |
| --- | --- | --- | --- |
| Existing | Current tanh overdrive, unchanged | KEY_1-KEY_5 | KEY_6-KEY_15 |
| Warm | Vintage-style warm soft clipping | KEY_6-KEY_10 | KEY_1-KEY_5 and KEY_11-KEY_15 |
| Fuzz | Aggressive fuzz saturation | KEY_11-KEY_15 | KEY_1-KEY_10 |

## Verified Code Anchors

- `OrganEngine::Init`, `SetDriveStep` / `GetDriveStep`, and `Process` own amount state and DSP. Existing drive uses quadratic pre-gain `1 + 63*d*d`, tanh shaping, and post compensation `0.33/tanh(0.33*g)`, before rotary and master gain. Keep these operations unchanged in Existing mode at nonzero steady amount.
- `PollControls` reads `KEY_26` as SHIFT. Volume rotary index `kVolEnc=5`; `ENC_6_SW` toggles `vol_drive_page_`. The existing SHIFT branch currently fine-tunes on either page.
- `UpdateLeds` uses a 500 ms timed bar. `white_key_led` maps KEY_1-KEY_15 to reversed hardware LED numbering. Use this mapping, never literal LED indices 0-14.
- `bar_pipe_`, `bar_page_`, and `bar_until_` are ISR-to-main UI state. Any new selector flag must follow the existing volatile cross-context convention. Keep rendering separate from note handling.
- Specification FR-4b and FR-5a explicitly assign SHIFT + Volume fine-tuning on both pages and must change. DSP-5, UI-1, and defaults also need updates.
- No ORGAN test framework exists. The Makefile uses WAVE libDaisy/DaisySP, target `CHOMPI_ORGAN_1.0`, `BOOT_SRAM`, `DEBUG=0`, and `OPT=-O3`. `make program-boot` installs the bootloader, not the new ORGAN application, and is not a required feature-validation step.

## Implementation Steps

### Phase 1: DSP Ownership and Regression Check

1. Add a small distortion processor beside the rotary-speaker processor, reusing existing DaisySP filtering primitives instead of introducing a DSP dependency. Expose named types Existing/Warm/Fuzz, `Init(sample_rate)`, a clamped type setter/getter, and per-sample processing driven by OrganEngine's existing smoothed drive amount. OrganEngine continues to own drive steps and amount smoothing. Initialize Existing explicitly from `OrganEngine::Init`; call the processor at the existing mono stage before rotary/master. No oscillator, voice, rotary, or routing changes.

   - **Existing:** retain the exact quadratic gain, tanh, and level-compensation arithmetic at settled nonzero amount. Do not add filtering to this branch.
   - **Warm:** gentler pre-gain, initially 1-16 with quadratic amount mapping, and smooth asymmetric tanh saturation with modestly different positive/negative knees for even harmonics. Remove resulting DC using a sample-rate-derived approximately 20 Hz blocker. Use a gentle approximately 6 kHz post low-pass for rounded highs. Zero input must produce silence after filter settling. Normalize around the existing 0.33 reference level and check both polarities. These constants are initial voicing values to finalize during listening, not claims of a specific vintage circuit.
   - **Fuzz:** stronger pre-gain, initially 1-128 with quadratic amount mapping, and symmetric steep near-hard saturation. Use an existing DaisySP overdrive primitive if its measured curve fits; otherwise use a simple bounded piecewise soft-knee clipper. Apply a sample-rate-derived approximately 10 kHz post low-pass to moderate harsh highs. Normalize near the existing reference level. No bit reduction, quantization, noise, or intentional DC.
   - **Bypass:** all types produce dry output at zero amount with no residual filtered tail. Preserve smooth amount changes, including returning to bypass. Do not add an abrupt step-zero branch while a nonzero smoothed amount is still audible. Use a finite transition-to-dry or tiny-state snap after inaudible settling so zero ultimately becomes exact bypass.
   - **Type transitions:** use a linear finite 10 ms ramp of a three-element nonnegative weight vector summing to one toward the selected one-hot target. If retargeted mid-ramp, start from the current weights, not an old mode. The getter returns the selected target immediately for LEDs. Stable mode processes only that branch; during fades process branches with nonzero weights. Keep lightweight filter histories primed from the live signal during bypass/inactive periods so activating Warm/Fuzz cannot expose stale tails. No heap allocation or expensive coefficient recalculation inside the sample loop.

2. Immediately after initial processor extraction/integration, run a focused regression check before adding UI changes. Add one standalone native C++ assert-based test source under ORGAN's tests directory, with no framework dependency. Compare Existing output to the original formula at representative positive/negative inputs and nonzero amounts; test initialization, clamping, and zero bypass. Use native-compatible DaisySP sources as appropriate. Run the ARM application build if the host toolchain is absent and explicitly record any unrun numerical checks. This step depends on step 1; a failing regression blocks UI work.

### Phase 2: Controls and Display

3. Update `PollControls`: handle `shift_held_ && vol_drive_page_` before the existing SHIFT fine-tune branch. Rotate type with signed delta and clamp to the three modes. Normal page-2 turns still call `SetDriveStep`; page-1 shifted turns still call `AddFineTuneCents`. Keep existing encoder-switch behavior and 8 ms chatter filtering. Publish a selector-active UI state after page toggles and SHIFT sampling. Detect selector exit in the polling context and re-arm the Volume amount bar for 500 ms only when still on page 2. Changing to page 1 while SHIFT is held closes the selector and shows the page-1 value, not a stale drive bar. Depends on steps 1-2.

4. Update `UpdateLeds` with selector rendering as the highest-priority keyboard display while SHIFT + page 2 is active, independent of bar timeout and other encoders/MIDI bars. Clear all 25 keyboard SMT LEDs, then use `white_key_led` to light exactly five red: Existing positions 0-4, Warm 5-9, Fuzz 10-14. All other keyboard LEDs are off during this overlay, including octave hints. Leave dedicated SHIFT/latch and knob indicators intact; the Volume knob remains red and tracks amount. Release uses the ordinary red amount meter and existing timeout. Physical/MIDI notes and octave-key functions remain functional underneath the visual overlay. Depends on step 3; validate control/display before proceeding.

### Phase 3: Coverage, Documentation, and Acceptance

5. Extend the same focused test source with finite/bounded output, silent-input/DC settling, zero bypass for every type, rapid repeated type changes, transition-weight normalization/convergence, and amount retention. Check Warm/Fuzz harmonic distinction with sine input or captured hardware output. Assess discontinuity relative to local waveform slope, not a naive fixed consecutive-sample threshold. Build tests with a native C++ compiler using the existing DaisySP source set; do not link ARM prebuilt archives into host tests. Add a tiny test build recipe only if needed; avoid creating a new framework. Record toolchain blockers honestly. Depends on phase 1; can proceed in parallel with controls/display implementation once the processor API is stable.

6. Update specification FR-4b, FR-5a, DSP-5, UI-1, and boot defaults to document the selector, exact groups, clamp behavior, temporary overlay, and fine-tuning only on page 1. Do not alter the existing bugfix plan or unrelated persistence requirements. Final ARM build and hardware acceptance follow all implementation steps. Documentation can proceed in parallel with step 5 after control behavior is fixed.

## Relevant Files

Paths below are relative to the ORGAN directory.

| File | Planned Role |
| --- | --- |
| [code/src/OrganEngine.h](../code/src/OrganEngine.h) | Integrate the processor in `Init` and `Process`, retaining amount smoothing and chain order. |
| [code/src/chompi_main.cpp](../code/src/chompi_main.cpp) | Update `PollControls`, selector/bar state, and `UpdateLeds`; reuse `white_key_led`. |
| Proposed: `code/src/Distortion.h` | Small owner for algorithms, filter state, and type transitions. |
| Proposed: `code/tests/distortion_test.cpp` | One focused native test source; standalone build recipe only if necessary. |
| [chompi-organ- specification.md](../chompi-organ-%20specification.md) | Normative control behavior and DSP documentation. |
| [code/src/Makefile](../code/src/Makefile) | Reuse ARM build configuration; modify only if required by the chosen source layout. A header processor needs no added CPP source. |
| [code/src/RotarySpeaker.h](../code/src/RotarySpeaker.h) | Reference only for existing sample-rate initialization and filter ownership style. |

## Verification

1. Run focused native DSP regression checks immediately after extraction, then expanded tests for new modes and transitions. Validate at the firmware sample rate and another rate to verify time/coefficient scaling.
2. From the ORGAN source directory, run `make DEBUG=0 OPT=-O3` using the existing ARM GNU/make environment, such as configured WSL/Git Bash on Windows. Confirm the application binary and successful BOOT_SRAM linker fit. No bootloader changes or automatic flashing.
3. Hardware UI matrix: page-2 SHIFT before any turn; hold longer than 500 ms; both ends clamp; each exact five-key red group with others fully off; release restores the same amount for 500 ms; press Volume while SHIFT is held; other encoder/MIDI bars cannot replace the selector; no stale selector afterward. Check zero, midpoint, and maximum amount for each type.
4. Regression matrix: page-1 volume and SHIFT fine-tune; page-2 unshifted amount; page switching; octave controls; latch; physical/MIDI note on/off without stuck notes; pipe controls; rotary; duplicated stereo headphone/master outputs.
5. Audio acceptance: Existing matches baseline recordings/formula; Warm is smoother, rounder, and distinctly different; Fuzz is markedly more aggressive without bitcrusher character. Test held notes/chords, rapid type retargeting, zero-to-max and max-to-zero changes, and silence/decays. No clicks, DC thumps, runaway output, or filter tails at settled zero. Compare representative loudness/headroom without claiming identical RMS for every waveform/type.
6. Worst-case timing: six held voices, all four pipes at high levels and the square end of morph, rotary maximum, distortion maximum, and repeated mode switching. Measure callback timing with existing project measurement facilities if present; otherwise use temporary debug timing instrumentation. Worst-case processing must remain below the callback deadline, `block_size/sample_rate`, with no audible dropouts. If three-way fades exceed budget, optimize locally before accepting; do not assume CPU headroom.

## Scope Boundaries

Included: three sounds, page-specific SHIFT selector, exact LED groups, smooth live switching, retained amount, focused validation, and specification changes.

Excluded: TAPE/WAVE/TEMPO changes, SD persistence, new MIDI assignments, encoder acceleration, keyboard-based type selection, bootloader updates, and unrelated bugfix work.

## Risks and Implementation Notes

- Existing DSP already uses tanh, so Warm requires distinct filtering/asymmetry/gain voicing, not simply a renamed tanh curve.
- Asymmetry causes DC; Warm must block it without affecting Existing.
- Fuzz generates harmonics above Nyquist. A fixed low-pass controls tone but is not complete anti-aliasing. Evaluate high notes/chords audibly; do not introduce oversampling without a demonstrated need and measured resource budget.
- `fonepole` coefficients are not durations. Derive the 10 ms switching ramp from sample rate rather than copying a smoothing coefficient and calling it 10 ms.
- Host test infrastructure is new and must use native-compatible DaisySP sources. ARM application build and hardware listening remain essential. Completion must distinguish automated checks from tests not run.
# Remove Hardware self test code from Chompi-tape

Status: Implemented 2026-10-06. Firmware builds cleanly; measured results below.
Hardware verification has NOT been performed — the hardware-dependent acceptance
gates (normal startup, shipping mode, MIDI, presets, battery/power, SD, recording,
looping, effects) remain open until tested on a unit.

## Agreed Behavior

- Delete the entire hardware-test boot mode from TAPE. The diagnostic code is removed outright — it is **not** migrated to a sibling firmware or any other build target.
- The removed code remains recoverable from git history if a diagnostic mode is ever needed again.
- Preserve TAPE's normal instrument functionality, shipping mode, battery protection, and preset persistence.
- After removal, holding the GAIN encoder during startup has no special effect; TAPE always boots into the normal instrument.

## Existing Implementation

The mode spans three primary files rather than only the test page:

- [TestPage.h](firmware/chompi-tape/code/src/TestPage.h): diagnostic state, LED rendering, encoder/button tracking, SD test, and completion criteria.
- [ui.h](firmware/chompi-tape/code/src/ui.h): test-page initialization and lifecycle, MIDI routing, toggle updates, and input dispatch.
- [chompi_main.cpp](firmware/chompi-tape/code/src/chompi_main.cpp): startup selection, oscillator audio path, and diagnostic power-controller polling.

Currently, holding the GAIN encoder (`ENC_6_SW`) during startup opens diagnostics. The separate `KEY_26`/`KEY_27`/`KEY_28` shipping-mode combination takes priority and must remain in TAPE.

### Diagnostic Coverage

All of the following is removed:

- Connected keys and encoder switches, excluding unused shift-register inputs and the completion key.
- Six encoders, tracking both directions after accumulated movement exceeds five counts in either direction.
- Both toggle-switch positions.
- LED feedback for the tests.
- Line-input jack detection.
- SD-card create, write, read, close, and delete operations using a temporary file.
- MIDI loopback using notes 60, 64, and 67, with 50 ms event intervals and a completion count of 20 successful receptions.
- Power-cable insertion, detected through the `VIN_RDY` rising edge.
- Battery-management-controller fault/status checks.
- Audio oscillator on all four outputs in one toggle position; existing TAPE engine processing in the other.
- The initial approximately 1.5-second input-ignore period and the completion criteria go away with the page.

### What Must Survive the Removal

`UserInterface::TestPresets()` is ordinary preset serialization despite its name. It and `WritePresets()` must remain in TAPE.

The battery-management drivers and normal power-protection logic stay; only the diagnostic polling block is removed. The shipping-mode key combination and startup control flushing/timing stay; only the volume-encoder diagnostic selection is removed. Normal (non-diagnostic) MIDI routing, channel filtering, note behavior, and transport semantics stay; only the diagnostic synthetic-key routing is removed.

## Implementation Plan

### 1. Establish the Baseline

Build current TAPE using GNU Arm Embedded 10.3-2021.10, the existing `-O3` setting, libraries, and linker script. Preserve its ELF, linker map, binary, and memory-region report before making changes.

The current [linker script](firmware/chompi-tape/code/src/chompi_sram.lds) allocates:

| Region | Capacity |
|---|---:|
| `SRAM_EXEC` | `237K - 768`, or 241,920 bytes |
| `SRAM` | `275K + 768`, or 282,368 bytes |

Code and read-only constants reside in execution SRAM. Initialized `.data` resides in normal SRAM but its load image also occupies execution SRAM. Account for both regions separately.

### 2. Remove Hardware Diagnostics from TAPE

- Delete TAPE's test-page header and remove its include, instance, and initialization from the UI.
- Remove `InTestMode()`, `TestMode()`, `TestPowerCable()`, and `TestBMC()` and their test-only callers.
- Remove completion-page closing, test toggle updates, and test-specific menu/rainbow/no-SD guards.
- Simplify MIDI routing to its normal instrument path while preserving channel filtering, note behavior, and transport semantics. Remove only diagnostic synthetic-key routing.
- Remove the test oscillator, initialization, and audio callback branch.
- Remove the diagnostic battery-controller polling block, not the battery drivers or normal power-protection logic.
- Remove `vol_state` collection and volume-encoder diagnostic startup selection. Preserve startup control flushing, timing, and the shipping-mode combination.
- Simplify the boot-time audio guard to its normal non-diagnostic behavior.

Keep the engine, every TAPE sample/loop/delay buffer, shared peripheral drivers, preset persistence, and ordinary battery handling. Remove only declarations/includes that become unused due to this removal; avoid unrelated cleanup.

### 3. Document and Measure the Result

Update TAPE's README to explain that diagnostics have been removed and the GAIN boot hold no longer opens them.

Rebuild TAPE with the exact baseline compiler, flags, libraries, and linker script. Publish measured before/after memory-region usage and remaining headroom. Run normal TAPE regressions and explicitly report any hardware verification that could not be performed.

## Measured Result (2026-10-06, GNU Arm Embedded 10.3-2021.10, -O3)

| Memory | Baseline | After Removal | Saved | New Headroom |
|---|---:|---:|---:|---:|
| `SRAM_EXEC` (241,920 B) | 241,432 B (99.80%) | 233,208 B (96.40%) | **8,224 B (~8.0 KiB)** | 8,712 B |
| `SRAM` (282,368 B) | 279,668 B (99.05%) | 278,892 B (98.77%) | **792 B (~0.77 KiB)** | 3,476 B |
| External SDRAM | 63,774,720 B | 63,774,720 B | none | unchanged |

Section-level: `.text` 238,772 → 230,548; `.bss` 277,748 → 276,956; `.data` unchanged.
`arm-none-eabi-nm` shows zero `TestPage` symbols in the final ELF. Baseline and
post-removal artifacts preserved in `code/src/build/baseline/`.

## Estimated SRAM Savings in TAPE

These are rough, unmeasured planning estimates, not guaranteed savings:

| Memory | Estimated Saving | Expected Contributors |
|---|---:|---|
| Execution SRAM (`SRAM_EXEC`) | **3-8 KiB** | Test-page methods, constants/vtable, and test-only UI/MIDI/startup/audio integration |
| Normal data SRAM | **0.3-1.0 KiB** | Test-page state, FatFS file object, button/encoder/map arrays, and oscillator |
| External SDRAM | **None expected** | TAPE still needs its sample, loop, and delay buffers |

Compiler inlining, alignment, library reuse, and FatFS configuration can change these figures. The file object's private sector buffer depends on the FatFS configuration.

Do not credit removal of the full DSP engine, FatFS, MIDI drivers, battery drivers, common UI, or audio buffers: TAPE still uses them. ELF debug information and symbol tables do not consume runtime SRAM and must not count as savings.

The README's reported 376-byte headroom is historical, not a measured current baseline. If it still applies to execution SRAM, the provisional new headroom would be approximately **3.4-8.4 KiB**. Confirm this with matching builds before relying on it for another feature.

No TAPE application ELF/map/binary was found in the nonignored workspace search during planning. Ignored artifacts and toolchain availability were not verified, and no compiler was executed.

### Measurement Procedure

1. Build the original and reduced TAPE configurations with identical compiler, flags, library builds, and linker layout.
2. Compare the linker memory-region reports and maps.
3. Use `arm-none-eabi-size -A` on both ELFs to inspect section sizes.
4. Use `arm-none-eabi-nm -S --size-sort -C` to attribute retained and removed symbols.
5. Report execution-SRAM load-image changes, normal `.data`/`.bss` changes, and changes in other RAM regions separately.
6. Account for `.data` load and runtime addresses without conflating the two budgets. Do not use binary-size difference alone as total RAM savings.

## Acceptance Gates

- TAPE normal startup, shipping mode, master/input gain, MIDI, menu, presets, battery/power, SD handling, recording, looping, loading, append, and effects remain unchanged.
- Holding GAIN at TAPE startup no longer opens diagnostics and has no other special effect.
- `TestPresets()` and `WritePresets()` preset serialization still work.
- Final TAPE source and symbols contain no hardware-test mode or test oscillator.
- Exact memory savings are recorded from equivalent builds; source inspection alone does not satisfy linker verification.

## Scope Boundaries

No sibling/standalone diagnostic firmware, no bootloader/launcher changes, no minimal-audio redesign, no flash relocation or linker expansion, no new test protocol, and no broad common-library refactor. The diagnostics are deleted, not relocated.

The separately planned looper-volume feature is not part of this removal.
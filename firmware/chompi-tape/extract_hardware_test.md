# Extract Hardware Test Firmware

Status: Planned only. No firmware implementation, build, or hardware verification has been performed.

## Agreed Behavior

- Create a sibling firmware at `firmware/chompi-hardware-test`, buildable for the existing multi-firmware launcher.
- Retain the full existing TAPE audio behavior, not a stub engine, direct-monitor replacement, or tone-only application.
- Enter diagnostics automatically when this firmware starts; no volume-encoder boot hold is required.
- After all checks pass and the CHOMPI key is pressed, latch an obvious PASS LED indication until reboot. Do not return to normal TAPE or restart automatically.
- Remove the entire hardware-test boot mode from TAPE only after the standalone firmware passes parity checks.
- Preserve TAPE's normal instrument functionality, shipping mode, battery protection, and preset persistence.

## Existing Implementation

The mode spans three primary files rather than only the test page:

- [TestPage.h](firmware/chompi-tape/code/src/TestPage.h): diagnostic state, LED rendering, encoder/button tracking, SD test, and completion criteria.
- [ui.h](firmware/chompi-tape/code/src/ui.h): test-page initialization and lifecycle, MIDI routing, toggle updates, and input dispatch.
- [chompi_main.cpp](firmware/chompi-tape/code/src/chompi_main.cpp): startup selection, oscillator audio path, and diagnostic power-controller polling.

Currently, holding the GAIN encoder (`ENC_6_SW`) during startup opens diagnostics. The separate `KEY_26`/`KEY_27`/`KEY_28` shipping-mode combination takes priority and must remain in TAPE.

### Diagnostic Coverage

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

Preserve the initial approximately 1.5-second input-ignore period and the existing completion criteria.

### Important Dependencies

Full TAPE audio behavior requires retaining the engine, file streaming, sample/loop/delay buffers, effects, options/preset managers, SD assets, and necessary UI/background services in the standalone application.

`TestPage::SetSwitchState()` also changes input monitoring through the engine. Preserve this interaction rather than treating the toggle as only an oscillator selector.

MIDI input is filtered to the configured input channel, while diagnostic notes are sent on channel index zero. Document matching channel settings for the fixture. The current completion logic aggregates received MIDI events; validate TRS and USB separately with fixtures without changing its pass criteria.

The cable test records insertion, not merely cable presence. Start the fixture with the cable absent, then insert it.

`UserInterface::TestPresets()` is ordinary preset serialization despite its name. It and `WritePresets()` must remain in TAPE.

## Implementation Plan

### 1. Establish the Baseline

Build current TAPE using GNU Arm Embedded 10.3-2021.10, the existing `-O3` setting, libraries, and linker script. Preserve its ELF, linker map, binary, and memory-region report before making changes.

Record the current diagnostic behavior on hardware, including both audio modes, MIDI channel settings, loopback fixtures, cable insertion, SD requirements, and completion.

The current [linker script](firmware/chompi-tape/code/src/chompi_sram.lds) allocates:

| Region | Capacity |
|---|---:|
| `SRAM_EXEC` | `237K - 768`, or 241,920 bytes |
| `SRAM` | `275K + 768`, or 282,368 bytes |

Code and read-only constants reside in execution SRAM. Initialized `.data` resides in normal SRAM but its load image also occupies execution SRAM. Account for both regions separately.

### 2. Create the Standalone Build Target

Create the sibling firmware with a README, its own `code/src/Makefile`, and an unchanged copy of TAPE's linker script. Use a unique binary target such as `CHOMPI_HARDWARE_TEST_v1.0`, `APP_TYPE=BOOT_SRAM`, and the same compiler and optimization settings as TAPE.

Snapshot TAPE's application source tree into the new project, excluding generated objects, dependency files, and build outputs. This duplication is intentional: it preserves full engine behavior without introducing a broad shared-code refactor or depending on TAPE application headers that will subsequently lose diagnostics.

Reuse the vendored libDaisy, DaisySP, and coreJSON libraries through explicit relative paths or overridable build variables. Follow the sibling-library reuse convention in the [USB-storage Makefile](firmware/chompi-usb-storage/code/src/Makefile). Check all source lists, include paths, and coreJSON paths after relocation.

This produces an independently buildable target within this repository, not a self-contained separate repository. Document its library dependencies and preserve existing license notices and attribution. Do not duplicate or modify the bootloader.

### 3. Adapt Startup and UI

Preserve the hardware, SD, engine, options/presets, audio callback, and background-service initialization order. Open diagnostics automatically after required initialization.

Do not let a startup shipping-mode combination bypass diagnostics in this dedicated application. Preserve shipping-mode behavior in TAPE itself.

Keep the existing oscillator and engine-processing toggle paths. Retain backing normal-page/support infrastructure where required by the existing engine and services, but do not expose a transition into normal TAPE after diagnostics.

### 4. Add Latched Completion and Failure Handling

Adapt the existing completion check to latch PASS after every required check succeeds and the CHOMPI key is pressed. Prevent the existing UI code from closing the page on `IsClosable()`.

Render a stable green PASS indication, stop automatic diagnostic MIDI messages, and silence diagnostic audio after completion. Remain in this state until reboot.

Explicitly initialize diagnostic state for deterministic startup. Preserve incomplete-test feedback and the existing CHOMPI-key completion check.

Missing or unwritable SD must produce a diagnostic failure rather than hanging or falling through to normal TAPE. Distinguish missing audio assets/settings from an SD I/O failure. Reuse suitable existing error rendering where practical.

### 5. Verify the Standalone Firmware

Before removing anything from TAPE, run the full diagnostic matrix against the original mode. Validate both audio paths, all outputs, MIDI fixtures, SD operations, power checks, completion, and reboot.

Build and link with the existing memory layout and binary format, then verify loading through the existing multi-firmware launcher convention. The local bootloader source has not been established as the external launcher implementation; do not invent launcher slots or change bootloader code. Use the existing [USB-storage deployment guidance](firmware/chompi-usb-storage/README.md) as a reference.

Because full TAPE audio is retained, this application may remain close to TAPE's memory ceiling. If the new completion/startup behavior overflows the link region, reduce diagnostics-specific overhead first. Removing unrelated functionality or changing memory-region allocations requires separate approval.

### 6. Remove Hardware Diagnostics from TAPE

After standalone parity is verified:

- Delete TAPE's test-page header and remove its include, instance, and initialization from the UI.
- Remove `InTestMode()`, `TestMode()`, `TestPowerCable()`, and `TestBMC()` and their test-only callers.
- Remove completion-page closing, test toggle updates, and test-specific menu/rainbow/no-SD guards.
- Simplify MIDI routing to its normal instrument path while preserving channel filtering, note behavior, and transport semantics. Remove only diagnostic synthetic-key routing.
- Remove the test oscillator, initialization, and audio callback branch.
- Remove the diagnostic battery-controller polling block, not the battery drivers or normal power-protection logic.
- Remove `vol_state` collection and volume-encoder diagnostic startup selection. Preserve startup control flushing, timing, and the shipping-mode combination.
- Simplify the boot-time audio guard to its normal non-diagnostic behavior.

Keep the engine, every TAPE sample/loop/delay buffer, shared peripheral drivers, preset persistence, and ordinary battery handling. Remove only declarations/includes that become unused due to this extraction; avoid unrelated cleanup.

### 7. Document and Measure the Result

Update TAPE's README to explain that diagnostics have moved and the GAIN boot hold no longer opens them. Add standalone build, launcher deployment, fixture, audio-asset, MIDI-channel, failure-state, and completion instructions. Add the new firmware to the repository listing.

Rebuild TAPE with the exact baseline compiler, flags, libraries, and linker script. Publish measured before/after memory-region usage and remaining headroom. Run normal TAPE regressions and explicitly report any hardware verification that could not be performed.

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

1. Build the original and extracted TAPE configurations with identical compiler, flags, library builds, and linker layout.
2. Compare the linker memory-region reports and maps.
3. Use `arm-none-eabi-size -A` on both ELFs to inspect section sizes.
4. Use `arm-none-eabi-nm -S --size-sort -C` to attribute retained and removed symbols.
5. Report execution-SRAM load-image changes, normal `.data`/`.bss` changes, and changes in other RAM regions separately.
6. Account for `.data` load and runtime addresses without conflating the two budgets. Do not use binary-size difference alone as total RAM savings.

## Acceptance Gates

- Standalone tests match the original for connected keys/buttons, encoder switches/directions, toggle, LEDs, jack detection, audio paths, and four outputs.
- SD write/read/delete and missing/unwritable-card behavior are verified with a suitable test card.
- TRS and USB MIDI loopback fixtures each work with matching channel settings and the existing reception count.
- Cable insertion and battery-controller health checks behave as before.
- Pressing CHOMPI before all checks pass cannot latch PASS; successful completion latches PASS, stops diagnostic traffic/audio, and never opens normal TAPE. Reboot clears the result.
- The new firmware builds, fits the existing memory layout, and loads through the launcher with documented TAPE assets/settings.
- TAPE normal startup, shipping mode, master/input gain, MIDI, menu, presets, battery/power, SD handling, recording, looping, loading, append, and effects remain unchanged.
- Holding GAIN at TAPE startup no longer opens diagnostics.
- Final TAPE source and symbols contain no hardware-test mode or test oscillator.
- Exact memory savings are recorded from equivalent builds; source inspection alone does not satisfy hardware or linker verification.

## Scope Boundaries

No minimal-audio redesign, fake engine, flash relocation, linker expansion, bootloader/launcher implementation changes, new test protocol, or broad common-library refactor.

The standalone firmware intentionally retains TAPE dependencies to satisfy the full-audio requirement. The separately planned looper-volume feature is not part of this extraction.
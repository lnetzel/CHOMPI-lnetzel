# CHOMPI Organ: vintage stereo delay

## Problem and approach

Add a warm, stereo BBD-inspired delay after the rotary speaker and its existing fuzz-output leveling, before the plate reverb and master gain. Give it a yellow Volume-encoder page and persist its mix/time in all preset paths. Preserve existing sounds at the default 0% mix.

Planning only: do not modify implementation files until implementation is approved.

## Confirmed requirements

- Volume push cycles: **Volume -> Distortion -> Delay -> Reverb -> Volume**.
- Delay page: yellow knob LED, retaining the existing brightness floor so the page remains visible at 0%.
- Turn: mix in 1-percentage-point clicks, clamped 0..100%; 0% is exact dry, 100% is delayed signal only. Use a complementary linear crossfade with a click-free ramp, matching the reverb convention.
- SHIFT + turn: delay time in 5 ms clicks, clamped 40..500 ms, with smooth BBD-like pitch-gliding transitions.
- Both edits show the usual yellow 15-white-key value bar for 500 ms. Mix maps 0..100% to 0..15 keys; time maps 40..500 ms to 0..15 keys.
- Fixed moderate feedback, approximately 35%; no feedback control.
- Stereo, not ping-pong: preserve stereo inputs with subtle L/R timing and slow modulation differences.
- Fixed darkening repeats and gentle saturation. No hiss, clock noise, self-oscillation, or additional tone/modulation controls. This is BBD-inspired DSP, not a physical circuit model.
- Defaults, older presets, and init patch: mix 0%, time 250 ms.
- Keep buffers/history running at dry mix and across page/preset changes. Loading a preset ramps mix/time without clearing echoes. Master volume silences all effects.
- Store mix and time; no new MIDI CCs. Existing controls work unchanged in both hardware-toggle modes.

## Codebase findings

- `firmware/chompi-organ/code/src/OrganEngine.h` currently renders distortion -> rotary -> fuzz output leveling -> plate reverb -> master gain.
- `chompi_main.cpp` owns the plate's external SDRAM buffer and initializes it only after hardware setup. Follow this ownership/init pattern for delay.
- Hardware audio is configured for 48 kHz. The linker defines 64 MiB external SDRAM, separate from tight internal SRAM regions.
- `OrganPreset.h` already supports optional JSON keys, range clamping, defaulting, and templated engine capture/apply. `PresetStorage.h` uses a 768-byte JSON buffer.
- Volume pages and bar-display sub-states currently share numeric IDs: adding page 3 would collide with the current reverb-decay bar ID. Separate/reassign these explicitly and update every dispatch/display branch together.
- Host assert-based DSP and preset tests live in `code/tests` and use the existing Makefile.
- A CPU meter already surrounds the audio callback behind `ORGAN_CPU_METER`; its documented make-variable activation is not currently wired in the application Makefile.
- Leave the unrelated untracked `firmware/chompi-organ/plans/add_percussion_plan.md` untouched.

## Implementation todos

### 1. Establish the baseline and resource gates

Use the pinned Arm GCC 10.3.1/MSYS2 workflow and existing host test runner. Record baseline link-region usage and test results before changes. Inspect the build's compiler-definition conventions to enable the existing CPU meter reliably when requested.

Do not flash hardware or modify toolchain/library dependencies unnecessarily.

### 2. Implement and integrate the stereo delay

Add `code/src/StereoBbdDelay.h`, host-testable with no Daisy dependencies:

- Caller-owned stereo float ring buffers in SDRAM: two power-of-two 32,768-sample lines, 256 KiB total. At 48 kHz this comfortably covers 500 ms plus interpolation capacity.
- Only small filter, modulation, index, and control state in internal SRAM. No heap allocation.
- Clear buffers in Init after SDRAM setup, never in a global constructor or on preset loads.
- Fractional interpolated reads with safe wrapping. Clamp actual read delays, including stereo offsets/modulation, to the supported time bounds.
- Preserve channel identity; use subtle bounded timing differences rather than ping-pong feedback or mono summing.
- Fixed stable feedback around 0.35, low-cost low-pass darkening and low-cut/DC control, and bounded gentle saturation.
- Sample-rate-derived mix smoothing with exact settled endpoints; slewed read time for pitch glides, with no coefficient recalculation or expensive transcendental operations in the per-sample hot path.
- Continue processing silence and dry bypass to preserve tails and history.

In `OrganEngine.h`, add clamped mix/time getters/setters and defaults. In `chompi_main.cpp`, allocate the SDRAM buffers and pass them through engine initialization. Insert delay after fuzz output leveling, immediately before the plate; leave master scaling and four output-channel routing unchanged.

### 3. Wire the four-page UI and presets

In `chompi_main.cpp`:

- Define four explicit Volume pages and non-colliding bar sub-states.
- Update encoder push cycling, unshifted turn, shifted turn, yellow bar color/value mapping, and knob LED rendering.
- Preserve master fine tune, distortion tone, and reverb decay on their respective pages; no SHIFT gesture falls through to fine tune on the new page.
- Preserve preset/modulation overlays and their existing LED priorities.

In `OrganPreset.h`:

- Add `delay_mix` and `delay_time_ms`; use an integer type that can represent 500 ms.
- Wire defaults/init, serialize/parse, and engine capture/apply. Keep the existing backward-compatible version-1 optional-key convention.
- Clamp mix to 0..100 and time to 40..500 ms. Encoder edits use 5 ms increments; loading an in-range integer-millisecond file value need not introduce unnecessary quantization.
- Verify maximum serialized size fits the actual storage buffer, including worst-case values.
- Existing bank save/copy/load paths should carry these fields automatically; extend tests to prove it.

### 4. Validate behavior, memory, and CPU

Add `code/tests/stereo_bbd_delay_test.cpp` and register it in the existing test/clean targets. Extend `organ_preset_test.cpp` and its fake engine.

Host tests:

- Nonzero-prefilled buffer clearing, silent initialization, deterministic reinitialization.
- Mix/time defaults and clamping, exact 40/500 ms endpoints and 250 ms default.
- Bit-exact settled dry bypass and wet-only output with no immediate dry feedthrough.
- Mix-ramp convergence/retargeting and time-slew continuity/retargeting, including endpoint-safe stereo modulation and ring wrap.
- Echo arrival near the requested time, measurable repeat decay, progressively darker repeats, distinguishable stereo paths without ping-pong behavior.
- Long impulse/sustained/anti-phase inputs remain finite and bounded; silence decays without self-oscillation.
- History survives bypass/parameter changes; in-place stereo processing is safe.
- New preset fields round-trip, older/missing keys default correctly, out-of-range/malformed values follow existing parser rules, capture/apply and init work, and bank copy/save preserve values.
- Serialized worst-case preset fits 768 bytes.

Run focused delay/preset tests, then the existing native suite for regression coverage. Build release and CPU-meter firmware with pinned GCC; verify fresh binaries and all linked regions below capacity. Ensure the CPU-meter make option actually enables the macro using the repository's compiler-flag pattern.

Hardware acceptance, explicitly separate from host/build verification:

- Test four-page cycling and all SHIFT functions in both toggle modes, yellow mix/time bars at endpoints and midpoint, preset save/load/copy/init, and master-muted tails.
- Exercise six held voices, maximum Character/drift, distortion, rotary, delay, and long reverb together, including encoder motion and preset SD activity.
- Target peak callback load below 85% on the existing meter, with no audible underruns. Host timing is not evidence of MCU headroom. If the target is missed, optimize the delay hot path first; do not silently reduce polyphony or change the agreed sound/range.
- Confirm the vintage voicing and smooth time glides by listening.
- If no board is accessible, report CPU/listening/UI checks as pending; do not claim hardware feasibility is verified.

### 5. Update directly related documentation

Update `firmware/chompi-organ/README.md` for the signal chain, four Volume pages, yellow bars, time range/steps/default, fixed voicing, tail behavior, stored fields/older-preset compatibility, tests, and CPU-meter validation command.

Update directly related signal-chain comments in `OrganEngine.h`, `chompi_main.cpp`, and `Distortion.h` where inserting delay makes their wording inaccurate. Avoid unrelated cleanup.

## Dependencies

- Baseline precedes DSP implementation.
- DSP integration precedes UI/preset wiring.
- DSP and UI/preset implementation precede final validation.
- Documentation follows the implemented interfaces and validation results.

## Resource and scope notes

- The planned delay adds 256 KiB SDRAM, approximately 0.39% of the linker's 64 MiB region. Existing plate storage is a separate 64 KiB. Final linker usage, not this estimate, is the memory acceptance evidence.
- Delay duration mainly affects buffer capacity, not per-sample CPU cost. CPU safety comes from a simple fixed-cost DSP path and actual callback-load measurement, not merely capping time.
- Exact filter/modulation voicing is an implementation detail within the confirmed dark, subtle, moderate-repeat character; keep it fixed and document the final choices.
- No new modulation destinations, MIDI assignments, tempo synchronization, tap tempo, feedback UI, bootloader changes, or preset-page persistence.

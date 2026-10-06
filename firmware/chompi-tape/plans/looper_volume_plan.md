# Looper Playback Volume Plan

Status: Planned only. No firmware implementation or build verification has been performed.

## Behavior

Add a third page to the clickable GAIN encoder:

1. Master output volume.
2. Input monitor volume.
3. Looper playback volume.

Short clicks cycle 1 -> 2 -> 3 -> 1. The existing long-hold battery display remains unchanged.

The new control adjusts playback only, linearly from mute to 100% of today's loop level. It does not change stored audio, recording level, or overdub feedback. Smooth gain changes to avoid clicks and zipper noise.

Always start at 100% on power-up. Do not persist the value to the SD card or remember it across power cycles. Retain the setting during the session, including loop clear, load, append, and sample changes, consistent with the other global volume controls.

## Existing Code

- [code/src/NormalPage.h](code/src/NormalPage.h): `knob_num_pages` controls page cycling. The GAIN encoder currently has two pages. `OnEncoderTurned()` already stores three pages of values, clamps them to 0..1, and uses 0.01 coarse increments. Gain application currently happens in the rendering method's `case 5`.
- [code/src/ui.h](code/src/ui.h): `enc_defaults[2][5]` is the unused third-page GAIN default, currently zero. `NormalPage::Init()` populates all page values from these defaults.
- [code/src/LooperEngine.h](code/src/LooperEngine.h): `Process()` supplies live input to `PopStereoSamps()`, then adds the returned loop audio to the output mix. This is the appropriate place to apply playback gain.
- [code/src/Sampler.h](code/src/Sampler.h): Owns buffer writes, overdub feedback, and playback/reset envelopes. Leave this implementation unchanged.
- [code/src/DSPEngine.h](code/src/DSPEngine.h): Calls `looper.Process()` between selectable pre/post-looper effects stages and exposes existing looper controls.
- [code/src/PresetManager.h](code/src/PresetManager.h): Stores sample-slot controls. Existing GAIN pages are not stored by `DumpValuePresets()`; no preset or JSON migration is needed.

## Implementation Steps

### 1. Playback Gain

In `LooperEngine`:

- Add `playback_gain_` and `playback_gain_target_`, both initialized to `1.f`, including initialization in `Init()`.
- Add `SetPlaybackGain(float)` and clamp the target to 0..1.
- Smooth the gain once per sample in `Process()` using the existing `daisysp::fonepole` pattern. A coefficient of `.001f` gives approximately a 21 ms time constant at 48 kHz; confirm responsiveness on hardware.
- Perform smoothing outside the playback-only branch so it also progresses during first recording. Preserve the existing early return for an idle empty loop; smoothing resumes when processing resumes.
- Multiply only the returned stereo loop contribution at the additive mix: `s162f(aol)` and `s162f(aor)`. Do not scale live inputs, stored samples, feedback, or reset envelopes.
- Do not reset the setting in clear, file-load, or append paths.

Add a thin `Engine::SetLooperPlaybackGain(float)` forwarding method in `DSPEngine.h`, alongside the existing looper gain controls. Avoid unused getters and additional audio buffers.

### 2. Encoder Integration

- Change the final entry in `knob_num_pages` from 2 to 3.
- Set `enc_defaults[2][5]` to `1.f`.
- Initialize DSP playback gain from this default in `NormalPage::Init()`, without requiring the user to visit page 3.
- Reuse the generic encoder storage, increment, and clamping behavior.
- Replace the current catch-all input-gain `else` in rendering `case 5` with explicit page handling: internal page 0 for master, page 1 for input monitor, and page 2 for looper playback.
- Apply the new stored value through `SetLooperPlaybackGain()` in the page-3 branch, following the existing gain-control pattern.
- Ensure page switching alone does not reassign the other volume values. Preserve battery-display priority and short-release page cycling.

### 3. LED and MIDI

Use the existing purple color for page 3, with brightness following the gain and a minimum brightness floor so the page remains identifiable at mute. Do not introduce a new VU meter or DSP telemetry.

The unused `cc_map[2][5]` currently contains zero, and physical turns unconditionally transmit their mapped CC. Suppress outgoing MIDI specifically for GAIN page 3 so it does not send accidental CC 0 messages. Leave other mappings unchanged and do not add a dedicated CC.

Incoming CC 25 already controls the selected GAIN page through the existing encoder event path. Preserve that behavior for page 3, including no echo of incoming CC changes. Existing outgoing page-1 CC 25 and page-2 CC 32 remain unchanged.

### 4. Documentation

Update [README.md](README.md) with the three pages, range, startup default, session-only state, and playback-only behavior.

Update [midi.md](midi.md) to describe CC 25 controlling page 3 when selected and the absence of outgoing MIDI for physical turns on that page.

Document audio-routing consequences:

- Gain affects loop playback on both headphone and line outputs.
- Gain precedes any post-looper effects, so effect tails may briefly remain after muting.
- Resampling captures the adjusted audible mix, including the loop volume setting.

Do not change firmware version metadata or the build target name as part of this feature.

## Build Constraints

The current README specifies GNU Arm Embedded 10.3-2021.10 and reports only 376 bytes of remaining firmware SRAM space. This is a linked firmware capacity constraint, not merely the storage cost of two new floats.

1. Establish a baseline build with the supported compiler before implementation.
2. Rebuild libDaisy/DaisySP only if required, following their existing Makefile conventions.
3. Build the changed firmware with `make` in `code/src`.
4. Compare linker/map SRAM usage and binary size with the baseline; report remaining headroom.
5. If linking overflows, first reduce this feature's code, LED, or API footprint. Removing unrelated features requires separate approval.

If the required toolchain is unavailable, report that blocker rather than claiming build verification. Do not alter the bootloader or linker configuration to fit the feature.

## Acceptance Checks

- Boot at unity without visiting page 3. Power cycling always restores 100%; no settings are written to SD.
- Short clicks cycle through all three pages and return to page 1. Long holds still display battery status without cycling.
- Each page retains its independent value. Page 3 remains identifiable when muted.
- With fixed master/input gain and effects bypassed, page 3 mutes only the loop while live keys and input remain audible.
- Gain 0.5 produces approximately half the loop amplitude (-6 dB); gain 1 matches existing playback. Stereo balance and both output pairs are preserved.
- Sweep gain and test fast turns, limits, mute/unmute, varied pitch, reverse, and scrub. Confirm no audible zipper clicks or abrupt gain jumps.
- Check adjustments while paused, empty, and recording, plus returning from the menu.
- Record and overdub with playback muted, then restore unity and verify recording and feedback remain intact.
- Clear, load, append, and sample-slot changes retain session volume.
- Verify effects before and after the looper, including expected post-effect tails. Confirm resampling captures the adjusted audible mix.
- Physical page-3 turns send no CC 0 or new CC. Existing page-1/page-2 MIDI outputs are unchanged. Incoming CC 25 controls the currently selected page, including page 3, without echo.

Hardware checks remain required acceptance gates; source inspection or a successful build does not substitute for them.

## Scope

TAPE firmware only. No changes to sample-buffer format or content, overdub feedback, sample presets, options JSON, other firmware, menu volume controls, dedicated MIDI CC assignments, bootloader, linker configuration, target names, or version metadata.
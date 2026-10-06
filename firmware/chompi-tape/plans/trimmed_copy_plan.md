# Plan: Copy Preset Region Into Looper

## Status

Implemented 2026-10-06. `FileCopier::CopyRequest` gained optional `trim`/`trim_start`/`trim_end` fields (default full-range), the `COPY_DEST` handler in MenuPage.h snapshots the saved start/end for SD-card preset-to-looper requests (rejecting invalid/empty ranges without pushing the request), and `FileCopier::CopyStart` seeks to the stereo-frame-aligned start offset within the parsed WAV data chunk and bounds `copysize` to the selection. Findings confirmed during implementation: the looper (slot 16) has no preset metadata (`PresetManager::Copy` bounds-checks it out) and LooperEngine always plays the full RAM buffer, so no destination-metadata fixup was needed.

Build verified with the ARM 10.3-2021.10 toolchain: +1,360 B .text, +224 B .bss (17 replicated CopyRequests), bin 234,568 B; ~6.9 KB SRAM_EXEC and ~3.2 KB SRAM headroom remain. Items 1-6 of the verification checklist below require CHOMPI hardware and an SD card and have NOT been run.

## Goal And Scope

When copying an SD-card preset into the looper buffer (slot 16), copy only the audio between the preset's saved start/end positions. Apply the same selection when appending to an existing loop.

For example, a 10-second sample trimmed to 25%-75% should contribute only the middle 5 seconds. The original SD-card sample and source preset must remain unchanged.

Use the existing chunked copier rather than loading the entire sample into temporary memory. Copy raw audio; do not render pitch, reverse playback, envelopes, or effects. Leave preset-to-preset copying, RAM transfers, saving recordings, and double-speed file generation unchanged. Live unsaved trim settings are outside the proposed scope.

## Current Code And Evidence

- [MenuPage.h](firmware/chompi-tape/code/src/MenuPage.h): the `KEY_26` / `COPY_DEST` handler creates `FileCopier::CopyRequest` and calls `PresetManager::Copy`. Source slots 15 and 16 use RAM; ordinary preset slots use SD files.
- [PresetManager.h](firmware/chompi-tape/code/src/PresetManager.h): `GetValue(mode, bank, slot, control)` exposes normalized start/end through controls 1 and 2. Slots are 1-based. Invalid preset metadata returns the `0xff` sentinel. `Copy` also transfers start/end settings to the destination.
- [FileCopier.h](firmware/chompi-tape/code/src/FileCopier.h): `CopyStart` calls `JumpToData`, positioning the read handle and setting `copysize` from the WAV data chunk. `CopyProcess` tracks bytes copied in `copyread` and limits each read to the remaining copy length. Currently, it copies the full sample.
- `CopySizeFull` ignores `copysize == 0`; an empty selected range must not fall through into full-file copying.
- [SampleReader.h](firmware/chompi-tape/code/src/SampleReader.h): start/end mapping uses stereo alignment. File playback assumes a standard WAV header, so do not blindly reuse its absolute offset calculation for the copier. Base copy offsets on the parsed WAV data payload and verify agreement for standard WAV files.
- `ApplyAppendCrossfade` already bounds its fade length by the first copied block and existing loop tail. A short selected region does not by itself require an additional minimum fade-length guard.
- [todo.md](firmware/chompi-tape/todo.md) already lists copy/paste to the looper respecting preset start/end positions.

## Implementation Steps

1. Confirm how the looper consumes preset metadata and how playback treats the start/end boundaries. Verify that the copied region matches the preset's audible selection, including stereo-frame rounding and endpoint conventions.
2. Add optional source-range fields to `FileCopier::CopyRequest`, with full-range defaults so existing callers retain their behavior. In the `MenuPage` copy handler, snapshot the saved start/end only for valid SD-card preset-to-looper requests, including append. Missing metadata should retain the existing full-range behavior. Invalid or empty explicit ranges should be rejected without modifying the existing loop or its metadata.
3. In `CopyStart`, calculate the stereo-frame-aligned source offset and selected length after locating the WAV data chunk. Seek relative to the valid data start, set `copysize` to the selected byte length, and leave `copyread` counting copied bytes from zero. Bound reads to the available data payload. Handle seek/read errors and zero-length selections explicitly before writes or append fading. Preserve the existing nonstandard-header policy rather than expanding WAV-format support.
4. For successful cropped transfers only, ensure the destination looper's start/end metadata covers the complete resulting buffer (0-1), avoiding a second trim. Preserve other parameters and the source preset. Append at the existing loop end and retain the existing splice fade. Check the ordering of `PresetManager::Copy` and asynchronous copy completion so rejected requests do not alter destination metadata.
5. Build with the repository-supported ARM toolchain and inspect the linked memory usage. New request fields are replicated in the 16-entry FIFO and active request, so include that cost in the memory budget. Keep changes localized and avoid unrelated refactors.
6. Run the verification checklist below. Mark the feature complete in the TODO only after implementation and verification, clearly distinguishing hardware checks that could not be run.

## Verification Checklist

1. Use a known 10-second stereo WAV with a 25%-75% preset trim. Copy into an empty looper and verify exactly the middle 5 seconds, with first/last frames matching the selected interval. Confirm that the source WAV and preset are unchanged.
2. Verify that 0%-100% preserves full-copy behavior. Test selections beginning or ending inside a copy block, ensuring no samples after the selected endpoint are copied and stereo channels remain aligned.
3. Test missing metadata, empty/reversed ranges, non-finite values, and out-of-range settings. Invalid explicit ranges must not copy the whole file or damage the existing loop. Validate zero-length handling independently of the copier's existing zero-size special case.
4. Append a selected region to an existing loop. Verify that the length increases by the selected-region length, subject to established buffer capacity, and that the existing splice fade remains effective. Check short selections and capacity limits.
5. Play the resulting loop and verify that start/end metadata does not trim it again. Confirm the selected interval agrees with sample playback's endpoint convention.
6. Confirm unchanged behavior for preset-to-preset copies, RAM-to-looper transfers, recording saves, and double-speed output generation. Verify that the new copy does not render source pitch, reverse, envelopes, or effects.
7. Build from `firmware/chompi-tape/code/src` with the supported ARM GCC toolchain and `make`, following repository build instructions. Compare memory usage before and after the change.

## Prerequisites And Remaining Uncertainty

- The other computer needs the same repository revision, required dependencies, and firmware toolchain to build. CHOMPI hardware and an SD card are needed for final playback checks.
- No firmware changes or tests were performed during planning. No automated unit tests were identified in the initial exploration; reuse suitable existing test infrastructure if available at implementation time.
- Exact toolchain availability, build commands, SRAM headroom, and compiler-version requirements were not independently verified. Check repository documentation and actual build output rather than relying on a numerical memory margin from this investigation.
- Looper metadata consumers, WAV-header edge cases, and exact playback endpoint semantics need a focused check before implementation. Keep the initial change limited to saved SD-card preset selections copied into the looper.
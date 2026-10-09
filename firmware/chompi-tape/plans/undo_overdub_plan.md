# TAPE Last-Overdub Undo Plan

Date: 2026-10-08

Status: planning only. This document does not authorize implementation. No firmware
changes or build/hardware verification were performed while preparing the plan.

## Agreed Behavior

- Support one undo level. One overdub session includes everything written from
  entering overdub until leaving it, including multiple loop revolutions and the
  recording fade-out tail.
- An initial base recording is not undoable. An automatic transition from initial
  recording into overdub must establish the undo session before its first
  destructive write.
- Play, pause, play again, speed changes, reverse, and scrub after recording do not
  invalidate completed undo history.
- In the existing shift/menu context, hold CHOMPI, press the transport wheel
  (`ENC_5_SW`), then release CHOMPI. The confirmation remains open.
- During confirmation, CHOMPI PTH LED 0 blinks red and the two wheel direction
  PTH LEDs 5 and 6 blink green in phase. Use the existing menu blink timing:
  250 ms on, 250 ms off. These are not PLAY/OVERDUB LEDs 7 and 8.
- The opening CHOMPI hold and opening wheel click never count as Yes or Cancel.
  Require fresh subsequent presses. Releasing CHOMPI does not dismiss the prompt.
- A fresh CHOMPI press confirms: fade down the looper contribution, suspend loop
  DSP reads/writes, restore the pre-overdub audio, and leave the loop paused.
  Preserve current pitch, direction, feedback, playback volume, and unrelated
  effects. PLAY subsequently starts the restored loop.
- A fresh wheel press cancels: leave stored audio and the current running/paused
  state unchanged. Neither requesting nor cancelling undo resets looper pitch.
- Pending confirmation locks other looper controls: physical and MIDI
  play/record, wheel rotation/pitch/scrub, and looper feedback edits. Musical notes
  and non-looper controls remain usable, including note releases. No timeout.
- Switching out of the shift/menu context cancels a pending prompt without
  changing audio. Once confirmed, restoration cannot be interrupted by toggle
  changes.
- Preserve the existing unshifted wheel pitch-reset behavior. With no valid
  completed undo, shifted wheel clicks are consumed without changing audio or
  pitch and do not open confirmation.
- Do not hijack an active save/copy/erase selection or copy operation. Exclude
  overlapping sampler recording and incomplete overdub fade-out tails from undo
  entry.
- A successful undo consumes history. No redo, multilevel history, undo of the
  initial recording, or undo of paste, append, or clear.
- Do not touch the existing copy paste behavior where Normal paste (selecting paste     destination with Play key) replaces looper buffer and Append-Paste (selecting paste destination with Overdeb key) is unaffected by this undo feature. UI cannot be altered and no distortion of audio is allowed in any copy/paste behavior. Not pasting to looper or another preset. 

## Architecture

The destructive overdub computation is in `FileSampler::PopStereoSamps` in
[Sampler.h](../code/src/Sampler.h). It scales old samples by `dub_gain_`, adds
interpolated input, applies the `ProcessHard` limiter, and writes through
`RamBuffer::StereoWrite`. Subtracting newly recorded input cannot reconstruct the
original audio after feedback reduction, limiting, and quantization.

Use a lazy RAM page snapshot: preserve each original page immediately before its
first destructive stereo write in the current session. Later revolutions must
not refresh its snapshot. This avoids a full-buffer copy at overdub entry and
does not require separate playback layers or SD backup files. A concurrent
background pre-copy while overdubbing would race with destructive writes.

### Memory Layout

The following tradeoffs were approved:

| Allocation | Bytes |
| --- | ---: |
| Existing looper allocation | 31,694,848 |
| New live loop half | 15,847,424 |
| Undo snapshot half | 15,847,424 |
| Sampler recording buffer, unchanged | 31,694,848 |
| Proposed page-generation table | 61,904 |

At 48 kHz, stereo, 16-bit PCM, maximum loop length becomes approximately
82.54 seconds, down from 165.08 seconds. Sampler recording capacity is unchanged.

- Proposed page size: 256 stereo frames, or 1,024 bytes. Each half contains
  exactly 15,476 pages.
- Store one `uint32_t` generation tag per page in SDRAM, not internal SRAM. A tag
  identifies whether that page has been captured for the current session. No
  per-sample bitmap is required.
- Capture a first-touched page before mutation, clamping the last page to valid
  loop bounds. Reverse writes target `write_head - 2` int16 elements, not
  `write_head`.
- Reserve generation zero. Handle generation wrap with a gated foreground tag
  reset outside capture/restoration; do not clear the table in a live callback or
  reuse stale tags.
- Untouched pages remain original in live memory. Restore only pages tagged for
  the session being undone. Never swap the partially populated snapshot into
  playback.
- Validate actual linker SDRAM usage and callback time. These figures are design
  estimates, not a new build measurement.

## Implementation Steps

### Phase 1: Capacity and Storage

1. Establish a baseline build and map using GNU Arm Embedded 10.3-2021.10. The
   existing README reports 8,712 bytes SRAM_EXEC and 3,476 bytes SRAM headroom;
   obtain current measurements. Preserve all existing user changes.
2. Add explicitly initialized capacity to `RamBufferMemory`, expressed in int16
   elements with even stereo alignment. Update `Clear` and all `RamBuffer`
   size/head/bounds operations to use the buffer's capacity rather than the
   global maximum. Preserve sampler capacity and sample-reader behavior.
   Depends on step 1.
3. Split `loop_mem` logically or into equal SDRAM arrays. Initialize `loop_buff`
   at half capacity and `chompi_buff` at its original capacity. Allocate undo
   audio and page tags in SDRAM and wire them through `Engine::Init`,
   `LooperEngine::Init`, and `FileSampler::Init`, or minimal explicit
   configuration. Do not pass undo storage through `FileCopier::Init`, whose
   `RamBuffer::Init` clears shared memory. Depends on step 2.
4. Audit `FileCopier::CopySizeFull`, `BlockWrite`, append capacity checks, and
   crossfade `Poke` indexing for the smaller destination. Exact-full and oversized
   replace/append operations must stop or truncate safely without touching undo
   memory, retaining established truncation behavior. Avoid pointer swaps that
   leave shared copier descriptors stale. Depends on steps 2-3.

### Phase 2: Snapshot Sessions and Restoration

5. Add small audio-owned session states: `Empty`, `Capturing`, `DrainingTail`,
   `Available`, `FadeOutForUndo`, and `RestoreSuspended`, with session generation
   and immutable recorded length. `FileSampler` owns capture-before-write;
   `LooperEngine` owns transport/session lifecycle. Publish coherent
   generation-based availability to the UI. Depends on step 3.
6. Immediately before the first destructive write to a page, copy its original
   bytes to undo storage and tag its generation, then perform the normal live
   write. Capture stored bytes, not FIFO-derived approximations or input alone.
   Cover loop wrapping, reverse, speed changes, scrub, feedback, and limiting.
   Initial `PushStereoSamps` is not captured. Handle initial-to-overdub transition
   separately from the existing full-capacity two-toggle path, which ends in
   playback rather than a genuine overdub. Depends on step 5.
7. Keep capture active after `record=false` until fade-tail writes finish. Publish
   availability only after the tail completes and at least one page was modified.
   A new session replaces previous history. Rapid off/on must finish the old
   generation and start a new one before new-session writes, not merge unrelated
   sessions. Play/pause and non-destructive transport changes retain history.
   Depends on step 6.
8. Expose `CanUndoLastOverdub`, an undo request carrying the expected generation,
   busy state, and acknowledged completion through `Engine`. UI/foreground code
   posts a small request; audio validates it at a block boundary and rejects stale
   generations. Fade only the looper contribution, clear record/play targets and
   arms, and enter explicit DSP suspension. Setting `playing=false` alone is
   insufficient because paused playback supports scrub and slew. Do not fade
   monitored input, other voices, or saved bytes. Depends on step 7.
9. After suspension is acknowledged, restore tagged pages in bounded foreground
   chunks, initially one 1-KiB page per iteration and tuned from measurements.
   Continue servicing voice streaming and UI work. Do not disable interrupts for
   bulk copies. Skip loop reads/writes in the audio callback while suspended and
   prevent copier mutations. On completion, commit at an audio block boundary:
   clear FIFO, interpolation, old-input, recording-edge, and envelope caches;
   position heads at the logical loop start, or end for current reverse; preserve
   current pitch/direction/feedback/playback volume; set `record=false`,
   `first_record=false`, and `playing=false`; clear deferred targets; consume
   history. PLAY then resumes a clean restored loop. Depends on step 8.
10. Invalidate history before clear, new base recording, load, replace, or append
    modifies loop audio, including append crossfade `Poke` calls. Copier writes
    to the loop must request and await audio acknowledgment before the first
    mutation, including partial/failed copy paths. Block such requests during
    confirmed undo. Reads from the loop and exports do not invalidate history.
    `OpenFile` completion is an additional guard, not the first invalidation
    point. Depends on steps 7-9.

### Phase 3: Confirmation UI

11. Extend `MenuPage` with confirmation states separate from preset selection:
    inactive, waiting for opening CHOMPI release, awaiting decision, and applying.
    Reuse menu entry, rendering, blink clock, and closability logic. Open only in
    the existing shift/menu context: `ui.h` opens `MenuPage` when `toggle_state`
    is true. Depends on the API from step 8; UI work can proceed alongside step 9
    using a stubbed API.
12. Intercept shifted `ENC_5_SW` presses before normal-page fall-through. Consume
    opening, confirming, and cancelling presses and their releases. Track owned
    press/release pairs so releases after the prompt closes cannot reset pitch or
    trigger another normal operation. Require fresh wheel presses for Cancel and
    fresh CHOMPI presses after release of the opening hold for Yes.
    `MenuPage::IsClosable` must retain pending confirmation after CHOMPI release.
    Clear held-state bookkeeping coherently on exit. Depends on step 11.
13. Render red PTH LED 0 and green PTH LEDs 5/6 with priority over normal
    shift/pitch indicators. Use the same pattern while pending and applying until
    cancellation or completion, then restore normal indicators. Do not stop
    playback at entry/cancellation or call `StopAllVoices`. Cancel on toggle exit
    or stale/invalidated generation. Depends on step 12.
14. Gate locked controls centrally in `UserInterface` and/or the shared `Engine`
    API: physical transport edges, wheel rotation, shifted feedback changes,
    MIDI CC 26/27, and looper pitch controls. Keep hardware/MIDI edge bookkeeping
    current so releases cannot leave latched states or replay delayed actions.
    Consume operation-selection buttons while modal; route musical note
    presses/releases appropriately without preset selection, sample recording,
    or auto-armed loop recording. Keep non-looper knobs usable. Reject all loop
    mutations during confirmed restoration. Depends on steps 12 and 8-10.

### Phase 4: Documentation and Verification

15. Update the TAPE README with the 82.5-second loop limit, unchanged sampler
    capacity, session definition, gesture/LEDs, control locks, paused-after-undo
    behavior, invalidation, one-level/no-redo behavior, and build limitations.
    Depends on integration.
16. Run focused tests, build/link gates, and hardware checks below. If required
    SRAM regions overflow, report measured overage and request a separate
    decision rather than removing unrelated features. Depends on step 15.

## Implementation Surfaces

| File | Responsibility |
| --- | --- |
| [RamBuffer.h](../code/src/RamBuffer.h) | Per-buffer capacity, clear, read/write/head/EOF bounds. |
| [chompi_main.cpp](../code/src/chompi_main.cpp) | SDRAM allocation, initialization, bounded foreground restoration without starving streaming. |
| [Sampler.h](../code/src/Sampler.h) | Capture before overwrite, tail status, DSP suspension, cache reset. |
| [LooperEngine.h](../code/src/LooperEngine.h) | Session lifecycle, record transitions, reset/load invalidation, undo transport state. |
| [DSPEngine.h](../code/src/DSPEngine.h) | Initialization wiring, loop processing gate, request/status facade. |
| [FileCopier.h](../code/src/FileCopier.h) | Write-start acknowledgment, destination bounds, append invalidation, shared descriptors. |
| [MenuPage.h](../code/src/MenuPage.h) | Confirmation state, button consumption, LED priority, menu closability. |
| [NormalPage.h](../code/src/NormalPage.h) | Preserve wheel pitch reset and note routing; change only if central routing needs it. |
| [ui.h](../code/src/ui.h) | Event and MIDI routing, locked looper-control handling. |
| [Makefile](../code/src/Makefile) | Existing build configuration; no intended changes. |
| [chompi_sram.lds](../code/src/chompi_sram.lds) | Verify linker region limits; no intended changes. |
| [README.md](../README.md) | Document feature, limits, and interaction. |

Prefer existing ownership surfaces; no new production helper file is required
initially. If focused host tests need a minimal new harness, do not assume
`UNIT_TEST` removes MCU, FatFS, or DaisySP dependencies.

## Verification Checklist

- Build baseline and final with GNU Arm Embedded 10.3-2021.10 using
  `make -C firmware/chompi-tape/code/src` from the repository root, with library
  prerequisites built as required. Inspect ELF/map and `arm-none-eabi-size`.
  Verify SRAM_EXEC, SRAM, DTCM, and SDRAM limits and placement of audio/tag arrays.
  Record the size delta. No verified TAPE application host harness currently
  exists; vendored library tests are not an application harness.
- Capacity: unchanged sampler limit; live/undo isolation; zero-length,
  exact-full, reverse boundary, and last-page cases; oversized replace/append;
  crossfade at capacity; safe truncation. Use canaries to detect snapshot damage.
- Snapshot accuracy: start with deterministic stereo data, overdub partial and
  multiple loops, then compare all valid restored samples byte-for-byte. Cover
  reverse, variable speed, scrub, feedback below one, and limiter saturation.
  Untouched pages remain original; revisited pages restore their first original
  version. Force generation wrap/reset in tests.
- Sessions: no initial-record undo; immediate initial-to-overdub capture;
  full-capacity transition does not fabricate a session; fade-tail inclusion;
  rapid off/on creates separate sessions; new overdub replaces history;
  successful undo consumes it. Play/pause/speed changes preserve availability.
  Load/append/clear invalidate before writes, including failed-copy cases.
- UI: opening press/release sequence latches without automatic Yes/Cancel;
  fresh presses confirm/cancel; trailing releases remain consumed after exit;
  LEDs 0/5/6 blink in phase; normal pitch reset remains intact; unavailable undo
  is a no-op; toggle cancellation and other-operation guards work.
- Playback: entry/cancellation preserve both playing and paused states, including
  overdub -> pause -> play -> confirmation. Confirmed undo fades only the loop,
  restores and remains paused. PLAY resumes at current direction/speed without
  stale cached audio, pitch reset, sample recording, or global voice stops.
  Post-looper effect tails may continue naturally.
- Control locks: physical/MIDI transport, pitch, and feedback are blocked while
  pending/applying; notes and non-loop effects remain usable; releases do not
  leave stuck flags; locked actions do not replay on exit; duplicate/stale undo
  requests are rejected.
- Hardware timing: measure worst-case first-page capture callback time at maximum
  loop length with seven voices and effects, restoration time, and streaming
  service latency. Check for underruns at record start, page boundaries, reverse,
  restoration, and UI transitions. Tune bounded page/chunk size from evidence.
  Do not claim glitch-free operation without hardware verification.

## Scope and Risks

TAPE only. Other firmware and the bootloader remain unchanged. No SD undo file,
dynamic audio allocation, separate playback layers, redo, unrelated refactors,
commits, or feature removal are included.

The primary risks are firmware code-space headroom, first-page copy deadlines,
audio/foreground ownership handoff, trailing-release event fall-through,
invalidation before copier writes, and paused scrub/slew behavior.

The approved decisions are the halved looper limit, wheel direction LEDs 5/6,
and locked looper controls during confirmation. Build fit and audio timing remain
implementation gates, not established results.
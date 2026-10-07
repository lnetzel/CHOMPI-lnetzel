# Reliable TAPE Preset Trimming

Status: planned, not implemented.

Correct the byte/frame mismatch in `FileSampleReader`, make preset trim changes
atomic across their target voices, and keep the UI and saved preset synchronized
with accepted playback boundaries. Preserve current immediate accept/reject
behavior; do not add deferred trimming or change encoder sensitivity.

## Findings

- [SampleReader.h](../code/src/SampleReader.h): `SetEndPoint()` converts SD payload
  bytes to stereo frames, then compares the result with `f_tell()`, which returns
  absolute bytes including the WAV header. `SetStartPoint()` has the analogous
  problem in reverse playback.
- [FileStreamingManager.h](../code/src/FileStreamingManager.h):
  `kMaxFileStreamingSamps = 8192` counts `int16_t` FIFO elements, not stereo frames.
  Capacity is 16,384 bytes or 4,096 stereo frames.
- [RamBuffer.h](../code/src/RamBuffer.h): `GetSize()` represents `int16_t` element
  counts. Divide by two for stereo frames; RAM stereo frames are not two bytes.
- [NormalPage.h](../code/src/NormalPage.h): `OnEncoderTurned()` rolls back a failed
  start change only for positive turns and a failed end change only for negative
  turns. MIDI absolute input reuses `turns` as a target value, not a direction.
- [DSPEngine.h](../code/src/DSPEngine.h): `SetStartPoint()` and `SetEndPoint()`
  return only the final voice result after potentially partially updating all
  seven voices. CUBBI targets `latest_voice` only.
- `CacheSamples()` preloads stopped voices. File position is not necessarily the
  audible playhead; a FIFO can contain samples across an automatic loop seek.
  Do not estimate audible position by blindly subtracting FIFO length.
- No TAPE application test harness was located. Vendored libDaisy has gtest
  infrastructure, but `SampleReader` includes MCU, FatFS, and DaisySP dependencies
  and does not automatically become host-testable under `UNIT_TEST`.

## Scope

- Modify only TAPE trim handling and focused tests/documentation.
- Preserve normalized preset format, the `0.003` fine encoder step, the UI's
  `0.01` minimum normalized gap, `kMinLoopLen`, loop/click envelopes, and CUBBI
  versus non-CUBBI targeting.
- Preserve `SetStartPointForce()` / `SetEndPointForce()` and
  [MenuPage.h](../code/src/MenuPage.h) sample-window shifting. Regression-check
  them rather than routing them through the new rejection semantics.
- Do not change tempo/wave firmware, bootloader, vendored libraries, streaming
  architecture, or sample format.
- Avoid blanket pending-I/O rejection. Only ambiguous active inward-boundary
  changes require rejection; safe outward moves and valid edits on inactive
  voices must remain usable.
- Do not implicitly retry or defer rejected changes. Rejection leaves all target
  voice boundaries, cache flags, encoder state, MIDI feedback value, and preset
  metadata unchanged.
- Do not flash without a separate request. Hardware validation remains a required
  external gate.

## Phase 1: Reader Validation

1. Introduce a small side-effect-free validation path shared by `SetStartPoint()`,
   `SetEndPoint()`, and engine preflight. Distinguish unchanged requests, valid
   boundary ordering/minimum length, active playback collision checks, and
   successful commit. Prefer a lightweight scalar helper that can be tested on a
   host; avoid per-voice persistent state and broad new abstractions.
2. Validate normalized input and boundary ordering before unsigned subtraction.
   Calculate usable SD payload size only when file size is at least the WAV
   header size. Align offsets as existing code does. Use stereo-frame counts for
   minimum-length checks and absolute aligned byte positions including the header
   for SD read-region checks. Convert FIFO element counts with `sizeof(int16_t)`.
3. Keep playback-safety guards directional: forward end shrinking and reverse
   start advancing are inward moves. A no-op returns success without invalidation;
   valid outward moves are not blocked by current file position. Inactive cached
   voices are not rejected solely because their file position lies past the
   proposed boundary. If file metadata is unavailable during opening, maintain
   valid normalized boundaries without underflow. Preserve `FileOpened()` callback
   ordering.
4. For active inward SD edits, use a conservative streaming read-region rule in
   bytes rather than treat `f_tell()` as the audible head. Forward end must not
   intersect the already-read/queued region plus a documented byte-converted
   safety margin. Reverse start must not intersect the lower edge of the reverse
   block, using `last_read_size_` and checked subtraction, plus its safety margin.
   Account for outstanding read/seek/open requests where the relevant region
   cannot be bounded; reject those cases without mutation. Preserve RAM behavior
   except shared input/minimum-length validation.
5. Commit `fstart_` / `fend_` only after acceptance. Invalidate `is_buffered` only
   for an accepted changed playback entry boundary: forward start or reverse end,
   matching current intended cache behavior. Rejection must not clear the FIFO,
   change cache state, or enqueue work.

Gate: focused arithmetic/reader validation tests pass before engine/UI changes.
Exact byte margins must be explicit in production code and boundary-threshold
tests. Confirm existing intended margins after proper unit conversion; avoid
arbitrary enlargement.

## Phase 2: Atomic Voice And UI Updates

6. After Phase 1, have engine setters validate every targeted voice using the
   same reader predicate. CUBBI validates `latest_voice`; non-CUBBI validates all
   seven. If any fails, return `false` without mutation. If all pass, apply the
   prevalidated values to all targets.
7. Prevent the audio callback from changing validation state between preflight
   and commit using libDaisy `ScopedIrqBlocker` in a short critical section. No
   FatFS I/O, FIFO draining, or file operations belong inside it. Merely AND-ing
   results after mutation still permits partial updates. Ensure inactive or
   not-yet-open voices cannot spuriously veto valid edits.
8. After steps 6-7, have `NormalPage::OnEncoderTurned()` roll back either trim
   control on any failed engine update, regardless of `turns` sign or whether
   input is MIDI absolute. Suppress preset writes on rejection and emit MIDI
   feedback using the retained value. Apply the same unchanged-state behavior
   for UI minimum-gap rejection. Preserve accepted-change `DumpValuePresets()`
   behavior.

Gate: mixed accept/reject voice tests demonstrate all-or-none mutation. UI tests
cover both encoder directions and MIDI absolute targets.

## Phase 3: Verification And Documentation

9. Add the smallest application-owned host C++ test target needed under
   `code/tests`, rather than modify vendored libDaisy tests. Exercise production
   validation/helper code rather than duplicate the buggy expressions in tests.
   Use narrow stubs only if needed for engine/UI integration. Prefer one test
   translation unit and one runner/build definition; omit unnecessary additions.
10. Cover these automated cases:
    - One-second, 48 kHz sample byte/frame regression and WAV header offset.
    - Exact read-margin thresholds and inward versus outward changes for forward
      end and reverse start.
    - No-op, stopped cached voice, outstanding reads/seeks, and reverse
      `last_read_size_` handling.
    - Short, header-only, and empty files; `start >= end` without unsigned wrap.
    - RAM element/frame mapping and rejected changes preserving cache flags.
    - Mixed seven-voice rejection and CUBBI latest-only updates.
    - Both encoder directions and MIDI absolute increase/decrease.
    - Saved preset and feedback staying synchronized with accepted boundaries.
11. Build with GNU Arm Embedded **10.3-2021.10**, as required by
    [README.md](../README.md). Verify `arm-none-eabi-gcc --version` first. Use a
    compatible GNU make shell/toolchain on Windows. If library archives are
    absent, build libDaisy and DaisySP using their existing Makefiles first.
    Run a clean final application build from the repository root:

    ```sh
    make -C firmware/chompi-tape/code/src clean
    make -C firmware/chompi-tape/code/src
    ```

    Check linker memory output/map. Documented baseline headroom is 8,712 bytes
    in SRAM_EXEC and 3,476 bytes in SRAM; these are not newly measured results.
12. Perform the hardware matrix below. Compare the audible boundary with the
    accepted encoder/preset value and listen for new clicks, silence, or
    inconsistent notes. Verify sample-window shifting and trimmed copying to the
    looper still apply the intended saved boundaries once.
13. Add a concise TAPE README fix note and a focused manual checklist under
    `plans`. Record actual test/build results and explicitly identify on-device
    checks not yet performed.

### Hardware Matrix

| Dimension | Cases |
| --- | --- |
| Sample source | Short and long SD presets; RAM buffer playback |
| Playback state | Playing; stopped/cached; retriggered |
| Playback behavior | Looping; one-shot; forward; reverse |
| Trim input | Start/end; inward/outward; repeated rapid knob turns; MIDI absolute |
| Voice mode | JAMMI seven-voice playback; CUBBI |
| Pitch | Normal speed; crossing the greater-than-1.5x double-file threshold |
| Persistence | Save and reload preset; accepted trim survives consistently |
| Existing workflows | Shifted sample window; trimmed copy/paste to looper |

## Dependencies

- Reader validation and tests first, then engine transaction, then UI rollback.
- Host harness setup can proceed alongside reader helper implementation once its
  interface is decided.
- Documentation/checklist preparation can proceed alongside Phase 2.
- Final firmware build and hardware checks require all production changes.

## Files

| File | Planned work |
| --- | --- |
| [SampleReader.h](../code/src/SampleReader.h) | Validation/commit split, consistent units, cache and callback handling |
| [DSPEngine.h](../code/src/DSPEngine.h) | Atomic targeted-voice preflight and commit |
| [NormalPage.h](../code/src/NormalPage.h) | Direction-independent rollback and preset/feedback synchronization |
| [README.md](../README.md) | Fix note and verification results |

Proposed additions, only as needed:

- `code/tests/trim_controls_test.cpp` and a minimal test runner/build definition.
- `plans/trim_controls_checklist.md` for repeatable on-device checks.

Reference-only surfaces:

- [FileStreamingManager.h](../code/src/FileStreamingManager.h) and
  [FileStreamingManager.cpp](../code/src/FileStreamingManager.cpp): byte-sized
  requests and callback sequencing.
- [RamBuffer.h](../code/src/RamBuffer.h): element-sized RAM storage.
- [MenuPage.h](../code/src/MenuPage.h): forced sample-window shifting.
- [Makefile](../code/src/Makefile): application build.
- [scopedirqblocker.h](../code/libs/libDaisy/src/util/scopedirqblocker.h): short
  atomic section.

## Caveats

This is a source-grounded implementation plan, not a hardware-confirmed diagnosis.
Build/test commands have not been executed as part of planning. The vendor test
target exists, but using it for TAPE application code requires additional host
dependency handling. Safety calculations must remain conservative across loop
seeks and pending reverse reads without adding a new playhead tracker.
# Plan: CHOMPI-PIPES record-to-SD (full audio chain → WAV)

Press-and-hold **PLAY/PAUSE (KEY_27) + LOOP (KEY_28)** for 2 s (Normal Mode only) to arm recording: a red bar fills the 15 white keys left→right during the hold, then a 4-second countdown runs on the first four white keys (all four light white; every 1 s the next one turns red), then recording starts and the instrument returns to normal play. While recording, the CHOMPI/SHIFT key LED (PTH 0) blinks red, and SHIFT keeps working normally. Hold the same chord for 1 s to stop (works in both modes). Takes are 48 kHz / 16-bit / stereo WAV files written to `PIPES/RECORDINGS/Recording 01.wav`, `Recording 02.wav`, … continuing as `Recording 100.wav`, `Recording 101.wav`, … past 99 (next free number, never an overwrite, no other file management).

## Checked in the code

- **Tap point:** [AudioCallback](../code/src/chompi_main.cpp#L1291) renders the full chain (pipes → distortion → rotary → fuzz leveling → stereo BBD delay → plate reverb → master gain) into `out` via `engine.Process(out, size)`. Copying `out` after that call captures exactly what is heard. Blocks are 24 samples at 48 kHz ([hardware.h](../code/src/hardware.h#L128)), so the callback runs at 2 kHz.
- **SD is already up:** [PresetStorage.h](../code/src/PresetStorage.h) mounts FatFS at boot (FAST, 4-bit SDMMC), creates `PIPES/` ([L58](../code/src/PresetStorage.h#L58)), polls card presence 1 Hz via `disk_status(0)` and exposes `NoSdCard()` ([L223](../code/src/PresetStorage.h#L223)). `USE_FATFS = 1` is already set in the [Makefile](../code/src/Makefile#L18) — no build-system change needed. FatFS is configured with LFN on and `_USE_FIND` off ([ffconf.h](../../chompi-wave/code/libs/libDaisy/src/sys/ffconf.h#L116)), so "Recording 01.wav" (with space) works but the next-free-number scan must use `f_stat`, not `f_findfirst`.
- **The DTCMRAM rule:** SDMMC IDMA cannot reach the stack — [PresetStorage.h L254–259](../code/src/PresetStorage.h#L254) documents the boot hang this caused. The recorder must be a global object with member/global buffers only (same as `preset_storage_`), and the `f_write` source buffer must live in D1 `.bss` (proven DMA-safe there).
- **Watchdog discipline:** IWDG (~2 s) is refreshed in [MainLoop](../code/src/chompi_main.cpp#L1948) only while `audio_ticks` advances; all FatFS work must run at thread level in small stages, exactly like the existing staged preset writer. A wedged card mid-`f_write` can still reset the device — write errors must abort the take, never retry forever.
- **SDRAM precedent:** `plate_delay_mem` / delay lines are `DSY_SDRAM_BSS` globals in chompi_main.cpp (~320 KiB of 64 MiB used). A 1 MiB record ring fits trivially.
- **Chord keys are already taken:** KEY_27 toggles percussion on the down-edge ([L750–765](../code/src/chompi_main.cpp#L750)); KEY_28 toggles latch on the down-edge, or starts the 2.5 s init-patch hold when SHIFT is held ([L767–830](../code/src/chompi_main.cpp#L767)). The init-hold machinery (`init_holding_`, `init_hold_t_`, `kInitHoldMs`, [L433–439](../code/src/chompi_main.cpp#L433)) and its red left→right white-key progress bar in UpdateLeds ([L1395–1408](../code/src/chompi_main.cpp#L1395)) are the exact pattern to mirror. White-key LED indices come from `white_key_led[15]` ([L165](../code/src/chompi_main.cpp#L165)); white key 0 = KEY_1 = leftmost.
- **PTH 0 LED chain:** priority is init-done white blink ([L1886](../code/src/chompi_main.cpp#L1886)) → SHIFT purple / preset blinks ([L1891+](../code/src/chompi_main.cpp#L1891)); the 250 ms `pre_blink` toggle is available for the recording blink. The `PIPES_CPU_METER` build overrides PTH 0 entirely (recording blink invisible in meter builds — debug only, acceptable).
- **Boot safety:** the KEY_26+27+28 shipping chord is checked before audio starts ([L2062](../code/src/chompi_main.cpp#L2062)) and `prev_key_[]` baselines are captured after the shift-register flush, so a held-at-boot chord cannot fire the record gesture.
- **WAV container:** libDaisy ships `WAV_FormatTypeDef` + `kWavFileChunkId` (used by [WavWriter.h](../../chompi-tape/code/libs/libDaisy/src/util/WavWriter.h)) — reuse the struct, not the WavWriter class (its 2×transfer-size double buffer is too small for cheap-card stalls).
- **Modulation Mode gate:** `mod_mode_` is sampled in PollControls ([L671](../code/src/chompi_main.cpp#L671)); the record gesture is gated on `!mod_mode_`.

## Steps

### Phase 1 – Recorder core (host-testable)

Files: **new** `firmware/chompi-pipes/code/src/SdRecorder.h`, **new** `firmware/chompi-pipes/code/tests/sd_recorder_test.cpp`, `firmware/chompi-pipes/code/tests/Makefile` (new target).

1. `SdRecorder` class (header-only; the instance will be a global in chompi_main.cpp):
   - `Init(int16_t* ring, uint32_t ring_frames, float sample_rate)` — caller-owned SDRAM ring; members only, nothing on the stack.
   - ISR side: `RequestStart()` / `RequestStop()` set volatile flags; `PushBlock(const float* interleaved, size_t n)` converts with `f2s16` and appends to the ring while capturing. SPSC ring: ISR advances head, main loop advances tail, unsigned wrap-safe count; counter reset per take. Ring full → overflow flag, **never** overwrite.
   - Main-loop side: `Service(now_ms)` state machine `kSIdle → kSOpen → kSRecord → kSClose` (+`kSError`):
     - `kSOpen`: `f_mkdir("PIPES/RECORDINGS")` (FR_EXIST fine; survives card re-mounts), find the next free number via `f_stat` — names are `Recording %02d.wav` for 1–99 and plain `Recording 100.wav`, `Recording 101.wav`, … above that, scanned up to a bound of 999; `f_open` (FA_WRITE|FA_CREATE_ALWAYS), write 44-byte `WAV_FormatTypeDef` header. All names taken, a full card, or any error → `kSError` (red flash, no recording, old takes never overwritten).
     - `kSRecord`: drain ring in ≤4 chunks of 16 KiB per pass through a member bounce buffer (`uint8_t bounce_[16384]`, D1 `.bss`) into `f_write`; `f_sync` once per second; auto-stop at `kRecMaxBytes` (~3.9 GiB ≈ 6 h, FAT32/WAV 32-bit size limit); write error or `bw != size` → abort path.
     - `kSClose`: drain remaining partial chunk, patch header sizes via `f_lseek(0)` + header `f_write`, `f_close`. Abort with a working card also patches the header so partial takes stay playable; card gone → drop.
   - Pure helpers split out for tests: ring math, `NextFreeNumber(const bool exists[])` + name formatting (zero-padded 2-digit below 100, plain above), header field computation, byte-limit check.
2. Tests (run on this machine with the ucrt64 g++ recipe from the build-pipes skill): ring wrap/full-refusal/SPSC ordering, `f2s16` clamping, `NextFreeNumber` (empty, gaps, 99→100 rollover + formatting), header bytes + patched sizes, auto-stop threshold. No FatFS mocking — pure parts only (follows `mod_dest_test`/`organ_preset_test` precedent).

### Phase 2 – Wiring (no UI yet)

File: `firmware/chompi-pipes/code/src/chompi_main.cpp`.

3. Globals: `static int16_t DSY_SDRAM_BSS rec_ring[2 * 262144];` (262,144 stereo frames = 1 MiB ≈ 5.5 s of card-stall slack) and `static SdRecorder sd_recorder_;`.
4. In `main()`: `sd_recorder_.Init(...)` right after `preset_storage_.Init()`/LoadAll, before `StartAudio` (baseline `prev_key_` capture is untouched).
5. In `AudioCallback` after `engine.Process(out, size)`: `sd_recorder_.PushBlock(out[0], size * 2);` — the only ISR addition (~48 converts + copies per block, well under 1% CPU).
6. In `MainLoop` right after `preset_storage_.Service(...)`: `sd_recorder_.Service(System::GetNow());`.

### Phase 3 – UI state machine + LEDs

File: `firmware/chompi-pipes/code/src/chompi_main.cpp` (PollControls + UpdateLeds).

7. **Percussion/latch move from down-edge to release-edge** (required for a deterministic chord): KEY_27/KEY_28 down-edges only record timestamps/"pending" flags; the toggle fires on release *unless* the chord consumed it. Behavior change to existing features — see open questions.
8. Record state machine in PollControls (volatiles `rec_state_`, `rec_state_t_`, mirroring the init-hold pattern):
   - `kRecIdle`: both chord keys down together (either order) **and** `!mod_mode_` **and** `!shift_held_` (SHIFT+KEY_28 keeps meaning init-patch hold) → `kRecHoldStart`. No card (`preset_storage_.NoSdCard()`) → flash the four countdown keys red ~1 s, stay idle.
   - `kRecHoldStart` (2 s): release of either key before 2 s → cancel; if the hold survived >400 ms, swallow both pending toggles (aborted gesture ≠ two quick presses). SHIFT pressed mid-hold or switch flipped to Modulation → cancel. At 2 s → consume both pending toggles, `kRecCountdown`.
   - `kRecCountdown` (4 s): a fresh press of PLAY or LOOP cancels back to idle (that press toggles nothing); otherwise at 4 s → `sd_recorder_.RequestStart()`, `kRecActive`.
   - `kRecActive`: chord held 1 s → `kRecHoldStop`; at 1 s → `sd_recorder_.RequestStop()`, `kRecIdle`. Percussion/latch taps keep working during recording (release-edge rule), latch included — recording on/off never interferes.
   - `kSError` (from `Service`): fast red blink on PTH 0 for ~2 s, then idle.
9. UpdateLeds overlays, inserted above the init-hold bar in the keyboard-overlay priority chain (preset modes keep top priority):
   - Hold phases: same red left→right fill as the init bar, `lit = elapsed * 15 / hold_ms`.
   - Countdown: all 25 SMT off, `white_key_led[0..3]` white; every 1 s boundary the next key turns red (key 1 red at +1 s … key 4 at +4 s). Notes keep playing under the overlay.
   - PTH 0 chain: init-done blink → recording red blink on `pre_blink` (but SHIFT-held purple wins while held, so shift operations keep their feedback) → existing preset/shift branches.
10. No MIDI CCs, no preset persistence — recordings are session-scope by design; `OrganPreset` untouched.

### Phase 4 – Docs + verification

Files: `firmware/chompi-pipes/README.md`, builds/tests only.

11. README controls section: record gesture, countdown, stop, blink indicator, file location/naming (incl. 3-digit rollover), ~6 h per-take limit, no-card and card-full behavior.
12. Host tests pass; clean ARM build (`make -j4`, build-pipes skill recipe) with 0 warnings; check bin/SRAM deltas and the map for `.sdram_bss` (+1 MiB).
13. Hardware acceptance: chord start (either press order), abort by early release (no percussion/latch side effect), cancel during countdown, latch + percussion still work idle **and** while recording, SHIFT ops during recording (purple shown), stop chord at 1 s in both modes, WAV plays on a computer (header correct, length matches), record ≥10 min take, card-full behavior, 99→100 name rollover, no-card gesture inert, `PIPES_CPU_METER=1` build shows no meaningful CPU increase while recording, no IWDG resets during long takes.

## Decisions (confirmed 2026-10-18) + remaining risks

1. **Release-edge percussion/latch** — CONFIRMED: KEY_27/KEY_28 toggles move from the down-edge to the release-edge so the chord is deterministic and a completed chord has no side effects.
2. **Countdown cancel** — CONFIRMED: a fresh PLAY or LOOP press during the 4 s countdown cancels; that press toggles nothing.
3. **Stop chord works in both modes** — CONFIRMED (start stays Normal-Mode-only).
4. **Numbering past 99** — CONFIRMED: continue as `Recording 100.wav`, `101.wav`, … Only a full card blocks a new take (red flash, nothing overwritten).
5. **Power cut mid-take** — CONFIRMED accepted: header patched only on clean stop; 1 Hz `f_sync` bounds data loss to ~1 s; tolerant players open the unpatched file.
6. **LED choices** (plan defaults, not objected to): hold bar red (same as init bar), countdown keys white→red, PTH 0 blink = 2 Hz red with SHIFT-purple override while SHIFT is held.
7. Remaining risk: cheap-card stalls deeper than the ring (~5.5 s) abort the take with an error flash; a fully wedged card can still trip the ~2 s IWDG. Same exposure class as the existing preset writer, just bigger writes — accepted.

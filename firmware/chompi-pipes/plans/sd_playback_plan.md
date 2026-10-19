# Plan: CHOMPI-PIPES playback mode for SD recordings

A **Playback Mode** that plays the 48 kHz / 16-bit / stereo WAV takes in
`PIPES/RECORDINGS/` (`Recording NN.wav`) straight from the SD card, one at a
time. Entry is **hold PLAY for 2 s** (PLAY alone; PLAY + LOOP stays the
record chord) with a blue fill bar across the
white keys; exit is the same gesture again **or** flipping the toggle switch
either way. The mode is pure playback: all 25 keys are inert as notes, MIDI In
is ignored, the take bypasses every FX stage, and the jam recorder is fully
blocked. The latest 15 takes are selected with a white key press (C3 =
newest). PLAY taps play/pause, LOOP taps loop the take; with loop off a
finished take advances to the next-newer one and wraps from the newest back to
the oldest of the 15. The selected take's white-key LED is blue: solid while paused,
blinking while playing (the unselected take keys are white). The master volume knob keeps working
(playback is injected after the FX chain, before the master gain).

Locked-in UX decisions (user, 2026-10-18): exit = both gestures; recorder
fully blocked (entering stops a running take); volume knob active; C3 =
newest; end-of-list wraps; entering loads the newest take paused.
Revised 2026-10-19: entry/exit = hold PLAY 2 s (no SHIFT; LOOP joining
the hold cancels, so the record chord keeps priority); a white-key
press selects its take directly (no SHIFT); the selected key is blue
(solid paused / blinking playing), unselected takes are white.

## What the firmware actually supports (checked in the code)

- **Takes on disk:** `PIPES/RECORDINGS/Recording NN.wav`, NN = 01..99
  zero-padded then plain 100..999, always 48 kHz / 16-bit / stereo with a
  canonical 44-byte PCM header. Naming and paths come from
  `SdrRecFileName` / `SdrRecPath` in
  [SdRecorder.h](../code/src/SdRecorder.h); the header layout is
  `SdrWavHeader`. Nothing ever overwrites an old take, but files can be
  deleted externally, so gaps in the numbering are possible.
- **The recorder already shows the staged-scan pattern:** `SdRecorder::Service`
  does a staged next-free-number scan (`kScanPerPass = 32` `f_stat` calls per
  main-loop pass) and drains a 1 MiB SPSC ring in <=4x 16 KiB chunks per pass.
  All FatFS calls run at thread level, never in the audio ISR; the instance is
  a global so its `FIL`/bounce buffers live in D1 .bss (the SDMMC IDMA cannot
  reach the DTCMRAM stack). The player must follow the exact same constraint
  class. See [SdRecorder.h](../code/src/SdRecorder.h) (`class SdRecorder`).
- **Audio path:** `AudioCallback` calls `engine.Process(out, size)` (block
  size 24, 2 kHz), then `sd_recorder_.PushBlock(rec_tap_l, rec_tap_r, size)`.
  See [chompi_main.cpp](../code/src/chompi_main.cpp) `AudioCallback`. Inside
  `OrganEngine::Process` the per-sample chain is: mix -> distortion -> rotary
  -> fuzz leveling -> stereo delay -> plate reverb -> **record tap write**
  (`rec_tap_l_[i] = out_l * kRecordTapGain`) -> **master gain** -> `out[0..3]`
  (phones L/R, master L/R). See [OrganEngine.h](../code/src/OrganEngine.h)
  `Process`. Injecting playback between the record-tap write and the master
  gain gives: no FX on the take, take never re-recorded, volume knob active.
- **Entry-gesture precedent:** the record chord (`kRecHoldStart` 2 s hold ->
  red fill bar on the 15 white keys) and the init-patch hold (2.5 s, red fill
  bar) already implement "hold + fill bar" UX with abort/consume logic, incl.
  the `kRecSwallowMs` release-edge swallow that keeps a completed hold from
  firing the key's normal toggle. See [chompi_main.cpp](../code/src/chompi_main.cpp)
  `PollControls`, the `RecState` machine.
- **PLAY/LOOP key handling:** both fire their toggles (percussion / latch) on
  the RELEASE edge via `play_pending_` / `loop_pending_`, and a completed or
  >400 ms aborted hold swallows them. Holding PLAY alone currently does
  nothing. See [chompi_main.cpp](../code/src/chompi_main.cpp) `PollControls`.
- **Mode gating precedent:** preset management is Normal-mode-only and a fresh
  SHIFT press confirms a pending selection; entering Modulation Mode cancels
  unconfirmed preset selections. `mod_mode_` is sampled from the physical
  switch every pass (`ModModeSwitchUp()`), so "flip the switch to exit" is a
  trivially reliable gesture. See [chompi_main.cpp](../code/src/chompi_main.cpp)
  `PollControls`, `PresetKeyEvent`.
- **MIDI In:** all MIDI In is drained by `midi.ProcessMidiIn()` in
  `AudioCallback` (notes, CCs) plus the deferred Program Change slot load
  (`midi.TakePendingPresetSlot()`). Gating those two calls gates every MIDI
  In effect. See [MidiHandler.h](../code/src/MidiHandler.h) `ProcessMidiIn`.
- **LEDs:** every key LED is float RGB via `SetSmtLedFloat` (25 SMT keys) and
  `SetPthLedFloat` (PTH 0 = CHOMPI key, PTH 7 = PLAY, PTH 8 = LOOP). Blue is
  already the "selection" color in the preset UI (save/copy destination =
  blue). The 250 ms `pre_blink` toggle drives blinking. See
  [chompi_main.cpp](../code/src/chompi_main.cpp) `UpdateLeds`.
- **SDRAM budget:** the recorder's 1 MiB `DSY_SDRAM_BSS rec_ring` took
  `.sdram_bss` from 384 KiB to 1408 KiB; a second 1 MiB playback ring fits
  easily in the Daisy Seed's SDRAM. See [chompi_main.cpp](../code/src/chompi_main.cpp)
  (rec_ring declaration, `sd_recorder_.Init(rec_ring, 262144, ...)`).
- **Host-test pattern:** the recorder's pure core (ring math, f2s16, naming,
  header layout) is compiled for host tests via `CHOMPI_SD_RECORDER_HOST`;
  tests live in [code/tests/sd_recorder_test.cpp](../code/tests/sd_recorder_test.cpp)
  with its own [Makefile](../code/tests/Makefile).

## Steps

### Phase 1 - Player core + host tests (no target code yet)

1. New file `firmware/chompi-pipes/code/src/SdPlayer.h`, structured like
   [SdRecorder.h](../code/src/SdRecorder.h): a pure, host-testable core first,
   the FatFS-facing class compiled out under `CHOMPI_SD_PLAYER_HOST`.
   Pure pieces:
   - `SdpS16ToF(int16_t)` -> float (inverse of `SdrF2S16`).
   - `SdpWavOk(const uint8_t header[44], uint32_t sample_rate)` -> validates
     RIFF/WAVE/fmt, PCM, 2 channels, 16-bit, matching rate, data size > 0.
     Foreign files dropped into the folder must fail this check.
   - Take-window logic over a descending scan result: store up to 15 take
     numbers sorted newest-first; `SdpSlotToTake(slot)` maps white-key slot
     0..14 -> take number (slot 0 = newest = highest number);
     `SdpNextSlot(slot, count)` advances with wraparound (newest end wraps to
     the oldest of the window).
2. New file `firmware/chompi-pipes/code/tests/sd_player_test.cpp` +
   [Makefile](../code/tests/Makefile) target, mirroring `sd_recorder_test.cpp`:
   s16->f conversion round-trip against `SdrF2S16`, header validation accept/
   reject table, slot mapping with gappy numbering (e.g. takes 3, 7, 998
   present), next-slot advance and the wrap at the newest take.

### Phase 2 - `SdPlayer` FatFS class (target-only section of SdPlayer.h)

3. `class SdPlayer`, same constraint class as `SdRecorder`: global instance
   (D1 .bss), caller-owned SDRAM ring, ISR side = flags + ring pop only, one
   staged FatFS step per `Service()` pass.
   - States: `kSIdle`, `kSScan` (staged `f_stat` scan **downward** from 999,
     collecting up to 15 existing take numbers, early exit once 15 found),
     `kSOpen` (staged `f_open` + 44-byte header read + `SdpWavOk`), `kSPlay`
     (fill the ring: <=4x 16 KiB `f_read` per pass; EOF -> loop ? `f_lseek`
     to data start : close + open the `SdpNextSlot` take), `kSError`
     (~2 s, mirrors the recorder).
   - ISR API: `PopBlock(float *l, float *r, size_t frames)` fills the
     deinterleaved playback buffers from the ring (silence + underrun flag on
     underflow); `RequestPlay(slot)`, `RequestPause()`, `RequestResume()`,
     `RequestStop()`, `RequestRescan()` for slot re-selection after takes are
     added/removed outside the mode.
   - Gapless advance: the ISR keeps popping while `Service()` closes/opens
     the next file (~tens of ms); the 262144-frame ring (~5.5 s) covers this
     trivially. Loop mode never leaves `kSPlay` (just `f_lseek`), so looping
     is sample-gapless too.
   - Position/size in `uint32_t`: takes are capped at `kSdRecMaxBytes`
     (~3.9 GiB), which fits.

### Phase 3 - Audio path

4. [OrganEngine.h](../code/src/OrganEngine.h): add
   `SetPlaybackIn(const float *l, const float *r)` (like `SetRecordTap`). In
   `Process()`, per sample, **after** the record-tap write and **before** the
   master-gain multiply: `out_l += pb_l[i]; out_r += pb_r[i];` (null-checked).
   No FX stage is touched; the tap order keeps playback out of any future
   take even if the recorder gate is ever relaxed.
5. [chompi_main.cpp](../code/src/chompi_main.cpp):
   - Globals: `static SdPlayer sd_player_;`,
     `static int16_t DSY_SDRAM_BSS play_ring[2 * 262144];`,
     `static float play_mix_l[24], play_mix_r[24];`.
   - `main()`: `sd_player_.Init(play_ring, 262144, sample_rate)` and
     `engine.SetPlaybackIn(play_mix_l, play_mix_r)` next to the recorder init.
   - `AudioCallback`: gate `midi.TakePendingPresetSlot()` +
     `midi.ProcessMidiIn()` on `!playback_mode_`; call
     `sd_player_.PopBlock(play_mix_l, play_mix_r, size)` before
     `engine.Process(out, size)`.
   - `MainLoop`: `sd_player_.Service(System::GetNow())` next to
     `sd_recorder_.Service(...)`. All FatFS users (presets, recorder, player)
     stay sequential small steps on the same thread, so no reentrancy issue.

### Phase 4 - Mode state machine + gestures (chompi_main.cpp)

6. New state: `static volatile int playback_mode_` (kPbOff, kPbEnterHold,
   kPbStopping (recorder closing), kPbScan, kPbOn) and transport
   `kTrPaused` / `kTrPlaying`.
   - **Entry:** PLAY held 2 s, Normal Mode only (switch DOWN), no
     preset mode active. Blue fill bar across the 15 white keys while held
     (same rendering as the record chord's red fill); early release or
     LOOP joining cancels (the record chord keeps priority); a completed
     entry swallows the PLAY percussion toggle via
     the existing `play_pending_` consume. No card -> ~1 s red flash, stay
     off (same refusal pattern as the record chord).
   - **Entry sequence:** release every sounding/latched note and clear the
     latch (`all-notes-off`, reuse the unlatch loop); if the recorder is
     busy, `sd_recorder_.RequestStop()` and wait for `kSIdle` (kPbStopping);
     then scan (kPbScan, white keys pulse dim blue as progress); zero takes
     found -> red flash, back to kPbOff; otherwise select slot 0 (newest),
     transport = paused.
   - **Exit:** hold PLAY 2 s again (same blue bar), or any flip of
     the toggle switch (lands in whatever mode the switch position says;
     sampled next to the existing `mod_mode_` edge check). On exit:
     `sd_player_.RequestStop()`, mode off, normal key handling resumes.
   - **In the mode (PollControls key loop):** keys 7..31 never reach note /
     latch / octave / preset handling (gate `PresetKeyEvent` and the note
     branch on `!playback_mode_`); a white key 0..14 press ->
     `RequestPlay(slot)` (transport state is preserved across a take switch);
     PLAY release-edge tap toggles pause/resume; LOOP release-edge tap
     toggles take-loop (its latch toggle and the SHIFT+LOOP init hold are
     gated); the record chord state machine is inert (gate on
     `!playback_mode_`); percussion toggle gated.
   - **Encoders:** ENC_6 turn adjusts master volume regardless of `vol_page_`
     (no page cycling; press inert); ENC_1..5 and their presses inert.
   - **MIDI In:** fully ignored via the Phase-3 gate (notes, CCs, PC preset
     loads). MIDI Out produces nothing anyway (no key/encoder events fire).

### Phase 5 - LED feedback (chompi_main.cpp UpdateLeds)

7. New top branch in `UpdateLeds` for playback mode (first match wins, like
   the preset branch):
   - White keys always show the take window while the mode is on: existing
     take = dim solid white; selected take = blue, solid while paused and
     blinking at the existing 250 ms `pre_blink` rate while playing;
     empty slots
     off. (Keys are inert, so no SHIFT-overlay is needed for display or
     selection.)
   - PLAY key LED (PTH 7) mirrors the transport in blue (blink = playing,
     solid = paused) instead of the amber percussion indicator; LOOP key LED
     (PTH 8) is solid blue while take-loop is on, off otherwise (replaces
     the red latch indicator in the mode).
   - Entry/exit holds draw the blue fill bar; kPbScan draws a dim blue
     chase/pulse; no-card / zero-takes refusal and player errors reuse the
     ~1-2 s red flash pattern. CHOMPI key LED (PTH 0) is untouched (no
     recording can be active in the mode).

### Phase 6 - Docs

8. [README.md](../README.md): new "Playback mode" section (entry/exit
   gestures, transport, take window, LED legend, recorder/MIDI restrictions).
9. [midi.md](../midi.md): note that Playback mode ignores all MIDI In.

### Phase 7 - Verification

10. Build and run the host tests per the build-pipes skill (`sd_player_test`
    alongside `sd_recorder_test`).
11. Build the firmware `.bin` with the MSYS2 recipe.
12. On-device: record two short takes; enter the mode (bar, newest selected,
    paused); PLAY/LOOP/SHIFT-select behave; wrap at the newest take; volume
    knob scales playback; FX knobs do not; MIDI In is inert; exit via both
    gestures; recording fully blocked inside the mode and works again after
    exit; card removal mid-playback -> stop, error flash, mode exits.

## Risks / open questions

- **Entry scan latency:** the downward `f_stat` scan early-exits after 15
  takes, but an empty card costs the full 999-stat sweep (worst case ~1-2 s
  of kPbScan at 32 stats/pass). Acceptable, and the pulse animation covers
  it; alternatively persist the last-known highest take number in a small
  state file - rejected for now (another writer on the card).
- **Invalid/foreign WAVs:** validation happens at open time, not during the
  scan (reading 999 headers is too slow). A take that fails `SdpWavOk` is
  skipped: the player auto-advances to the next take with a brief red flash
  on its slot. If every take fails, the mode exits with the error flash.
- **Take switch while playing** keeps the transport state (a playing
  selection keeps playing the new take from 0:00; paused stays paused).
  Alternative if that feels wrong in practice: always switch to paused.
- **Card removal mid-playback:** playback stops, ~2 s error flash, mode
  exits to the switch-selected mode (mirrors how the preset UI cancels on
  card removal).
- **CPU headroom:** playback adds one ring pop + two float adds per sample
  and main-loop `f_read` filling; recording is fully blocked in the mode, so
  SD bandwidth is read-only. The optional `PIPES_CPU_METER` build can
  confirm the margin.

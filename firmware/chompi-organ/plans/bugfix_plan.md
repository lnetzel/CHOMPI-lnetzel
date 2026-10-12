# CHOMPI-Organ Bugfix Plan

Status: **investigation done, nothing fixed yet**. Pick up from "Implementation order" at the bottom.

Scope: everything under `firmware/chompi-organ/code/src/` (`chompi_main.cpp`, `OrganEngine.h`,
`RotarySpeaker.h`, `MidiHandler.h`, `hardware.h`, `temp_led_stuff.h`, `encoder.h/.cpp`), plus the
libDaisy it links (`firmware/chompi-wave/code/libs/libDaisy`, read-only reference — shared with
WAVE/TAPE, so all fixes should stay in the organ's own files).

---

## 1. Reported symptom

> Play many notes at once, turn knobs, "do a lot" → after a while it hangs. Audio freezes on the
> active voices, nothing responds, power cycle needed.

Clarified with user:

| Question | Answer |
|---|---|
| Frozen audio sounds like | Buzzy/robotic short loop (tiny snippet repeating) |
| LEDs at hang | Frozen in last state |
| Connected | Happens on battery only, and with USB to a computer |
| Rotary / drive / morph up? | Not sure |

### Interpretation

A buzzy repeating snippet + frozen LEDs = **the CPU is stuck somewhere no priority-0 interrupt can
run**. The SAI audio DMA is circular, so it keeps replaying the last 24-sample block forever
(24 samples @ 48 kHz = 2 kHz buzz). Possible ways to get there:

- a spin loop with interrupts globally disabled (`__disable_irq` / `ScopedIrqBlocker`),
- a spin loop inside any interrupt (all of them share priority 0, so none can preempt another),
- a HardFault / `Error_Handler()` (`bkpt` + `while(1)`).

Ruled out for this symptom: the low-battery STOP-mode loop (LEDs would go dark), the 15 s
low-battery warning (audio keeps running, LEDs flash amber), plain CPU overload (keys are polled in
the audio ISR, so releasing keys would recover it — though overload can *trigger* other bugs, see B4).

### Key platform facts (libDaisy in this repo)

- `src/sys/dma.c`, `src/per/uart.cpp`, `src/usbd/usbd_conf.c`: **every** IRQ (SAI audio DMA,
  UART, UART TX DMA2_Stream4, LED TIM DMA2_Stream5, I2C DMA1_Stream6, USB) is NVIC priority **0**.
- `src/sys/stm32h7xx_hal_conf.h`: `TICK_INT_PRIORITY 0x0E` → SysTick can never preempt those ISRs.
  **`HAL_GetTick()` is frozen inside any ISR or while IRQs are disabled**, so every HAL wait loop
  with a timeout becomes an infinite loop in those contexts.
- `src/per/uart.cpp` `StartDmaTx()`: `while(huart_.gState != HAL_UART_STATE_READY) {}` — no
  timeout. `InitDma()` failure → `Error_Handler()` → `bkpt 255; while(1)`.
- `src/per/uart.cpp` `DmaListenStart()`: disables `DMA1_Stream5_IRQn` (MIDI RX DMA) at the NVIC.
- `src/per/tim_channel.cpp` `StartDma()`: one shared `static DMA_HandleTypeDef timhdma` +
  `globaltim`/`globalcb` for both LED chains; calls `HAL_DMA_Init` + `HAL_TIM_PWM_Start_DMA`.
- `src/per/i2c.cpp`: `TransmitBlocking` spins `while(HAL_I2C_GetState() != READY) {}`;
  `DmaTransferFinished` runs `HAL_I2C_Init` inside `ScopedIrqBlocker`.

---

## 2. Findings

Severity: **H** = plausible cause of the reported hang, **M** = real bug, other symptom,
**L** = minor / cleanup / perf.

### A. Hang candidates

#### A1 (H) MIDI out spins on a UART flag with interrupts disabled — `MidiHandler.h`

- `QueueDmaMessage()` (~L190-L203) does `__disable_irq(); DequeueDmaMessage(); __enable_irq();`
- `ProcessMidiOut()` (~L104-L113) wraps `DequeueDmaMessage()` in `ScopedIrqBlocker`.
- Both end in `uart_handler_.DmaTransmit()` → libDaisy `StartDmaTx()` → `while(gState != READY){}`
  and `HAL_DMA_Init()` (tick-based timeout, frozen with IRQs off). If the UART is not READY at that
  instant, nothing can ever make it READY → permanent lockup with audio looping the last block.
- Runs on **every key press, key release and encoder click** (note on/off + CC out), even with no
  MIDI cable plugged in — matches "do a lot of stuff → hangs".

**Fix**
- Never call into `UartHandler` with IRQs disabled. Remove `__disable_irq` / `ScopedIrqBlocker`
  around the transmit.
- Only the main loop starts transfers (see A2). `QueueDmaMessage()` only enqueues.

#### A2 (H) MIDI out DMA chain runs HAL from the UART ISR, queue is unsafe — `MidiHandler.h`

- `TxEndCallback()` (~L219) runs in the UART IRQ and immediately calls `DequeueDmaMessage()` →
  `DmaTransmit` → `StartDmaTx` → `HAL_DMA_Init` / `HAL_UART_Transmit_DMA` inside an ISR (frozen tick).
- On any error path libDaisy calls `end_callback(ERR)` synchronously from `StartDmaTx`, so
  `TxEndCallback` → `Dequeue` → `StartDmaTx` → ... recurses once per queued message.
- 64-entry ring has **no full check**: `QueueDmaMessage()` overwrites unsent entries when 64 are
  pending; head laps tail and the queue then looks empty / resends stale data.
- `DequeueDmaMessage()` (~L208-L211): `if(fifo_head == fifo_tail) fifo_head = (fifo_head + 1) % 64;`
  — bogus; advances head past a slot that was never written.
- `fifo_head`, `fifo_tail`, `uart_tx_busy` are shared with an ISR but not `volatile`.

**Fix (rewrite the TX queue as a plain SPSC ring)**
- `volatile size_t head, tail; volatile bool busy;`
- `QueueDmaMessage()`: if `(head + 1) % N == tail` drop the message (optionally count drops);
  else copy + advance head. No transmit here.
- `TxEndCallback()` (ISR): `tail = (tail + 1) % N; busy = false;` — nothing else, no HAL calls.
- `ProcessMidiOut()` (main loop): `if(!busy && head != tail) { busy = true; clean DCache; DmaTransmit(...); }`
  with IRQs enabled. Since only the ISR clears `busy` and only the main loop sets it, no critical
  section is needed.
- Delete the `head++` branch in `DequeueDmaMessage()` (or delete the function).
- In `MainLoop()` (`chompi_main.cpp` ~L572) call `midi.ProcessMidiOut()` **after** draining `out_q`
  as well, so a fresh message goes out in the same loop pass.
- Throughput is unchanged: MIDI DIN is ~1 ms per 3-byte message, main loop runs every ~10-20 µs.

#### A3 (H) LED DMA watchdog races the LED DMA ISR — `temp_led_stuff.h`

- `EndOfLeds()` (L205) runs in the DMA2_Stream5 ISR and restarts the *other* chain via
  `StartDma()` (HAL_DMA_Init + HAL_TIM_PWM_Start_DMA) ~1000×/s.
- `LedChainWatchdog()` (L232), called from `MainLoop()`, calls `ledSmtPwm.StartDma()` from the
  main loop with no protection. Both write libDaisy's shared static `timhdma`, `globaltim`,
  `globalcb`. If the ISR fires mid-restart, the HAL handle is corrupted and the ISR can spin in
  `HAL_DMA_Init` (frozen tick) → audio ISR never runs again.
- Most likely trigger: a burst of main-loop starvation (heavy load, see B4) → no LED frames for
  100 ms → watchdog restarts while the delayed DMA ISR is just becoming pending.
- The fact that this watchdog exists at all means the chain *has* died before.

**Fix**
- Require 2 consecutive stale checks (~200 ms) before restarting.
- Restart with only the LED DMA IRQ masked (not global IRQs, so audio keeps running and the
  HAL tick isn't relevant):
  `HAL_NVIC_DisableIRQ(DMA2_Stream5_IRQn)` → stop both PWM channels → `ledSmtPwm.Start()` +
  `ledSmtPwm.StartDma(...)` → `HAL_NVIC_EnableIRQ(DMA2_Stream5_IRQn)`. If a stale pending IRQ from
  the dead frame turns out to fire right after re-enable, also `HAL_NVIC_ClearPendingIRQ()` before it.
- Optional bigger change (only if LEDs still stall): stop the free-running ping-pong. `EndOfLeds`
  for SMT starts PTH (keep), `EndOfLeds` for PTH just sets `led_frame_done = true`; `MainLoop`
  starts the next SMT frame every 16 ms after `UpdateLeds()`. Removes ~95 % of HAL calls from ISR
  context and makes the watchdog unnecessary.

#### A4 (H, safety net) No hardware watchdog — `chompi_main.cpp`, `hardware.h`

Even with A1-A3 fixed, any future lockup needs a power cycle.

**Fix**
- Start IWDG1 right after `hw.StartAudio()` (`chompi_main.cpp` ~L663). LSI ≈ 32 kHz; prescaler 64,
  reload ≈ 1000 → ~2 s. HAL IWDG is compiled into libDaisy (`HAL_IWDG_MODULE_ENABLED`,
  `stm32h7xx_hal_iwdg.c` in its Makefile); direct register writes (`IWDG1->KR = 0xCCCC / 0x5555 /
  0xAAAA`, `PR`, `RLR`) are an equally simple option.
- `AudioCallback()` increments `volatile uint32_t audio_ticks`. `MainLoop()` refreshes the IWDG only
  if `audio_ticks` changed since the last refresh → catches a stuck ISR, a stuck main loop, and
  IRQs left disabled.
- `hardware.h` `LowBatteryLockoutCheck()` 15 s amber warning loop (~L280): refresh IWDG inside it
  (audio keeps running there, but the main loop doesn't).
- STOP-mode lockout loop (~L313): IWDG keeps running in STOP and will reset once; after reset the
  boot code enters STOP again *before* the IWDG is started, so the device ends up asleep. Acceptable,
  but document it. (Alternative: set the `FZ_IWDG_STOP` option byte — affects the bootloader too, avoid.)
- Bootloader: after an IWDG reset the IWDG is off again (software mode), so the bootloader is
  unaffected. Verify the bootloader → app jump doesn't take > 2 s on the normal path (it isn't
  running yet at that point anyway).

### B. Other real bugs

#### B1 (M) MIDI-in dies permanently after any UART receive error — `MidiHandler.h` + libDaisy

- In listen mode any RX error (framing / noise / overrun — e.g. plugging/unplugging a DIN cable)
  makes HAL abort the RX DMA via `HAL_DMA_Abort_IT`. The abort-complete callback needs the
  DMA1_Stream5 IRQ, which libDaisy disabled at the NVIC → `HAL_UART_ErrorCallback` never fires,
  `listener_mode_` stays `true`, so `MidiHandler::Listen()` (called every audio block) never restarts.
- In the other ordering (error delivered via `HAL_UART_ErrorCallback`), libDaisy's
  `DmaTransferFinished(ERR)` calls `HAL_UART_Init()` mid-TX from the ISR and invokes our TX
  callback with ERR.

**Fix**
- In `MainLoop()` (not the ISR): every ~50 ms, if `uart_handler_.CheckError() != 0` (public in
  `per/uart.h`) or no RX DMA progress is possible, call `DmaListenStop()` then
  `uart_midi.StartReceive()`. Needs a bench test: plug/unplug a DIN source while playing.
- Remove `uart_midi.Listen()` from the audio-ISR path (`GetMidi()`); call it from the main loop
  so any `DmaListenStart()` re-init never runs inside the audio ISR.

#### B2 (M) Dropped MIDI note-offs → stuck notes on external gear — `chompi_main.cpp`

- `out_q` (`FIFO<OutMsg, 64>`, L209) is filled from the audio ISR; `PushBack` silently fails when
  full (e.g. during main-loop starvation or USB TX retries). A dropped note-off leaves the
  connected synth/DAW hanging.
- After A2, the DMA ring can also drop.

**Fix**
- Never drop note-offs: on full, drop the oldest CC instead, or keep a 128-bit "pending note-off"
  bitmap that the main loop flushes. Simplest: raise `out_q` to 256 and make CCs coalescing
  (only the latest value per CC is sent — store per-pipe "dirty" flags instead of queuing every click).

#### B3 (M) Stuck voice when re-pressing a latched key after an octave change — `chompi_main.cpp`

- Latch on, play key K at octave 0, release (note N latched), shift octave, press K again:
  `active_note_[K]` is overwritten with N+12 (L311-L316) and `latched_[K] = false`; note N is never
  released (no engine NoteOff, no MIDI note-off).

**Fix**
- On key down, if `active_note_[swid] != 0` and the stored note differs from the new one, send
  `engine.NoteOff(old)` + queue MIDI note-off before starting the new note.

#### B4 (M) Audio CPU headroom unknown — `OrganEngine.h`, `RotarySpeaker.h`

Per sample: up to 24 `sinf` oscillators + 24 polyBLEP squares (any morph between 0 and 64 runs both),
10 one-pole smoothers, rotary 4× `sinf` + 4× `floorf`, overdrive 2× `tanhf`. Rough estimate
50-70 % of 480 MHz in the worst case — close enough that peaks (voice steals, all effects on) may
overrun. Overrun makes the audio ISR run back-to-back, starving the main loop, LED and UART ISRs —
which then sets up A3 (and MIDI out stalls). Never measured.

**Fix / measure**
- Debug build flag (e.g. `ORGAN_CPU_METER`): wrap `AudioCallback` with `daisy::CpuLoadMeter`
  (`util/CpuLoadMeter.h`); show peak load on PTH LED 0 (green < 70 %, amber 70-85 %, red > 85 %).
- Cheap wins regardless:
  - `OrganEngine::Process` (~L357): `post = 0.33f / tanhf(0.33f * g)` is computed every sample;
    compute only when `drive_` actually changed (or once per block).
  - `RotarySpeaker::Process` (L81-L83): the two mics are 180° apart, so
    `sin(x + π) = -sin(x)`. Compute one `sinf` for the horn and one for the drum and negate for the
    other mic → 4 `sinf` → 2.
  - Optionally replace the `WAVE_SIN` oscillators with a phase accumulator + polynomial/table sine
    (24 `sinf` per sample is the dominant cost).

#### B5 (M) Low-battery STOP lockout can trigger under heavy load on USB — `hardware.h`

- `while(batt_low_bounce == 0xff && (legacy_cable_bounce == 0xff || iindpm_stat_bounce == 0xff))`
  (~L307-L314) enters STOP and **never re-reads the charger**, so it can only exit by power cycle.
- `iindpm_stat` = charger input current limit active. With a low battery on a weak USB port, many
  voices + bright LEDs + high volume can hold IINDPM for 8 × 20 ms = 160 ms → device sleeps.
- Not the reported symptom (LEDs go dark), but it is another "needs power cycle" path.

**Fix**
- Debounce IINDPM over a much longer window (e.g. ≥ 2 s), or drop IINDPM from the condition and
  rely on `legacy_cable` only; re-read status after wake-up in the loop.

#### B6 (L) I2C status read every 20 ms with blocking calls — `hardware.h`

- `LowBatteryLockoutCheck()` runs every 20 ms from `MainLoop()` (`chompi_main.cpp` L597):
  `TransmitBlocking` (spins `while(HAL_I2C_GetState() != READY){}` with no timeout if a previous
  DMA read never finishes, e.g. bus held low) + `ReceiveDma`. A stuck I2C bus freezes the main loop
  (LEDs + MIDI out), audio keeps running. With A4 this becomes a reset instead.
- `ReceiveDma(address | 0B10000000, ...)` (~L345): the OR'ed bit is meaningless (libDaisy shifts
  the address and HAL masks it); works by accident. Use `address`.

**Fix**
- Poll every 200-500 ms instead of 20 ms; skip `MpReadAll()` while the previous `read_ready` is
  still false; use `address` without the OR.

#### B7 (L) USB MIDI TX can block the main loop — `MidiHandler.h`

- `usb_midi.Send*()` → libDaisy `Tx()` retries 3× with `DelayUs(100)` when the host isn't reading
  (e.g. computer connected, no app open). With a burst of 20 messages that is 6 ms of frozen main
  loop (LED lag, MIDI DIN lag, more `out_q` pressure).

**Fix**
- Set `usb_midi_cfg.transport_config.tx_retry_count = 0`, or stop sending USB MIDI after N
  consecutive failures until a USB MIDI message is received / USB reconnects.

### C. Minor / cleanup

| ID | File | Issue | Suggested fix |
|---|---|---|---|
| C1 | `OrganEngine.h` `NoteOn` (L192-L224) | Steal can pick a voice already in `STEAL`, silently overwriting its pending note (that key is held but silent). Retrigger check ignores `STEAL` voices whose `pending_note == note`, so two voices can end up on the same note. | Prefer non-`STEAL` voices for stealing; include `STEAL && pending_note == note` in the retrigger search. |
| C2 | `OrganEngine.h` `note_held_` | Shared between keys and MIDI: releasing the key clears "held" while MIDI still holds the same note (LED only). | Separate key/MIDI held arrays or a count. |
| C3 | `chompi_main.cpp` L496 | `IsNoteHeld(key_map[swid])` ignores `octave_`, so MIDI-held LEDs are wrong at octave ≠ 0. | Use `key_map[swid] + 12 * octave_` (clamped). |
| C4 | `hardware.h` L158, `encoder.cpp` | `enc[5]` (volume) is initialised with an empty click `Pin()`, but `Debounce()` still calls `sw_.Debounce()` on it → reads an invalid pin; a spurious `RisingEdge()` would zero its tracker. | Skip `sw_.Debounce()` when the click pin is invalid, or add a `Debounce()` variant without the switch. |
| C5 | `hardware.h` | `#define ENC_5_SW 4` unused; wheel push not handled. | Remove or document. |
| C6 | `temp_led_stuff.h` `fill_led_data()` | Rewrites the DMA output buffers while DMA is reading them → occasional one-frame LED tearing. | Double-buffer, or fill only between frames (comes for free with the A3 optional change). |
| C7 | `chompi_main.cpp` | Encoder chatter filter drops whole deltas inside 8 ms; very fast turns lose clicks. | Accumulate instead of drop if it matters in practice. |
| C8 | `MidiHandler.h` `Init` | `MidiUartTransport transport = uart_midi.GetMutableTransport();` copies the transport by value; works because `UartHandler` is a pimpl, but fragile. | Use a reference: `auto &transport = ...`. |

---

## 3. Implementation order

1. **A2 + A1** — rewrite the MIDI TX ring (`MidiHandler.h`), call `ProcessMidiOut()` after draining
   `out_q`. Biggest suspect.
2. **A3** — guard `LedChainWatchdog()` restart (`temp_led_stuff.h`).
3. **A4** — IWDG with audio-alive check (`chompi_main.cpp`), refresh in the 15 s battery loop
   (`hardware.h`).
4. **B4 measure** — CPU meter debug flag; apply the cheap wins (overdrive post gain, rotary sinf halving).
5. **B2, B3** — no dropped note-offs, latch/octave stuck voice.
6. **B1** — MIDI-in recovery after UART error; move `Listen()` out of the audio ISR.
7. **B5, B6, B7** — battery/I2C/USB robustness.
8. **C*** — as time permits.

Steps 1-3 are independent of each other and can land in one change; build and stress-test after it.

## 4. Verification

1. `make` in `firmware/chompi-organ/code/src` — no new warnings.
2. Stress test ≥ 10 min, once on battery and once with USB to a computer: all 6 voices busy with
   fast chords (voice stealing), spin all 4 pipe encoders on both pages, wheel, volume and overdrive,
   toggle latch and octave repeatedly. No hang, LEDs never stall.
3. CPU meter (debug build): peak < 85 % in worst-case settings (6 voices, morph mid, drive + rotary up).
4. Fault injection (debug build only): a `while(1)` in `AudioCallback`, and separately
   `__disable_irq(); while(1);` in `MainLoop` → device resets within ~2 s and boots back into the organ.
5. MIDI monitor in a DAW (DIN and USB) while turning knobs fast and playing chords: notes and CCs in
   order, every note-on has a note-off, no garbage bytes.
6. B1: plug/unplug a DIN MIDI source repeatedly while playing; MIDI-in keeps working.
7. B3: latch on → play key → release → change octave → press same key → unlatch: no hanging voice.

## 5. Out of scope

- Changing libDaisy (shared with WAVE/TAPE). The same MIDI/LED patterns exist in
  `chompi-wave`/`chompi-tempo`/`chompi-tape` copies and could get the same fixes later.
- Redesigning the low-battery UX beyond B5.

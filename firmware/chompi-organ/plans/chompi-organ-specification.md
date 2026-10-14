# CHOMPI Organ Emulator — Requirements Specification (Draft)

**Status:** Draft  
**Target platform:** CHOMPI sampler, using the Daisy Seed as its audio-processing core  
**Purpose:** Define the initial requirements for a playable, four-pipe organ instrument implemented in CHOMPI firmware.

## 1. Goals

Implement a polyphonic organ emulator that uses four organ-pipe registrations per played note. The instrument should be playable from CHOMPI's keyboard keys and controlled primarily through the existing knob encoders.

The initial implementation should prioritize reliable note handling, simple controls, low latency, and enough DSP headroom to explore organ-style overdrive and other effects later.

## 2. Functional requirements

### FR-0 — Packaging

The organ shall be delivered as a separate, standalone CHOMPI firmware image (`.bin`), in the same way as the tape, tempo and wave firmwares. It is selected by installing that firmware, not by switching modes inside another firmware.

The firmware shall be a fresh, minimal project. Only the hardware and UI layer (hardware definitions, encoder/key/LED handling, event queue) shall be copied from the existing CHOMPI firmwares; no wavetable, sampler or looper code shall be carried over.

The firmware build `TARGET` / version name shall be `CHOMPI_ORGAN_1.0`.

Source code shall live in `firmware/chompi-organ/code/src` (same layout as the other firmwares) and build with Arm GNU toolchain 10.3-2021.10 (same as wave and tape).

The implementation is done when the firmware builds cleanly; hardware verification follows separately.

### FR-0a — SD card presets

The organ shall provide 15 persistent preset slots on the SD card, one small versioned JSON file per slot in `ORGAN/` (`P01.json` … `P15.json`, ~300 B). All slots shall be cached in RAM; load, save, copy and delete shall act on the cache and the engine immediately and shall never wait for the card. SD I/O shall run only from the main loop as a staged, non-blocking writer (one small FatFS step per pass, write-as-`.tmp`-then-rename), so it never stalls the audio ISR or the watchdog.

- **Load:** SHIFT + a white key (C3–C5 = slots 1–15) applies a filled slot instantly and makes it the current slot; empty slots do nothing. Held and latched notes shall keep sounding (every applied change is ramped or phase-continuous).
- **Save:** SHIFT + KEY_25 enters save select (pressing KEY_25 again cancels); a white key chooses any slot; a fresh SHIFT press confirms. The saved slot becomes the current slot.
- **Copy:** SHIFT + KEY_24 enters copy select (pressing KEY_24 again cancels); white keys choose a filled source, then a different destination; a fresh SHIFT press confirms.
- **Delete:** SHIFT + KEY_23 enters erase select (pressing KEY_23 again cancels); a white key chooses a filled slot; a fresh SHIFT press confirms. Deleting the current slot clears "current".
- The SAVING/COPYING/ERASING animations shall show for at least 1 s. While a selection mode is active, keyboard keys select slots only (no notes, no latch changes); encoders, the wheel and MIDI keep working, and releases of notes held before entering a mode are still delivered.
- Boot shall always use the factory defaults (section 5); the card is read into the RAM cache at boot, but a preset is applied only on load. If the card is missing or unreadable, the organ shall remain fully playable with the presets disabled (white keys red while SHIFT is held; gestures inert).
- Saved per slot: per-pipe level, Character, attack, release, mute and volume/Character-modulation amounts, plus vibrato depth, rotary amount, fuzz amount and tone, and reverb mix and decay. Not saved: main volume (FR-4), fine tune, octave shift, latch, page and wheel selections.

### FR-1 — Organ pipes / registrations

The instrument shall provide four independently adjustable pipe registrations:

| Registration | Footage | Relative pitch |
|---|---:|---:|
| Pipe 1 | 8′ | Fundamental (1×) |
| Pipe 2 | 4′ | One octave above (2×) |
| Pipe 3 | 2⅔′ | One octave plus an equal-tempered fifth above (≈ 3×) |
| Pipe 4 | 2′ | Two octaves above (4×) |

Each played note shall be able to sound all four registrations simultaneously, subject to each registration's volume setting.

### FR-2 — Pipe volume controls

The four non-wheel knob encoders, left to right, shall control the volume of the corresponding pipe registration:

| Encoder | Firmware ID | Controls |
|---|---|---|
| Encoder 1 (leftmost) | `ENC_4` | 8′ pipe volume |
| Encoder 2 | `ENC_1` | 4′ pipe volume |
| Encoder 3 | `ENC_2` | 2⅔′ pipe volume |
| Encoder 4 | `ENC_3` | 2′ pipe volume |

`ENC_5` (the transport wheel) is not a pipe-volume control; it is assigned to the rotary speaker (FR-4c) and vibrato (FR-4d).

Control behaviour:

- Turning an encoder to the left shall lower that pipe's volume.
- Turning an encoder to the right shall raise that pipe's volume.
- Each pipe's volume shall be adjustable independently of the other pipes.
- The control range shall have 32 steps (clicks) from silence (step 0) to maximum (step 32).
- One click shall always change the level by exactly one step (no acceleration).
- The level shall map to gain with a squared curve: gain = (step / 32)². This makes each click sound roughly like an even step in loudness.

### FR-2b — Per-pipe encoder pages (volume / Character / envelope)

Each of the four pipe encoders shall have four pages, cycled per pipe by pressing that pipe's encoder push-switch (1 → 2 → 3 → 4 → 1):

- **Page 1 (default at boot):** pipe volume (FR-2).
- **Page 2:** pipe Character. Turning the encoder sets that pipe's tonewheel-inspired harmonic coloration: 64 steps from the exact clean sine (step 0) to clearly aged but still organ-like (step 64), one step per click, no acceleration.

- Pages are per pipe: each pipe's Character is adjusted independently, and pipes can have different amounts.
- Character changes shall apply to sounding notes immediately and without clicks.
- Character amounts are stored in the SD-card presets (FR-0a); the page state itself is not saved and always starts on page 1.
- **Page 3:** pipe attack time. Turning the encoder sets how fast that pipe fades in after a key press: 32 steps, log-spaced from 5 ms (step 0, boot default — the original fixed timing) to 10 s (step 32). While a page-3 encoder is being turned, the white-key value bar shall be uniformly purple; the knob LED shall be purple (brightness tracking the step with a floor so the page stays visible at step 0).
- **Page 4:** pipe release time. Same 32-step log-spaced 5 ms–10 s mapping as page 3, applied to the fade-out after the key is released. The 5 ms floor (not a true 0 ms) keeps the release click-free. The white-key value bar and the knob LED shall be uniformly yellow.

- Pages are per pipe: each pipe's envelope is adjusted independently, and pipes can have different attack/release times, so one pipe can fade up later than another.
- Attack/release changes shall apply to sounding notes immediately and without clicks.
- Each pipe has its own gate envelope per voice; a voice shall remain allocated until every pipe's release has finished (the slowest pipe holds the voice). Voice stealing keeps the fixed 5 ms steal fade on all pipes.
- Attack/release settings are stored in the SD-card presets (FR-0a); both boot at step 0 (5 ms), identical to the original fixed timing. The page state itself is not saved and always starts on page 1.
- Attack and release have no MIDI CC mapping (unlike pipe level, CC 70–73, and Character, CC 74–77).
- While a pipe encoder is on page 2, its knob LED shall be blue (brightness tracking the Character amount); on page 1 the knob LED is white as before.
- While a page-2 encoder is being turned, the white-key value bar shall be blue (leftmost white key = clean, rightmost = fully aged), with the same ~0.5 s timeout as the volume bar.
- **SHIFT + pipe-encoder press** shall toggle that pipe's mute instead of cycling its page (the press is fully consumed). While muted the pipe shall be silent — click-free, via the existing ~1 ms level-gain slew, with the level step preserved and restored on unmute — and the pipe's knob LED shall be solid red, overriding the page color. Mute boots unmuted, is stored in the SD-card presets (FR-0a), and has no MIDI CC (level CC 70–73 still edits a muted pipe's step, which stays silent until unmuted). Muted pipes keep their oscillators running at gain 0.
- **SHIFT + pipe-encoder turn** shall set the value for all four pipes at once, per the turned pipe's current page: page 1 sets all four levels, page 2 all four Character amounts, page 3 all four attack times and page 4 all four release times to the turned pipe's new clamped value (an absolute broadcast, not a relative delta). The white-key bar shall show the turned pipe's page value, which all four pipes then share; global level and Character sets shall emit the corresponding MIDI CCs for all four pipes.

### FR-2c — Modulation Mode (per-pipe drift amounts)

The hardware toggle switch next to SHIFT/CHOMPI shall select the UI mode: **DOWN = Normal Mode** (the behavior specified in FR-2 through FR-5), **UP = Modulation Mode**. The switch shall be read through the existing debounced toggle state.

In Modulation Mode only the four pipe encoders' turn/push behavior and their knob LEDs change; the volume encoder and its pages, the transport wheel, the keys, octave shift, the latch, SHIFT + pipe-encoder press (mute) and all other LED indicators shall behave as in Normal Mode.

- Each pipe encoder push shall toggle that pipe between modulation page 1 and modulation page 2 (per-pipe state, separate from the Normal Mode pages, which are preserved).
- **Modulation page 1 — volume-modulation amount:** the knob LED shall be white. **Modulation page 2 — Character-modulation amount:** the knob LED shall be blue. Brightness shall be 0.2 + 0.8 × amount so the page stays visible at 0; a muted pipe's knob LED shall stay solid red.
- Turning a pipe encoder shall set that pipe's amount for its modulation page: 32 steps, 0–100 % (~3 % per click), clamped, no wrap. **SHIFT + turn** shall apply the delta to the turned pipe and broadcast the resulting clamped value to all four pipes (same absolute-broadcast pattern as Normal Mode).
- While a pipe encoder is being turned (or its page toggled), the white-key value bar shall show the amount for ~0.5 s with the same rounding as the other 32-step bars: uniformly white on page 1, uniformly blue on page 2.
- Flipping the switch mid-bar shall cancel the old mode's bar; the knob LEDs shall immediately show the active mode's pages.
- All modulation amounts shall boot at 0 (the sound is then identical to Normal Mode), are stored in the SD-card presets (FR-0a), and shall have no MIDI CCs. User/MIDI-facing values (pipe steps, Character steps, CCs, Normal Mode bars) shall remain the un-modulated centers. The drift itself is specified in DSP-8.

### FR-3 — Default pipe levels

At instrument initialization (when no saved levels are loaded), all four pipe registrations shall be set to step 24 of 32 (gain 0.5625).

The default settings shall produce a usable organ sound without requiring the user to adjust the encoders first.

The maximum individual pipe level is gain 1.0 (step 32). Headroom is handled by the fixed mixer scale in DSP-2.

### FR-4 — Main volume

The fifth encoder, located on the right (`ENC_6`), shall control the overall output volume of the organ instrument. It shall keep the existing CHOMPI hardware-volume behaviour unchanged (as in the other firmwares); no separate software main-volume stage is required.

- Turning right shall increase the overall volume.
- Turning left shall decrease the overall volume.
- Main volume shall affect the combined output of all four registrations and all active voices.
- Changing main volume shall not change the individual pipe-volume settings.
- `ENC_6` has further pages beyond main volume (distortion, reverb), cycled by its push-switch; see FR-4b.

### FR-4b — Distortion (volume-encoder page 2)

The `ENC_6` encoder shall have three pages, cycled by pressing the `ENC_6` push-switch:

- **Page 1 (default at boot):** main volume (FR-4).
- **Page 2:** distortion amount. Turning right increases the distortion from 0 % (step 0, exact bypass) to 100 % (step 32); 32 steps, one step per click, no acceleration.
- **Page 3:** plate-reverb mix.

- The distortion is a fuzz saturation stage with a tone control, applied to the organ mix before the master volume, so the drive character does not depend on the listening level (see DSP-5).
- **SHIFT + `ENC_6` turn on page 2 adjusts the distortion tone.** Turning right brightens, turning left darkens; 1 percentage point per click, clamped to 0–100 % without wrap. While SHIFT is held on page 2, the key LEDs shall show the tone instead of any value bar or held notes (an active preset selection mode takes precedence, FR-0a): a red bar across the 15 white keys with rounded percentage mapping (no keys lit at 0 %, eight at 50 %, all fifteen at 100 %), with all other key LEDs off. The tone shall be shown immediately when SHIFT is pressed on page 2, without requiring rotation, and continuously throughout the hold. Releasing SHIFT shall restore the (unchanged) distortion-amount bar for ~0.5 s.
- Changing the tone shall never change the amount, master volume, tuning or note state. Step 0 of the amount is an exact dry bypass at every tone setting.
- The distortion tone defaults to 100 % at every boot and is stored in the SD-card presets (FR-0a). The distortion amount defaults to 0 % at boot and is stored in the SD-card presets (FR-0a).
- While page 2 is active, the `ENC_6` knob LED shall be red, with brightness tracking the distortion amount (with a floor so the page stays visible at 0 %) — including while the tone bar is shown.
- While the distortion amount is being turned (without SHIFT), the white-key value bar shall be uniformly red (no gradient), with the same ~0.5 s timeout as the volume bar.
- SHIFT + `ENC_6` fine-tune (FR-5a) is not available on page 2.

### FR-4c — Rotary speaker (transport wheel)

The transport wheel (`ENC_5`) shall control the amount of the rotary-speaker simulation (DSP-6):

- Turning right increases the rotary amount from 0 % (step 0, exact bypass) to 100 % (step 32); 32 steps, one step per click, no acceleration.
- The rotary amount defaults to step 10 (~30 %) at boot and is stored in the SD-card presets (FR-0a).
- The effect runs after the overdrive and before the master volume, turning the mono organ mix into a stereo signal.
- While the wheel is being turned, the white-key value bar shall be uniformly purple, filling left to right, with the same ~0.5 s timeout as the volume bar.

### FR-4d — Vibrato (transport wheel)

Pressing the transport wheel (`ENC_5`) shall toggle the wheel's assignment between the rotary-speaker amount (FR-4c) and the vibrato depth; rotary shall be selected at boot.

- The vibrato shall be global pitch modulation across all organ voices from one shared, free-running 6 Hz LFO (DSP-7), independent of the rotary speaker's localized Doppler.
- Turning the wheel while vibrato is selected shall adjust the depth over 51 integer values, 0–50, in one-cent steps mapped directly to peak pitch deviation in cents: depth 50 moves the pitch by ±50 cents (one semitone peak-to-peak). Depth 0 shall be an exact no-vibrato bypass.
- The vibrato depth shall default to 0 at every boot and shall not be saved.
- While vibrato is selected and the wheel is being turned, the white-key value bar shall be uniformly pink, filling left to right (0–50 mapped across the 15 white keys), with the same ~0.5 s timeout as the volume bar.
- The two LEDs at the wheel column shall indicate the selection steadily until the next toggle: the left LED purple for rotary, the right LED pink for vibrato, with the unselected LED off.
- Wheel rotation shall change only the selected control; the rotary behavior, range and control resolution shall remain unchanged.

### FR-5 — Keyboard input

The CHOMPI white and black keys shall serve as the organ keyboard.

- Pressing a key shall start the corresponding note.
- Releasing a key shall end the note according to the selected envelope/release behaviour.
- White and black keys shall follow the intended chromatic keyboard mapping.
- The implementation shall support overlapping key presses up to the six-voice polyphony limit.

The lowest key shall play C3 (MIDI note 48, ≈130.8 Hz); keys ascend chromatically from there. Key event interfaces shall follow the existing CHOMPI firmware architecture.

### FR-5a — Tuning

- Tuning shall be 12-tone equal temperament with A4 = 440 Hz.
- Holding SHIFT and turning `ENC_6` on volume-encoder page 1 shall fine-tune the whole instrument by ±50 cents, 1 cent per click. On page 2, SHIFT + `ENC_6` adjusts the distortion tone instead (FR-4b); fine-tune is not available on the other pages. Without SHIFT on page 1, `ENC_6` remains main volume (FR-4).
- Fine-tune shall reset to 0 cents at every boot (not saved).

### FR-5b — MIDI

- MIDI shall use fixed channel 1 for both input and output.
- **Note in:** Note On/Off on channel 1 shall play notes exactly like key presses (same voice pool and stealing). Incoming velocity shall scale that note's volume.
- **Note out:** Key presses shall send Note On/Off on channel 1 (keys have no velocity; a fixed velocity is sent).
- **Pipe-level CCs (in and out):**

  | CC | Controls |
  |---:|---|
  | 70 | 8′ pipe level |
  | 71 | 4′ pipe level |
  | 72 | 2⅔′ pipe level |
  | 73 | 2′ pipe level |
  | 74 | 8′ pipe waveform morph |
  | 75 | 4′ pipe waveform morph |
  | 76 | 2⅔′ pipe waveform morph |
  | 77 | 2′ pipe waveform morph |

  Incoming CC values 0–127 map to steps 0–32 (rounded). Turning a pipe encoder sends the matching CC with the step scaled to 0–127. Loading a preset (FR-0a) re-sends the level and Character CCs so controllers resync.

### FR-5c — Octave shift

Like the CHOMPI-WAVE octave shift, but with a ±2 octave range:

- Holding SHIFT and pressing KEY_16 (C♯3) shall shift the keyboard down one octave per press; SHIFT + KEY_17 (D♯3) shall shift up one octave per press.
- The offset shall be clamped to ±2 octaves and shall reset to 0 at every boot (not saved).
- Octave shift shall transpose played keys and their MIDI note-out; incoming MIDI notes are not transposed.
- While SHIFT is held, the KEY_16/KEY_17 key LEDs shall indicate the active shift direction (dim orange = 1 octave, bright = 2 octaves).
- These two key presses shall not play notes while SHIFT is held.
- If the octave shift is changed while keys are being played, the active voices shall also be transposed to the new octave; note-off tracking shall use the note that was started at note-on, so changing octave mid-note never leaves stuck notes.

### FR-5d — Note latch (LOOP key)

KEY_28 (the LOOP/Overdub key) shall toggle a key latch:

- Pressing LOOP toggles the latch on and off. The LOOP key LED (PTH 8) shall be solid red while the latch is active and off otherwise.
- While the latch is active, releasing a key shall not stop its note; the note keeps sounding. Notes started while the latch is active also latch.
- Pressing a key whose note is currently latched shall release that note from the latch (toggle-off): the note stops and the press shall not retrigger it. The subsequent key release shall have no effect.
- Pressing LOOP while no keys are held shall arm the latch: subsequently played notes latch.
- Pressing LOOP again shall unlatch: each latched note whose key is not physically held shall be released (5 ms release per DSP-3). A latched note whose key is physically held down again at unlatch time keeps sounding and releases normally when the key is released.
- The latch applies only to physical keyboard keys; incoming MIDI Note Off messages shall always release their notes.
- MIDI note-out Note Off for a latched key shall be sent when the note actually stops (at toggle-off or unlatch), not at physical key release.
- Latched notes count toward the six-voice polyphony limit and follow the FR-6 voice-stealing policy.
- The latch state is not saved and is off at boot.

### FR-6 — Polyphony

The instrument shall support **six simultaneous notes**.

- Each voice shall generate the four pipe registrations at the appropriate pitches.
- The nominal maximum is 24 active oscillators (6 voices × 4 registrations).
- When a seventh note is requested while all six voices are active, the firmware shall steal the oldest voice (earliest note-on), regardless of whether it is held or releasing.
- If a note is played again while its previous instance is still releasing, that same voice shall be reused instead of allocating a new one.
- A stolen voice shall fade out over 5 ms before it starts the new note.
- The six-voice limit is fixed.

## 3. Audio and DSP requirements

### DSP-1 — Oscillator generation

Each pipe registration uses a per-pipe morphable waveform: sine at morph step 0, square at step 64, crossfading smoothly for in-between steps (smoothstep-eased so the morph lingers near the pure waveforms). Square shall use an anti-aliased (polyBLEP) oscillator so the high registrations do not alias. The implementation should remain structured so that other oscillator waveforms can be added later without redesigning the voice architecture.

For a played fundamental frequency `f`, the nominal registration frequencies shall be:

- 8′: `f`
- 4′: `2 × f`
- 2⅔′: `2^(19/12) × f` (equal-tempered octave + fifth, ≈ 2.997 × f, about 2 cents below a pure 3× harmonic)
- 2′: `4 × f`

- **Foldback:** If a pipe's frequency would exceed 20 kHz, it shall be shifted down by whole octaves until it is at or below 20 kHz (classic organ foldback).
- **Phase:** Oscillators shall be free-running; phase is not reset on note-on (the envelope prevents clicks).

### DSP-2 — Mixing and headroom

The four registrations shall be mixed for each active voice, then combined into the organ output.

The mixer shall use a fixed output scale of 1/24, so that 6 voices × 4 pipes at maximum level (and maximum MIDI velocity) cannot exceed full scale. No limiter or clipper is used.

Output shall be mono: the same signal on left and right. Audio input shall be ignored.

Intentional organ overdrive should be implemented as a separate, controllable processing stage in a later iteration, rather than relying on accidental digital clipping.

### DSP-3 — Note envelopes

Each voice shall use a fixed gate envelope: 5 ms linear attack to full level on note-on, sustain at full level while held, and 5 ms linear release to silence on note-off. Envelope times are not user-adjustable in v1.

### DSP-4 — Processing budget

The audio sample rate and block size shall be the same as in chompi-wave.

The design shall be suitable for real-time operation on the CHOMPI Daisy Seed platform. Six voices with four sine oscillators each are expected to be a modest oscillator workload. No on-device CPU-load indicator is required.

### DSP-5 — Fuzz distortion with tone control

The distortion is a fuzz saturation stage on the final organ mix, placed before the master-volume gain, so the drive character does not depend on the listening level. The 32-step drive amount `d` (0–1) maps to a strong quadratic pre-gain `g = 1 + 127 × d²` (1×–128×) into a symmetric bounded soft-knee clipper (near-hard saturation, |y| ≤ 2/3), followed by a one-pole low-pass and the 0.33 reference normalization. No bit reduction, no intentional DC. Step 0 (0 %) is an exact dry bypass at every tone setting.

- **Tone:** SHIFT + turn on page 2 sweeps the low-pass cutoff logarithmically, `500 × (10000/500)^(tone/100)` Hz for tone 0–100 %: 500 Hz (dark) at 0 % and the original 10 kHz at 100 %. Counterclockwise darkens; 0 % is not a mute. At 100 % the original coefficient expression, clipping curve, gain mapping and normalization are preserved exactly, so there is no additional filtering and the settled sound matches the previous Fuzz bit-for-bit.
- Tone changes shall compute the target filter coefficient outside the sample loop (no per-sample transcendental work, no heap) and ramp the live coefficient linearly over ~10 ms (sample-rate derived), retaining the filter history, snapping to the exact endpoint and retargeting from the live coefficient during rapid turns.
- When the amount target returns to 0, the fuzz shall transition wet→dry over ~10 ms and settle to an exact dry bypass with no residual filtered tail.
- **Output leveling:** an input-referenced output gain shall be applied equally to both stereo channels after the rotary and before the plate reverb, so the leveled level also feeds the reverb. Because the fuzz is a hard limiter whose settled output level is nearly independent of the input level, a fixed trim cannot match every registration: the dry distortion input and the untrimmed fuzz output shall be measured per audio block (RMS), smoothed over ~50 ms, and the fuzzed level servo-matched down to the dry level (`trim = in_rms / out_rms`), clamped to ≤ 1 (never boosted, the clean sound is never trimmed) and floored at −30 dB. At drive 0 the output equals the input, so the trim is exactly unity; folded through the live wet amount, it ramps click-free with the wet/bypass crossfade and is exactly unity at settled bypass. It introduces no user control and does not change the main-volume value or display.
- The drive amount shall be smoothed so changes do not produce clicks.

### DSP-6 — Rotary speaker

The rotary speaker is a 60s Hammond/Leslie-style mono-to-stereo stage, placed after the overdrive and before the master-volume gain:

- An 800 Hz one-pole crossover splits the mix into a drum band (low) and a horn band (high = input minus low).
- The horn band runs through a shared modulated delay line read by two virtual mics 180° apart: the delay wobble (±0.4 ms at 100 %) produces the Doppler pitch shift, and a sinusoidal gain locked to the same rotor phase produces the horn's amplitude modulation (depth 45 % at 100 %).
- The drum band gets a deeper (70 %) soft amplitude throb — one squared-sine pulse per revolution — with no Doppler, and rotates ~13 % slower than the horn (mechanical slip), so the two rotors beat against each other.
- The amount (0–1, in 32 steps, smoothed) scales all modulation depths and also ramps the rotor speed from a slow chorale-like swirl (2 Hz horn) to the classic fast tremolo (6.8 Hz horn), like a Leslie spinning up.
- Step 0 (0 %) is an exact bypass (both channels carry the dry mix) and shall cost no CPU.

### DSP-7 — Vibrato

The vibrato is a global pitch-modulation stage applied at the oscillators, before the mix and all effect stages:

- One shared, free-running 6 Hz sine LFO; every voice shall see the same instantaneous pitch multiplier 2^(depth/1200 · sin(2π·phase)).
- The depth (0–50 cents peak, one-cent steps) scales the multiplier; depth 0 shall be an exact bypass (multiplier exactly 1, no LFO computation).
- The multiplier shall be applied to each voice's stored base frequencies every sample: oscillator phase shall never be reset and frequency shall never accumulate, so pitch neither clicks nor drifts and the base tuning is preserved.
- The LFO phase shall advance continuously regardless of note or depth state; changing depth mid-cycle only rescales the deviation.

### DSP-8 — Modulation drift (Modulation Mode)

Eight independent, free-running drift modulators (4 pipes × {volume, Character}) shall run continuously in the engine, once per audio block, regardless of mode or modulation amount (changing an amount shall never restart a modulator).

- **Waveform:** a smooth raised-cosine swing between the window edges, u = (1 − cos(π·phase)) / 2, alternating rising and falling halves; the slope is zero at both edges so direction changes have no corners.
- **Timing:** each half shall draw its own random duration from 2.5–10 s, so every full cycle (bottom→top→bottom) takes 5–20 s and is asymmetric. Each modulator has its own xorshift32 PRNG state (distinct seeds) and a randomized start phase/direction, so the pipes are desynchronized from boot.
- **Depth and window:** the modulation works in each parameter's normalized domain — volume x = level_step/32 (the squared gain curve applies to the modulated value), Character a = char_step/64 (before the smoothstep easing). The amount scales the window half-width: d = 0.20 × amount (40 % total window at 100 %). The window [center−d, center+d] shall be slid (not clipped) to stay inside [0,1]: lo < 0 → [0, 2d]; hi > 1 → [1−2d, 1]. The modulated value is lo + (hi−lo) × u.
- **Exactness:** amount 0 shall return the unmodulated center bit-exactly; mute shall still force silence.
- **Smoothing:** modulated gain and Character targets are recomputed per block; the existing ~1 ms per-sample gain slew and the Character ~10 ms linear ramp shall smooth the block-rate steps.

## 4. User interaction requirements

- The six encoders shall retain the control assignments specified in FR-2, FR-4, FR-4b and FR-4c while the organ instrument is active.
- Encoder direction shall be consistent: left decreases the relevant level; right increases it.
- Pipe volume changes shall take effect smoothly without audible clicks.
- The main volume control shall change loudness without altering the balance between the four registrations.
- LED feedback (CHOMPI has no display):
  - While a pipe encoder, the main-volume encoder or the transport wheel is being turned, the key LEDs shall show the value as a bar on the white keys only (leftmost white key = 0 %, rightmost white key = 100 %). Volume bars (page-1 pipes and main volume) shall color the lit keys by position: green for the first 50 %, orange 50–75 %, red above 75 %. The page-2 waveform bar is uniformly blue; the distortion-amount bar is uniformly red; the rotary bar (FR-4c) is uniformly purple; the vibrato bar (FR-4d) is uniformly pink. The bar disappears about 0.5 s after the last click.
  - While SHIFT is held on volume-encoder page 2, the key LEDs shall instead show the distortion tone (FR-4b) as the highest-priority keyboard display: a red bar across the 15 white keys (rounded percentage mapping: no keys lit at 0 %, eight at 50 %, all fifteen at 100 %) with all other key LEDs off, including the octave hints. Other encoder or MIDI value bars shall not replace the tone bar. On SHIFT release the distortion-amount bar shall reappear for ~0.5 s. Keys, octave controls and the latch shall remain functional underneath the overlay.
  - Pipe knob LEDs shall show the pipe's value: white for page 1 (volume), blue for page 2 (Character), purple for page 3 (attack) and yellow for page 4 (release). A muted pipe's knob LED shall instead be solid red, overriding the page color (FR-2b). The `ENC_6` knob LED follows the position-gradient colors on page 1 (volume) and is red on page 2 (distortion, FR-4b), also while the tone bar is shown.
  - In Modulation Mode (FR-2c), the pipe knob LEDs shall instead show the modulation amounts: white for modulation page 1 (volume modulation), blue for modulation page 2 (Character modulation), brightness 0.2 + 0.8 × amount; a muted pipe stays solid red. Modulation value bars are uniformly white (page 1) or blue (page 2).
  - Otherwise, key LEDs shall light up for currently held notes.
  - The two LEDs at the transport-wheel column shall steadily indicate the wheel's selection (FR-4d): left purple for rotary, right pink for vibrato, until the next toggle.
  - The LOOP key LED shall be solid red while the note latch is active (FR-5d); latched notes' key LEDs stay lit like held notes.
  - While SHIFT is held, the KEY_16/KEY_17 key LEDs shall indicate the octave-shift state (see FR-5c).

## 5. Initial parameter defaults

| Parameter | Initial default |
|---|---|
| 8′ pipe volume | Step 24 / 32 (gain 0.5625) |
| 4′ pipe volume | Step 24 / 32 (gain 0.5625) |
| 2⅔′ pipe volume | Step 24 / 32 (gain 0.5625) |
| 2′ pipe volume | Step 24 / 32 (gain 0.5625) |
| Main volume | 25 % of maximum at boot; existing hardware-volume behaviour otherwise |
| Octave shift | 0 (not saved) |
| Polyphony | 6 voices |
| Pipe Character | Step 32 / 64 (saved in presets) |
| Pipe mute | Unmuted at boot (saved in presets, no MIDI CC) |
| Encoder page | Page 1 (volume) for all pipes (not saved) |
| Overdrive amount | 0 % (step 0, saved in presets) |
| Distortion tone | 100 % (saved in presets) |
| Rotary speaker | ~30 % (step 10, saved in presets) |
| Vibrato depth | 0 cents (saved in presets) |
| Wheel selection | Rotary (not saved) |
| Mode switch | Normal Mode (toggle down; not saved) |
| Pipe volume-modulation amounts | 0 % (step 0 / 32, saved in presets, no MIDI CC) |
| Pipe Character-modulation amounts | 0 % (step 0 / 32, saved in presets, no MIDI CC) |
| Modulation Mode encoder page | Modulation page 1 (volume modulation) for all pipes (not saved) |

The organ always boots on these defaults; SD-card presets (FR-0a) are read into the RAM cache at boot but apply only when a slot is loaded.

## 6. Acceptance criteria

The initial implementation is acceptable when:

1. All four registrations can sound simultaneously for a played note.
2. Each of the first four encoders independently changes the volume of its assigned registration.
3. Turning any of those encoders left lowers its registration volume and turning right raises it.
4. All four registration levels start equal (step 24) when no saved levels exist.
5. The fifth encoder on the right controls the overall organ volume.
6. The white and black keys play the expected chromatic notes.
7. Six notes can be held and heard simultaneously, with all four registrations available on each note.
8. Playing a seventh note follows the documented voice-stealing policy.
9. Note-on, note-off, and encoder changes do not produce unacceptable clicks or audio dropouts.
10. The instrument runs in real time on CHOMPI hardware without audio underruns under normal operation.
11. With the toggle switch up (Modulation Mode), the pipe encoders set the per-pipe volume/Character modulation amounts; at 0 % the sound is identical to Normal Mode, and with amounts raised each pipe's volume and Character drift slowly and independently within the documented windows without clicks.

## 7. Out of scope for the first implementation

The following features are potential future extensions, not requirements for the initial version:

- Vibrato or chorus.
- Reverb.
- Multiple preset banks (one bank of 15 slots is in scope, see FR-0a).
- Independent per-pipe tuning, modulation, or effects.

The initial architecture should avoid unnecessarily preventing these extensions.

## 8. Open questions

All open questions have been resolved:

1. ~~**Encoder interaction:**~~ Resolved — see FR-2 (no acceleration) and section 4 (LED level bar).
2. ~~**Parameter persistence:**~~ Resolved — see FR-0a (15 SD-card preset slots, explicit load/save).
3. ~~**Volume curve:**~~ Resolved for pipes — see FR-2 (32 steps, squared curve). Main volume follows existing hardware behaviour (FR-4).
4. ~~**Voice stealing:**~~ Resolved — see FR-6 (oldest note).
5. ~~**Envelope behaviour:**~~ Resolved — see DSP-3 (5 ms / 5 ms).
6. ~~**Mode integration:**~~ Resolved — see FR-0 (separate firmware).
7. ~~**Output gain:**~~ Resolved — see DSP-2 (fixed 1/24 scale).

---

## Implementation note

This specification describes desired behaviour, not a prescribed code structure. Before implementation, map keyboard events, encoder events, audio rendering, parameter storage, and mode selection onto the existing open-source CHOMPI firmware architecture.

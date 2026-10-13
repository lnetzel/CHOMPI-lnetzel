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

### FR-0a — SD card

An SD card is expected at runtime. It shall be used to save and load the four pipe levels and the four pipe waveform-morph settings. Main volume is not saved by the organ; it follows the existing hardware-volume behaviour (FR-4). The storage format shall be designed so that it can later hold multiple presets/registrations.

- **Save:** Settings shall be auto-saved a few seconds after the last encoder change. SD writes shall never interrupt audio.
- **Load:** Settings shall be loaded at boot. If the card or settings file is missing or unreadable, defaults (see section 5) shall be used and the instrument shall remain playable.

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

### FR-2b — Per-pipe encoder pages (volume / waveform)

Each of the four pipe encoders shall have two pages, toggled per pipe by pressing that pipe's encoder push-switch:

- **Page 1 (default at boot):** pipe volume (FR-2).
- **Page 2:** pipe waveform morph. Turning the encoder morphs that pipe's oscillator from sine (step 0, leftmost) to square (step 64, rightmost), with a smooth crossfade between the two waveforms. The crossfade is smoothstep-eased so the blend lingers near the pure waveforms (more usable sweet spots, especially near sine). The control range shall have 64 steps, one step per click, no acceleration.

- Pages are per pipe: each pipe's waveform is adjusted independently, and pipes can have different waveforms.
- Waveform changes shall apply to sounding notes immediately and without clicks.
- Waveform settings shall be auto-saved to SD and loaded at boot like pipe levels (FR-0a); the page state itself is not saved and always starts on page 1.
- While a pipe encoder is on page 2, its knob LED shall be blue (brightness tracking the morph position); on page 1 the knob LED is white as before.
- While a page-2 encoder is being turned, the white-key value bar shall be blue (leftmost white key = sine, rightmost = square), with the same ~0.5 s timeout as the volume bar.

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

- The distortion is a selectable-character saturation stage applied to the organ mix before the master volume, so the drive character does not depend on the listening level (see DSP-5). Three types are available: **Warm** (vintage-style warm soft clipping), **Fuzz** (aggressive fuzz saturation), and **BrokenSpeaker** (harsh speaker-like breakup with narrow presence).
- **SHIFT + `ENC_6` turn on page 2 selects the distortion type.** Turning right moves Warm → Fuzz → BrokenSpeaker, turning left moves back; the selection clamps at the ends and shall not wrap. While SHIFT is held on page 2, the key LEDs shall show the selection instead of any value bar or held notes: exactly five red white keys — KEY_1–KEY_5 for Warm, KEY_6–KEY_10 for Fuzz, KEY_11–KEY_15 for BrokenSpeaker — with all other key LEDs off. The selection shall be shown immediately when SHIFT is pressed on page 2, without requiring rotation, and continuously throughout the hold. Releasing SHIFT shall restore the (unchanged) distortion-amount bar for ~0.5 s.
- Selecting a type shall never change the amount, master volume, tuning or note state. The amount applies equally to all three types; step 0 is an exact dry bypass for every type.
- The distortion type defaults to Warm at every boot and is not saved. The distortion amount defaults to 0 % at boot and is not saved.
- While page 2 is active, the `ENC_6` knob LED shall be red, with brightness tracking the distortion amount (with a floor so the page stays visible at 0 %) — including while the type-selector overlay is shown.
- While the distortion amount is being turned (without SHIFT), the white-key value bar shall be uniformly red (no gradient), with the same ~0.5 s timeout as the volume bar.
- SHIFT + `ENC_6` fine-tune (FR-5a) is not available on page 2.

### FR-4c — Rotary speaker (transport wheel)

The transport wheel (`ENC_5`) shall control the amount of the rotary-speaker simulation (DSP-6):

- Turning right increases the rotary amount from 0 % (step 0, exact bypass) to 100 % (step 32); 32 steps, one step per click, no acceleration.
- The rotary amount defaults to 0 % at boot and is not saved.
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
- Holding SHIFT and turning `ENC_6` on volume-encoder page 1 shall fine-tune the whole instrument by ±50 cents, 1 cent per click. On page 2, SHIFT + `ENC_6` selects the distortion type instead (FR-4b); fine-tune is not available on the other pages. Without SHIFT on page 1, `ENC_6` remains main volume (FR-4).
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

  Incoming CC values 0–127 map to steps 0–32 (rounded). Turning a pipe encoder sends the matching CC with the step scaled to 0–127. Values changed by CC are auto-saved like encoder changes (FR-0a).

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
- Pressing LOOP while no keys are held shall arm the latch: subsequently played notes latch.
- Pressing LOOP again shall unlatch: each latched note whose key is not physically held shall be released (5 ms release per DSP-3). A latched note whose key is physically held down again at unlatch time keeps sounding and releases normally when the key is released.
- The latch applies only to physical keyboard keys; incoming MIDI Note Off messages shall always release their notes.
- MIDI note-out Note Off for a latched key shall be sent when the note actually stops (at unlatch), not at physical key release.
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

### DSP-5 — Distortion (selectable character)

The distortion is a saturation stage on the final organ mix, placed before the master-volume gain, so the drive character does not depend on the listening level. One of three types is active at a time (FR-4b); all three share the same 32-step drive amount `d` (0–1), and step 0 (0 %) is an exact dry bypass for every type:

- **Warm** (boot default): gentler pre-gain `g = 1 + 15 × d²` (1×–16×, quadratic feel) into a smooth asymmetric tanh saturation (a small input bias gives modestly different positive/negative knees, producing even harmonics), followed by a ~20 Hz DC blocker (the asymmetry creates DC) and a ~6 kHz one-pole low-pass for rounded highs. Post gain normalized around the 0.33 reference level.
- **Fuzz:** strong pre-gain `g = 1 + 127 × d²` (1×–128×, quadratic feel) into a symmetric bounded soft-knee clipper (near-hard saturation, |y| ≤ 2/3), followed by a ~10 kHz one-pole low-pass to moderate harsh highs. Same 0.33 reference normalization. No bit reduction, no intentional DC.
- **BrokenSpeaker:** aggressive hard clipping pre-gain `g = 1 + 199 × d²` (1×–200×, quadratic feel) into a symmetric hard clip (|y| ≤ 0.7), followed by ~400 Hz high-pass filtering (removes extreme lows) and a ~4 kHz one-pole low-pass to create a narrow vocal-like presence peak. Fully wet while amount > 0, settling to exact dry bypass at amount 0. This voicing simulates a narrow speaker-like bandpass with harsh breakup and resonant coloration.

Type changes shall crossfade the per-type outputs linearly over ~10 ms: a three-element nonnegative weight vector ramps toward the selected one-hot while always summing to one, and retargeting mid-fade continues from the current weights. Filter states of inactive types shall track the live signal, so switching exposes no stale tails. When the amount target returns to 0, all types shall transition wet→dry over ~10 ms and settle to an exact dry bypass with no residual filtered tail.

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

## 4. User interaction requirements

- The six encoders shall retain the control assignments specified in FR-2, FR-4, FR-4b and FR-4c while the organ instrument is active.
- Encoder direction shall be consistent: left decreases the relevant level; right increases it.
- Pipe volume changes shall take effect smoothly without audible clicks.
- The main volume control shall change loudness without altering the balance between the four registrations.
- LED feedback (CHOMPI has no display):
  - While a pipe encoder, the main-volume encoder or the transport wheel is being turned, the key LEDs shall show the value as a bar on the white keys only (leftmost white key = 0 %, rightmost white key = 100 %). Volume bars (page-1 pipes and main volume) shall color the lit keys by position: green for the first 50 %, orange 50–75 %, red above 75 %. The page-2 waveform bar is uniformly blue; the distortion-amount bar is uniformly red; the rotary bar (FR-4c) is uniformly purple; the vibrato bar (FR-4d) is uniformly pink. The bar disappears about 0.5 s after the last click.
  - While SHIFT is held on volume-encoder page 2, the key LEDs shall instead show the distortion-type selector (FR-4b) as the highest-priority keyboard display: exactly five red white keys (KEY_1–KEY_5 Warm, KEY_6–KEY_10 Fuzz, KEY_11–KEY_15 BrokenSpeaker) with all other key LEDs off, including the octave hints. Other encoder or MIDI value bars shall not replace the selector. On SHIFT release the distortion-amount bar shall reappear for ~0.5 s. Keys, octave controls and the latch shall remain functional underneath the overlay.
  - Pipe knob LEDs shall show the pipe's value: white for page 1 (volume), blue for page 2 (waveform morph). The `ENC_6` knob LED follows the position-gradient colors on page 1 (volume) and is red on page 2 (distortion, FR-4b), also while the type-selector overlay is shown.
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
| Pipe waveform | Sine (morph step 0) |
| Encoder page | Page 1 (volume) for all pipes (not saved) |
| Overdrive amount | 0 % (step 0, not saved) |
| Distortion type | Existing (not saved) |
| Rotary speaker | 0 % (step 0, not saved) |
| Vibrato depth | 0 cents (not saved) |
| Wheel selection | Rotary (not saved) |

Saved levels from the SD card (FR-0a) override these defaults at boot.

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

## 7. Out of scope for the first implementation

The following features are potential future extensions, not requirements for the initial version:

- Vibrato or chorus.
- Reverb.
- Multiple presets/registrations (saving the current levels is in scope, see FR-0a).
- Independent per-pipe tuning, modulation, or effects.

The initial architecture should avoid unnecessarily preventing these extensions.

## 8. Open questions

All open questions have been resolved:

1. ~~**Encoder interaction:**~~ Resolved — see FR-2 (no acceleration) and section 4 (LED level bar).
2. ~~**Parameter persistence:**~~ Resolved — see FR-0a (auto-save to SD, load at boot).
3. ~~**Volume curve:**~~ Resolved for pipes — see FR-2 (32 steps, squared curve). Main volume follows existing hardware behaviour (FR-4).
4. ~~**Voice stealing:**~~ Resolved — see FR-6 (oldest note).
5. ~~**Envelope behaviour:**~~ Resolved — see DSP-3 (5 ms / 5 ms).
6. ~~**Mode integration:**~~ Resolved — see FR-0 (separate firmware).
7. ~~**Output gain:**~~ Resolved — see DSP-2 (fixed 1/24 scale).

---

## Implementation note

This specification describes desired behaviour, not a prescribed code structure. Before implementation, map keyboard events, encoder events, audio rendering, parameter storage, and mode selection onto the existing open-source CHOMPI firmware architecture.

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

An SD card is expected at runtime. It shall be used to save and load the four pipe levels. Main volume is not saved by the organ; it follows the existing hardware-volume behaviour (FR-4). The storage format shall be designed so that it can later hold multiple presets/registrations.

- **Save:** Levels shall be auto-saved a few seconds after the last encoder change. SD writes shall never interrupt audio.
- **Load:** Levels shall be loaded at boot. If the card or settings file is missing or unreadable, defaults (see section 5) shall be used and the instrument shall remain playable.

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

`ENC_5` (the transport wheel) is not a pipe-volume control and has no function in v1. Encoder push-switches have no function in v1.

Control behaviour:

- Turning an encoder to the left shall lower that pipe's volume.
- Turning an encoder to the right shall raise that pipe's volume.
- Each pipe's volume shall be adjustable independently of the other pipes.
- The control range shall have 32 steps (clicks) from silence (step 0) to maximum (step 32).
- One click shall always change the level by exactly one step (no acceleration).
- The level shall map to gain with a squared curve: gain = (step / 32)². This makes each click sound roughly like an even step in loudness.

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

### FR-5 — Keyboard input

The CHOMPI white and black keys shall serve as the organ keyboard.

- Pressing a key shall start the corresponding note.
- Releasing a key shall end the note according to the selected envelope/release behaviour.
- White and black keys shall follow the intended chromatic keyboard mapping.
- The implementation shall support overlapping key presses up to the six-voice polyphony limit.

The lowest key shall play C3 (MIDI note 48, ≈130.8 Hz); keys ascend chromatically from there. Key event interfaces shall follow the existing CHOMPI firmware architecture.

### FR-5a — Tuning

- Tuning shall be 12-tone equal temperament with A4 = 440 Hz.
- Holding SHIFT and turning `ENC_6` shall fine-tune the whole instrument by ±50 cents, 1 cent per click. Without SHIFT, `ENC_6` remains main volume (FR-4).
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

  Incoming CC values 0–127 map to steps 0–32 (rounded). Turning a pipe encoder sends the matching CC with the step scaled to 0–127. Levels changed by CC are auto-saved like encoder changes (FR-0a).

### FR-5c — Octave shift

Like the CHOMPI-WAVE octave shift, but with a ±2 octave range:

- Holding SHIFT and pressing KEY_16 (C♯3) shall shift the keyboard down one octave per press; SHIFT + KEY_17 (D♯3) shall shift up one octave per press.
- The offset shall be clamped to ±2 octaves and shall reset to 0 at every boot (not saved).
- Octave shift shall transpose played keys and their MIDI note-out; incoming MIDI notes are not transposed.
- While SHIFT is held, the KEY_16/KEY_17 key LEDs shall indicate the active shift direction (dim orange = 1 octave, bright = 2 octaves).
- These two key presses shall not play notes while SHIFT is held; note-off tracking shall use the note that was started at note-on, so changing octave mid-note never leaves stuck notes.

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

The initial sound source may use sine-wave oscillators for all four registrations. The implementation should be structured so that other oscillator waveforms or waveform morphing can be added later without redesigning the voice architecture.

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

## 4. User interaction requirements

- The five encoders shall retain the control assignments specified in FR-2 and FR-4 while the organ instrument is active.
- Encoder direction shall be consistent: left decreases the relevant level; right increases it.
- Pipe volume changes shall take effect smoothly without audible clicks.
- The main volume control shall change loudness without altering the balance between the four registrations.
- LED feedback (CHOMPI has no display):
  - While a pipe encoder or the main-volume encoder is being turned, the key LEDs shall show the level as a bar on the white keys only (leftmost white key = 0 %, rightmost white key = 100 %; pipe bars use per-pipe colors, the volume bar is white). The bar disappears about 0.5 s after the last click.
  - Otherwise, key LEDs shall light up for currently held notes.
  - While SHIFT is held, the KEY_16/KEY_17 key LEDs shall indicate the octave-shift state (see FR-5c).

## 5. Initial parameter defaults

| Parameter | Initial default |
|---|---|
| 8′ pipe volume | Step 24 / 32 (gain 0.5625) |
| 4′ pipe volume | Step 24 / 32 (gain 0.5625) |
| 2⅔′ pipe volume | Step 24 / 32 (gain 0.5625) |
| 2′ pipe volume | Step 24 / 32 (gain 0.5625) |
| Main volume | Existing hardware-volume behaviour |
| Octave shift | 0 (not saved) |
| Polyphony | 6 voices |
| Pipe waveform | Sine |
| Overdrive | Not required in the first implementation |

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

- Waveform morphing (for example, sine to square).
- Organ overdrive/distortion inspired by the Yamaha YC series.
- Vibrato or chorus.
- Reverb.
- Multiple presets/registrations (saving the current levels is in scope, see FR-0a).
- Independent per-pipe tuning, modulation, or effects.
- Rotary-speaker simulation.

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

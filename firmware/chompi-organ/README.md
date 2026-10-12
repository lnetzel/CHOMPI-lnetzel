# CHOMPI Organ Emulator (chompi-organ)

Polyphonic organ firmware for the CHOMPI hardware: 6 voices x 4 pipe
registrations (8', 4', 2 2/3', 2') with per-pipe levels, per-pipe
tonewheel-style Character, overdrive, and a rotary-speaker stage.

## Controls

### Pipe encoders (physical knobs 1-4, left to right)

Each pipe encoder has two independent pages; press the encoder's push
switch to toggle that pipe between them (pages are per-pipe, not global):

- **Page 1 - pipe level** (white knob LED): 32 steps, gain = (step/32)^2.
  Boot default is step 16 (50%). The white-key bar shows the level with a
  green/orange/red position gradient.
- **Page 2 - Character** (blue knob LED, brightness tracks the amount with
  a floor so the page stays visible at 0): 64 steps of tonewheel-inspired
  harmonic coloration. Step 0 is the exact clean sine; step 64 is clearly
  aged but still organ-like. The white-key bar is uniformly blue.

Each pipe has its own Character amount, applied to that pipe across all
six voices; adjusting one pipe never changes the others' amounts or
spectra. Voicing varies subtly and deterministically by note and pipe
(+/-15% harmonic strength), so repeated pitches on a pipe sound identical
regardless of which voice slot renders them. Character is pickup/
tonewheel-*inspired* coloration - it is not an accurate physical Hammond
model and does not simulate neighboring-tonewheel leakage. Muted pipes
and finished envelopes stay silent; there is no idle background tone.

Character amounts boot at zero and are kept for the session (no SD-card
persistence).

### Transport wheel (ENC_5)

Rotary-speaker amount, 32 steps; 0 is an exact bypass. Purple white-key
bar while turning.

### Volume encoder (ENC_6)

Push toggles between master volume (page 1, gradient LED) and overdrive
amount (page 2, red LED with brightness floor; 0 is an exact bypass).
SHIFT + turn = fine tune (+/-50 cents). Boot default volume is 25%.

### Keys

25 playable keys (C3-C5). SHIFT + C#3/D#3 = octave down/up (+/-2).
KEY_28 (LOOP) toggles the key latch. KEY_26+27+28 held at power-on enters
shipping mode.

## MIDI (channel 1, DIN and USB)

| CC | Function |
| --- | --- |
| 70-73 | Pipe 1-4 levels (0-127 -> 0-32 steps, rounded) |
| 74-77 | Pipe 1-4 Character amounts (0-127 -> 0-64 steps, rounded) |

Notes are received and sent; CCs are both received and sent (outgoing CCs
reflect only the adjusted pipe).

**Semantic change:** CC 74-77 previously selected a sine->square waveform
morph. That morph is removed; old automation that selected "square"
(CC value 127) now selects maximum Character. The sine-only sound at
Character 0 is unchanged.

## Signal flow

sine -> Character shaping (per pipe) -> pipe gain -> voice envelope/
velocity -> voice mix -> overdrive -> rotary speaker -> master gain ->
outputs. Character 0 exactly bypasses shaping; the clean-sine engine is
bit-identical to the previous firmware's sine-only sound.

## Building

ARM GNU 10.3-2021.10 toolchain, BOOT_SRAM app type:

```sh
make -C firmware/chompi-organ/code/src DEBUG=0 OPT=-O3
```

Output: `code/src/build/CHOMPI_ORGAN_1.0.bin` (copy to the SD card root to
install; do not flash the bootloader).

## Tests

Native DSP tests for the Character shaper live in `code/tests/`:

```sh
make -C firmware/chompi-organ/code/tests test
```

They need only a host C++ compiler (no MCU/Daisy dependencies) and cover
step clamping/guards, exact clean endpoints, ramp convergence and
retargeting, voice-slot-independent voicing, cycle-mean DC, 2nd/3rd
harmonic content, amplitude bounds, and high-frequency anti-alias
suppression.

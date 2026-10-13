# CHOMPI Organ Emulator (chompi-organ)

Polyphonic organ firmware for the CHOMPI hardware: 6 voices x 4 pipe
registrations (8', 4', 2 2/3', 2') with per-pipe levels, per-pipe
tonewheel-style Character, selectable distortion (Warm/Fuzz/BrokenSpeaker),
a rotary-speaker stage, a compact vintage-1960s-inspired plate
reverb, and global 6 Hz pitch vibrato.

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

The wheel controls one of two effects; pressing the wheel toggles the
assignment (boots on rotary). The two LEDs at the wheel show the
selection steadily until the next toggle: left LED purple = rotary,
right LED pink = vibrato. Turning the wheel changes only the selected
effect and shows its white-key value bar for 500 ms.

- Rotary-speaker amount: 32 steps; 0 is an exact bypass. Purple bar.
- Vibrato depth: 0-50 cents peak deviation in one-cent steps from one
  shared, free-running 6 Hz LFO across all voices (depth 50 = one
  semitone peak-to-peak); 0 is an exact bypass and boots at 0. Pink bar.
  Not saved.

### Volume encoder (ENC_6)

Push cycles three pages: master volume (page 1, gradient LED) ->
distortion amount (page 2, red LED with brightness floor; 0 is an exact
bypass) -> plate-reverb mix (page 3, blue LED with brightness floor).
Turning or pressing the encoder shows the current page's white-key value
bar for 500 ms.

Page 2's distortion has three selectable characters: Warm (vintage soft
clipping), Fuzz (aggressive symmetric saturation), and BrokenSpeaker
(harsh speaker-like breakup with narrow presence). SHIFT + turn on page 2
moves Warm -> Fuzz -> BrokenSpeaker (clamped at the ends, no wrap); the
amount is not changed. While SHIFT is held on page 2, exactly five red
white-keys show the selection (KEY_1-5 Warm, KEY_6-10 Fuzz, KEY_11-15
BrokenSpeaker) and all other key LEDs are off; releasing SHIFT restores
the amount bar for 500 ms. The type boots to Warm and is not saved.

Page 3 adjusts the reverb mix by 1% per click, clamped to 0-100%: 0% is
exact dry and 100% is wet only (a linear complementary crossfade with a
~10 ms click-free ramp). The white-key bar is uniformly blue: no keys at
0%, half (8 keys) at 50%, all 15 at 100%.

SHIFT + turn on page 3 adjusts the plate decay: 100 log-spaced steps
from 0.5 s (boot default) to 20 s, shown on the same blue white-key bar
while turning. The wet level and tank headroom are normalized across the
range, so the decay changes the tail duration, not the loudness.

The plate is a fixed voicing apart from the decay (no tone/predelay
controls): a compact Griesinger/Dattorro tank adapted from chompi-wave's
reverb, voiced dark (~4.5 kHz damping) with subtle slow modulation and a
~120 Hz low-cut on the tank send. Plate-inspired; not an accurate
physical EMT emulation. The tank runs continuously, so tails ring out
naturally after note release and survive page changes; turning the
master volume to zero silences dry and wet alike (the reverb sits before
the master gain).

The mix and decay boot at 0% / 0.5 s and are kept for the session only
(no SD-card persistence, no MIDI CC).

SHIFT + turn = fine tune (+/-50 cents) on page 1 only (page 2 selects
the distortion type, page 3 adjusts decay). Boot default volume is 25%.

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

pitch vibrato (shared 6 Hz LFO at the oscillators) -> sine -> Character
shaping (per pipe) -> pipe gain -> voice envelope/velocity -> voice mix
-> distortion (Warm/Fuzz/BrokenSpeaker) -> rotary
speaker -> plate reverb -> master gain -> headphone + master stereo
output pairs. Character 0 exactly bypasses shaping; the clean-sine
engine is bit-identical to the previous firmware's sine-only sound.
Reverb 0% is bit-exact dry; vibrato depth 0 is an exact bypass.

## Building

ARM GNU 10.3-2021.10 toolchain, BOOT_SRAM app type:

```sh
make -C firmware/chompi-organ/code/src DEBUG=0 OPT=-O3
```

Output: `code/src/build/CHOMPI_ORGAN_1.0.bin` (copy to the SD card root to
install; do not flash the bootloader).

## Tests

Native DSP tests for the Character shaper and the plate reverb live in
`code/tests/`:

```sh
make -C firmware/chompi-organ/code/tests test
```

They need only a host C++ compiler (no MCU/Daisy dependencies). The
Character tests cover step clamping/guards, exact clean endpoints, ramp
convergence and retargeting, voice-slot-independent voicing, cycle-mean
DC, 2nd/3rd harmonic content, amplitude bounds, and high-frequency
anti-alias suppression. The reverb tests cover deterministic
initialization (garbage-prefilled delay buffer), bounded decaying
impulse response, mix clamping, ramp convergence/retargeting, bit-exact
dry at 0%, wet-only 100% against a reference instance, and long-silence
stability.

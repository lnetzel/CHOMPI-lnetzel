# CHOMPI Organ Emulator (chompi-organ)

Polyphonic organ firmware for the CHOMPI hardware: 6 voices x 4 pipe
registrations (8', 4', 2 2/3', 2') with per-pipe levels, per-pipe
tonewheel-style Character, fuzz distortion with a tone control,
a rotary-speaker stage, a compact vintage-1960s-inspired plate
reverb, global 6 Hz pitch vibrato, and a Modulation Mode that drifts
each pipe's volume and Character slowly and independently.

## Controls

The hardware toggle switch next to the SHIFT (CHOMPI) key selects the
mode: **DOWN = Normal Mode** (all controls below), **UP = Modulation
Mode** (per-pipe drift amounts, see the Modulation Mode section). The
switch only changes what the four pipe encoders turn/push and their
knob LEDs show; everything else behaves the same in both modes.

### Pipe encoders - Normal Mode (physical knobs 1-4, left to right)

Each pipe encoder has four independent pages; press the encoder's push
switch to cycle that pipe through them 1 -> 2 -> 3 -> 4 -> 1 (pages are
per-pipe, not global):

- **Page 1 - pipe level** (white knob LED): 32 steps, gain = (step/32)^2.
  Boot default is step 16 (50%). The white-key bar shows the level with a
  green/orange/red position gradient.
- **Page 2 - Character** (blue knob LED, brightness tracks the amount with
  a floor so the page stays visible at 0): 64 steps of tonewheel-inspired
  harmonic coloration. Step 0 is the exact clean sine; step 64 is clearly
  aged but still organ-like. The white-key bar is uniformly blue.
- **Page 3 - attack time** (purple knob LED with brightness floor): 32
  log-spaced steps from 5 ms (step 0, boot default - the original fixed
  timing) to 10 s (step 32). The white-key bar is uniformly purple.
- **Page 4 - release time** (yellow knob LED with brightness floor): the
  same 32-step 5 ms..10 s log mapping, applied to the fade-out after the
  key is released. The 5 ms floor keeps the release click-free. The
  white-key bar is uniformly yellow.

Each pipe has its own gate envelope, so pipes fade in and out at
different rates - e.g. let the 2' pipe swell up seconds after the 8'
fundamental speaks. A voice stays allocated until every pipe's release
has finished; voice stealing keeps the fixed 5 ms steal fade. Attack and
release boot at step 0 (5 ms) and are kept for the session (no SD-card
persistence, no MIDI CC).

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

SHIFT adds two pipe-control gestures on the pipe encoders:

- **SHIFT + press** toggles that pipe's mute (the page does not cycle).
  A muted pipe is silenced click-free (its level step is preserved and
  restored on unmute), and its knob LED goes solid red, overriding the
  page color. Mute is session-only, boots unmuted and has no MIDI CC;
  level CC 70-73 still edits a muted pipe's step, which stays silent
  until unmuted.
- **SHIFT + turn** sets the value for all four pipes at once, per the
  turned pipe's current page: page 1 sets all four levels to the same
  value, page 2 all four Character amounts, page 3 all four attack
  times, page 4 all four release times. The white-key bar shows the
  value all four pipes now share.

### Pipe encoders - Modulation Mode (toggle switch UP)

In Modulation Mode the organ drifts on its own: each pipe's volume and
Character slowly and independently wander around their Normal Mode
settings, giving the organ a living, organic movement. The four pipe
encoders set the per-pipe modulation AMOUNTS on two pages; press the
encoder's push switch to toggle that pipe between them (per-pipe,
separate from the Normal Mode pages, which are preserved):

- **Mod page 1 - volume modulation amount** (white knob LED): 32
  steps, 0-100% (~3% per click), clamped, no wrap. The white-key bar is
  uniformly white.
- **Mod page 2 - Character modulation amount** (blue knob LED): 32
  steps, same mapping. The white-key bar is uniformly blue.

Knob LED brightness = 0.2 + 0.8 x amount (the floor keeps the page
visible at 0%); a muted pipe's knob LED stays solid red. SHIFT + turn
applies the delta to the turned pipe and broadcasts the resulting
value to all four pipes, like in Normal Mode. SHIFT + press still
toggles the pipe's mute in both modes.

The modulation works in each parameter's normalized domain: volume
x = level/32 (the squared gain curve is applied to the modulated
value), Character a = step/64 (before its smoothstep easing). The
amount scales a window around the user value: at 100% the window is
+/-20% (40% total); the window slides - never clips - to stay inside
0-100%, so a 95% center swings 60-100% and a 0% center swings 0-40%
(a silent pipe becomes audible). A muted pipe stays silent. Amount 0
is exactly the unmodulated sound.

Each of the eight modulators (4 pipes x volume/Character) is a slow,
free-running raised-cosine swing: every half-cycle draws its own
random 2.5-10 s duration, so a full bottom-to-top-to-bottom cycle
takes 5-20 s, is asymmetric, and never repeats in lockstep with the
other pipes. The modulators run continuously regardless of mode or
amount - flipping the switch or turning an amount never restarts
them; the modulation amounts only scale how much of the ongoing drift
reaches the sound.

All modulation amounts boot at 0 (the organ sounds exactly like Normal
Mode until amounts are raised) and are session-only: no SD-card
persistence, no MIDI CC. The Normal Mode bars and MIDI CCs keep
showing the un-modulated center values.

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

Page 2's distortion is a fuzz with a tone control. SHIFT + turn on
page 2 changes the tone by 1 percentage point per click, clamped to
0-100% (no wrap); the amount is not changed. The tone sweeps the fuzz's
post low-pass logarithmically from 500 Hz (0%, dark) to the original
10 kHz (100%, boot default - no additional darkening, the historical
fuzz sound); counterclockwise darkens, and 0% is not a mute. Tone
changes are smoothed click-free (~10 ms). While SHIFT is held on page 2
the tone shows immediately as a red bar across the 15 white keys (none
lit at 0%, eight at 50%, all fifteen at 100%) and all other key LEDs
are off; releasing SHIFT restores the amount bar for 500 ms. The tone
boots to 100% and is kept for the session only (no SD-card persistence,
no MIDI CC).

To keep the perceived loudness even at any distortion amount, an
automatic output leveling trims the fuzzed signal after the rotary and
before the plate reverb, so the leveled level also feeds the reverb. The
fuzz is a hard limiter - its output level is nearly constant at high
drive - so a fixed trim cannot match every registration: instead, the
dry and fuzzed signal levels are measured per audio block, smoothed over
~50 ms, and the fuzzed level is matched down to the dry level (never
boosted, never more than -30 dB, exactly unity for the clean sound).
Quiet single notes get a deep trim, loud full chords get barely any;
clean and distorted sounds keep the same power. The trim is folded
through the fuzz's own wet/bypass fade, so it ramps click-free and is
exactly unity at settled bypass; it introduces no user control, and the
main-volume value and display are unaffected.

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

SHIFT + turn = fine tune (+/-50 cents) on page 1 only (page 2 adjusts
the distortion tone, page 3 adjusts decay). Boot default volume is 25%.

### Keys

25 playable keys (C3-C5). SHIFT + C#3/D#3 = octave down/up (+/-2).
KEY_28 (LOOP) toggles the key latch; while latched, re-pressing a key
releases its note. KEY_26+27+28 held at power-on enters shipping mode.

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
-> fuzz distortion (tone-controlled) -> rotary
speaker -> plate reverb -> master gain (with the automatic fixed fuzz
output trim) -> headphone + master stereo
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
stability. The distortion tests cover the fuzz's tone control (endpoints,
monotonic darkening, click-free ramps), a bit-exact regression against
the previous Fuzz at tone 100%, exact dry bypass at every tone, and the
input-referenced fuzz output leveling (dry-matched loudness, -30 dB
floor). The drift-modulator tests cover the sliding-window math
(including the amount-0 exact-center guarantee), the 2.5-10 s random
half durations and 5-20 s cycles, per-block continuity, modulator
independence across the 8 seeds, and fixed-seed determinism at 48 kHz
and 32 kHz.

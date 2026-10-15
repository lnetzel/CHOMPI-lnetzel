# CHOMPI Organ Emulator (chompi-organ)

Polyphonic organ firmware for the CHOMPI hardware: 6 voices x 4 pipe
registrations (8', 4', 2 2/3', 2') with per-pipe levels, per-pipe
tonewheel-style Character, fuzz distortion with a tone control,
a rotary-speaker stage, a compact vintage-1960s-inspired plate
reverb, global 6 Hz pitch vibrato, a Modulation Mode that drifts
each pipe's volume and Character slowly and independently, and 15
SD-card preset slots.

## Controls

The hardware toggle switch next to the SHIFT (CHOMPI) key selects the
mode: **DOWN = Normal Mode** (all controls below), **UP = Modulation
Mode** (shared drift-amount destination, see the Modulation Mode
section). The switch changes what the four pipe encoders turn/push and
what their knob LEDs show, and Modulation Mode adds its own SHIFT key
gestures; preset management and octave selection are Normal-Mode-only.
Everything else behaves the same in both modes.

### Pipe encoders - Normal Mode (physical knobs 1-4, left to right)

Each pipe encoder has two independent pages; press the encoder's push
switch to toggle that pipe between them 1 -> 2 -> 1 (pages are
per-pipe, not global, and are preserved across mode changes):

- **Page 1 - pipe level** (white knob LED): 32 steps, gain = (step/32)^2.
  Boot default is step 16 (50%). The white-key bar shows the level with a
  green/orange/red position gradient.
- **Page 2 - Character** (blue knob LED, brightness tracks the amount with
  a floor so the page stays visible at 0): 64 steps of tonewheel-inspired
  harmonic coloration. Step 0 is the exact clean sine; step 64 is clearly
  aged but still organ-like. The white-key bar is uniformly blue.

The attack and release times are edited in Modulation Mode: select the
attack or release destination there (see the Modulation Mode section)
and the pipe encoders edit the actual envelope times - 32 log-spaced
steps from 5 ms (step 0, boot default - the original fixed timing) to
10 s (step 32); the 5 ms release floor keeps the release click-free.
The white-key bars are uniformly purple (attack) / yellow (release).

Each pipe has its own gate envelope, so pipes fade in and out at
different rates - e.g. let the 2' pipe swell up seconds after the 8'
fundamental speaks. A voice stays allocated until every pipe's release
has finished; voice stealing keeps the fixed 5 ms steal fade. Attack and
release boot at step 0 (5 ms), are stored in the SD-card presets (see
the Presets section) and have no MIDI CC.

Each pipe has its own Character amount, applied to that pipe across all
six voices; adjusting one pipe never changes the others' amounts or
spectra. Voicing varies subtly and deterministically by note and pipe
(+/-15% harmonic strength), so repeated pitches on a pipe sound identical
regardless of which voice slot renders them. Character is pickup/
tonewheel-*inspired* coloration - it is not an accurate physical Hammond
model and does not simulate neighboring-tonewheel leakage. Muted pipes
and finished envelopes stay silent; there is no idle background tone.

Character amounts boot at step 0 (the exact clean sine) and are stored
in the SD-card presets (see the Presets section).

SHIFT adds two pipe-control gestures on the pipe encoders:

- **SHIFT + press** toggles that pipe's mute (the page does not cycle).
  A muted pipe is silenced click-free (its level step is preserved and
  restored on unmute), and its knob LED goes solid red, overriding the
  page color. Mute boots unmuted, is stored in the SD-card presets and
  has no MIDI CC; level CC 70-73 still edits a muted pipe's step, which
  stays silent until unmuted.
- **SHIFT + turn** sets the value for all four pipes at once, per the
  turned pipe's current page: page 1 sets all four levels to the same
  value, page 2 all four Character amounts. The white-key bar shows the
  value all four pipes now share.

### Pipe encoders - Modulation Mode (toggle switch UP)

In Modulation Mode the organ drifts on its own: each pipe's volume and
Character slowly and independently wander around their Normal Mode
settings, giving the organ a living, organic movement. The four pipe
encoders all edit ONE shared destination, selected with SHIFT + a
physical black key (the black keys are numbered left to right):

- **Black key 1 (C#3) - attack time** (purple): the actual pipe attack
  envelope time, 32 log-spaced steps from 5 ms to 10 s.
- **Black key 2 (D#3) - release time** (yellow): the actual pipe
  release envelope time, the same 32-step 5 ms..10 s log mapping.
- **Black key 3 (F#3) - volume modulation amount** (white): 32 steps,
  0-100% (~3% per click), clamped, no wrap.
- **Black key 4 (G#3) - Character modulation amount** (blue): 32
  steps, same mapping.

Black key 5 onward selects nothing. Volume modulation is selected at
boot; the selection is remembered across mode changes until reboot and
is not stored in the presets.

The four destination keys are always lit in Modulation Mode - the
selected key at 100% of its destination color, the other three at
exactly 75% - with or without SHIFT held, during value bars and during
the distortion-tone display; every other black key stays dark, even
while its key plays a note. A turn adjusts the selected parameter for
that pipe and shows the matching white-key value bar (purple, yellow,
white or blue) for 500 ms. The knob LEDs use the selected destination's
color with brightness = 0.2 + 0.8 x that pipe's value (the floor keeps
the encoder visible at 0%); a muted pipe's knob LED stays solid red.
Unshifted encoder presses do nothing in Modulation Mode. SHIFT + turn
applies the delta to the turned pipe and broadcasts the resulting value
to all four pipes, like in Normal Mode. SHIFT + press still toggles the
pipe's mute in both modes.

With SHIFT held in Modulation Mode the white keys still play notes
(preset load/save and octave changes are Normal-Mode-only); without
SHIFT all 25 keys play as usual.

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
Mode until amounts are raised) and are stored in the SD-card presets;
there is no MIDI CC for them. The Normal Mode bars and MIDI CCs keep
showing the un-modulated center values.

### Transport wheel (ENC_5)

The wheel controls one of two effects; pressing the wheel toggles the
assignment (boots on rotary). The two LEDs at the wheel show the
selection steadily until the next toggle: left LED purple = rotary,
right LED pink = vibrato. Turning the wheel changes only the selected
effect and shows its white-key value bar for 500 ms.

- Rotary-speaker amount: 32 steps; 0 is an exact bypass and boots at
  0. Purple bar.
- Vibrato depth: 0-50 cents peak deviation in one-cent steps from one
  shared, free-running 6 Hz LFO across all voices (depth 50 = one
  semitone peak-to-peak); 0 is an exact bypass and boots at 0. Pink bar.
  The depth is stored in the SD-card presets.

### Volume encoder (ENC_6)

Push cycles three pages: master volume (page 1, gradient LED) ->
distortion amount (page 2, red LED with brightness floor; 0 is an exact
bypass) -> plate-reverb mix (page 3, blue LED with brightness floor).
Turning or pressing the encoder shows the current page's white-key value
bar for 500 ms.

Page 2's distortion is a fuzz with a tone control. SHIFT + turn on
page 2 changes the tone by 1 percentage point per click, clamped to
0-100% (no wrap); the amount is not changed. The tone sweeps the
cutoff of the fuzz's post low-pass - two identical cascaded one-pole
stages with an approximately 12 dB/octave stopband roll-off -
logarithmically from 500 Hz (0%, dark) to 10 kHz (100%, boot default).
The mapped frequency is the per-stage cutoff; the combined response's
-3 dB point sits lower, and 100% is intentionally a little darker than
the previous single-pole fuzz. Counterclockwise darkens, and 0% is not
a mute. Tone changes are smoothed click-free (~10 ms). While SHIFT +
turning, and for 500 ms after the last click, the tone shows as a red
bar across the 15 white keys (none lit at 0%, eight at 50%, all
fifteen at 100%); the tone bar takes priority over any pipe value bar
while it is active, and in Modulation Mode the destination-key LEDs
stay visible. Holding SHIFT on page 2 in Normal Mode shows the preset
slot overview like on any other page. The tone boots to 100% and is
stored in the SD-card presets; it has no MIDI CC.

To keep the perceived loudness even at any distortion amount, an
automatic output leveling trims the fuzzed signal after the rotary and
before the plate reverb, so the leveled level also feeds the reverb. The
fuzz is a hard limiter - its output level is nearly constant at high
drive - so a fixed trim cannot match every registration: instead, the
dry and fuzzed signal levels are measured per audio block, smoothed over
~50 ms, and the fuzzed level is matched down to just above the dry
level (+1.5 dB headroom, so the damping reduces that much less; never
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
from 0.2 s (boot default) to 20 s, shown on the same blue white-key bar
while turning. The wet level and tank headroom are normalized across the
range, so the decay changes the tail duration, not the loudness.

The plate is a fixed voicing apart from the decay (no tone/predelay
controls): a stereo-input Griesinger/Dattorro tank adapted from
chompi-wave's reverb. Each channel gets its own send and allpass
diffuser chain (different lengths per channel) and excites its own side
of the tank, so the rotary speaker's left/right movement survives in
the reverb; the tank's decay-scaled cross-feedback then progressively
couples the two sides into one diffuse field (well separated at short
decays, blended at long ones). Voiced dark (~4.5 kHz damping) with
subtle slow modulation and a ~120 Hz low-cut per send. Plate-inspired;
not an accurate physical EMT emulation. The tank runs continuously, so
tails ring out naturally after note release and survive page changes;
turning the master volume to zero silences dry and wet alike (the
reverb sits before the master gain).

The mix and decay boot at 0% / 0.2 s and are stored in the SD-card
presets; they have no MIDI CC.

SHIFT + turn = fine tune (+/-50 cents) on page 1 only (page 2 adjusts
the distortion tone, page 3 adjusts decay). The fine-tune bar centers
on white key 8: exactly centered is that key alone in green, and any
offset spans red from the center toward the offset (left = down in
pitch, right = up; all seven keys on that side at +/-50 cents). Boot
default volume is 25%.

### Keys

25 playable keys (C3-C5). SHIFT + C#3/D#3 = octave down/up (+/-2,
Normal Mode only - the octave never changes while the switch is UP).
KEY_28 (LOOP) toggles the key latch; while latched, re-pressing a key
releases its note. SHIFT + LOOP does not toggle the latch - it starts
the init-patch hold (see the Presets section). KEY_26+27+28 held at
power-on enters shipping mode. With SHIFT held in Normal Mode the
white keys become the 15 preset slots (see the Presets section); in
Modulation Mode SHIFT + a black key selects the modulation destination
and the white keys keep playing.

## SD-card presets

The organ keeps 15 preset slots on the SD card (folder `ORGAN/`, one
small `P01.json` .. `P15.json` file per slot, ~300 B, human-readable).
All 15 slots are cached in RAM: loading, saving, copying and deleting
act on the cache instantly and never wait for the card, and the
background writer does one small step per main-loop pass, so nothing
freezes or glitches the audio. A save is written as a `.tmp` file that
is then renamed over the old file, so a power loss mid-save can corrupt
at most the slot being written. The organ always boots on its factory
defaults; a preset is only applied when you load it.

Gestures (SHIFT = the CHOMPI key). Load, save, copy, delete and their
confirmations are Normal-Mode-only: entering Modulation Mode cancels
any unconfirmed selection, while an already-confirmed write finishes
in the background without taking over the Modulation-mode LEDs.

- **Load:** hold SHIFT and press a white key (C3-C5 = slots 1-15). A
  filled slot applies instantly and becomes the current slot; an empty
  slot does nothing. Held and latched notes keep sounding - every
  change is ramped or phase-continuous.
- **Save:** SHIFT + KEY_25 enters save select (pressing KEY_25 again
  cancels). Press any white key to choose the slot, then release and
  press SHIFT to confirm. The current engine state is captured, the
  SAVING animation shows for ~1 s and the slot becomes the current slot.
- **Copy:** SHIFT + KEY_24 enters copy select (pressing KEY_24 again
  cancels). Press a filled source slot (it turns green), then a
  different destination slot, then release and press SHIFT to confirm.
- **Delete:** SHIFT + KEY_23 enters erase select (pressing KEY_23 again
  cancels). Press a filled slot, then release and press SHIFT to
  confirm. Deleting the current slot clears "current".
- **Init:** hold SHIFT, then press LOOP and keep both held for 2.5 s.
  A red bar fills the 15 white keys while you hold; releasing either
  key early cancels. At 2.5 s the init sound is applied live (held and
  latched notes keep sounding) and the CHOMPI key LED blinks white for
  ~1 s. Init needs no SD card. Init values: pipe levels 16 (50%),
  Character 0 (clean sine), attack and release 0 (5 ms), mutes off,
  volume/Character modulation 0, vibrato, rotary and drive 0,
  distortion tone 100%, reverb mix 0, reverb decay 0 (0.2 s) - the
  same sound as power-on. Not touched: master volume, fine tune,
  octave, latch state, page selections and the wheel mode. After init
  no slot is "current" (no white slot in the SHIFT slot view).

While a selection mode is active the keys select slots only (no notes,
no latch changes); notes held before entering keep sounding and release
normally, and the encoders, wheel and MIDI keep working throughout.

LEDs, while SHIFT is held or a selection mode is active:

- The 15 white keys are the 15 slots: filled = purple (solid in save
  select / copy-destination select, blinking dim in erase select /
  copy-source select), the current slot = white (solid in save /
  copy-destination select, blinking in erase / copy-source select),
  empty slots blink dim grey in save select / copy-destination select
  and stay off in erase select / copy-source select, the selected slot
  = blue (save / copy destination) or red (erase target), the copy
  source = green.
- The three top black keys show the operations: delete (KEY_23) = red,
  copy (KEY_24) = green, save (KEY_25) = blue; during an operation only
  that operation's LED is lit.
- The CHOMPI key LED is purple while SHIFT is held, blinks red once a
  slot is selected, and blinks white during the ~1 s SAVING / COPYING /
  ERASING animations.

With no SD card the white keys show red while SHIFT is held and all
preset gestures are inert; the organ stays fully playable. Removing the
card mid-session disables the presets; re-inserting it re-mounts the
card and fills still-empty, untouched slots from it. A card pulled
mid-write can leave a stray `.tmp` file, which is ignored.

Saved in every preset: per-pipe level, Character, attack, release, mute
and volume/Character-modulation amounts, plus vibrato depth, rotary
amount, fuzz amount and tone, and reverb mix and decay. NOT saved:
master volume, fine tune, octave shift, latch, page selections, the
Modulation-mode destination and the wheel's rotary/vibrato selection.

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

## Building on Windows

You need:

- **MSYS2** (https://www.msys2.org) for a shell and GNU Make.
- **Arm GNU Toolchain 10.3-2021.10** (`arm-none-eabi-gcc` 10.3.1), installed to
  `C:\armgnu\10.3-2021.10`. Use exactly this version: the prebuilt libraries
  and the SRAM budget are tuned to it, and newer compilers can overflow SRAM.
- The `chompi-wave` folder next to this one. This project reuses its libDaisy
  and DaisySP libraries and coreJSON sources (via relative paths), plus the
  reverb's `fx_engine.h`.

Open an **MSYS2 UCRT64** shell, then install Make once:

```sh
pacman -S --needed make
```

In each new shell, put the toolchain on the `PATH`, then build from
`code/src`:

```sh
export PATH="/c/armgnu/10.3-2021.10/bin:$PATH"
cd /path/to/firmware/chompi-organ/code/src
make check-toolchain   # prints GCC 10.3.1 if the toolchain is right
make -j4 DEBUG=0 OPT=-O3
```

The output is `code/src/build/CHOMPI_ORGAN_1.0.bin` (BOOT_SRAM app type). The
build links the prebuilt `../chompi-wave/code/libs/libDaisy/build/libdaisy.a`
and `../chompi-wave/code/libs/DaisySP/build/libdaisysp.a`; if those are
missing, run `make` once in `../chompi-wave/code/libs/libDaisy` and
`../chompi-wave/code/libs/DaisySP` first.

To install it, copy `CHOMPI_ORGAN_1.0.bin` to the root of the SD card
(deleting any other `.bin`), put the card in CHOMPI and power on; do not
flash the bootloader. If the board has never had the CHOMPI bootloader,
install it first as described in the
[bootloader guide](../chompi-bootloader-v6.4-beta/README.md).

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
dry at 0%, wet-only 100% against a reference instance, long-silence
stability, and the stereo tank behavior (distinguishable L/R paths with
decay-scaled cross-coupling, decorrelation of identical L/R content,
approximate L/R symmetry, and bounded anti-phase response). The
distortion tests cover the fuzz's tone control (endpoints,
monotonic darkening, click-free ramps), a bit-exact regression of the
two-stage low-pass cascade against an independent reference
implementation, the measured frequency response against the analytical
digital cascade magnitude (2-4 kHz roll-off at 48 kHz and 32 kHz),
exact dry bypass at every tone, and the input-referenced fuzz output
leveling (dry-matched loudness with +1.5 dB headroom, -30 dB floor). The drift-modulator tests cover the sliding-window math
(including the amount-0 exact-center guarantee), the 2.5-10 s random
half durations and 5-20 s cycles, per-block continuity, modulator
independence across the 8 seeds, and fixed-seed determinism at 48 kHz
and 32 kHz. The preset tests cover the versioned JSON round-trip of
every field, defaults for missing keys, clamping of out-of-range and
garbage values, version tolerance, the 15-slot bank's save/copy/erase
and dirty tracking, engine capture/apply against a fake engine, and
the init-patch values. The Modulation-mode UI tests cover the physical
black-key destination mapping, the boot default and remembered
selection, the exact 75%/100% destination-key intensities and colors,
the two-page Normal-mode cycling, the encoder press behavior, and the
SHIFT broadcast clamping.

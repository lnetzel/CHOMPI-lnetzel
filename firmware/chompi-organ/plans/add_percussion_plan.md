# CHOMPI-organ: CPU-conscious Hammond-style percussion

## Problem and approach

Add the decaying harmonic attack associated with Hammond percussion, toggled by the Play/Pause key beside Overdub. Preserve six voices and four oscillators per voice: the existing engine is already close to its practical CPU limit.

Reuse each sounding voice's raw third pipe sine (pipe index 2), before Character, pipe gain and pipe envelopes. Mix this through one shared percussion decay, with inexpensive per-voice click-prevention gates. No additional oscillator, voice, audio buffer, or per-sample transcendental calculation is needed.

This is Hammond-inspired percussion, not a complete B3 circuit emulation.

## Research

- [HammondWiki: Percussion](https://www.dairiki.org/HammondWiki/Percussion) explains the added decaying second/third harmonic and single-trigger operation: only a new note from an all-keys-up state retriggers the envelope.
- [Electric Druid: Technical aspects of the Hammond organ](https://electricdruid.net/technical-aspects-of-the-hammond-organ/) provides tonewheel/contact context, including the historical drawbar contact sacrificed for percussion.
- The current engine already generates a 19-semitone pipe at 2.9966142 times the fundamental. Reusing this instead of an exact 3x harmonic is an explicitly accepted tuning/CPU trade-off.
- The real instrument's lost top drawbar is a wiring consequence, not essential to the percussion effect. Do not remove or mute any existing CHOMPI pipe.

## Confirmed scope and behavior

- Classic single triggering, not separate envelopes for every note.
- One fixed, fast, third-harmonic-like sound; no harmonic selector, decay controls, or new MIDI CC.
- Play/Pause is hardware KEY_27, transport LED PTH 7. Toggle once on a press edge in Normal and Modulation modes; holding or releasing the button does not toggle again. Use a solid amber indicator while enabled, dark while disabled.
- Keep Overdub/latch, SHIFT gestures, preset selection, and the existing boot recovery chord intact. A key held at startup must not toggle percussion.
- Save on/off in presets. Boot, init patch, and older presets without the field default to off.
- Percussion is independent of pipe levels, mutes, Character, modulation amounts and long pipe attack/release settings. It still follows note pitch, fine tuning, foldback, vibrato, velocity and master gain, and passes through the existing fuzz/rotary/reverb chain.
- Logical held notes govern rearming, including MIDI and latched notes. Releasing physical keys while latch sustains them does not rearm.
- Toggling on while notes are already held does not synthesize a new attack. Wait for an all-notes-released transition and the next note-on.
- Subsequent legato notes do not restart the decay; they may share its remaining tail. Chord notes share one decay rather than restarting it per voice.
- Release tails and voice allocation do not determine rearming. Held pitches that have lost a voice through stealing still count as held.
- Use short smooth transitions for enabling/disabling, releases, and steals. Disabled, settled audio preserves the existing rendering and normalization exactly.
- Keep existing pitch-based note ownership semantics; redesigning overlapping MIDI/local ownership of the same pitch is out of scope.

## Sound and headroom specification

Initial fixed sound constants, to be checked by rendering and listening:

- Peak percussion amplitude: 0.5 of a full-scale single pipe before existing mix scaling.
- Attack smoothing: approximately 1 ms.
- Exponential decay: 60 ms half-life, approximately -60 dB after 600 ms; explicitly settle to zero to avoid denormals and needless residual work.
- Independent note-release and toggle/steal fades: approximately 5 ms.
- Compute sample-rate-dependent coefficients only during initialization, not inside the sample/voice loop.

With four bounded pipes plus 0.5 percussion per voice, the conservative six-voice sum bound rises from 24 to 27. Smoothly interpolate the enabled normalization from 1/24 to 1/27 with the percussion enable transition; restore exactly 1/24 at settled off. This is approximately 1 dB of dry attenuation when enabled. Coordinate gain and percussion ramps so intermediate states also respect the bound.

These sound constants are implementation defaults, not claims about measured original Hammond circuit timings.

## Implementation todos

1. **Implement shared percussion DSP and voice integration.**
   - Add a small dependency-free `Percussion.h` helper, following existing host-testable DSP helpers, for shared decay, enable transitions, and minimal reusable gate logic.
   - In `code/src/OrganEngine.h`, expose enable getter/setter, initialize off, and trigger from the held-note transition before setting the note held.
   - Reuse existing held-note bookkeeping. Avoid scanning 128 pitches per audio sample; do bookkeeping on note events, with duplicate note-on/off handling.
   - Capture pipe 2's raw sample before Character and mix the percussion separately from the pipe envelope.
   - Advance the shared envelope once per sample, never once per voice.
   - Handle release, same-note retrigger, all-keys-up/new phrase during long release tails, delayed stolen-note starts, cancellation of pending notes, and voice reuse. Old release-tail voices must not gain a fresh attack when another note starts.
   - Use exact-zero/bypass paths when disabled or decayed. No added work proportional to a new oscillator bank.
2. **Wire the hardware toggle and LED.**
   - In `code/src/chompi_main.cpp`, add KEY_27 edge handling outside playable-key routing and reflect engine state on PTH LED 7.
   - Preserve startup edge baselining, the boot recovery chord, both switch modes, preset gestures, and existing transport behavior.
   - Keep engine mutation in the existing audio/control context; LED reads follow existing state-display conventions.
3. **Extend preset persistence and compatibility.**
   - In `code/src/OrganPreset.h`, add numeric 0/1 `percussion` with default 0 using existing serialization, parsing/clamping, capture and apply patterns.
   - Keep the optional-field-compatible preset version; missing keys retain off.
   - Check serialized maximum size against both documented caller buffers and `PresetStorage::kJsonSize`; adjust tightly coupled buffer-size documentation only if needed.
   - Extend `code/tests/organ_preset_test.cpp`, including its fake engine and equality helpers, for round-trip, defaults, init, clamping, old JSON and capture/apply.
4. **Add focused DSP/integration regression tests.**
   - Add `code/tests/percussion_test.cpp` and register it in the existing test Makefile and clean target.
   - Test sample-rate-independent half-life/endpoints, finite output, shared-envelope advancement, all-notes-released rearming, duplicate events, chord/legato behavior, enabling under held notes, and off/on transitions.
   - Cover gate behavior for long pipe envelopes, release tails, retriggers, steals and cancelled pending notes. Exercise actual engine integration where the native dependency setup permits; do not treat helper-only tests as proof of engine routing.
   - Check the intended harmonic in a rendered output and independence from Character/pipe controls.
   - Verify the analytical 27-unit mix bound and transition behavior, and exact settled-off equivalence against the original arithmetic path.
5. **Document and validate.**
   - Update `firmware/chompi-organ/README.md`: toggle/LED, single-trigger articulation, latch/MIDI interaction, fixed sound, preset compatibility, signal flow, headroom and CPU trade-off.
   - Run focused percussion and preset tests first, then the existing native organ test target as regression coverage.
   - Load the build-firmware skill before compiling. Compile the organ using its pinned Windows/MSYS2/Arm GCC 10.3.1 workflow; also verify the CPU-meter configuration compiles.
   - Review the diff for unrelated changes and unintended voice-count/oscillator changes.

## Verification boundaries and hardware acceptance

A host test or successful firmware build cannot establish MCU audio deadline safety.

On hardware, compare the existing CPU meter with percussion off/on under six-note chords, rapid all-up retriggering, voice stealing, maximum Character/drift/vibrato, fuzz, rotary and reverb. Record peak CPU and listen for glitches, watchdog resets and clicks. Use the existing meter's >85% red threshold as a warning, not a proof of safety: the feature must leave reliable deadline margin, including event-heavy blocks. If the target board cannot be measured during implementation, explicitly report CPU/audio acceptance as pending rather than claim performance safety.

Listening acceptance: an audible short harmonic ping after a full release, no fresh attack during legato/latch sustain, correct Play/Pause indication, and no unwanted cut to existing pipe release tails. Confirm preset recall/init behavior and startup with the Play/Pause key held.

No repository files are changed in this planning phase. Do not flash hardware, commit changes, or publish a release unless separately requested.

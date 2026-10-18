# Plan: CHOMPI-PIPES interactive MIDI regression tool

A single Windows `.exe` built from Python with `mido`, `python-rtmidi` and PyInstaller `--onefile`. It finds the CHOMPI over USB MIDI and runs through a list of tests by asking the user questions. For each test the user answers **y / n / r / s**: y = yes, n = no (the tool then asks for an optional comment), r = replay, s = skip. Tests that check MIDI coming *out* of PIPES are scored automatically. At the end the tool writes a timestamped `.md` report.

## What the firmware actually supports (checked in the code)

- It only listens on **channel 1**. Messages on other channels are ignored. See [MidiHandler.h](../code/src/MidiHandler.h).
- **IN:**
  - Note On and Note Off. A Note On with velocity 0 also stops the note, and velocity sets the loudness.
  - **CC 70–73** set the levels of the 4 pipes (8′, 4′, 2⅔′, 2′). The 0–127 value is turned into 0–32 steps with `(v*32+63)/127`.
  - **CC 74–77** set each pipe's Character, mapped to 0–64 steps.
  - Everything else is ignored: Program Change, Pitch Bend, Aftertouch, SysEx, Clock, CC64 and CC123.
  - When a level or Character CC comes in, PIPES shows a level bar on screen (`CcLevelCallback`). It does not send that CC back out.
- **OUT:**
  - Pressing a key sends a note at fixed velocity 100. The octave shift is applied, and with latch on the Note Off is held back until unlatch.
  - The encoders send CC 70–77 (level page and Character page). With SHIFT, one turn sends the CC for all 4 pipes.
  - The attack and release pages send no CC.
  - All of this is in [chompi_main.cpp](../code/src/chompi_main.cpp) (key handling ~L395–465, sending in the main loop ~L985–1005).
- **Voices and controls:**
  - It plays 6 notes at once; a 7th note replaces the oldest one.
  - Latch and octave shift do **not** change incoming MIDI notes.
  - At start-up, pipe level is step 16 and Character is step 32, both equal to CC value 64.
- **USB name:** libDaisy reports itself as "Daisy Seed Built In" (full-speed USB) or "CHOMPI" (high-speed USB), so the tool matches the port name loosely and falls back to a list.

## Steps

### Phase 1 – Scaffolding (in `firmware/chompi-pipes/tools/midi-test/`)

1. Create `requirements.txt` (mido, python-rtmidi, pyinstaller) and `build.bat`. The script sets up a virtual environment, installs the packages, runs `pyinstaller --onefile --name chompi-pipes-midi-test` and puts the exe in `dist/`.
2. `midi_io.py`:
   - **Finding the device:**
     - It looks for a port name containing "Daisy Seed Built In", "CHOMPI" or "Daisy".
     - If there are several matches or none, it shows a numbered list to pick from.
     - `--port` overrides the choice.
   - **Ports:** it opens both the input and output port.
   - **Helpers:** `send(msg)` keeps a log of every byte sent, and `expect(predicate, timeout)` / `expect_none(timeout)` check incoming messages.
   - **Panic:** sends Note Off for all 128 notes on channel 1. A real "All Notes Off" message won't work because the firmware ignores CC123.
3. `tests.py` holds the test list as data. Each test has: an ID, a section, a title, the action (what MIDI to send), the question to ask (written so that "y" always means it behaved as expected), whether it is manual or auto, any physical steps the user must do first, the expected result, and links to the relevant code. That way a new feature only needs new entries here.
4. `report.py` writes `chompi-pipes-midi-report_YYYYMMDD-HHMMSS.md` to the folder the tool is run from.
5. `chompi_midi_test.py` is the main program:
   - **Startup:** shows a banner and asks for the firmware version or commit plus a free-text note.
   - **Command-line options:** `--list`, `--only B3,C1`, `--section C` and `--port`.
   - **Running tests:** runs them in order. A held test note keeps sounding *while* the user answers and stops after the reply.
   - **Exiting:** Ctrl+C sends panic and still writes a partial report.
   - **Restore:** at the end it sets all 8 CCs back to 64 (the start-up values).

### Phase 2 – Test list (depends on step 3)

**A. Setup / baseline**
- A1: port found and opened.
- A2: sets a known starting sound: CC70 at 127 (8′ pipe only), CC71–73 at 0, CC74–77 at 0 (clean sine).

**B. Notes in**
- B1: C3 (48) sounds.
- B2: a 0x80 Note Off stops it.
- B3: a Note On with velocity 0 stops it.
- B4: chromatic scale C3–C5.
- B5: very low and very high notes (24, 96, 108).
- B6: velocity 127, 64 and 20 get quieter.
- B7: 3-note chord, then 6-note chord.
- B8: a 7th note replaces the oldest.
- B9: the same note twice, then one Note Off. The expected result comes from reading `OrganEngine::NoteOn`/`NoteOff` during implementation.
- B10: notes on channels 2, 10 and 16 stay silent.

**C. CC in** (a note is held during each question)
- C1–C4: each pipe on its own via CC70–73, with the expected interval stated (unison, +1 octave, +octave+fifth, +2 octaves).
- C5: CC70 swept 0→127 smoothly.
- C6: CC value 0 is silent.
- C7–C10: Character for each pipe, CC74–77, 0 vs 127 (clean vs aged).
- C11: the level bar appears on screen when a CC arrives.
- C12: CC70 value 64 gives about half volume.

**D. Unsupported messages / robustness**
- D1–D7: send Program Change, Pitch Bend, Aftertouch, Poly Aftertouch, SysEx, Clock + Start/Stop, and CC 1/7/64/69/78/123. The expected result is no change in sound, and a test note should still play afterwards.

**E. MIDI out** (checked automatically, with on-screen instructions)
- E1: pressing the first key gives Note On 48 with velocity 100 on channel 1, then Note Off.
- E2: octave up gives note 60; E3: octave down gives note 36.
- E4: with latch on, the Note Off is held back until unlatch.
- E5: a pipe 1 encoder turn on the level page sends CC70; turning to full and to zero gives values 127 and 0.
- E6: SHIFT + encoder sends CC70–73.
- E7: the Character page sends CC74.
- E8: the attack and release pages send nothing within 2 s.
- E9: CC sent *to* PIPES is not sent back (auto).
- E10: MIDI notes sent *to* PIPES are not sent back (auto).

**F. Physical controls vs MIDI in**
- F1: with latch on, a MIDI Note Off still stops the note.
- F2: with octave +1, MIDI C3 still plays as C3 (compared with a reference note played first).
- F3: muting a pipe on the hardware wins over a CC level change (if the mute control exists; check against the README).

**G. Stress**
- G1: 500 fast note on/off pairs, then "any stuck notes?". This re-checks bug B2 from [bugfix_plan.md](bugfix_plan.md).
- G2: CC70 flooded with 1000 messages, then PIPES still responds.
- G3: chords played rapidly for 10 s.

### Phase 3 – Report and docs (parallel with Phase 2)

6. **Report contents:**
   - **Header:** date, tool version, firmware version, port name and OS.
   - **Summary table:** counts of PASS, FAIL, SKIP and AUTO-FAIL.
   - **Table of all tests.**
   - **Failures section:** for each failure, the test ID, what was expected, the exact bytes sent (hex), what was received (for auto tests), the user's comment and links to the relevant code, e.g. `MidiHandler.h` `ProcessMidiIn` or `chompi_main.cpp` around L985.
7. A short `README.md` in the tool folder covering how to build, run, the command-line options and how to add a test.

## Relevant files

- `firmware/chompi-pipes/tools/midi-test/` (new): `chompi_midi_test.py`, `midi_io.py`, `tests.py`, `report.py`, `requirements.txt`, `build.bat`, `README.md`
- Reference only:
  - [MidiHandler.h](../code/src/MidiHandler.h): `ProcessMidiIn`, `SendStepCC`
  - [OrganEngine.h](../code/src/OrganEngine.h): `NoteOn`/`NoteOff` stealing, start-up defaults
  - [chompi_main.cpp](../code/src/chompi_main.cpp): key, octave and latch handling, plus the encoder CC code
  - [README.md](../README.md): how to switch pages and which keys to press, for the on-screen instructions

## Verification

1. Run `build.bat` on Windows and check that `dist/chompi-pipes-midi-test.exe` runs on a machine without Python.
2. `--list` prints every test ID.
3. Dry run with a loopback port (loopMIDI): the tool connects, the auto tests E9/E10 pass and E1 times out cleanly as AUTO-FAIL.
4. Run the full suite against a real CHOMPI and check the report renders in VS Code and every failure has bytes, a comment and code links.
5. Pressing Ctrl+C mid-test leaves no stuck notes and still writes a partial report.

## Decisions

- Windows only, USB MIDI only. 5-pin DIN/TRS is left out.
- The tool doesn't change any firmware.
- Test IDs stay fixed so reports can be compared between builds.
- Nothing can be checked through MIDI for distortion, rotary, vibrato, reverb, envelopes, tuning or volume, because they have no MIDI CC. They are only covered where they interact with MIDI (F tests).

## Further considerations

1. Optional `--quick` mode that skips the stress tests and the very low/high note test (G and B5) for short checks after small changes.
2. If PIPES gains MIDI CCs for distortion, reverb and so on later, they're added by putting new entries in `tests.py`.

# CHOMPI ORGAN Per-Pipe Attack/Release Pages Plan

Status: implemented 2026-10-13. Build/test verification pending.

Add pipe-encoder pages 3 (attack time) and 4 (release time) to CHOMPI ORGAN.
Each pipe gets its own gate-envelope timing so pipes can fade in/out at
different rates (e.g. the 2' pipe swelling up seconds after the 8'
fundamental). The engine's single shared 5 ms gate envelope is replaced by a
per-pipe envelope; step 0 reproduces the original fixed timing exactly, so
boot defaults are unchanged.

## Confirmed Decisions

- Page cycle per pipe encoder push: 1 (level) -> 2 (Character) -> 3 (attack)
  -> 4 (release) -> 1.
- 32 steps, log-spaced 5 ms to 10 s: seconds = 5 ms * 2000^(step/32)
  (EnvelopeMap.h). Log spacing keeps the short end usable, same rationale as
  the plate-reverb decay.
- Attack minimum is the original 5 ms; release minimum is a 5 ms floor (not
  a true 0 ms) so key release stays click-free.
- No MIDI CC in/out for attack/release (level keeps CC 70-73, Character
  CC 74-77).
- Key bar: uniformly purple for attack, uniformly yellow for release. Knob
  LEDs mirror the same colors with the usual 0.2 brightness floor.
- Attack/release are not saved; both boot at step 0. Page state is not saved
  and always starts on page 1.
- Voice allocation: a voice stays in RELEASE until every pipe's envelope has
  reached 0 (the slowest pipe holds the voice). Voice stealing keeps the
  fixed 5 ms steal fade on all pipes.
- Same-note retrigger attacks from the current envelope level per pipe, so
  long release tails are not cut (StartVoice never resets pipe_env).

## Verified Code Anchors

- Page state was `character_page_[4]` bool in chompi_main.cpp, toggled by the
  ENC_1..4_SW push edge in `PollControls`; now `pipe_page_[4]` int cycled % 4.
- Encoder routing for pipe encoders lives in the `PollControls` encoder loop
  (`pipe_encoder[p] == i`); bar confirmation uses `bar_pipe_/bar_page_/
  bar_until_` with a 500 ms timeout.
- Key-bar colors: `BarKind` enum + `BarKeyColor()`; pipe bar rendering in
  `UpdateLeds()` (`bar_page_` 0=level, 1=Character, 2=attack, 3=release).
  `bar_page_ == 3` also means reverb decay on the volume bar, but the
  `p == kVolBar` branch runs first, so there is no collision.
- Engine: `OrganEngine.h` `Voice` struct holds `pipe_env[4]`; the
  ATTACK/RELEASE/STEAL state machine in `Process()` ramps each pipe with
  `attack_inc_[p]` / `release_inc_[p]` / `steal_inc_`; the mix applies
  `psig * pipe_gain_[p] * pipe_env[p]` per pipe.
- Mapping lives in `EnvelopeMap.h` (pure math, no Daisy deps) so the native
  tests can include it directly.

## Implementation Steps (done)

- Phase 1 - engine: EnvelopeMap.h; per-pipe step state + clamped accessors
  (`SetPipeAttackStep`/`SetPipeReleaseStep`); `Voice::pipe_env[4]` replacing
  the shared `env`; per-pipe ramps in `Process()`; STEAL fade unchanged.
- Phase 2 - UI: 4-page cycle, encoder routing, purple/yellow bars + knob
  LEDs, boot init.
- Phase 3 - tests & docs: `code/tests/envelope_map_test.cpp`; spec FR-2b and
  README updated.

## Relevant Files

| File | Role |
| --- | --- |
| `code/src/EnvelopeMap.h` | step->seconds/increment mapping (new) |
| `code/src/OrganEngine.h` | per-pipe envelope state machine |
| `code/src/chompi_main.cpp` | page cycle, bars, knob LEDs |
| `code/tests/envelope_map_test.cpp` | native mapping tests (new) |
| `plans/chompi-organ-specification.md` | FR-2b updated |
| `README.md` | controls docs updated |

## Verification

- `make -C code/tests test` - mapping endpoints (5 ms / 10 s), monotonicity,
  log midpoint, clamping, increment sanity.
- `cd code/src && make -j4` - clean ARM build; SRAM impact ~130 B.
- On-device: boot = original sound; staggered per-pipe fade-in; long release
  tails; stealing/latch still click-free; page cycle 1->2->3->4->1 with bar
  confirmation.

## Scope Boundaries

- Included: per-pipe attack/release, 4-page cycle, bars + knob LEDs, engine
  rework, mapping test, docs.
- Excluded: MIDI CCs for attack/release, flash persistence, volume-encoder
  pages, per-voice (rather than per-pipe) envelope times.

## Risks & Notes

- A 10 s release holds its voice until the tail finishes; dense playing with
  long releases consumes voices faster - oldest-first stealing covers it.
- CPU: +4 ramp/mul operations per pipe per sample per voice - trivial.
- RAM: ~96 B extra in voices + 32 B engine state.

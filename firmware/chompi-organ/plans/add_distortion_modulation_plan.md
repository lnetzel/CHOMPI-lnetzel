# Plan: Distortion modulation (Modulation Mode destination 6)

Status: IMPLEMENTED 2026-10-17.

## Problem and approach

Give the distortion drive the same organic drift the pipe volume,
Character and pipe length already have. The drive is shared by all four
pipes, so unlike the existing destinations this is ONE global
modulation amount, not four per-pipe amounts.

## Confirmed behavior (user decisions 2026-10-17)

- **Destination key**: SHIFT + black key 8 (F#4, SwId KEY_23 = 29) in
  Modulation Mode selects the distortion-modulation destination, red
  LEDs (the overdrive color). It acts exactly like the volume and
  Character modulation destinations - remembered across mode changes
  until reboot, never stored in presets - except only ONE encoder
  edits it. Black keys 6, 7, 9 and 10 select nothing. KEY_23 doubles
  as the erase-operation key in Normal Mode; no conflict, because
  destination selection is Modulation-Mode-only and preset operations
  are Normal-Mode-only.
- **Encoder**: the leftmost physical knob (pipe 1, PCB ENC_4) adjusts
  the global amount. The other three pipe knobs are inert while this
  destination is selected. SHIFT + turn behaves the same as an
  unshifted turn (there is nothing to broadcast). No MIDI CC.
- **Depth**: 0..32 steps like the other modulation amounts; the same
  free-running irregular raised-cosine DriftModulator swings the drive
  inside a +/-20% full-scale window around the set drive level
  (DriftModulator::ApplyWindow, sliding window: at 0% drive the
  distortion drifts in and out of audibility). Amount 0 is bit-exact
  the unmodulated drive.
- **Persistence**: the amount is stored in the SD-card presets as the
  global "drive_mod" field (0..32, default 0). The schema version
  stays 1: old files parse to the default via the QueryInt fallback
  and old firmware ignores the unknown key.

## Implementation

- [ModDest.h](../code/src/ModDest.h): kModDestDriveMod (sixth
  destination), kModDestKeySw = {7, 12, 13, 14, 21, 29},
  ModDestAfterKey, ModDestColor red (1, 0, 0).
- [OrganEngine.h](../code/src/OrganEngine.h): uint8_t drive_mod_step_
  (boot 0), a thirteenth DriftModulator drive_mod_ (seed 0xC3D2E1F0,
  base rate), drive_mod_target_ member. SetDriveModStep /
  GetDriveModStep clamp to 0..kMaxModSteps. Prepare() advances the
  drive modulator once per block and computes drive_mod_target_ =
  ApplyWindow(drive_target_, step/32, u) before
  distortion_.PrepareBlock(); Process() slews drive_ toward
  drive_mod_target_ (the existing ~1 ms per-sample fonepole), so
  block-rate window updates are click-free and amount 0 stays
  bit-exact.
- [OrganPreset.h](../code/src/OrganPreset.h): drive_mod field,
  SetDefaults/SetInit = 0, "drive_mod":%u in Serialize, QueryInt parse
  clamped 0..kMaxMod with default 0, CaptureFromEngine /
  ApplyToEngine.
- [chompi_main.cpp](../code/src/chompi_main.cpp): static_assert of
  kModDestKeySw[5] against SwId::KEY_23; kModBarDriveMod = 9 bar
  sub-state + kModDestBarPage entry (red bar via kBarDrive); the
  mod-mode turn handler intercepts kModDestDriveMod first (p == 0
  only); the destination LED overlay's black-key clear loop is now a
  destination-LED mask because KEY_23's SMT LED is 7, outside the old
  0..4 assumption (SMT 5, 6, 8, 9 forced off); knob LEDs show the red
  amount on knob 1 only (knobs 2-4 off; the muted-pipe solid red
  override is unchanged).
- Tests: mod_dest_test.cpp (key 8 selects, keys 6/7/9/10 inert, red
  color) and organ_preset_test.cpp (FakeEngine drive_mod API,
  round-trip, clamp 99 -> 32, missing-key default 0, capture/apply,
  SetInit, size bound). Host suite passes (tonewheel_character_test
  L273 and vibrato_test L91 remain pre-existing failures, untouched).
  ARM build clean, no warnings: bin 170,432 B, SRAM_EXEC 71.74%.

## Hardware verification checklist (PENDING)

- SHIFT + F#4 (black key 8) selects the red destination; key LED 100%
  red, the other five destination keys at 75%.
- Leftmost knob adjusts the amount; knobs 2-4 do nothing; red value
  bar on the white keys; knob 1 LED red with the 0.2 brightness floor.
- The drive audibly drifts around the set level; at drive 0 the
  distortion fades in and out; amount 0 is exactly the static drive.
- Changing the amount never restarts the drift (free-running).
- Preset round-trip: save with a nonzero amount, power-cycle, load -
  the amount and the drift return; presets written before this change
  load with drive_mod 0.

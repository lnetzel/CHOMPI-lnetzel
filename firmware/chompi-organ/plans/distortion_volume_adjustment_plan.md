# Distortion Volume Adjustment Plan

## Goal

Make CHOMPI-Organ's FUZZ automatically quieter without changing its
distortion character or requiring a user-controlled setting.

Start with a fixed **-4 dB output reduction**, equivalent to an amplitude
multiplier of **0.6309573445** (about 37% lower signal amplitude). This is
an initial engineering value within the reported adjustment range, not
guaranteed perceived-loudness matching for every registration.

## Scope

- No new encoder page, control, MIDI CC, preset field, or saved setting.
- Do not change the main-volume value, display, or control behavior.
- Apply the same fixed reduction at every settled nonzero FUZZ amount.
- Leave FUZZ input gain, reference normalization, clipping curve, and
  10 kHz filter unchanged.
- Leave rotary, dropout, reverb processing, and other distortion
  voicings unchanged.
- Do not introduce a compressor, limiter, extra filtering, automatic
  loudness tracking, or drive-dependent compensation.

## Design

Apply the gain equally to both stereo channels **after plate reverb**,
alongside master gain and before writing the headphone and master output
pairs. This is equivalent to reducing the listening volume while FUZZ is
active, without changing any upstream DSP calculations.

Use the existing finite, approximately 10 ms type-selection and
wet/bypass fades to bring compensation in and out smoothly. Gate the
trim using FUZZ's actual activity:

```cpp
1.f - (1.f - kFuzzOutputGain) * weight_[kFuzz] * wet_fuzz_
```

Do not gate on the smoothed drive value or a binary selected-type check.
The wet fade already follows the drive target and reaches exact bypass
in finite time, even while the smoothed drive value remains nonzero.
No additional gain smoother is planned initially.

At settled FUZZ, the complete output should equal the previous output
multiplied by 0.6309573445, within floating-point rounding. During mode
and bypass transitions, gain intentionally varies. Perceived tone can
still differ at a lower listening level.

The trim lowers both dry sound and existing reverb tails while FUZZ is
active. Switching away from FUZZ or returning distortion to zero
smoothly restores unity gain, so existing tails also rise smoothly in
level. Tail-specific gain tracking is outside this change's scope.

## Implementation

1. Update [Distortion.h](../code/src/Distortion.h).
   - Add a named fixed constant, `kFuzzOutputGain = 0.6309573445f`.
   - Add a const `GetOutputGain()` getter using the activity formula
     above, following the ownership pattern of `GetBrokenWetAmount()`.
   - Keep `Distortion::Process()` output and all shaping/filtering
     calculations unchanged.
   - Document that OrganEngine applies the trim after the effects chain.
2. Update [OrganEngine.h](../code/src/OrganEngine.h).
   - After `plate_.Process()`, query `GetOutputGain()` once per sample.
   - Apply identical compensation to left and right channels alongside
     `master_gain_`, before writing all four output channels.
   - Preserve unity behavior for settled bypass and other settled modes.
   - Add no allocations, transcendental calculations, coefficient work,
     or extra ramp state to the audio loop.
   - Leave `SetMasterGain()`, `GetMasterGain()`, and controls unchanged.
3. Extend [distortion_test.cpp](../code/tests/distortion_test.cpp).
   - Reuse existing assert-based tests and waveform/spectral helpers;
     do not add a test framework or separate test file.
   - Cover the fixed trim, activity fades, and bypass behavior described
     below while retaining existing numerical and harmonic tests.
4. Update [README.md](../README.md).
   - Document automatic fixed FUZZ output compensation and its location
     in the signal flow.
   - State that it introduces no new user control.

## Verification

### Native Tests

- Initial gain is exactly unity.
- Settled FUZZ gain is the fixed multiplier across drive steps 1-32.
- Settled Tape Saturation and BrokenSpeaker gain remains exactly unity.
- Returning the drive target to zero restores exact unity after the wet
  fade, even with a nonzero smoothed drive argument.
- Gain remains bounded between the fixed multiplier and unity.
- Type and wet transitions converge in finite time at 32 and 48 kHz.
- Rapid retargets and zero/nonzero drive changes follow the existing
  ramps without abrupt gain jumps.
- FUZZ shaping output remains unchanged by adding the gain getter.
- Settled sample scaling, RMS/peak scaling, and normalized harmonic
  ratios agree with the original waveform within numerical tolerance.

Run the focused test first, then the full suite:

```sh
make -C firmware/chompi-organ/code/tests distortion_test
./firmware/chompi-organ/code/tests/distortion_test
make -C firmware/chompi-organ/code/tests test
```

On a Windows host, the executable may be named `distortion_test.exe`.
These component tests are not a full OrganEngine integration test.

### Firmware Build

Use the documented ARM GNU 10.3-2021.10 toolchain and existing dependencies:

```sh
make -C firmware/chompi-organ/code/src DEBUG=0 OPT=-O3
```

Expected artifact: `code/src/build/CHOMPI_ORGAN_1.0.bin`.
Do not flash the bootloader or deploy automatically.

### Hardware Listening

Begin at a low listening volume. Compare quiet and loud single notes,
chords, registrations, every FUZZ drive step, and multiple master-volume
positions. Test rotary and reverb both disabled and enabled, including
long reverb tails, rapid type switching, and returning drive to zero.

Confirm the following:

- FUZZ retains its character at the lower output level.
- The fixed reduction is sufficient without making low-drive FUZZ
  unacceptably quiet.
- There are no clicks or abrupt volume jumps during transitions.
- Both headphone and master stereo pairs receive the same reduction.
- The main-volume value and display do not change.
- Other settled modes and distortion bypass retain their original level.
- Reverb-tail level restoration when leaving FUZZ is acceptable.

No native OrganEngine test harness was identified, so complete-chain
listening/integration verification requires hardware. Report any
unavailable toolchain or hardware checks explicitly.
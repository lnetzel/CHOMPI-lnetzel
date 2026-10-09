# Plan: MAGIC page 4 — Sample Rate Reducer (port from Tempo)

Port Tempo's `daisysp::SampleRateReducer` + knob mapping **verbatim** into Tape as a 4th page on the MAGIC encoder. Turning right adds more reduction; 0 = bypass. Session-only like other FX (not saved to presets), default 0. Tape's DaisySP `sampleratereducer.cpp` is byte-identical to Tempo's and `daisysp.h` already includes it — no lib changes.

Note: in Tempo the reducer is page 3 of the first (pitch) knob (pitch / pan / reducer), with no shift modifier.

## Exact algorithm to port

Tempo reference: `setSampleReducer` in `firmware/chompi-tempo/code/src/SampleEngine.h` (L473-484), `Process` (L135-145), UI in `MenuPage.h` (L556-561).

```cpp
void SetSampleReducer(float amount) {
    if (amount > 0.f) {
        amount = (1.0f - amount) * 0.45f;
        amount = fclamp(amount, .01f, 1.f);
        rd_l_.SetFreq(amount);
        rd_r_.SetFreq(amount);
        reduce_ = true;
    }
    else {
        reduce_ = false;
    }
}
```

- `Process()` is always called, even when bypassed, to keep state continuous.
- UI: `+= turns * 0.01`, clamp 0..1 (Tape's `NormalPage::OnEncoderTurned` already does this generically).
- Like Tempo, the first step above 0 already gives ~2.2x reduction — keep as is.

## Decisions

- LED: dim green → full green (`color_xfade(green * .1f, green, value)`), distinct from filter's purple.
- CHOMPI (shift) + turn MAGIC on page 4: does nothing; LED 4 off.
- Outgoing MIDI CC: 34. Incoming CC 23 on page 4 sets reduction (existing generic path).
- Chain position: **first** in the FX chain (top of the `ApplyFx` per-sample loop, before `fx_env_` gain, DC block, filter).
- No preset/SD changes (`PRE_VERSION`, `kMaxControls` unchanged). No extra smoothing (matches Tempo).

## Steps

### Phase A — DSP

1. `code/src/DSPEngine.h`
   - Members: `daisysp::SampleRateReducer rd_l_, rd_r_;` and `bool reduce_;`.
   - `Init` (near `filter_.Init`, ~L179): `rd_l_.Init(); rd_r_.Init(); reduce_ = false;`.
   - Add `SetSampleReducer(float)` (verbatim, above) next to `SetFilter` (~L1187).
   - `ApplyFx` (~L272): at the top of the per-sample loop:
     `if (reduce_) out = rd.Process(out); else rd.Process(out);` for L and R.

### Phase B — UI and arrays

2. `code/src/ui.h`
   - `enc_defaults` → `[4][6]`, new row `{0.f, 0.f, 0.f, 0.f, 0.f, 0.f}`.
   - `enc_values` → `[4][6]`; `enc_rows[4]`; `def_rows[4]`.
3. `code/src/NormalPage.h`
   - `cc_map` → `[4][6]`, new row `{0, 0, 0, 34, 0, 0}`.
   - `knob_num_pages[3] = 4`.
   - `Init` loop: `page < 4`.
   - `Draw` case 3: make filter branch explicit `else if (page == 2)`; new `else` branch calls `fx_->SetSampleReducer(value)` and sets the green LED fade. Existing `!switch_state` LED blanking unchanged.
4. `code/src/MenuPage.h`
   - Shift LED display (~L238-245): filter explicit `knob_page[3] == 2`; page 4 → LED 4 off.
   - `OnEncoderTurned` encoder 3: page 3 is already a no-op — no change.
   - FX reset on ENC_3_SW (~L812-819): add `enc_values[3][3] = enc_defaults[3][3];` and `fx_->SetSampleReducer(enc_values[3][3]);`.

### Phase C — Docs and build

5. `midi.md`
   - CC 23 row: add "Page 4: sample rate reduction".
   - Outgoing table: add `| Sample rate reduction | MAGIC | 4 | 34 | 0–127 absolute |`.
6. Bump Makefile `TARGET`, build with the 10.3-2021.10 toolchain recipe, record .text / SRAM_EXEC / .bin deltas.

## Verification

1. Build succeeds, no SRAM_EXEC overflow.
2. Hardware:
   - MAGIC click cycles 4 pages; page 4 LED dim → full green.
   - Turning right progressively increases reduction; full left bypasses without a click.
   - Pages 1–3 unchanged.
   - Shift + turn on page 4 does nothing; LED 4 off.
   - Shift + MAGIC click resets reduction to 0.
   - CC 34 sent while turning on page 4; incoming CC 23 on page 4 sets reduction.
   - Works with FX both pre- and post-looper (`fx_pre_loop` toggle).

## Risk

SRAM_EXEC was ~99.5% (~1.2 KB free) at last build. New code: `SampleRateReducer::Process` + per-sample branch + UI. If it overflows, first shrink candidate is the `kLooperPasteGainQ12` loop (if present in the tree); the DSP stays verbatim.

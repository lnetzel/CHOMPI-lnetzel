# Plan: Replace the organ plate reverb with a high-quality stereo plate

## Problem and approach

The current plate reverb collapses its stereo input to a mono send before exciting two wet tank paths. This loses the rotary speaker's left/right differences in the reverb excitation. Replace the current algorithm with a higher-quality plate-style stereo reverb that accepts and preserves stereo input through its tank, rather than merely producing a stereo return from a mono send.

Keep the existing OrganEngine call surface, reverb mix and decay controls, and dry/wet behavior. At 0% mix, output must remain a bit-exact copy of the stereo input. The dry organ/rotary signal path and its behavior are out of scope.

## Todos

1. Inspect existing reverb integration, memory allocation, controls, and focused native tests.
2. Implement a new stereo-input plate algorithm within the existing reverb component, preserving its interface and control behavior.
3. Add regression and stereo-response tests, and update the directly related signal-flow/reverb documentation.
4. Run focused native tests and inspect resource implications for the embedded target.

## Notes and acceptance criteria

- Left and right input signals must excite distinguishable paths in the wet tank; do not sum to mono before the tank.
- The wet response should have a spacious, diffuse plate character, with decorrelation and controlled cross-coupling rather than simple independent echoes.
- Preserve the existing mix/decay controls, initialization/reset behavior, mix ramp, exact 0% dry bypass, and stereo dry signal.
- Exercise mono and strongly different stereo inputs, checking useful stereo separation without runaway feedback, non-finite samples, or unbounded output.
- Keep the implementation compatible with the MCU build and existing SDRAM/delay-memory setup; validate memory and per-sample CPU costs.
- Update the signal-flow documentation so it describes the actual stereo-in behavior.

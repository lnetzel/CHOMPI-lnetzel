# CHOMPI-PIPES — MIDI Implementation

All messages use a fixed MIDI channel: **1**.
Transports: 5-pin DIN (UART) and USB MIDI, both directions.
No other message types (pitch bend, aftertouch, program change, SysEx, clock) are processed or sent.

## MIDI In

| Function | Type / CC | Value span |
|---|---|---|
| Note On | Note On (0x90) | Note 0–127; velocity 1–127 (scaled to 0.0–1.0). Velocity 0 = Note Off |
| Note Off | Note Off (0x80) | Note 0–127; velocity ignored |
| Pipe 1 level | CC 70 | 0–127 (mapped to 0–32 steps, rounded) |
| Pipe 2 level | CC 71 | 0–127 (mapped to 0–32 steps, rounded) |
| Pipe 3 level | CC 72 | 0–127 (mapped to 0–32 steps, rounded) |
| Pipe 4 level | CC 73 | 0–127 (mapped to 0–32 steps, rounded) |
| Pipe 1 Character | CC 74 | 0–127 (mapped to 0–64 steps, rounded) |
| Pipe 2 Character | CC 75 | 0–127 (mapped to 0–64 steps, rounded) |
| Pipe 3 Character | CC 76 | 0–127 (mapped to 0–64 steps, rounded) |
| Pipe 4 Character | CC 77 | 0–127 (mapped to 0–64 steps, rounded) |

## MIDI Out

| Function | Type / CC | Value span |
|---|---|---|
| Note On | Note On (0x90) | Note 0–127; velocity fixed at 100 |
| Note Off | Note Off (0x80) | Note 0–127; velocity fixed at 0 |
| Pipe 1 level | CC 70 | 0–127 (step 0–32 scaled: round(step × 127 / 32)) |
| Pipe 2 level | CC 71 | 0–127 (step 0–32 scaled: round(step × 127 / 32)) |
| Pipe 3 level | CC 72 | 0–127 (step 0–32 scaled: round(step × 127 / 32)) |
| Pipe 4 level | CC 73 | 0–127 (step 0–32 scaled: round(step × 127 / 32)) |
| Pipe 1 Character | CC 74 | 0–127 (step 0–64 scaled: round(step × 127 / 64)) |
| Pipe 2 Character | CC 75 | 0–127 (step 0–64 scaled: round(step × 127 / 64)) |
| Pipe 3 Character | CC 76 | 0–127 (step 0–64 scaled: round(step × 127 / 64)) |
| Pipe 4 Character | CC 77 | 0–127 (step 0–64 scaled: round(step × 127 / 64)) |

Notes:

- Incoming messages on channels other than 1 are discarded.
- CC out is sent when pipe levels / Character amounts change from local controls; notes out are sent for local key presses/releases.
- Character = tonewheel color; 0 = clean sine, 127 ≈ step 64 = fully aged.

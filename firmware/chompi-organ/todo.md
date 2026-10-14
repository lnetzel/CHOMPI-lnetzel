# TODO CHOMPI-ORGAN

## LFO MODULATIONS
* ~~Switch up to enter LFO world~~ DONE: Modulation Mode (toggle UP),
  per plans/modulation_mode_plan.md
    * ~~each of the 4 pipe encoders now controls the modulation amounts~~
      DONE (mod pg.1 = volume, mod pg.2 = Character, 32 steps)
    * Transport wheel controls speed of the LFOs? NOT done (fixed
      random 2.5-10 s half durations instead)
* Destinations
    * ~~Tonewheel character~~ DONE
    * ~~Pipe Volume~~ DONE
    * Distortion level (still open)
* Sources
    * ~~Random Time Triangle~~ DONE as raised-cosine drift with random
      per-half durations (DriftModulator.h)
    * Triangle (not done)

## Presets
* ~~Save and load presets just like on tape but only one bank. ALL values
  are saved~~ DONE: 15 SD-card slots (ORGAN/P01..P15.json), per
  plans/preset_management_plan.md

## Tweaks
* Even out the volume for the distortions
* Broken speaker doesn't sound good enough, chnage to some lo-fi version
* 

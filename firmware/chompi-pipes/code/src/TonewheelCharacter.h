/** @file TonewheelCharacter.h
 *  @brief Per-pipe tonewheel-inspired "Character" coloration for ORGAN.
 *
 *  Replaces the page-2 sine->square morph with four independent Character
 *  amounts (one per pipe, shared across all six voices). Step 0 is the
 *  existing clean sine; step 64 is clearly aged but still organ-like.
 *
 *  The coloration is inexpensive harmonic shading derived algebraically
 *  from the existing normalized sine sample s = sin(ph):
 *
 *      second = 2*s*s - 1            = -cos(2*ph)   (2nd harmonic)
 *      third  = s*(3 - 4*s*s)        =  sin(3*ph)   (3rd harmonic)
 *
 *  No extra oscillator, no extra sinf/powf, no per-sample random or
 *  allocation. Harmonic strengths vary deterministically by note and pipe
 *  identity (+/-15%) so repeated pitches on a pipe have a stable voicing
 *  regardless of which voice slot renders them, and each harmonic is
 *  anti-alias-masked against the final folded pipe frequency.
 *
 *  Bass grit: on the fundamental pipe (8', pipe kGritPipe) the harmonic
 *  gains are additionally multiplied by (1 + kGritBoost *
 *  BassMask(folded_hz)), a smooth low-frequency weight that is 1 at or
 *  below kGritFullHz and 0 at or above kGritZeroHz, and they start from
 *  the pipe's own hotter caps (kGritMaxSecond/kGritMaxThird) instead of
 *  the gentler shared voicing. Real organs
 *  reinforce low fundamentals with quint/harmonic ranks because the ear
 *  reconstructs "bass" largely from harmonics (missing fundamental);
 *  Character on the 8' pipe therefore adds progressively more grit
 *  toward the bottom octave. Grit is part of the Character blend, so
 *  step 0 remains the exact clean sine, and norm is computed from the
 *  boosted gains so the aged-wave magnitude bound is unchanged.
 *
 *  This is an oscillator waveform shaper, not a generic zero-preserving
 *  audio filter: the second-harmonic term is -1 when s == 0, so
 *  ShapeSample(0, ...) != 0 at nonzero Character. It IS zero-mean over
 *  complete sine cycles, and pipe gain/envelopes are applied after
 *  shaping (in OrganEngine), so muted pipes stay silent.
 *
 *  Header-only, no MCU/Daisy dependencies: native-testable.
 */
#pragma once
#include <stdint.h>
#include <math.h>

namespace chompi
{

class TonewheelCharacter
{
  public:
    static constexpr int kNumVoices = 6;
    static constexpr int kNumPipes  = 4;
    static constexpr int kMaxSteps  = 64;

    /** Finite amount ramp length, sample-rate derived (~10 ms). */
    static constexpr float kRampSeconds = .010f;

    /** Maximum-aged harmonic voicing relative to the fundamental, before
     *  normalization (implementation constants, not user controls).
     *  Calibrated points: 4.5% 2nd, 10% 3rd harmonic - clearly audible
     *  but still organ-like, not extreme. */
    static constexpr float kMaxSecond = .045f;
    static constexpr float kMaxThird  = .10f;

    /** Deterministic per-(note, pipe) voicing variation: +/-15%. */
    static constexpr float kVariation = .15f;

    /** Harmonic anti-alias mask: a harmonic at kAaFull * sample_rate or
     *  below passes at full strength; its strength fades smoothly to zero
     *  at kAaZero * sample_rate and stays zero above. */
    static constexpr float kAaFull = .35f;
    static constexpr float kAaZero = .45f;

    /** Bass grit (8' pipe only): harmonic gains are multiplied by
     *  (1 + kGritBoost * BassMask(folded_hz)); the mask is 1 at or below
     *  kGritFullHz and smoothstepped to 0 at kGritZeroHz. Absolute Hz on
     *  purpose: this is a psychoacoustic band, not sample-rate relative.
     *  The grit pipe also gets its own, hotter harmonic caps (2x the
     *  shared ones) so the bottom end is not limited by the "still
     *  organ-like" voicing chosen for pipes 1-3. Bottom octave at full
     *  Character: ~40% fundamental / 18% 2nd / 41% 3rd after norm -
     *  deliberately hot; calibrate by ear on hardware (back kGritBoost
     *  down first if too hairy). */
    static constexpr int   kGritPipe      = 0;   /* 8' fundamental */
    static constexpr float kGritFullHz    = 200.f;
    static constexpr float kGritZeroHz    = 700.f;
    static constexpr float kGritBoost     = 4.f; /* bottom-octave x5 gains */
    static constexpr float kGritMaxSecond = .09f;
    static constexpr float kGritMaxThird  = .20f;

    /** Cached voicing for one voice/pipe: variation-scaled,
     *  anti-alias-masked harmonic gains plus the maximum-aged
     *  normalization bound. */
    struct PipeCoeffs
    {
        float second_gain; /**< 2nd-harmonic gain vs. fundamental */
        float third_gain;  /**< 3rd-harmonic gain vs. fundamental */
        float norm;        /**< 1/(1+|g2|+|g3|): bounds aged wave to |1| */
    };

    void Init(float sample_rate)
    {
        sample_rate_ = sample_rate;
        ramp_inc_    = 1.f / (kRampSeconds * sample_rate);
        for(int p = 0; p < kNumPipes; p++)
        {
            steps_[p]  = 0;
            amount_[p] = target_[p] = 0.f;
        }
        for(int v = 0; v < kNumVoices; v++)
        {
            for(int p = 0; p < kNumPipes; p++)
            {
                coeffs_[v][p].second_gain = 0.f;
                coeffs_[v][p].third_gain  = 0.f;
                coeffs_[v][p].norm        = 1.f;
            }
        }
    }

    /** Set a pipe's Character step (0..kMaxSteps, clamped). Retargets the
     *  amount ramp from the current amount; exact settled endpoints. */
    void SetStep(int pipe, int step)
    {
        if(pipe < 0 || pipe >= kNumPipes)
            return;
        step         = step < 0 ? 0 : (step > kMaxSteps ? kMaxSteps : step);
        steps_[pipe] = static_cast<uint8_t>(step);
        target_[pipe] = static_cast<float>(step) * (1.f / kMaxSteps);
    }

    int GetStep(int pipe) const
    {
        if(pipe < 0 || pipe >= kNumPipes)
            return 0;
        return steps_[pipe];
    }

    /** Modulation Mode: set the ramp destination directly (0..1,
     *  clamped), keeping steps_ as the user-facing center. The existing
     *  ~10 ms linear ramp smooths the block-rate updates from
     *  OrganEngine::Prepare(), so per-block target steps are click-free.
     *  Passing the exact center value restores the unmodulated behavior
     *  bit-exactly. SetStep behavior is unchanged. */
    void SetModulatedTarget(int pipe, float target)
    {
        if(pipe < 0 || pipe >= kNumPipes)
            return;
        target_[pipe]
            = target < 0.f ? 0.f : (target > 1.f ? 1.f : target);
    }

    /** Advance all four pipe amounts by one sample. Call once per sample
     *  (not once per voice), even while voices are idle, so controls keep
     *  settling. */
    void Advance()
    {
        for(int p = 0; p < kNumPipes; p++)
        {
            float       a = amount_[p];
            const float t = target_[p];
            if(a < t)
            {
                a += ramp_inc_;
                amount_[p] = a > t ? t : a; /* snap: exact endpoint */
            }
            else if(a > t)
            {
                a -= ramp_inc_;
                amount_[p] = a < t ? t : a;
            }
        }
    }

    /** Raw ramp amount 0..1 (diagnostics/tests). */
    float Amount(int pipe) const
    {
        if(pipe < 0 || pipe >= kNumPipes)
            return 0.f;
        return amount_[pipe];
    }

    /** Smoothstep-eased amount 0..1: the flat lower end keeps the first
     *  clicks gentle. */
    float EasedAmount(int pipe) const
    {
        return Smooth(Amount(pipe));
    }

    /** Recompute the cached voicing for one voice/pipe. Call when the
     *  voice's frequencies are initialized or updated (note-on, fine
     *  tune). folded_hz is the FINAL folded pipe frequency; note and pipe
     *  identity drive the deterministic variation. Voice-slot independent:
     *  the same (note, pipe, folded_hz) always yields the same coeffs. */
    void UpdateVoiceCoeffs(int voice, int note, int pipe, float folded_hz)
    {
        if(voice < 0 || voice >= kNumVoices || pipe < 0 || pipe >= kNumPipes)
            return;
        PipeCoeffs &c = coeffs_[voice][pipe];
        const float v2 = 1.f + kVariation * DetHash(note, pipe, 0);
        const float v3 = 1.f + kVariation * DetHash(note, pipe, 1);
        /* Bass grit: the 8' pipe's harmonics grow toward the bottom
         * octave from its own hotter caps; other pipes use the shared
         * caps with the exact literal 1.f multiplier, so they stay
         * bit-exact the pre-grit voicing. norm is computed from the
         * boosted gains, so the aged-wave bound holds. */
        const bool  is_grit = pipe == kGritPipe;
        const float cap2    = is_grit ? kGritMaxSecond : kMaxSecond;
        const float cap3    = is_grit ? kGritMaxThird : kMaxThird;
        const float grit = is_grit ? 1.f + kGritBoost * BassMask(folded_hz)
                                   : 1.f;
        c.second_gain  = cap2 * v2 * AaMask(2.f * folded_hz) * grit;
        c.third_gain   = cap3 * v3 * AaMask(3.f * folded_hz) * grit;
        c.norm = 1.f / (1.f + fabsf(c.second_gain) + fabsf(c.third_gain));
    }

    const PipeCoeffs &Coeffs(int voice, int pipe) const
    {
        return coeffs_[voice][pipe];
    }

    /** Shape one normalized sine sample s (|s| <= 1) toward the bounded
     *  aged waveform by the eased amount (convex blend, no division).
     *  Settled Character zero returns the original sine exactly. */
    static float ShapeSample(float s, const PipeCoeffs &c, float eased_amount)
    {
        if(eased_amount <= 0.f)
            return s;
        const float second = 2.f * s * s - 1.f;       /* -cos(2*ph) */
        const float third  = s * (3.f - 4.f * s * s); /*  sin(3*ph) */
        const float aged
            = c.norm * (s + c.second_gain * second + c.third_gain * third);
        return s + eased_amount * (aged - s);
    }

  private:
    static float Smooth(float t) { return t * t * (3.f - 2.f * t); }

    /** Deterministic hash of (note, pipe, which-harmonic) -> [-1, 1).
     *  Pure integer mixing: no RNG state, no allocation, identical result
     *  on every call and on every voice slot. */
    static float DetHash(int note, int pipe, int which)
    {
        uint32_t h = static_cast<uint32_t>(note & 0x7f) * 2654435761u
                     ^ static_cast<uint32_t>(pipe & 0x3) * 2246822519u
                     ^ static_cast<uint32_t>(which + 1) * 3266489917u;
        h ^= h >> 15;
        h *= 2654435761u;
        h ^= h >> 13;
        return static_cast<float>(h >> 8) * (1.f / 8388608.f) - 1.f;
    }

    /** Smooth anti-alias fade for one harmonic: 1 at or below
     *  kAaFull * sr, smoothstepped to 0 at kAaZero * sr, 0 above. */
    float AaMask(float harmonic_hz) const
    {
        const float full = kAaFull * sample_rate_;
        const float zero = kAaZero * sample_rate_;
        if(harmonic_hz <= full)
            return 1.f;
        if(harmonic_hz >= zero)
            return 0.f;
        const float t = (harmonic_hz - full) / (zero - full);
        return 1.f - Smooth(t);
    }

    /** Smooth low-frequency weight for the 8'-pipe bass grit: 1 at or
     *  below kGritFullHz, smoothstepped to 0 at kGritZeroHz, 0 above.
     *  Absolute Hz on purpose (psychoacoustic band), so it does not
     *  depend on the sample rate. */
    static float BassMask(float hz)
    {
        if(hz <= kGritFullHz)
            return 1.f;
        if(hz >= kGritZeroHz)
            return 0.f;
        const float t = (hz - kGritFullHz) / (kGritZeroHz - kGritFullHz);
        return 1.f - Smooth(t);
    }

    float    sample_rate_;
    float    ramp_inc_; /* amount per sample for a full 0..1 swing */
    uint8_t  steps_[kNumPipes];
    float    amount_[kNumPipes];  /* current ramp position, 0..1 */
    float    target_[kNumPipes];  /* ramp destination, 0..1 */
    PipeCoeffs coeffs_[kNumVoices][kNumPipes];
};

} // namespace chompi

#include "Dsp/Building.h"
#include "Effects/IEffect.h"

#include <juce_audio_basics/juce_audio_basics.h>

#include <algorithm>
#include <cmath>

// resonator (wave10/fx, INTERFACES.md §12.3): the voice through a feedback comb tuned to a note, so peaks stand on the
// note's harmonic series (a metal tube, a machine's body). Loop: one period of delay, damping and the feedback g that
// gives the chosen ring time (T60 of the fundamental). The damping (明るさ) is a symmetric 5-tap low-pass read straight
// around the delay tap, (b z + 1 - 2b + b / z)^2 with b = 0.25 (dark) .. 0 (bright): linear phase, so it delays every
// frequency by exactly the period and the peaks stay on the harmonic series at any brightness (a one-pole would bend
// the upper ones sharp). The note glides 30 ms (cubic reads of a fractional delay); ring time and brightness are
// recomputed every 32 samples from 30 ms ramps. g < 1 and the damping never exceeds 1, so it cannot oscillate.
// Level: the comb input is scaled by (1 - g), so no frequency is ever boosted; then the wet is brought back to the
// input's loudness: input power over 100 ms against the comb's over 20 ms, gain at most +36 dB, following that ratio
// (rises 100 ms, falls 10 ms, so it does not overshoot while a ring builds up) only while the input is present (above
// -80 dBFS and within 10 dB of its 1 s level), so a pause lets the tail ring out instead of being pulled down. Very high
// notes at the longest ring still come out quieter (B6: -12 dB on the synthetic voice: its harmonics rarely meet the
// narrow peaks). Latency 0.

namespace koe
{
namespace
{
constexpr int kMinMidi = 24; // C1, the lowest note (octave 1)
constexpr float kMaxBoostDb = 36.0f;

class ResonatorEffect final : public IEffect
{
public:
    void prepare (double sampleRate, int) override
    {
        sr = sampleRate;
        line.prepare (int (sr / noteHz (kMinMidi)) + 8);
        delay.prepare (sr, 30.0f);
        decay.prepare (sr, 30.0f);
        bright.prepare (sr, 30.0f);
        mix.prepare (sr, 30.0f);
        dcCut.setHighpass (sr, 30.0f);
        inCoef = dsp::onePoleCoeff (100.0f, sr);
        outCoef = dsp::onePoleCoeff (20.0f, sr);
        slowCoef = dsp::onePoleCoeff (1000.0f, sr);
        gainUp = dsp::onePoleCoeff (100.0f, sr);
        gainDown = dsp::onePoleCoeff (10.0f, sr);
        maxBoost = dsp::dbToGain (kMaxBoostDb);
    }

    void reset() override
    {
        delay.snap (periodSamples());
        decay.snap (decay.target);
        bright.snap (bright.target);
        mix.snap (mix.target);
        line.reset();
        dcCut.reset();
        pIn = pOut = pSlow = 0.0f;
        gain = 1.0f;
        sinceCoeffs = 0;
    }

    void setParam (int index, float v) override
    {
        switch (index)
        {
            case 0: note = int (v); delay.setTarget (periodSamples()); break;
            case 1: octave = int (v); delay.setTarget (periodSamples()); break;
            case 2: decay.setTarget (v); break;
            case 3: bright.setTarget (v); break;
            case 4: mix.setTarget (v); break;
            default: break;
        }
    }

    void process (float* x, int n) override
    {
        juce::ScopedNoDenormals noDenormals;
        for (int i = 0; i < n; ++i)
        {
            const float period = delay.next();
            const float decayMs = decay.next(), brightness = bright.next();
            if (sinceCoeffs-- <= 0)
            {
                updateCoefficients (period, decayMs, brightness);
                sinceCoeffs = 31;
            }
            const float in = x[i];
            const float d = std::max (3.0f, period - 1.0f); // y[n - period] is d samples back before the push
            const float back = c0 * (line.readCubic (d - 2.0f) + line.readCubic (d + 2.0f))
                             + c1 * (line.readCubic (d - 1.0f) + line.readCubic (d + 1.0f)) + c2 * line.readCubic (d);
            const float y = (1.0f - g) * dcCut.process (in) + g * back;
            line.push (y);

            const float pIn1 = in * in, py = y * y;
            pIn = pIn1 + inCoef * (pIn - pIn1);
            pOut = py + outCoef * (pOut - py);
            pSlow = pIn1 + slowCoef * (pSlow - pIn1);
            if (pIn > 1.0e-8f && pIn > 0.1f * pSlow)
            {
                const float target = std::min (maxBoost, std::sqrt (pIn / (pOut + 1.0e-12f)));
                gain = target + (target > gain ? gainUp : gainDown) * (gain - target);
            }
            const float m = mix.next();
            x[i] = in + m * (gain * y - in);
        }
    }

private:
    static float noteHz (int midi) noexcept { return 440.0f * std::pow (2.0f, float (midi - 69) / 12.0f); }
    float periodSamples() const noexcept { return float (sr) / noteHz (kMinMidi + 12 * (octave - 1) + note); }

    void updateCoefficients (float period, float decayMs, float brightness) noexcept
    {
        const float b = 0.25f * (1.0f - brightness);
        c0 = b * b;
        c1 = 2.0f * b * (1.0f - 2.0f * b);
        c2 = (1.0f - 2.0f * b) * (1.0f - 2.0f * b) + 2.0f * b * b;
        g = std::min (0.9995f, std::pow (10.0f, -3.0f * period / (decayMs * 0.001f * float (sr))));
    }

    double sr = 48000.0;
    dsp::DelayLine line;
    dsp::Biquad dcCut;
    dsp::Ramp delay, decay, bright, mix;
    int note = 0, octave = 3;
    float c0 = 0.0f, c1 = 0.0f, c2 = 1.0f, g = 0.0f;
    float inCoef = 0.0f, outCoef = 0.0f, slowCoef = 0.0f, gainUp = 0.0f, gainDown = 0.0f, maxBoost = 1.0f;
    float pIn = 0.0f, pOut = 0.0f, pSlow = 0.0f, gain = 1.0f;
    int sinceCoeffs = 0;
};
} // namespace

KOE_REGISTER_EFFECT ("resonator", ResonatorEffect)
} // namespace koe

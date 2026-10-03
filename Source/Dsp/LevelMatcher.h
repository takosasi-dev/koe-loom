#pragma once

#include "Dsp/Building.h"

#include <algorithm>
#include <cmath>

namespace koe::dsp
{
/**
    Keeps a converter's output level equal to its (latency-aligned) input level (F-02-12).
    Pitch shifting with preserved formants changes the energy (fewer/more harmonics under the same
    envelope, roughly -3 dB per octave up), so a slow RMS ratio follower corrects it. Time constants are
    long (~0.4 s) so it never pumps on syllables; it holds while the input is near silence.
    This is internal to the converter; it is not an output AGC (D-16 forbids that).
*/
class LevelMatcher
{
public:
    void prepare (double sr, int latencySamples)
    {
        latency = std::max (0, latencySamples);
        delay.prepare (latency + 1);
        // long energy averages keep the ratio unbiased by syllable-rate level changes; the ratio of two
        // equally filtered energies is meaningful almost immediately after a reset
        envCoeff = onePoleCoeff (2500.0f, sr);
        gainCoeff = onePoleCoeff (400.0f, sr);
        reset();
    }
    void reset()
    {
        delay.reset();
        inMs = outMs = 0.0f;
        gain = target = 1.0f;
    }
    /** in = converter input (undelayed), out = converter output, corrected in place. */
    void process (const float* in, float* out, int n)
    {
        constexpr float floorMs = 1.0e-7f;        // -70 dBFS
        constexpr float maxG = 3.98f, minG = 0.25f; // +12 / -12 dB
        for (int i = 0; i < n; ++i)
        {
            delay.push (in[i]);
            const float d = delay.readInt (latency);
            inMs = d * d + envCoeff * (inMs - d * d);
            outMs = out[i] * out[i] + envCoeff * (outMs - out[i] * out[i]);
            if (inMs > floorMs && outMs > floorMs * 0.01f)
                target = std::clamp (std::sqrt (inMs / outMs), minG, maxG);
            gain = target + gainCoeff * (gain - target);
            out[i] *= gain;
        }
    }

private:
    DelayLine delay;
    int latency = 0;
    float envCoeff = 0.0f, gainCoeff = 0.0f;
    float inMs = 0.0f, outMs = 0.0f, gain = 1.0f, target = 1.0f;
};
} // namespace koe::dsp

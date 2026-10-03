#include "Dsp/Building.h"
#include "Effects/IEffect.h"

#include <juce_audio_basics/juce_audio_basics.h>

#include <algorithm>
#include <cmath>

// modulation (koeloom_effects.md §3, §5.1). One modulated delay line, two cubic-interpolated taps.
//   chorus : taps around 14 ms and 21 ms, LFOs 90 degrees apart, no feedback.
//   flanger: one tap sweeping 0.3 ms .. 0.3 ms + 2A, `feedback` returned into the line; the wet level is
//            scaled by sqrt(1 - fb^2) so strong feedback rings without getting much louder.
//   vibrato: one tap, wet only (`mix` ignored, treated as 1).
// The sweep width A follows `depth` but is capped so the pitch wobble stays musical at high rates
// (chorus 2.5 %, flanger 6 %, vibrato 3 % peak deviation). Tap centres/widths glide (60 ms one-pole)
// and gains/feedback/mix ramp, so mode and knob changes bend smoothly instead of clicking.

namespace koe
{
namespace
{
class ModulationEffect final : public IEffect
{
public:
    void prepare (double sampleRate, int) override
    {
        sr = sampleRate;
        line.prepare (int (sr * 0.05));
        for (auto* r : { &g1, &g2, &fb, &mix }) r->prepare (sr, 40.0f);
        glide = dsp::onePoleCoeff (60.0f, sr);
        updateTargets();
    }

    void reset() override
    {
        line.reset();
        updateTargets();
        for (auto* r : { &g1, &g2, &fb, &mix }) r->snap (r->target);
        for (int k = 0; k < 4; ++k) cur[k] = target[k];
        phase = 0.0;
    }

    void setParam (int index, float v) override
    {
        switch (index)
        {
            case 0: mode = std::clamp (int (v), 0, 2); break;
            case 1: rateHz = v; break;
            case 2: depth = v; break;
            case 3: feedback = v; break;
            case 4: mixParam = v; break;
            default: return;
        }
        updateTargets();
    }

    void process (float* x, int n) override
    {
        juce::ScopedNoDenormals noDenormals;
        const double inc = rateHz / sr;
        const float maxD = float (line.capacity() - 3);
        for (int i = 0; i < n; ++i)
        {
            for (int k = 0; k < 4; ++k) cur[k] = target[k] + glide * (cur[k] - target[k]);
            const float s1 = float (std::sin (2.0 * 3.14159265358979323846 * phase));
            const float s2 = float (std::sin (2.0 * 3.14159265358979323846 * (phase + 0.25)));
            phase += inc;
            if (phase >= 1.0) phase -= 1.0;

            const float tap1 = line.readCubic (std::clamp (cur[0] + cur[1] * s1, 1.0f, maxD));
            const float tap2 = line.readCubic (std::clamp (cur[2] + cur[3] * s2, 1.0f, maxD));
            const float in = x[i];
            line.push (in + fb.next() * tap1);
            const float wet = g1.next() * tap1 + g2.next() * tap2;
            x[i] = in + mix.next() * (wet - in);
        }
    }

private:
    void updateTargets() noexcept
    {
        const float ms = float (sr * 0.001);
        const float perHz = float (sr / (2.0 * 3.14159265358979323846 * rateHz)); // A = deviation * perHz
        float a;
        switch (mode)
        {
            case 0: // chorus
                a = depth * std::min (4.0f * ms, 0.025f * perHz);
                setTarget (14.0f * ms, a, 21.0f * ms, 0.85f * a, 0.6f, 0.6f, 0.0f, mixParam);
                break;
            case 1: // flanger
                a = depth * std::min (3.0f * ms, 0.06f * perHz);
                setTarget (0.3f * ms + a, a, 0.3f * ms + a, a, std::sqrt (1.0f - feedback * feedback), 0.0f, feedback, mixParam);
                break;
            default: // vibrato
                a = depth * std::min (8.0f * ms, 0.03f * perHz);
                setTarget (0.3f * ms + a, a, 0.3f * ms + a, a, 1.0f, 0.0f, 0.0f, 1.0f);
                break;
        }
    }

    void setTarget (float c1, float a1, float c2, float a2, float gain1, float gain2, float feedbackGain, float mixGain) noexcept
    {
        target[0] = c1; target[1] = a1; target[2] = c2; target[3] = a2;
        g1.setTarget (gain1);
        g2.setTarget (gain2);
        fb.setTarget (feedbackGain);
        mix.setTarget (mixGain);
    }

    double sr = 48000.0, phase = 0.0;
    int mode = 0;
    float rateHz = 1.0f, depth = 0.5f, feedback = 0.0f, mixParam = 0.5f, glide = 0.0f;
    float target[4] {}, cur[4] {}; // tap1 centre, tap1 width, tap2 centre, tap2 width (samples)
    dsp::DelayLine line;
    dsp::Ramp g1, g2, fb, mix;
};
} // namespace

KOE_REGISTER_EFFECT ("modulation", ModulationEffect)
} // namespace koe

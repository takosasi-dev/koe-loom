#include "Dsp/Building.h"
#include "Effects/IEffect.h"

#include <juce_audio_basics/juce_audio_basics.h>

#include <algorithm>
#include <cmath>

// phaser (koeloom_effects.md §3, §5.1). A chain of first-order all-pass filters whose break frequency
// sweeps exponentially around 900 Hz (+-2.3 octaves at depth 1: about 180 Hz .. 4.5 kHz) with a sine
// LFO at rateHz; the chain output is fed back to its input (`feedback`). All 12 stages always run and
// the output taps between stage counts with a gliding (40 ms) fractional position, so changing
// `stages` morphs instead of clicking. mix is linear (0.5 gives the deepest notches).

namespace koe
{
namespace
{
constexpr int kMaxStages = 12;

class PhaserEffect final : public IEffect
{
public:
    void prepare (double sampleRate, int) override
    {
        sr = sampleRate;
        for (auto* r : { &stagePos, &depth, &fb, &mix }) r->prepare (sr, 40.0f);
    }

    void reset() override
    {
        for (auto* r : { &stagePos, &depth, &fb, &mix }) r->snap (r->target);
        for (auto& s : x1) s = 0.0f;
        for (auto& s : y1) s = 0.0f;
        last = 0.0f;
        phase = 0.0;
    }

    void setParam (int index, float v) override
    {
        switch (index)
        {
            case 0: stagePos.setTarget (std::clamp (v, 2.0f, float (kMaxStages))); break;
            case 1: rateHz = v; break;
            case 2: depth.setTarget (v); break;
            case 3: fb.setTarget (v); break;
            case 4: mix.setTarget (v); break;
            default: break;
        }
    }

    void process (float* x, int n) override
    {
        juce::ScopedNoDenormals noDenormals;
        const double inc = rateHz / sr;
        const float piOverSr = float (3.14159265358979323846 / sr);
        for (int i = 0; i < n; ++i)
        {
            const float lfo = float (std::sin (2.0 * 3.14159265358979323846 * phase));
            phase += inc;
            if (phase >= 1.0) phase -= 1.0;
            const float f = 900.0f * std::exp2 (depth.next() * 2.3f * lfo);
            const float t = std::tan (piOverSr * f);
            const float a = (t - 1.0f) / (t + 1.0f);

            const float in = x[i];
            float tap[kMaxStages + 1];
            float v = in + fb.next() * last;
            tap[0] = v;
            for (int s = 0; s < kMaxStages; ++s)
            {
                const float y = a * v + x1[s] - a * y1[s];
                x1[s] = v;
                y1[s] = y;
                v = y;
                tap[s + 1] = y;
            }
            const float sp = stagePos.next();
            const int lo = int (sp);
            const float frac = sp - float (lo);
            const float wet = frac > 0.0f ? tap[lo] + frac * (tap[lo + 1] - tap[lo]) : tap[lo];
            last = wet;
            x[i] = in + mix.next() * (wet - in);
        }
    }

private:
    double sr = 48000.0, phase = 0.0;
    float rateHz = 0.5f, last = 0.0f;
    float x1[kMaxStages] {}, y1[kMaxStages] {};
    dsp::Ramp stagePos, depth, fb, mix;
};
} // namespace

KOE_REGISTER_EFFECT ("phaser", PhaserEffect)
} // namespace koe

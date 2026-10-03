#include "Dsp/Building.h"
#include "Effects/IEffect.h"

#include <juce_audio_basics/juce_audio_basics.h>

#include <algorithm>
#include <array>
#include <cmath>

// granular (koeloom_effects.md §5.2). The input is recorded continuously (1.36 s ring). Every
// sr / density samples a grain starts: grainMs long, Hann window, read at 2^(pitchSt/12) times the
// speed (cubic interpolation). Its start lies in the newest 200 ms: delay = the minimum the pitch
// needs (a grain read faster than real time must start far enough back not to overtake the write head)
// + spray * random * 200 ms. Grain length, pitch and position are latched when a grain starts, so
// moving any parameter never clicks. Up to 16 grains overlap (40 / s * 200 ms = 8 at most).
// Level: the grains are divided by max (1, sum of their window values) — a convex mix when they
// overlap, so dense coherent grains (spray 0, pitch 0) are not louder than the voice.
// mix is linear (0 = voice, 1 = grains). Random generator seeded in reset(). Latency 0.

namespace koe
{
namespace
{
class GranularEffect final : public IEffect
{
public:
    void prepare (double sampleRate, int) override
    {
        sr = sampleRate;
        ring.prepare (int (sr * 0.85) + 64); // 200 ms spray + 200 ms * (4 - 1) for +12 st
        mix.prepare (sr, 30.0f);
        sprayRange = float (sr * 0.2);
    }

    void reset() override
    {
        ring.reset();
        for (auto& g : grains) g.left = 0;
        rng.seed (0x6A41u);
        untilNext = 0;
        mix.snap (mix.target);
    }

    void setParam (int index, float v) override
    {
        switch (index)
        {
            case 0: grainMs = v; break;
            case 1: density = v; break;
            case 2: spray = v; break;
            case 3: pitchSt = v; break;
            case 4: mix.setTarget (v); break;
            default: break;
        }
    }

    void process (float* x, int n) override
    {
        juce::ScopedNoDenormals noDenormals;
        for (int i = 0; i < n; ++i)
        {
            const float in = x[i];
            ring.push (in);
            if (--untilNext <= 0)
            {
                startGrain();
                untilNext = std::max (1, int (sr / density));
            }

            float sum = 0.0f, wsum = 0.0f;
            for (auto& g : grains)
            {
                if (g.left <= 0) continue;
                const float w = 0.5f - 0.5f * std::cos (g.phase);
                sum += w * ring.readCubic (g.delay);
                wsum += w;
                g.phase += g.phaseInc;
                g.delay += g.drift;
                --g.left;
            }
            const float wet = sum / std::max (1.0f, wsum);
            const float m = mix.next();
            x[i] = in + m * (wet - in);
        }
    }

private:
    struct Grain
    {
        float delay = 0.0f, drift = 0.0f, phase = 0.0f, phaseInc = 0.0f;
        int left = 0;
    };

    void startGrain() noexcept
    {
        Grain* g = nullptr;
        for (auto& c : grains)
            if (c.left <= 0) { g = &c; break; }
        if (g == nullptr) return; // all busy (cannot happen within the parameter ranges)
        const int len = std::max (8, int (dsp::msToSamples (grainMs, sr)));
        const float rate = std::exp2 (pitchSt / 12.0f);
        const float minDelay = 2.0f + std::max (0.0f, float (len) * (rate - 1.0f));
        g->delay = minDelay + spray * rng.nextFloat01() * sprayRange;
        g->drift = 1.0f - rate; // the write head moves 1, the read head `rate` per sample
        g->phase = 0.0f;
        g->phaseInc = dsp::kTwoPi / float (len);
        g->left = len;
    }

    double sr = 48000.0;
    dsp::DelayLine ring;
    std::array<Grain, 16> grains {};
    dsp::Rng rng;
    dsp::Ramp mix;
    float grainMs = 60.0f, density = 12.0f, spray = 0.3f, pitchSt = 0.0f, sprayRange = 9600.0f;
    int untilNext = 0;
};
} // namespace

KOE_REGISTER_EFFECT ("granular", GranularEffect)
} // namespace koe

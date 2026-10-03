#include "Dsp/Building.h"
#include "Effects/IEffect.h"

#include <juce_audio_basics/juce_audio_basics.h>

#include <algorithm>
#include <cmath>

// ensemble (koeloom_effects.md §3, §5.1). Up to 8 taps on one delay line, centres spread 9..34 ms.
// Each voice has its own slow LFO (rate spread 0.7x..1.3x by the golden ratio, evenly spaced phases)
// plus a small fast "shimmer" LFO, so the voices never move together - many slightly late, slightly
// detuned copies = a crowd. Peak pitch deviation is capped at 1.2 % (x depth). Voice gains ramp
// (50 ms) to 1/sqrt(voices), so the wet level stays put and changing `voices` fades them in/out.
// LFOs run at control rate (every 16 samples) with per-sample linear delay interpolation. mix is linear.

namespace koe
{
namespace
{
constexpr int kMaxVoices = 8;
constexpr int kCtrl = 16;

class EnsembleEffect final : public IEffect
{
public:
    void prepare (double sampleRate, int) override
    {
        sr = sampleRate;
        line.prepare (int (sr * 0.05));
        for (auto& g : gains) g.prepare (sr, 50.0f);
        depth.prepare (sr, 40.0f);
        mix.prepare (sr, 40.0f);
        updateGains();
    }

    void reset() override
    {
        line.reset();
        updateGains();
        for (auto& g : gains) g.snap (g.target);
        depth.snap (depth.target);
        mix.snap (mix.target);
        for (int v = 0; v < kMaxVoices; ++v)
        {
            slow[v] = double (v) / kMaxVoices;
            fast[v] = std::fmod (double (v) * 0.37, 1.0);
        }
        ctrlLeft = 0;
        computeDelays (delayNow);
        for (int v = 0; v < kMaxVoices; ++v) delayStep[v] = 0.0f;
    }

    void setParam (int index, float v) override
    {
        switch (index)
        {
            case 0: voices = std::clamp (int (v), 2, kMaxVoices); updateGains(); break;
            case 1: rateHz = v; break;
            case 2: depth.setTarget (v); break;
            case 3: mix.setTarget (v); break;
            default: break;
        }
    }

    void process (float* x, int n) override
    {
        juce::ScopedNoDenormals noDenormals;
        for (int i = 0; i < n; ++i)
        {
            depth.next();
            if (ctrlLeft-- <= 0)
            {
                float next[kMaxVoices];
                for (int v = 0; v < kMaxVoices; ++v) { slow[v] += slowInc (v); fast[v] += 5.3 * slowInc (v); }
                for (int v = 0; v < kMaxVoices; ++v) { slow[v] -= std::floor (slow[v]); fast[v] -= std::floor (fast[v]); }
                computeDelays (next);
                for (int v = 0; v < kMaxVoices; ++v) delayStep[v] = (next[v] - delayNow[v]) / float (kCtrl);
                ctrlLeft = kCtrl - 1;
            }
            const float in = x[i];
            line.push (in);
            float wet = 0.0f;
            for (int v = 0; v < kMaxVoices; ++v)
            {
                delayNow[v] += delayStep[v];
                const float g = gains[v].next();
                if (g > 0.0f) wet += g * line.readLinear (delayNow[v]);
            }
            x[i] = in + mix.next() * (wet - in);
        }
    }

private:
    double slowInc (int v) const noexcept { return rateHz * (0.7 + 0.6 * std::fmod (v * 0.6180339887, 1.0)) * kCtrl / sr; }

    void computeDelays (float* out) noexcept
    {
        const float ms = float (sr * 0.001);
        const float d = depth.value;
        for (int v = 0; v < kMaxVoices; ++v)
        {
            const float voiceRate = float (slowInc (v) * sr / kCtrl);
            const float width = d * std::min (4.0f * ms, 0.012f * float (sr) / (2.0f * dsp::kPi * voiceRate));
            const float centre = (9.0f + 25.0f * float (v) / float (kMaxVoices - 1)) * ms;
            out[v] = centre + width * (float (std::sin (dsp::kTwoPi * slow[v])) + 0.15f * float (std::sin (dsp::kTwoPi * fast[v])));
        }
    }

    void updateGains() noexcept
    {
        const float g = 1.0f / std::sqrt (float (voices));
        for (int v = 0; v < kMaxVoices; ++v) gains[v].setTarget (v < voices ? g : 0.0f);
    }

    double sr = 48000.0;
    double slow[kMaxVoices] {}, fast[kMaxVoices] {};
    float delayNow[kMaxVoices] {}, delayStep[kMaxVoices] {};
    int voices = 4, ctrlLeft = 0;
    float rateHz = 0.8f;
    dsp::DelayLine line;
    dsp::Ramp gains[kMaxVoices];
    dsp::Ramp depth, mix;
};
} // namespace

KOE_REGISTER_EFFECT ("ensemble", EnsembleEffect)
} // namespace koe

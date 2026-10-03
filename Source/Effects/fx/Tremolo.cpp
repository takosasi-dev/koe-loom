#include "Dsp/Building.h"
#include "Effects/IEffect.h"

#include <cmath>

// tremolo (koeloom_effects.md §3, §5.1): gain = 1 - depth * (0.5 - 0.5 cos(2 pi phase)), so the level
// swings between 1 and 1 - depth at rateHz. The cycle starts at full level on reset().

namespace koe
{
namespace
{
class TremoloEffect final : public IEffect
{
public:
    void prepare (double sampleRate, int) override
    {
        sr = sampleRate;
        depth.prepare (sr, 30.0f);
    }

    void reset() override
    {
        depth.snap (depth.target);
        phase = 0.0;
    }

    void setParam (int index, float v) override
    {
        if (index == 0) rateHz = v;
        else if (index == 1) depth.setTarget (v);
    }

    void process (float* x, int n) override
    {
        const double inc = rateHz / sr;
        for (int i = 0; i < n; ++i)
        {
            const float lfo = 0.5f - 0.5f * float (std::cos (2.0 * 3.14159265358979323846 * phase));
            x[i] *= 1.0f - depth.next() * lfo;
            phase += inc;
            if (phase >= 1.0) phase -= 1.0;
        }
    }

private:
    double sr = 48000.0, phase = 0.0;
    float rateHz = 5.0f;
    dsp::Ramp depth;
};
} // namespace

KOE_REGISTER_EFFECT ("tremolo", TremoloEffect)
} // namespace koe

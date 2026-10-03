#include "Dsp/Building.h"
#include "Effects/IEffect.h"

#include <cmath>

// ringmod (koeloom_effects.md §3, §5.1): the input times a freqHz sine. The carrier phase is continuous
// and the frequency glides (30 ms), so knob moves never step the waveform. mix is linear.

namespace koe
{
namespace
{
class RingModEffect final : public IEffect
{
public:
    void prepare (double sampleRate, int) override
    {
        sr = sampleRate;
        freq.prepare (sr, 30.0f);
        mix.prepare (sr, 30.0f);
    }

    void reset() override
    {
        freq.snap (freq.target);
        mix.snap (mix.target);
        phase = 0.0;
    }

    void setParam (int index, float v) override
    {
        if (index == 0) freq.setTarget (v);
        else if (index == 1) mix.setTarget (v);
    }

    void process (float* x, int n) override
    {
        for (int i = 0; i < n; ++i)
        {
            const float carrier = float (std::sin (2.0 * 3.14159265358979323846 * phase));
            const float m = mix.next();
            x[i] *= (1.0f - m) + m * carrier;
            phase += double (freq.next()) / sr;
            if (phase >= 1.0) phase -= 1.0;
        }
    }

private:
    double sr = 48000.0, phase = 0.0;
    dsp::Ramp freq, mix;
};
} // namespace

KOE_REGISTER_EFFECT ("ringmod", RingModEffect)
} // namespace koe

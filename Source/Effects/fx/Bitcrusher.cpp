#include "Dsp/Building.h"
#include "Effects/IEffect.h"

#include <cmath>

// bitcrusher (koeloom_effects.md §5.1): sample-and-hold at rateHz (phase accumulator, no anti-aliasing
// by design) and mid-tread rounding to 2^(bits-1) steps per polarity (silence stays silent). bits and
// rateHz ramp over 25 ms; fractional bits in between give a continuous step size, so a change never jumps.

namespace koe
{
namespace
{
class BitcrusherEffect final : public IEffect
{
public:
    void prepare (double sampleRate, int) override
    {
        sr = sampleRate;
        for (auto* r : { &bits, &rateHz, &mix }) r->prepare (sr, kRampMs);
    }

    void reset() override
    {
        for (auto* r : { &bits, &rateHz, &mix }) r->snap (r->target);
        phase = 1.0f; // capture the first sample
        held = 0.0f;
    }

    void setParam (int index, float v) override
    {
        switch (index)
        {
            case 0: bits.setTarget (v); break;
            case 1: rateHz.setTarget (v); break;
            case 2: mix.setTarget (v); break;
            default: break;
        }
    }

    void process (float* x, int n) override
    {
        for (int i = 0; i < n; ++i)
        {
            const float steps = std::exp2 (bits.next() - 1.0f), m = mix.next();
            phase += rateHz.next() / float (sr);
            if (phase >= 1.0f)
            {
                phase -= float (int (phase));
                held = x[i];
            }
            const float crushed = std::round (held * steps) / steps;
            x[i] = (1.0f - m) * x[i] + m * crushed;
        }
    }

private:
    static constexpr float kRampMs = 25.0f;

    double sr = 48000.0;
    dsp::Ramp bits, rateHz, mix;
    float phase = 1.0f, held = 0.0f;
};

KOE_REGISTER_EFFECT ("bitcrusher", BitcrusherEffect)
} // namespace
} // namespace koe

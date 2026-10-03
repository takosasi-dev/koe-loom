#include "Dsp/Building.h"
#include "Effects/IEffect.h"

#include <array>

// isolator (koeloom_effects.md §5.1): 3 bands from two 4th-order Linkwitz-Riley crossovers (two cascaded
// Butterworth biquads each). The low band goes through the 2nd-order all-pass that LP4 + HP4 of the upper
// crossover equals, so with all gains at 0 dB the sum is an all-pass: flat magnitude, no comb.
// Band gains ramp linearly (25 ms) in the linear domain; crossovers are recomputed every 16 samples while moving.

namespace koe
{
namespace
{
class IsolatorEffect final : public IEffect
{
public:
    void prepare (double sampleRate, int) override
    {
        sr = sampleRate;
        for (auto& r : gain) r.prepare (sr, kRampMs);
        lowMidHz.prepare (sr, kRampMs);
        midHighHz.prepare (sr, kRampMs);
    }

    void reset() override
    {
        for (auto& r : gain) r.snap (r.target);
        lowMidHz.snap (lowMidHz.target);
        midHighHz.snap (midHighHz.target);
        for (auto& b : f) b.reset();
        design();
        countdown = kCoefInterval;
        pending = false;
    }

    void setParam (int index, float v) override
    {
        if (index >= 0 && index < 3) gain[size_t (index)].setTarget (dsp::dbToGain (v));
        else if (index == 3) lowMidHz.setTarget (v);
        else if (index == 4) midHighHz.setTarget (v);
    }

    void process (float* x, int n) override
    {
        for (int i = 0; i < n; ++i)
        {
            const float gl = gain[0].next(), gm = gain[1].next(), gh = gain[2].next();
            lowMidHz.next();
            midHighHz.next();
            if (--countdown <= 0)
            {
                countdown = kCoefInterval;
                const bool moving = lowMidHz.isRamping() || midHighHz.isRamping();
                if (moving || pending) design();
                pending = moving;
            }
            const float in = x[i];
            const float low = f[ap2].process (f[lp1b].process (f[lp1a].process (in)));
            const float rest = f[hp1b].process (f[hp1a].process (in));
            const float mid = f[lp2b].process (f[lp2a].process (rest));
            const float high = f[hp2b].process (f[hp2a].process (rest));
            x[i] = gl * low + gm * mid + gh * high;
        }
    }

private:
    static constexpr float kRampMs = 25.0f, kButterQ = 0.70710678f;
    static constexpr int kCoefInterval = 16;
    enum { lp1a, lp1b, hp1a, hp1b, lp2a, lp2b, hp2a, hp2b, ap2, numFilters };

    void design() noexcept
    {
        const float f1 = lowMidHz.value, f2 = midHighHz.value;
        f[lp1a].setLowpass (sr, f1); f[lp1b].setLowpass (sr, f1);
        f[hp1a].setHighpass (sr, f1); f[hp1b].setHighpass (sr, f1);
        f[lp2a].setLowpass (sr, f2); f[lp2b].setLowpass (sr, f2);
        f[hp2a].setHighpass (sr, f2); f[hp2b].setHighpass (sr, f2);
        f[ap2].setAllpass (sr, f2, kButterQ);
    }

    double sr = 48000.0;
    std::array<dsp::Ramp, 3> gain;
    dsp::Ramp lowMidHz, midHighHz;
    std::array<dsp::Biquad, numFilters> f;
    int countdown = 0;
    bool pending = false;
};

KOE_REGISTER_EFFECT ("isolator", IsolatorEffect)
} // namespace
} // namespace koe

#include "Dsp/Building.h"
#include "Effects/IEffect.h"

#include <algorithm>
#include <cmath>

// deesser (koeloom_effects.md §5.1): a dynamic peak EQ. The RBJ band-pass (0 dB peak) plus its notch
// add up to the input exactly, so y = x + (g - 1) * band changes only the band, and g = 1 is bit-transparent
// apart from rounding. The band's peak envelope above the threshold is held there (soft knee), capped at
// reductionDb.

namespace koe
{
namespace
{
class DeesserEffect final : public IEffect
{
public:
    void prepare (double sampleRate, int) override
    {
        sr = sampleRate;
        freq.prepare (sr, kRampMs);
        threshold.prepare (sr, kRampMs);
        reduction.prepare (sr, kRampMs);
        env.setTimes (sr, 0.5f, 40.0f);
        grCoef = dsp::onePoleCoeff (2.0f, sr);
    }

    void reset() override
    {
        freq.snap (freq.target);
        threshold.snap (threshold.target);
        reduction.snap (reduction.target);
        band.reset();
        env.reset();
        designedFreq = -1.0f;
        countdown = 0;
        grDb = 0.0f;
    }

    void setParam (int index, float v) override
    {
        switch (index)
        {
            case 0: freq.setTarget (v); break;
            case 1: threshold.setTarget (v); break;
            case 2: reduction.setTarget (v); break;
            default: break;
        }
    }

    void process (float* x, int n) override
    {
        for (int i = 0; i < n; ++i)
        {
            const float f = freq.next(), thr = threshold.next(), red = reduction.next();
            if (--countdown <= 0)
            {
                countdown = kCoefInterval;
                if (f != designedFreq) { band.setBandpass (sr, f, kQ); designedFreq = f; }
            }
            const float b = band.process (x[i]);
            const float over = dsp::gainToDb (env.process (b)) - thr;
            float target = over >= 0.5f * kKneeDb ? over : (over > -0.5f * kKneeDb ? (over + 0.5f * kKneeDb) * (over + 0.5f * kKneeDb) / (2.0f * kKneeDb) : 0.0f);
            target = std::min (target, red);
            grDb = target + grCoef * (grDb - target);
            if (grDb > 1.0e-4f) x[i] += (dsp::dbToGain (-grDb) - 1.0f) * b;
        }
    }

private:
    static constexpr float kRampMs = 25.0f, kKneeDb = 6.0f, kQ = 1.2f;
    static constexpr int kCoefInterval = 16;

    double sr = 48000.0;
    dsp::Ramp freq, threshold, reduction;
    dsp::Biquad band;
    dsp::EnvelopeFollower env;
    float grCoef = 0.0f, grDb = 0.0f, designedFreq = -1.0f;
    int countdown = 0;
};

KOE_REGISTER_EFFECT ("deesser", DeesserEffect)
} // namespace
} // namespace koe

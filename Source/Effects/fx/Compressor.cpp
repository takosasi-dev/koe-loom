#include "Dsp/Building.h"
#include "Effects/IEffect.h"

#include <algorithm>
#include <cmath>

// compressor (koeloom_effects.md §3). Feed-forward, peak level in dB, 6 dB soft knee, attack/release
// applied to the gain reduction in the log domain ("smooth decoupled" detector, Giannoulis et al. 2012).

namespace koe
{
namespace
{
class CompressorEffect final : public IEffect
{
public:
    void prepare (double sampleRate, int) override
    {
        sr = sampleRate;
        threshold.prepare (sr, kRampMs);
        ratio.prepare (sr, kRampMs);
        makeup.prepare (sr, kRampMs);
        setTimes();
    }

    void reset() override
    {
        threshold.snap (threshold.target);
        ratio.snap (ratio.target);
        makeup.snap (makeup.target);
        grDb = 0.0f;
    }

    void setParam (int index, float v) override
    {
        switch (index)
        {
            case 0: threshold.setTarget (v); break;
            case 1: ratio.setTarget (v); break;
            case 2: attackMs = v; setTimes(); break;
            case 3: releaseMs = v; setTimes(); break;
            case 4: makeup.setTarget (v); break;
            default: break;
        }
    }

    void process (float* x, int n) override
    {
        for (int i = 0; i < n; ++i)
        {
            const float thr = threshold.next();
            const float slope = 1.0f - 1.0f / std::max (1.0f, ratio.next());
            const float mk = makeup.next();
            const float over = dsp::gainToDb (std::abs (x[i])) - thr;
            float target = 0.0f;
            if (over >= 0.5f * kKneeDb)
                target = slope * over;
            else if (over > -0.5f * kKneeDb)
            {
                const float o = over + 0.5f * kKneeDb;
                target = slope * o * o / (2.0f * kKneeDb);
            }
            grDb = target + (target > grDb ? att : rel) * (grDb - target);
            x[i] *= dsp::dbToGain (mk - grDb);
        }
    }

private:
    static constexpr float kRampMs = 25.0f, kKneeDb = 6.0f;

    void setTimes() noexcept
    {
        att = dsp::onePoleCoeff (attackMs, sr);
        rel = dsp::onePoleCoeff (releaseMs, sr);
    }

    double sr = 48000.0;
    dsp::Ramp threshold, ratio, makeup;
    float attackMs = 10.0f, releaseMs = 120.0f, att = 0.0f, rel = 0.0f, grDb = 0.0f;
};

KOE_REGISTER_EFFECT ("compressor", CompressorEffect)
} // namespace
} // namespace koe

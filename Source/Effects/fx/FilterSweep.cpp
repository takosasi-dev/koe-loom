#include "Dsp/Building.h"
#include "Effects/IEffect.h"

#include <algorithm>
#include <array>
#include <cmath>

// filtersweep (koeloom_effects.md §5.1): a sine LFO moves the cutoff of a TPT state-variable filter
// (Simper) from baseHz up to depthOct octaves. The LFO starts at its lowest point on reset(). The SVF
// gives all three outputs at once, so a mode change is a 25 ms crossfade instead of a click. Resonant
// gain is tamed to a peak of about +sqrt(Q) in every mode.

namespace koe
{
namespace
{
struct Svf
{
    float k = 1.0f, a1 = 0.0f, a2 = 0.0f, a3 = 0.0f, ic1 = 0.0f, ic2 = 0.0f;

    void set (double sr, float hz, float q) noexcept
    {
        const float g = std::tan (dsp::kPi * std::clamp (hz, 10.0f, float (sr) * 0.45f) / float (sr));
        k = 1.0f / std::max (q, 0.1f);
        a1 = 1.0f / (1.0f + g * (g + k));
        a2 = g * a1;
        a3 = g * a2;
    }
    void process (float x, float& lp, float& bp, float& hp) noexcept
    {
        const float v3 = x - ic2;
        const float v1 = a1 * ic1 + a2 * v3;
        const float v2 = ic2 + a2 * ic1 + a3 * v3;
        ic1 = 2.0f * v1 - ic1;
        ic2 = 2.0f * v2 - ic2;
        lp = v2;
        bp = v1;
        hp = x - k * v1 - v2;
    }
    void reset() noexcept { ic1 = ic2 = 0.0f; }
};

class FilterSweepEffect final : public IEffect
{
public:
    void prepare (double sampleRate, int) override
    {
        sr = sampleRate;
        for (auto* r : { &rateHz, &baseHz, &depthOct, &q, &mix }) r->prepare (sr, kRampMs);
        for (auto& w : modeWeight) w.prepare (sr, kRampMs);
    }

    void reset() override
    {
        for (auto* r : { &rateHz, &baseHz, &depthOct, &q, &mix }) r->snap (r->target);
        for (auto& w : modeWeight) w.snap (w.target);
        svf.reset();
        phase = 0.75f; // sin = -1: the sweep starts at baseHz
        countdown = 0;
    }

    void setParam (int index, float v) override
    {
        switch (index)
        {
            case 0:
                for (size_t m = 0; m < modeWeight.size(); ++m) modeWeight[m].setTarget (int (v) == int (m) ? 1.0f : 0.0f);
                break;
            case 1: rateHz.setTarget (v); break;
            case 2: baseHz.setTarget (v); break;
            case 3: depthOct.setTarget (v); break;
            case 4: q.setTarget (v); break;
            case 5: mix.setTarget (v); break;
            default: break;
        }
    }

    void process (float* x, int n) override
    {
        for (int i = 0; i < n; ++i)
        {
            const float rate = rateHz.next(), base = baseHz.next(), depth = depthOct.next(), qq = q.next(), m = mix.next();
            const float wl = modeWeight[0].next(), wb = modeWeight[1].next(), wh = modeWeight[2].next();
            phase += rate / float (sr);
            if (phase >= 1.0f) phase -= 1.0f;
            if (--countdown <= 0)
            {
                countdown = kCoefInterval;
                const float lfo = 0.5f + 0.5f * std::sin (dsp::kTwoPi * phase);
                svf.set (sr, base * std::exp2 (depth * lfo), qq);
                const float rq = 1.0f / std::sqrt (std::max (qq, 0.1f));
                passGain = std::min (1.0f, rq);
                bandGain = rq;
            }
            float lp, bp, hp;
            const float dry = x[i];
            svf.process (dry, lp, bp, hp);
            const float wet = passGain * (wl * lp + wh * hp) + bandGain * wb * bp;
            x[i] = (1.0f - m) * dry + m * wet;
        }
    }

private:
    static constexpr float kRampMs = 25.0f;
    static constexpr int kCoefInterval = 16;

    double sr = 48000.0;
    dsp::Ramp rateHz, baseHz, depthOct, q, mix;
    std::array<dsp::Ramp, 3> modeWeight; // lowpass, bandpass, highpass
    Svf svf;
    float phase = 0.75f, passGain = 1.0f, bandGain = 1.0f;
    int countdown = 0;
};

KOE_REGISTER_EFFECT ("filtersweep", FilterSweepEffect)
} // namespace
} // namespace koe

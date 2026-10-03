#include "Dsp/Building.h"
#include "Effects/IEffect.h"

#include <algorithm>
#include <cmath>

// autowah (koeloom_effects.md §5.1): the input's peak envelope (5 ms / 80 ms) moves the centre of a
// band-pass from baseHz up to rangeOct octaves. The filter is a TPT state-variable filter (Simper), which
// stays stable and quiet under fast modulation at any Q. Band-pass gain is bp / sqrt(Q) (peak +sqrt(Q)),
// a middle ground between "0 dB peak" (too quiet at high Q) and the raw resonance. The wet path is then
// brought back to the input's loudness (100 ms RMS, up to +18 dB) so the effect keeps the level (spec §5.8).

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
    float bandpass (float x) noexcept
    {
        const float v3 = x - ic2;
        const float v1 = a1 * ic1 + a2 * v3;
        const float v2 = ic2 + a2 * ic1 + a3 * v3;
        ic1 = 2.0f * v1 - ic1;
        ic2 = 2.0f * v2 - ic2;
        return v1;
    }
    void reset() noexcept { ic1 = ic2 = 0.0f; }
};

class AutoWahEffect final : public IEffect
{
public:
    void prepare (double sampleRate, int) override
    {
        sr = sampleRate;
        for (auto* r : { &sensitivity, &baseHz, &rangeOct, &q, &mix }) r->prepare (sr, kRampMs);
        env.setTimes (sr, 5.0f, 80.0f);
        level.prepare (sr, 18.0f);
    }

    void reset() override
    {
        for (auto* r : { &sensitivity, &baseHz, &rangeOct, &q, &mix }) r->snap (r->target);
        env.reset();
        svf.reset();
        level.reset();
        countdown = 0;
    }

    void setParam (int index, float v) override
    {
        switch (index)
        {
            case 0: sensitivity.setTarget (v); break;
            case 1: baseHz.setTarget (v); break;
            case 2: rangeOct.setTarget (v); break;
            case 3: q.setTarget (v); break;
            case 4: mix.setTarget (v); break;
            default: break;
        }
    }

    void process (float* x, int n) override
    {
        for (int i = 0; i < n; ++i)
        {
            const float sens = sensitivity.next(), base = baseHz.next(), range = rangeOct.next(), qq = q.next(), m = mix.next();
            const float e = env.process (x[i]);
            if (--countdown <= 0)
            {
                countdown = kCoefInterval;
                // sensitivity 0..1 = envelope gain -6..+34 dB: at 1 the filter is fully open from about -34 dBFS
                const float pos = std::min (1.0f, e * dsp::dbToGain (-6.0f + 40.0f * sens));
                svf.set (sr, base * std::exp2 (range * pos), qq);
                wetGain = 1.0f / std::sqrt (std::max (qq, 0.1f));
            }
            const float dry = x[i];
            const float wet = level.process (dry, svf.bandpass (dry) * wetGain);
            x[i] = (1.0f - m) * dry + m * wet;
        }
    }

private:
    static constexpr float kRampMs = 25.0f;
    static constexpr int kCoefInterval = 16;

    double sr = 48000.0;
    dsp::Ramp sensitivity, baseHz, rangeOct, q, mix;
    dsp::EnvelopeFollower env;
    Svf svf;
    dsp::RmsMatch level;
    float wetGain = 1.0f;
    int countdown = 0;
};

KOE_REGISTER_EFFECT ("autowah", AutoWahEffect)
} // namespace
} // namespace koe

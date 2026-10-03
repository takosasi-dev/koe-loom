#include "Dsp/Building.h"
#include "Effects/IEffect.h"

#include <algorithm>
#include <array>
#include <cmath>

// formantfilter (koeloom_effects.md §5.1): three parallel band-passes (TPT state-variable filters, 0 dB
// peak) at the F1/F2/F3 of the Japanese vowels あいうえお, linearly interpolated between neighbours for
// fractional `vowel`. Bandwidths 80/100/150 Hz. With lfoRateHz > 0 the position swings by
// ±2*depth around `vowel` (clamped to 0..4); the swing fades in/out over 25 ms when the rate leaves/reaches 0.
// The narrow bands lose a lot of level, so the wet signal is level-matched to the input (100 ms RMS),
// following spec §5.8 (effects do not change the loudness).

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
    float bandpassNormalized (float x) noexcept
    {
        const float v3 = x - ic2;
        const float v1 = a1 * ic1 + a2 * v3;
        const float v2 = ic2 + a2 * ic1 + a3 * v3;
        ic1 = 2.0f * v1 - ic1;
        ic2 = 2.0f * v2 - ic2;
        return k * v1;
    }
    void reset() noexcept { ic1 = ic2 = 0.0f; }
};

/** Scales the wet signal so its RMS (100 ms) follows the dry RMS; holds while the input is below -80 dBFS. */
struct LevelMatch
{
    float coef = 0.0f, pIn = 0.0f, pOut = 0.0f, gain = 1.0f, maxGain = 1.0f;
    void prepare (double sr, float maxGainDb) noexcept { coef = dsp::onePoleCoeff (100.0f, sr); maxGain = dsp::dbToGain (maxGainDb); }
    void reset() noexcept { pIn = pOut = 0.0f; gain = 1.0f; }
    float process (float dry, float wet) noexcept
    {
        pIn = dry * dry + coef * (pIn - dry * dry);
        pOut = wet * wet + coef * (pOut - wet * wet);
        if (pIn > 1.0e-8f) gain = std::min (maxGain, std::sqrt (pIn / (pOut + 1.0e-12f)));
        return wet * gain;
    }
};

class FormantFilterEffect final : public IEffect
{
public:
    void prepare (double sampleRate, int) override
    {
        sr = sampleRate;
        for (auto* r : { &vowel, &lfoRateHz, &depth, &mix, &lfoOn }) r->prepare (sr, kRampMs);
        level.prepare (sr, 24.0f);
    }

    void reset() override
    {
        for (auto* r : { &vowel, &lfoRateHz, &depth, &mix, &lfoOn }) r->snap (r->target);
        for (auto& b : band) b.reset();
        level.reset();
        phase = 0.0f;
        countdown = 0;
    }

    void setParam (int index, float v) override
    {
        switch (index)
        {
            case 0: vowel.setTarget (v); break;
            case 1: lfoRateHz.setTarget (v); lfoOn.setTarget (v > 0.0f ? 1.0f : 0.0f); break;
            case 2: depth.setTarget (v); break;
            case 3: mix.setTarget (v); break;
            default: break;
        }
    }

    void process (float* x, int n) override
    {
        for (int i = 0; i < n; ++i)
        {
            const float v = vowel.next(), rate = lfoRateHz.next(), d = depth.next(), m = mix.next(), on = lfoOn.next();
            phase += rate / float (sr);
            if (phase >= 1.0f) phase -= 1.0f;
            if (on == 0.0f && rate == 0.0f) phase = 0.0f; // next start swings up from the centre
            if (--countdown <= 0)
            {
                countdown = kCoefInterval;
                const float pos = std::clamp (v + on * 2.0f * d * std::sin (dsp::kTwoPi * phase), 0.0f, 4.0f);
                const int i0 = std::min (3, int (pos));
                const float t = pos - float (i0);
                for (size_t k = 0; k < 3; ++k)
                {
                    const float hz = kFormantHz[i0][k] + t * (kFormantHz[i0 + 1][k] - kFormantHz[i0][k]);
                    band[k].set (sr, hz, std::clamp (hz / kBandwidthHz[k], 2.0f, 25.0f));
                }
            }
            const float dry = x[i];
            float wet = 0.0f;
            for (size_t k = 0; k < 3; ++k) wet += kBandGain[k] * band[k].bandpassNormalized (dry);
            x[i] = (1.0f - m) * dry + m * level.process (dry, wet);
        }
    }

private:
    static constexpr float kRampMs = 25.0f;
    static constexpr int kCoefInterval = 16;
    // F1/F2/F3 (Hz) for あ い う え お (adult averages; Japanese う is unrounded, so its F2 is fairly high)
    static constexpr float kFormantHz[5][3] = { { 750, 1200, 2600 }, { 300, 2300, 3000 }, { 350, 1350, 2400 }, { 500, 1900, 2600 }, { 500, 850, 2500 } };
    static constexpr float kBandwidthHz[3] = { 80, 100, 150 };
    static constexpr float kBandGain[3] = { 1.0f, 0.9f, 0.6f };

    double sr = 48000.0;
    dsp::Ramp vowel, lfoRateHz, depth, mix, lfoOn;
    std::array<Svf, 3> band;
    LevelMatch level;
    float phase = 0.0f;
    int countdown = 0;
};

KOE_REGISTER_EFFECT ("formantfilter", FormantFilterEffect)
} // namespace
} // namespace koe

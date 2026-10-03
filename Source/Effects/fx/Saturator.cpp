#include "Dsp/Building.h"
#include "Effects/IEffect.h"

#include <algorithm>
#include <array>
#include <cmath>

// saturator (koeloom_effects.md §5.1). Curve x / (1 + c|x|): tape uses c = 1 on both halves (very soft,
// odd harmonics), tube uses c = 0.4 on the negative half (asymmetric, adds even harmonics). The curve is
// run with first-order antiderivative anti-aliasing (ADAA), then a 10 Hz DC blocker, the toneHz low-pass
// (12 dB/oct) and a level match that keeps the wet RMS (100 ms) at the input's (spec §5.8).
// A mode change crossfades the two curves over 25 ms.

namespace koe
{
namespace
{
/** Antiderivative of x / (1 + c|x|), with c chosen by the sign of x (both branches are 0 at x = 0). */
inline double curveIntegral (double x, double cNeg) noexcept
{
    const double c = x >= 0.0 ? 1.0 : cNeg, a = std::abs (x);
    return a / c - std::log1p (c * a) / (c * c);
}

inline double curve (double x, double cNeg) noexcept { return x / (1.0 + (x >= 0.0 ? 1.0 : cNeg) * std::abs (x)); }

inline float adaa (double x, double x1, double cNeg) noexcept
{
    const double dx = x - x1;
    if (std::abs (dx) < 1.0e-5) return float (curve (0.5 * (x + x1), cNeg));
    return float ((curveIntegral (x, cNeg) - curveIntegral (x1, cNeg)) / dx);
}

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

class SaturatorEffect final : public IEffect
{
public:
    void prepare (double sampleRate, int) override
    {
        sr = sampleRate;
        for (auto* r : { &driveGain, &toneHz, &mix }) r->prepare (sr, kRampMs);
        for (auto& w : modeWeight) w.prepare (sr, kRampMs);
        dcBlock.setCutoff (sr, 10.0f);
        level.prepare (sr, 12.0f);
    }

    void reset() override
    {
        for (auto* r : { &driveGain, &toneHz, &mix }) r->snap (r->target);
        for (auto& w : modeWeight) w.snap (w.target);
        dcBlock.reset();
        tone.reset();
        level.reset();
        prevIn = 0.0;
        designedTone = -1.0f;
        countdown = 0;
    }

    void setParam (int index, float v) override
    {
        switch (index)
        {
            case 0:
                for (size_t m = 0; m < modeWeight.size(); ++m) modeWeight[m].setTarget (int (v) == int (m) ? 1.0f : 0.0f);
                break;
            case 1: driveGain.setTarget (dsp::dbToGain (v)); break;
            case 2: toneHz.setTarget (v); break;
            case 3: mix.setTarget (v); break;
            default: break;
        }
    }

    void process (float* x, int n) override
    {
        for (int i = 0; i < n; ++i)
        {
            const float dry = x[i];
            const double in = double (dry * driveGain.next());
            const float tf = toneHz.next(), m = mix.next(), wTape = modeWeight[0].next(), wTube = modeWeight[1].next();
            if (--countdown <= 0)
            {
                countdown = kCoefInterval;
                if (tf != designedTone) { tone.setLowpass (sr, tf); designedTone = tf; }
            }
            float s = 0.0f;
            if (wTape > 0.0f) s += wTape * adaa (in, prevIn, 1.0);
            if (wTube > 0.0f) s += wTube * adaa (in, prevIn, kTubeNeg);
            prevIn = in;
            s -= dcBlock.process (s);
            const float wet = level.process (dry, tone.process (s));
            x[i] = (1.0f - m) * dry + m * wet;
        }
    }

private:
    static constexpr float kRampMs = 25.0f;
    static constexpr double kTubeNeg = 0.4;
    static constexpr int kCoefInterval = 16;

    double sr = 48000.0;
    dsp::Ramp driveGain, toneHz, mix;
    std::array<dsp::Ramp, 2> modeWeight; // tape, tube
    dsp::OnePoleLowpass dcBlock;
    dsp::Biquad tone;
    LevelMatch level;
    double prevIn = 0.0;
    float designedTone = -1.0f;
    int countdown = 0;
};

KOE_REGISTER_EFFECT ("saturator", SaturatorEffect)
} // namespace
} // namespace koe

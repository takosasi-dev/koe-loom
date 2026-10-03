#include "Dsp/Building.h"
#include "Effects/IEffect.h"

#include <algorithm>
#include <array>
#include <cmath>

// distortion (koeloom_effects.md §5.1). overdrive = tanh, distortion = hard clip, fuzz = tanh with a 0.6
// bias and +6 dB more drive (strongly asymmetric). Each curve runs with first-order antiderivative
// anti-aliasing (ADAA), which matters at 36 dB of drive. Then a 10 Hz DC blocker, the toneHz low-pass
// (12 dB/oct) and a level match that keeps the wet RMS (100 ms) at the input's, so drive changes the
// character, not the loudness (spec §5.8). A shape change crossfades over 25 ms.

namespace koe
{
namespace
{
constexpr double kFuzzBias = 0.6;
const double kTanhFuzzBias = std::tanh (kFuzzBias);

inline double logCosh (double x) noexcept
{
    const double a = std::abs (x);
    return a + std::log1p (std::exp (-2.0 * a)) - 0.69314718055994531;
}

inline double curve (int shape, double x) noexcept
{
    switch (shape)
    {
        case 0: return std::tanh (x);
        case 1: return std::clamp (x, -1.0, 1.0);
        default: return std::tanh (x + kFuzzBias) - kTanhFuzzBias;
    }
}

inline double curveIntegral (int shape, double x) noexcept
{
    switch (shape)
    {
        case 0: return logCosh (x);
        case 1: { const double a = std::abs (x); return a <= 1.0 ? 0.5 * x * x : a - 0.5; }
        default: return logCosh (x + kFuzzBias) - x * kTanhFuzzBias;
    }
}

inline float adaa (int shape, double x, double x1) noexcept
{
    const double dx = x - x1;
    if (std::abs (dx) < 1.0e-5) return float (curve (shape, 0.5 * (x + x1)));
    return float ((curveIntegral (shape, x) - curveIntegral (shape, x1)) / dx);
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

class DistortionEffect final : public IEffect
{
public:
    void prepare (double sampleRate, int) override
    {
        sr = sampleRate;
        for (auto* r : { &driveGain, &toneHz, &mix }) r->prepare (sr, kRampMs);
        for (auto& w : shapeWeight) w.prepare (sr, kRampMs);
        dcBlock.setCutoff (sr, 10.0f);
        level.prepare (sr, 12.0f);
    }

    void reset() override
    {
        for (auto* r : { &driveGain, &toneHz, &mix }) r->snap (r->target);
        for (auto& w : shapeWeight) w.snap (w.target);
        dcBlock.reset();
        tone.reset();
        level.reset();
        prevIn.fill (0.0);
        designedTone = -1.0f;
        countdown = 0;
    }

    void setParam (int index, float v) override
    {
        switch (index)
        {
            case 0:
                for (size_t s = 0; s < shapeWeight.size(); ++s) shapeWeight[s].setTarget (int (v) == int (s) ? 1.0f : 0.0f);
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
            const float tf = toneHz.next(), m = mix.next();
            if (--countdown <= 0)
            {
                countdown = kCoefInterval;
                if (tf != designedTone) { tone.setLowpass (sr, tf); designedTone = tf; }
            }
            float s = 0.0f;
            for (int k = 0; k < 3; ++k)
            {
                const double xk = k == 2 ? 2.0 * in : in; // fuzz: +6 dB
                const float w = shapeWeight[size_t (k)].next();
                if (w > 0.0f) s += w * adaa (k, xk, prevIn[size_t (k)]);
                prevIn[size_t (k)] = xk;
            }
            s -= dcBlock.process (s);
            const float wet = level.process (dry, tone.process (s));
            x[i] = (1.0f - m) * dry + m * wet;
        }
    }

private:
    static constexpr float kRampMs = 25.0f;
    static constexpr int kCoefInterval = 16;

    double sr = 48000.0;
    dsp::Ramp driveGain, toneHz, mix;
    std::array<dsp::Ramp, 3> shapeWeight; // overdrive, distortion, fuzz
    std::array<double, 3> prevIn {};
    dsp::OnePoleLowpass dcBlock;
    dsp::Biquad tone;
    LevelMatch level;
    float designedTone = -1.0f;
    int countdown = 0;
};

KOE_REGISTER_EFFECT ("distortion", DistortionEffect)
} // namespace
} // namespace koe

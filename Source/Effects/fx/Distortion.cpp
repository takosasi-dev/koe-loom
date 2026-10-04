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
// Wave 7 shapes (appended, the first three are untouched):
//   rectifier  0.8 |tanh x| + 0.2 tanh x (mostly full-wave: octave-up even harmonics, a little of the
//              fundamental kept like a half-wave), ADAA, then an 80 Hz high-pass for the rectified DC/envelope
//   hardclip   250 Hz high-pass + 900 Hz +6 dB push and +6 dB more gain before the hard clip (tight and buzzy)
//   wavefolder sin(pi/2 x) with ADAA; the folder gets half the drive in dB (2x .. 15.9x), so 36 dB folds about
//              four times instead of thirty (keeps the aliasing down without oversampling; latency stays 0)
// A new shape's own filters are cleared when it is selected from silence (weight 0).

namespace koe
{
namespace
{
constexpr double kFuzzBias = 0.6;
const double kTanhFuzzBias = std::tanh (kFuzzBias);
constexpr double kHalfPi = 1.57079632679489661923;
enum Shape { overdrive, distortion, fuzz, rectifier, hardclip, wavefolder, kNumShapes };

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
        case 3: return 0.8 * std::abs (std::tanh (x)) + 0.2 * std::tanh (x);
        case 4: return std::sin (kHalfPi * x);
        default: return std::tanh (x + kFuzzBias) - kTanhFuzzBias;
    }
}

inline double curveIntegral (int shape, double x) noexcept
{
    switch (shape)
    {
        case 0: return logCosh (x);
        case 1: { const double a = std::abs (x); return a <= 1.0 ? 0.5 * x * x : a - 0.5; }
        case 3: return (x >= 0.0 ? 1.0 : -0.6) * logCosh (x); // 0.8 sign(x) logCosh + 0.2 logCosh
        case 4: return -std::cos (kHalfPi * x) / kHalfPi;
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
        rectHp.setHighpass (sr, 80.0f, 0.7071f);
        clipHp.setHighpass (sr, 250.0f, 0.7071f);
        clipPush.setPeak (sr, 900.0f, 0.8f, 6.0f);
    }

    void reset() override
    {
        for (auto* r : { &driveGain, &toneHz, &mix }) r->snap (r->target);
        for (auto& w : shapeWeight) w.snap (w.target);
        dcBlock.reset();
        tone.reset();
        level.reset();
        prevIn.fill (0.0);
        for (int s = rectifier; s < kNumShapes; ++s) clearShape (s);
        designedTone = -1.0f;
        countdown = 0;
    }

    void setParam (int index, float v) override
    {
        switch (index)
        {
            case 0:
                for (size_t s = 0; s < shapeWeight.size(); ++s)
                {
                    const bool on = int (v) == int (s);
                    if (on && s >= size_t (rectifier) && shapeWeight[s].value == 0.0f) clearShape (int (s));
                    shapeWeight[s].setTarget (on ? 1.0f : 0.0f);
                }
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
            const float g = driveGain.next();
            const double in = double (dry * g);
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
            if (const float w = shapeWeight[rectifier].next(); w > 0.0f)
                s += w * rectHp.process (adaa (3, in, prevIn[rectifier]));
            prevIn[rectifier] = in;
            if (const float w = shapeWeight[hardclip].next(); w > 0.0f)
            {
                const double xk = 2.0 * double (clipPush.process (clipHp.process (float (in))));
                s += w * adaa (1, xk, prevIn[hardclip]);
                prevIn[hardclip] = xk;
            }
            {
                const double xk = 2.0 * double (dry) * std::sqrt (double (g));
                if (const float w = shapeWeight[wavefolder].next(); w > 0.0f) s += w * adaa (4, xk, prevIn[wavefolder]);
                prevIn[wavefolder] = xk;
            }
            s -= dcBlock.process (s);
            const float wet = level.process (dry, tone.process (s));
            x[i] = (1.0f - m) * dry + m * wet;
        }
    }

private:
    void clearShape (int s) noexcept
    {
        if (s == rectifier) rectHp.reset();
        if (s == hardclip) { clipHp.reset(); clipPush.reset(); prevIn[hardclip] = 0.0; }
    }

    static constexpr float kRampMs = 25.0f;
    static constexpr int kCoefInterval = 16;

    double sr = 48000.0;
    dsp::Ramp driveGain, toneHz, mix;
    std::array<dsp::Ramp, kNumShapes> shapeWeight; // Shape order
    std::array<double, kNumShapes> prevIn {};
    dsp::Biquad rectHp, clipHp, clipPush;
    dsp::OnePoleLowpass dcBlock;
    dsp::Biquad tone;
    LevelMatch level;
    float designedTone = -1.0f;
    int countdown = 0;
};

KOE_REGISTER_EFFECT ("distortion", DistortionEffect)
} // namespace
} // namespace koe

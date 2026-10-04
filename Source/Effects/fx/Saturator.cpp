#include "Dsp/Building.h"
#include "Effects/IEffect.h"

#include <algorithm>
#include <array>
#include <cmath>

// saturator (koeloom_effects.md §5.1). Curve x / (1 + c|x|): tape uses c = 1 on both halves (very soft,
// odd harmonics), tube uses c = 0.4 on the negative half (asymmetric, adds even harmonics). The curve is
// run with first-order antiderivative anti-aliasing (ADAA), then a 10 Hz DC blocker, the toneHz low-pass
// (12 dB/oct) and a level match that keeps the wet RMS (100 ms) at the input's (spec §5.8).
// A mode change crossfades the curves over 25 ms.
// Wave 7 modes (appended, tape / tube untouched):
//   transistor tanh x on the positive half, 0.5 tanh 2x on the negative half (clips earlier and harder on one
//              side: hard, asymmetric, even + odd harmonics), with ADAA
//   tapewear   the tape curve, then a worn transport: 2 ms delay swaying with wow (0.5 Hz) and flutter (6.5 Hz
//              + 1.3 Hz), a -9 dB high shelf at 3 kHz (worn heads) and random level dropouts (up to -3 dB).
//              The dry path is delayed by the 2 ms centre while tapewear is selected; getLatencySamples() reports it.

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

inline double logCosh (double x) noexcept
{
    const double a = std::abs (x);
    return a + std::log1p (std::exp (-2.0 * a)) - 0.69314718055994531;
}

/** transistor: tanh x for x >= 0, 0.5 tanh 2x below (antiderivatives logCosh x and logCosh(2x) / 4). */
inline float adaaTransistor (double x, double x1) noexcept
{
    auto f = [] (double v) { return v >= 0.0 ? std::tanh (v) : 0.5 * std::tanh (2.0 * v); };
    auto F = [] (double v) { return v >= 0.0 ? logCosh (v) : 0.25 * logCosh (2.0 * v); };
    const double dx = x - x1;
    if (std::abs (dx) < 1.0e-5) return float (f (0.5 * (x + x1)));
    return float ((F (x) - F (x1)) / dx);
}

enum Mode { tape, tube, transistor, tapewear, kNumModes };
constexpr float kWearCentreMs = 2.0f;

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
        centre = std::max (1, int (std::lround (kWearCentreMs * 0.001 * sr)));
        wearLine.prepare (2 * centre + 8);
        dryLine.prepare (2 * centre + 8);
        wow.setRate (sr, 0.5f);
        flutter.setRate (sr, 6.5f);
        drift.setRate (sr, 1.3f);
        headLoss.setHighShelf (sr, 3000.0f, -9.0f);
        dropSmooth.setCutoff (sr, 8.0f);
        dropInterval = std::max (1, int (0.05 * sr));
    }

    void reset() override
    {
        for (auto* r : { &driveGain, &toneHz, &mix }) r->snap (r->target);
        for (auto& w : modeWeight) w.snap (w.target);
        dcBlock.reset();
        tone.reset();
        level.reset();
        prevIn = 0.0;
        clearWear();
        designedTone = -1.0f;
        countdown = 0;
    }

    void setParam (int index, float v) override
    {
        switch (index)
        {
            case 0:
                mode = int (v);
                if (mode == tapewear && modeWeight[tapewear].value == 0.0f) clearWear();
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
            float dryOut = dry;
            if (const float w = modeWeight[transistor].next(); w > 0.0f) s += w * adaaTransistor (in, prevIn);
            if (const float w = modeWeight[tapewear].next(); w > 0.0f)
            {
                s += w * wear (adaa (in, prevIn, 1.0));
                dryLine.push (dry);
                dryOut = dry + w * (dryLine.readInt (centre) - dry);
            }
            prevIn = in;
            s -= dcBlock.process (s);
            const float wet = level.process (dry, tone.process (s));
            x[i] = (1.0f - m) * dryOut + m * wet;
        }
    }

    int getLatencySamples() const override { return mode == tapewear ? centre : 0; }

private:
    /** Worn transport: swaying delay, head loss, dropouts. */
    float wear (float sat) noexcept
    {
        wearLine.push (sat);
        const float swayMs = 0.9f * wow.nextSine() + 0.06f * flutter.nextSine() + 0.15f * drift.nextSine();
        float t = headLoss.process (wearLine.readCubic (float (centre) + swayMs * 0.001f * float (sr)));
        if (--dropCountdown <= 0)
        {
            dropCountdown = dropInterval;
            const float r = rng.nextFloat01();
            dropTarget = 1.0f - 0.3f * r * r * r * r; // mostly near 1, now and then down to -3 dB
        }
        return t * dropSmooth.process (dropTarget);
    }

    void clearWear() noexcept
    {
        wearLine.reset();
        dryLine.reset();
        headLoss.reset();
        dropSmooth.reset();
        dropSmooth.z = 1.0f;
        dropTarget = 1.0f;
        dropCountdown = 0;
        rng.seed (0x7a9e5eedu);
        wow.reset (0.0f);
        flutter.reset (0.0f);
        drift.reset (0.4f);
    }

    static constexpr float kRampMs = 25.0f;
    static constexpr double kTubeNeg = 0.4;
    static constexpr int kCoefInterval = 16;

    double sr = 48000.0;
    dsp::Ramp driveGain, toneHz, mix;
    std::array<dsp::Ramp, kNumModes> modeWeight; // Mode order
    dsp::OnePoleLowpass dcBlock;
    dsp::Biquad tone;
    LevelMatch level;
    double prevIn = 0.0;
    int mode = 0, centre = 96, dropCountdown = 0, dropInterval = 2400;
    dsp::DelayLine wearLine, dryLine;
    dsp::Lfo wow, flutter, drift;
    dsp::Biquad headLoss;
    dsp::OnePoleLowpass dropSmooth;
    dsp::Rng rng;
    float dropTarget = 1.0f;
    float designedTone = -1.0f;
    int countdown = 0;
};

KOE_REGISTER_EFFECT ("saturator", SaturatorEffect)
} // namespace
} // namespace koe

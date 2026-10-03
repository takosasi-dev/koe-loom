#include "Dsp/Building.h"
#include "Effects/IEffect.h"

#include <juce_audio_basics/juce_audio_basics.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <initializer_list>

// whisper (koeloom_effects.md §5.2, F-04-21): a noise-carrier channel vocoder. `bands` constant-Q
// band-passes (each two cascaded 2nd-order sections) from 150 Hz to 9 kHz, log spaced. Each voice band's
// envelope (attack 5 ms, release 50 ms) scales the same band of white noise. White noise has more
// energy in the wider high bands (amplitude ~ sqrt (fc)), so each band is weighted fc^-0.5 (flat), and
// brightness tilts that by fc^(0.5 * brightness) (+-3 dB / oct around 1 kHz; smoothed 30 ms, weights
// recomputed every 32 samples). The result is brought to the input's loudness (100 ms RMS match,
// spec §5.8). Changing `bands` ducks the output for 5 ms, rebuilds the bank, and fades back in.
// mix is linear. Noise seeded in reset(). Latency 0.

namespace koe
{
namespace
{
constexpr int kMaxBands = 32;
constexpr float kNoiseGain = 8.0f; // one band keeps ~1 % of white noise: +18 dB so the match stays well inside its 24 dB

class WhisperEffect final : public IEffect
{
public:
    void prepare (double sampleRate, int) override
    {
        sr = sampleRate;
        duck.prepare (sr, 5.0f);
        mix.prepare (sr, 30.0f);
        bright.prepare (sr, 30.0f);
        match.prepare (sr, 24.0f);
        att = dsp::onePoleCoeff (5.0f, sr);
        rel = dsp::onePoleCoeff (50.0f, sr);
        rebuild();
    }

    void reset() override
    {
        rebuild();
        duck.snap (1.0f);
        mix.snap (mix.target);
        bright.snap (bright.target);
        updateWeights();
        match.reset();
        rng.seed (0x5157u);
        sinceWeights = 0;
    }

    void setParam (int index, float v) override
    {
        switch (index)
        {
            case 0:
                wantBands = std::clamp (int (v), 1, kMaxBands);
                if (wantBands != bands) duck.setTarget (0.0f);
                break;
            case 1: bright.setTarget (v); break;
            case 2: mix.setTarget (v); break;
            default: break;
        }
    }

    void process (float* x, int n) override
    {
        juce::ScopedNoDenormals noDenormals;
        for (int i = 0; i < n; ++i)
        {
            if (wantBands != bands && duck.value <= 0.0f)
            {
                rebuild();
                updateWeights();
                duck.setTarget (1.0f);
            }
            bright.next();
            if (--sinceWeights <= 0) { updateWeights(); sinceWeights = 32; }

            const float in = x[i];
            const float noise = kNoiseGain * rng.nextBipolar();
            float wet = 0.0f;
            for (int k = 0; k < bands; ++k)
            {
                const float a = section (in, za1[k], za2[k], k), y = section (a, zb1[k], zb2[k], k);
                const float r = std::abs (y);
                env[size_t (k)] = r + (r > env[size_t (k)] ? att : rel) * (env[size_t (k)] - r);
                const float c = section (section (noise, zc1[k], zc2[k], k), zd1[k], zd2[k], k);
                wet += c * env[size_t (k)] * weight[size_t (k)];
            }
            wet = match.process (in, wet) * duck.next();
            x[i] = in + mix.next() * (wet - in);
        }
    }

private:
    using Bank = std::array<float, kMaxBands>;

    // band-pass section k (TDF-II; b1 = 0, b2 = -b0)
    float section (float v, float& z1, float& z2, int k) const noexcept
    {
        const float y = b0[size_t (k)] * v + z1;
        z1 = -a1[size_t (k)] * y + z2;
        z2 = -b0[size_t (k)] * v - a2[size_t (k)] * y;
        return y;
    }

    void rebuild() noexcept
    {
        bands = wantBands;
        const float lo = 150.0f, hi = 9000.0f;
        const float bw = std::log2 (hi / lo) / float (bands);  // octaves per band
        const float q = std::sqrt (std::exp2 (bw)) / (std::exp2 (bw) - 1.0f) * 0.8f; // slightly wider: 2 sections
        for (int k = 0; k < bands; ++k)
        {
            const float fc = lo * std::exp2 (bw * (float (k) + 0.5f));
            dsp::Biquad d;
            d.setBandpass (sr, fc, q);
            b0[size_t (k)] = d.b0;
            a1[size_t (k)] = d.a1;
            a2[size_t (k)] = d.a2;
            logFc[size_t (k)] = std::log2 (fc / 1000.0f);
        }
        for (auto* z : { &za1, &za2, &zb1, &zb2, &zc1, &zc2, &zd1, &zd2, &env }) z->fill (0.0f);
    }

    void updateWeights() noexcept
    {
        const float e = 0.5f * (bright.value - 1.0f);
        for (int k = 0; k < bands; ++k) weight[size_t (k)] = std::exp2 (e * logFc[size_t (k)]);
    }

    double sr = 48000.0;
    int bands = 20, wantBands = 20, sinceWeights = 0;
    float att = 0.0f, rel = 0.0f;
    Bank b0 {}, a1 {}, a2 {}, logFc {}, weight {}, env {};
    Bank za1 {}, za2 {}, zb1 {}, zb2 {}, zc1 {}, zc2 {}, zd1 {}, zd2 {};
    dsp::Ramp duck, mix, bright;
    dsp::RmsMatch match;
    dsp::Rng rng;
};
} // namespace

KOE_REGISTER_EFFECT ("whisper", WhisperEffect)
} // namespace koe

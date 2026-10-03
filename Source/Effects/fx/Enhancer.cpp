#include "Dsp/Building.h"
#include "Effects/IEffect.h"

#include <algorithm>
#include <cmath>

// enhancer (koeloom_effects.md §5.1): the band above freqHz (12 dB/oct high-pass) is driven into a
// smooth cubic soft clip with a small even-order term (2nd + 3rd harmonics, C1-continuous at the clip
// point so it adds little aliasing), scaled back by the drive, high-passed again (6 dB/oct at freqHz/2,
// removes the even term's DC) and added to the voice: y = x + mix * h. mix 0 = the voice unchanged.

namespace koe
{
namespace
{
class EnhancerEffect final : public IEffect
{
public:
    void prepare (double sampleRate, int) override
    {
        sr = sampleRate;
        freq.prepare (sr, kRampMs);
        amount.prepare (sr, kRampMs);
        mix.prepare (sr, kRampMs);
    }

    void reset() override
    {
        freq.snap (freq.target);
        amount.snap (amount.target);
        mix.snap (mix.target);
        hp.reset();
        post.reset();
        designedFreq = -1.0f;
        countdown = 0;
    }

    void setParam (int index, float v) override
    {
        switch (index)
        {
            case 0: freq.setTarget (v); break;
            case 1: amount.setTarget (v); break;
            case 2: mix.setTarget (v); break;
            default: break;
        }
    }

    void process (float* x, int n) override
    {
        for (int i = 0; i < n; ++i)
        {
            const float f = freq.next(), a = amount.next(), m = mix.next();
            if (--countdown <= 0)
            {
                countdown = kCoefInterval;
                if (f != designedFreq)
                {
                    hp.setHighpass (sr, f);
                    post.setCutoff (sr, 0.5f * f);
                    designedFreq = f;
                }
                drive = dsp::dbToGain (a * kMaxDriveDb);
            }
            const float v = std::clamp (drive * hp.process (x[i]), -1.0f, 1.0f);
            const float v2 = v * v;
            const float s = (v - v2 * v * (1.0f / 3.0f) + kEven * v2 * (2.0f - v2)) / drive;
            const float h = s - post.process (s);
            x[i] += m * h;
        }
    }

private:
    static constexpr float kRampMs = 25.0f, kMaxDriveDb = 30.0f, kEven = 0.2f;
    static constexpr int kCoefInterval = 16;

    double sr = 48000.0;
    dsp::Ramp freq, amount, mix;
    dsp::Biquad hp;
    dsp::OnePoleLowpass post;
    float drive = 1.0f, designedFreq = -1.0f;
    int countdown = 0;
};

KOE_REGISTER_EFFECT ("enhancer", EnhancerEffect)
} // namespace
} // namespace koe

#include "Dsp/Building.h"
#include "Effects/IEffect.h"

#include <juce_audio_basics/juce_audio_basics.h>

#include <algorithm>
#include <cmath>

// voicechar (koeloom_effects.md §3, §5.1). One chain per kind:
//   HP -> LP (pre-drive) -> presence peak -> compressor -> tanh drive -> HP -> LP x2 (post-drive) -> makeup
// telephone: 350-3400 Hz band, light drive. radio: 220-4800 Hz, asymmetric (tube-like) drive.
// megaphone: 500-4200 Hz, strong 1.6 kHz horn peak, hard drive. walkie: 650-2700 Hz, heavy fast
// compression and drive. `intensity` moves every setting from neutral (0) to the full character (1):
// cutoffs glide exponentially from 20 Hz / 20 kHz, peak/drive/makeup in dB, ratio linearly; below
// intensity 0.05 the chain also fades back to the untouched input, so 0 is exactly the original.
// Changing `kind` crossfades from the old chain to a fresh one (30 ms). mix is linear.

namespace koe
{
namespace
{
struct Character
{
    float hpHz, lpHz, peakHz, peakQ, peakDb, thresholdDb, ratio, driveDb, bias, makeupDb;
};

// makeupDb was set so the synthetic test voice keeps its RMS within about 1 dB at intensity 1.
constexpr Character kCharacters[4] = {
    { 350.0f, 3400.0f, 1800.0f, 1.0f, 4.0f, -26.0f, 3.0f, 6.0f, 0.0f, 8.0f },     // telephone
    { 220.0f, 4800.0f, 2500.0f, 0.8f, 3.0f, -24.0f, 3.0f, 9.0f, 0.25f, 6.5f },    // radio
    { 500.0f, 4200.0f, 1600.0f, 1.4f, 9.0f, -22.0f, 2.0f, 18.0f, 0.1f, 8.3f },    // megaphone
    { 650.0f, 2700.0f, 1400.0f, 1.0f, 5.0f, -34.0f, 8.0f, 14.0f, 0.0f, 20.0f },   // walkie
};

struct Chain
{
    void reset()
    {
        for (auto* f : { &hp1, &hp2, &lpPre, &peak, &lpPost1, &lpPost2 }) f->reset();
        env = 0.0f;
    }

    void configure (double sr, int kind, float intensity)
    {
        const auto& c = kCharacters[kind];
        const float i = intensity;
        const float hp = 20.0f * std::pow (c.hpHz / 20.0f, i);
        const float lp = 20000.0f * std::pow (c.lpHz / 20000.0f, i);
        hp1.setHighpass (sr, hp, 0.5412f);
        hp2.setHighpass (sr, hp, 1.3066f);
        lpPre.setLowpass (sr, std::min (lp * 1.15f, 20000.0f), 0.7071f);
        lpPost1.setLowpass (sr, lp, 0.5412f); // 4th-order Butterworth after the drive
        lpPost2.setLowpass (sr, lp, 1.3066f);
        peak.setPeak (sr, c.peakHz, c.peakQ, c.peakDb * i);
        threshold = c.thresholdDb;
        slope = 1.0f - 1.0f / (1.0f + (c.ratio - 1.0f) * i);
        drive = dsp::dbToGain (c.driveDb * i);
        bias = c.bias * i;
        biasOut = std::tanh (bias);
        makeup = dsp::dbToGain (c.makeupDb * i);
    }

    float process (float x, float att, float rel) noexcept
    {
        float y = peak.process (lpPre.process (hp1.process (x)));
        const float a = std::abs (y);
        env = a > env ? a + att * (env - a) : a + rel * (env - a);
        const float over = dsp::gainToDb (env) - threshold;
        if (over > 0.0f) y *= dsp::dbToGain (-slope * over);
        y = (std::tanh (drive * y + bias) - biasOut) / drive; // unity small-signal gain
        return lpPost2.process (lpPost1.process (hp2.process (y))) * makeup; // hp2 also removes the drive's DC
    }

    dsp::Biquad hp1, hp2, lpPre, peak, lpPost1, lpPost2;
    float env = 0.0f, threshold = 0.0f, slope = 0.0f, drive = 1.0f, bias = 0.0f, biasOut = 0.0f, makeup = 1.0f;
};

class VoiceCharEffect final : public IEffect
{
public:
    void prepare (double sampleRate, int) override
    {
        sr = sampleRate;
        intensity.prepare (sr, 30.0f);
        mix.prepare (sr, 30.0f);
        chainFade.prepare (sr, 30.0f);
        att = dsp::onePoleCoeff (2.0f, sr);
        rel = dsp::onePoleCoeff (90.0f, sr);
    }

    void reset() override
    {
        intensity.snap (intensity.target);
        mix.snap (mix.target);
        chainFade.snap (1.0f);
        cur = 0;
        kindOf[0] = kindOf[1] = kind;
        chains[0].reset();
        chains[1].reset();
        configure();
    }

    void setParam (int index, float v) override
    {
        switch (index)
        {
            case 0:
            {
                const int k = std::clamp (int (v), 0, 3);
                if (k != kind)
                {
                    kind = k;
                    cur ^= 1;
                    kindOf[cur] = k;
                    chains[cur].reset();
                    chains[cur].configure (sr, k, intensity.value);
                    chainFade.snap (0.0f);
                    chainFade.setTarget (1.0f);
                }
                break;
            }
            case 1: intensity.setTarget (v); break;
            case 2: mix.setTarget (v); break;
            default: break;
        }
    }

    void process (float* x, int n) override
    {
        juce::ScopedNoDenormals noDenormals;
        for (int i = 0; i < n; ++i)
        {
            const float it = intensity.next();
            if ((++sinceConfig & 31) == 0 && it != configured) configure();

            const float in = x[i];
            float wet = chains[cur].process (in, att, rel);
            if (chainFade.isRamping())
            {
                const float f = chainFade.next();
                wet = f * wet + (1.0f - f) * chains[cur ^ 1].process (in, att, rel);
            }
            const float engage = std::min (1.0f, it * 20.0f);
            wet = in + engage * (wet - in);
            const float m = mix.next();
            x[i] = in + m * (wet - in);
        }
    }

private:
    void configure() noexcept
    {
        configured = intensity.value;
        chains[0].configure (sr, kindOf[0], intensity.value);
        chains[1].configure (sr, kindOf[1], intensity.value);
    }

    double sr = 48000.0;
    Chain chains[2];
    int kind = 0, cur = 0, kindOf[2] { 0, 0 }, sinceConfig = 0;
    float att = 0.0f, rel = 0.0f, configured = -1.0f;
    dsp::Ramp intensity, mix, chainFade;
};
} // namespace

KOE_REGISTER_EFFECT ("voicechar", VoiceCharEffect)
} // namespace koe

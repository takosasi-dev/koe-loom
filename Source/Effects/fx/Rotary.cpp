#include "Dsp/Building.h"
#include "Effects/IEffect.h"

#include <juce_audio_basics/juce_audio_basics.h>

#include <algorithm>
#include <cmath>

// rotary (koeloom_effects.md §3, §5.1). Mono rotating-speaker model. A complementary 800 Hz split feeds
// a horn (highs) and a drum (lows). Each rotor angle drives, at the same rate, a distance change
// (modulated delay -> Doppler pitch), a level change (directivity -> tremolo) and, for the horn, a
// duller tone when it faces away. The drum turns at 0.85x the horn speed. Rotor speeds follow rateHz
// with inertia (horn 0.7 s, drum 1.6 s), like a real cabinet spinning up/down. mix is linear.

namespace koe
{
namespace
{
class RotaryEffect final : public IEffect
{
public:
    void prepare (double sampleRate, int) override
    {
        sr = sampleRate;
        horn.prepare (int (sr * 0.004));
        drum.prepare (int (sr * 0.004));
        split.setLowpass (sr, 800.0f);
        depth.prepare (sr, 40.0f);
        mix.prepare (sr, 40.0f);
        hornInertia = dsp::onePoleCoeff (700.0f, sr);
        drumInertia = dsp::onePoleCoeff (1600.0f, sr);
    }

    void reset() override
    {
        horn.reset();
        drum.reset();
        split.reset();
        tone.reset();
        depth.snap (depth.target);
        mix.snap (mix.target);
        hornRate = rateHz;
        drumRate = rateHz * 0.85f;
        hornPhase = 0.0;
        drumPhase = 0.25;
    }

    void setParam (int index, float v) override
    {
        if (index == 0) rateHz = v;
        else if (index == 1) depth.setTarget (v);
        else if (index == 2) mix.setTarget (v);
    }

    void process (float* x, int n) override
    {
        juce::ScopedNoDenormals noDenormals;
        const float ms = float (sr * 0.001);
        const double twoPi = 2.0 * 3.14159265358979323846;
        for (int i = 0; i < n; ++i)
        {
            hornRate = rateHz + hornInertia * (hornRate - rateHz);
            drumRate = 0.85f * rateHz + drumInertia * (drumRate - 0.85f * rateHz);
            hornPhase += hornRate / sr;
            drumPhase += drumRate / sr;
            if (hornPhase >= 1.0) hornPhase -= 1.0;
            if (drumPhase >= 1.0) drumPhase -= 1.0;
            const float hc = float (std::cos (twoPi * hornPhase)); // 1 = facing the listener
            const float dc = float (std::cos (twoPi * drumPhase));
            const float d = depth.next();

            const float in = x[i];
            const float low = split.process (in);
            const float high = in - low;
            horn.push (high);
            drum.push (low);

            // facing = closest = shortest delay and loudest
            const float hornOut = horn.readCubic (1.0f + 0.6f * ms * d * (1.0f - hc)) * (1.0f - 0.35f * d * (0.5f - 0.5f * hc));
            const float drumOut = drum.readCubic (1.0f + 0.25f * ms * d * (1.0f - dc)) * (1.0f - 0.3f * d * (0.5f - 0.5f * dc));

            if ((i & 7) == 0) tone.setCutoff (sr, 3000.0f + 13000.0f * (1.0f - d * (0.5f - 0.5f * hc)));
            const float wet = tone.process (hornOut) + drumOut;
            x[i] = in + mix.next() * (wet - in);
        }
    }

private:
    double sr = 48000.0, hornPhase = 0.0, drumPhase = 0.25;
    float rateHz = 6.0f, hornRate = 6.0f, drumRate = 5.1f, hornInertia = 0.0f, drumInertia = 0.0f;
    dsp::DelayLine horn, drum;
    dsp::Biquad split;
    dsp::OnePoleLowpass tone;
    dsp::Ramp depth, mix;
};
} // namespace

KOE_REGISTER_EFFECT ("rotary", RotaryEffect)
} // namespace koe

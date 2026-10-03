#include "Dsp/Building.h"
#include "Effects/IEffect.h"

#include <algorithm>
#include <cmath>

// slicer (koeloom_effects.md §3, §5.1). One step = one `division` note at `bpm` (120 BPM 1/16 = 125 ms);
// the 8-step `pattern` (table in §5.1) says pass (1) or cut (0). The step clock starts at reset()
// (= slot ON / chain built). The gate moves linearly over smoothMs (0 = hard switch) and is then
// shaped with a raised cosine; cut level = 1 - depth. Tempo changes keep the position inside the step.

namespace koe
{
namespace
{
constexpr unsigned char kPatterns[8][8] = {
    { 1, 0, 1, 0, 1, 0, 1, 0 }, { 1, 1, 0, 0, 1, 1, 0, 0 }, { 1, 0, 1, 1, 0, 1, 1, 0 }, { 1, 1, 1, 0, 1, 1, 1, 0 },
    { 1, 0, 0, 1, 0, 0, 1, 0 }, { 1, 1, 0, 1, 0, 1, 1, 0 }, { 1, 0, 1, 0, 0, 1, 0, 1 }, { 1, 1, 1, 1, 0, 0, 0, 0 },
};

class SlicerEffect final : public IEffect
{
public:
    void prepare (double sampleRate, int) override
    {
        sr = sampleRate;
        depth.prepare (sr, 30.0f);
        stepLen = computeStepLen();
    }

    void reset() override
    {
        depth.snap (depth.target);
        stepLen = computeStepLen();
        posInStep = 0.0;
        step = 0;
        gate = float (kPatterns[pattern][0]);
    }

    void setParam (int index, float v) override
    {
        switch (index)
        {
            case 0: bpm = v; retime(); break;
            case 1: pattern = std::clamp (int (v), 0, 7); break;
            case 2: division = std::clamp (int (v), 0, 2); retime(); break;
            case 3: depth.setTarget (v); break;
            case 4: smoothMs = v; break;
            default: break;
        }
    }

    void process (float* x, int n) override
    {
        const float slew = smoothMs > 0.0f ? float (1.0 / (smoothMs * 0.001 * sr)) : 2.0f;
        for (int i = 0; i < n; ++i)
        {
            const float target = float (kPatterns[pattern][step]);
            gate += std::clamp (target - gate, -slew, slew);
            const float shaped = 0.5f - 0.5f * std::cos (dsp::kPi * gate);
            x[i] *= 1.0f - depth.next() * (1.0f - shaped);

            posInStep += 1.0;
            if (posInStep >= stepLen)
            {
                posInStep -= stepLen;
                step = (step + 1) & 7;
            }
        }
    }

private:
    double computeStepLen() const noexcept
    {
        static constexpr double kDenominator[3] = { 8.0, 16.0, 32.0 };
        return sr * 60.0 / double (bpm) * 4.0 / kDenominator[division];
    }

    void retime() noexcept
    {
        const double newLen = computeStepLen();
        if (newLen == stepLen) return;  // hosts may resend unchanged values every block
        posInStep *= newLen / stepLen;  // same relative position inside the step
        stepLen = newLen;
    }

    double sr = 48000.0, stepLen = 6000.0, posInStep = 0.0;
    float bpm = 120.0f, smoothMs = 5.0f, gate = 1.0f;
    int pattern = 0, division = 1, step = 0;
    dsp::Ramp depth;
};
} // namespace

KOE_REGISTER_EFFECT ("slicer", SlicerEffect)
} // namespace koe

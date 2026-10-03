#include "Dsp/NoiseGate.h"

#include "Dsp/Building.h"

#include <algorithm>
#include <cmath>

namespace koe
{
void NoiseGate::prepare (double sampleRate)
{
    sr = sampleRate;
    reset();
}

void NoiseGate::reset()
{
    env = 0.0f;
    gain = 0.0f;
    holdLeft = 0;
    gateOpen = false;
    open.store (false);
}

void NoiseGate::setParams (float thresholdDb, float attackMs, float holdMs, float releaseMs) noexcept
{
    thrDb.store (thresholdDb);
    attMs.store (attackMs);
    holdMsA.store (holdMs);
    relMs.store (releaseMs);
}

void NoiseGate::process (float* x, int n, bool forceOpen)
{
    if (forceOpen)
    {
        const float upF = 1.0f / std::max (1.0f, dsp::msToSamples (attMs.load(), sr));
        for (int i = 0; i < n; ++i) { gain = std::min (1.0f, gain + upF); x[i] *= gain; }
        gateOpen = true;
        open.store (true, std::memory_order_relaxed);
        return;
    }
    const float openThr = dsp::dbToGain (thrDb.load());
    const float closeThr = openThr * 0.7079f; // -3 dB hysteresis
    const float envAtt = dsp::onePoleCoeff (0.5f, sr), envRel = dsp::onePoleCoeff (20.0f, sr);
    const float up = 1.0f / std::max (1.0f, dsp::msToSamples (attMs.load(), sr));
    const float down = 1.0f / std::max (1.0f, dsp::msToSamples (relMs.load(), sr));
    const int hold = int (dsp::msToSamples (holdMsA.load(), sr));

    for (int i = 0; i < n; ++i)
    {
        const float a = std::abs (x[i]);
        env = a > env ? a + envAtt * (env - a) : a + envRel * (env - a);
        if (env >= openThr) { gateOpen = true; holdLeft = hold; }
        else if (env < closeThr)
        {
            if (holdLeft > 0) --holdLeft;
            else gateOpen = false;
        }
        gain = gateOpen ? std::min (1.0f, gain + up) : std::max (0.0f, gain - down);
        x[i] *= gain;
    }
    open.store (gateOpen, std::memory_order_relaxed);
}
} // namespace koe

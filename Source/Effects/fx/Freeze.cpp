#include "Dsp/Building.h"
#include "Effects/IEffect.h"

#include <juce_audio_basics/juce_audio_basics.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <vector>

// freeze (koeloom_effects.md §5.2, F-04-19). The input runs through a 1.36 s history. trigger (freezeToggle)
// OFF -> ON copies the newest 1.5 * grainMs: the last grainMs is the loop, the half grain before it is
// the pre-roll. The loop plays over and over; its last half grain crossfades (equal power) into the
// pre-roll, which is the audio that really preceded the loop start, so the wrap is seamless.
// ON / OFF crossfade 20 ms between the live voice and the loop (OFF = back to the voice in 20 ms).
// Re-triggering ON while the 20 ms fade-out is still running resumes the same loop instead of
// recapturing (no jump). grainMs is latched at capture. reset() = OFF at once (F-04-19). mix only
// matters while frozen: out = voice + fade * mix * (loop - voice). Latency 0.

namespace koe
{
namespace
{
class FreezeEffect final : public IEffect
{
public:
    void prepare (double sampleRate, int) override
    {
        sr = sampleRate;
        history.prepare (int (sr * 0.76) + 64); // 1.5 * 500 ms
        frozen.assign (size_t (sr * 0.76) + 64, 0.0f);
        fade.prepare (sr, 20.0f);
        mix.prepare (sr, 30.0f);
    }

    void reset() override
    {
        history.reset();
        on = false;
        uiState.store (0);
        fade.snap (0.0f);
        mix.snap (mix.target);
        pos = 0;
    }

    void setParam (int index, float v) override
    {
        if (index == 0) grainMs = v;
        else if (index == 1) mix.setTarget (v);
    }

    void trigger (EffectTrigger t) override
    {
        if (t != EffectTrigger::freezeToggle) return;
        on = ! on;
        if (on && fade.value <= 0.0f) capture();
        fade.setTarget (on ? 1.0f : 0.0f);
        uiState.store (on ? 1 : 0);
    }

    int getUiState() const override { return uiState.load(); }

    void process (float* x, int n) override
    {
        juce::ScopedNoDenormals noDenormals;
        for (int i = 0; i < n; ++i)
        {
            const float in = x[i];
            history.push (in);
            const float f = fade.next(), m = mix.next();
            if (f <= 0.0f) continue; // OFF: the voice passes untouched

            // loop sample at pos; the last `pre` samples of the loop fade into the pre-roll
            float y = frozen[size_t (pre + pos)];
            const int intoFade = pos - (len - pre);
            if (intoFade >= 0)
            {
                const float a = (float (intoFade) + 0.5f) / float (pre) * 0.5f * dsp::kPi;
                y = std::cos (a) * y + std::sin (a) * frozen[size_t (intoFade)];
            }
            if (++pos >= len) pos = 0;
            x[i] = in + f * m * (y - in);
        }
    }

private:
    void capture() noexcept
    {
        len = std::max (2, int (dsp::msToSamples (grainMs, sr)));
        pre = std::max (1, len / 2);
        const int total = len + pre;
        for (int k = 0; k < total; ++k) frozen[size_t (k)] = history.readInt (total - 1 - k); // oldest first
        pos = 0;
    }

    double sr = 48000.0;
    dsp::DelayLine history;
    std::vector<float> frozen; // [pre-roll | loop]
    dsp::Ramp fade, mix;
    float grainMs = 120.0f;
    int len = 2, pre = 1, pos = 0;
    bool on = false;
    std::atomic<int> uiState { 0 };
};
} // namespace

KOE_REGISTER_EFFECT ("freeze", FreezeEffect)
} // namespace koe

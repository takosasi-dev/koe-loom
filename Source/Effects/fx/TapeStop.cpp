#include "Dsp/Building.h"
#include "Effects/IEffect.h"

#include <juce_audio_basics/juce_audio_basics.h>

#include <algorithm>
#include <atomic>
#include <cmath>

// tapestop (wave10/fx, INTERFACES.md §12.3). trigger (tapeStopToggle) toggles between stopping and starting, like
// freeze. A motion value u runs 1 -> 0 over stopMs (stopping) or 0 -> 1 over startMs (starting); the tape speed is
// curve (u) (直線 u, なめらか smoothstep, 低い音で粘る u^2: drops fast, then lingers low). The voice is written into
// a 10.4 s history and read back at that speed, so pitch and tempo fall together; the read head falls behind by
// (1 - speed) per sample. The last tenth of the speed fades to silence, and a stopped tape is silent. Starting from a
// stopped tape reads from the live voice (spinning up what is said now); starting while still stopping carries on
// from where the head is. Once back at full speed, a 20 ms crossfade jumps to the live voice: no delay is left, and
// from then on the output is the input exactly (latency 0). A toggle during that crossfade takes effect when it ends.
// reset() = playing at once. The history is never cleared: every read is of samples written after the stop began.

namespace koe
{
namespace
{
constexpr float kMaxMotionMs = 5000.0f; // stopMs / startMs maximum (EffectRegistry.cpp)
constexpr float kFadeSpeed = 0.1f;

class TapeStopEffect final : public IEffect
{
public:
    void prepare (double sampleRate, int) override
    {
        sr = sampleRate;
        history.prepare (int (sr * (2.0 * kMaxMotionMs * 0.001 + 0.4)));
        maxLag = double (history.capacity() - 4);
        xfadeLen = std::max (1, int (sr * 0.020));
        setTimes();
    }

    void reset() override
    {
        stopping = false;
        uiState.store (0);
        u = 1.0;
        lag = 0.0;
        xfadePos = xfadeLen;
    }

    void setParam (int index, float v) override
    {
        if (index == 0) stopMs = v;
        else if (index == 1) startMs = v;
        else if (index == 2) curve = int (v);
        setTimes();
    }

    void trigger (EffectTrigger t) override
    {
        if (t != EffectTrigger::tapeStopToggle) return;
        stopping = ! stopping;
        uiState.store (stopping ? 1 : 0);
    }

    int getUiState() const override { return uiState.load(); }

    void process (float* x, int n) override
    {
        juce::ScopedNoDenormals noDenormals;
        for (int i = 0; i < n; ++i)
        {
            const float in = x[i];
            history.push (in);
            if (xfadePos < xfadeLen) // back at full speed: crossfade from the tape to the live voice
            {
                const float tape = read();
                const float w = 0.5f - 0.5f * std::cos (dsp::kPi * (float (xfadePos) + 0.5f) / float (xfadeLen));
                x[i] = tape + w * (in - tape);
                if (++xfadePos == xfadeLen) lag = 0.0;
                continue;
            }
            if (! stopping && u >= 1.0) continue; // playing: the input untouched

            if (stopping) u = std::max (0.0, u - stopInc);
            else
            {
                if (u <= 0.0) lag = 0.0; // a stopped tape starts from the live voice
                u = std::min (1.0, u + startInc);
            }
            if (u <= 0.0) { x[i] = 0.0f; continue; } // stopped

            const float speed = speedAt (float (u));
            lag = std::min (maxLag, lag + double (1.0f - speed)); // ponytail: toggling mid-way forever pins the head at 10 s back
            x[i] = read() * std::min (1.0f, speed / kFadeSpeed);
            if (! stopping && u >= 1.0) xfadePos = 0;
        }
    }

private:
    void setTimes() noexcept
    {
        stopInc = 1.0 / std::max (1.0, double (stopMs) * 0.001 * sr);
        startInc = 1.0 / std::max (1.0, double (startMs) * 0.001 * sr);
    }

    float speedAt (float v) const noexcept
    {
        if (curve == 1) return v * v * (3.0f - 2.0f * v);
        if (curve == 2) return v * v;
        return v;
    }

    float read() const noexcept
    {
        const float d = float (lag);
        return d < 1.0f ? history.readLinear (d) : history.readCubic (d);
    }

    double sr = 48000.0, stopInc = 0.0, startInc = 0.0, u = 1.0, lag = 0.0, maxLag = 0.0;
    dsp::DelayLine history;
    float stopMs = 1200.0f, startMs = 400.0f;
    int curve = 0, xfadeLen = 1, xfadePos = 1;
    bool stopping = false;
    std::atomic<int> uiState { 0 };
};
} // namespace

KOE_REGISTER_EFFECT ("tapestop", TapeStopEffect)
} // namespace koe

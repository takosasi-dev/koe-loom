#pragma once

#include "Core/Constants.h"

#include <atomic>
#include <vector>

namespace koe
{
/**
    Always-on output limiter (§9.2): 1 ms look-ahead, ceiling -1 dBFS, cannot be disabled.
    Gain = moving average (look-ahead long) of a min-held required gain, so the gain has reached
    its target by the time the peak leaves the delay line; a final clamp guards rounding.
    Non-finite input samples are treated as 0.
*/
class Limiter
{
public:
    void prepare (double sampleRate, float ceilingDb = kLimiterCeilingDb, float lookaheadMs = 1.0f, float releaseMs = 60.0f);
    void reset();
    void process (float* samples, int numSamples);
    int getLatencySamples() const noexcept { return lookahead; }

    /** UI: true if the limiter reduced gain by more than 0.1 dB since the last call (F-08-2). */
    bool fetchAndClearActive() noexcept { return active.exchange (false); }
    float getCeiling() const noexcept { return ceiling; }

private:
    float ceiling = 0.891f;
    int lookahead = 48;
    float releaseCoeff = 0.999f;
    std::vector<float> delay, reqRing, avgRing;
    int pos = 0, reqPos = 0;
    float held = 1.0f;
    double avgSum = 0.0;
    std::atomic<bool> active { false };
};
} // namespace koe

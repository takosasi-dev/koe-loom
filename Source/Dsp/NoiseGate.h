#pragma once

#include <atomic>

namespace koe
{
/**
    Noise gate (F-03-2): opens when the peak envelope exceeds the threshold, closes (after hold)
    when it falls 3 dB below it. Gain ramps up over attackMs and down over releaseMs; closed = silence.
    Parameters may be changed from any thread (atomics); isOpen() feeds the UI indicator (F-03-3).
*/
class NoiseGate
{
public:
    void prepare (double sampleRate);
    void reset();
    void setParams (float thresholdDb, float attackMs, float holdMs, float releaseMs) noexcept;
    /** forceOpen = gate switched off: opens smoothly (attack ramp) instead of jumping. */
    void process (float* samples, int numSamples, bool forceOpen = false);
    bool isOpen() const noexcept { return open.load (std::memory_order_relaxed); }

private:
    double sr = 48000.0;
    std::atomic<float> thrDb { -45.0f }, attMs { 5.0f }, holdMsA { 80.0f }, relMs { 120.0f };
    float env = 0.0f, gain = 0.0f;
    int holdLeft = 0;
    bool gateOpen = false;
    std::atomic<bool> open { false };
};
} // namespace koe

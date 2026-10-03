#pragma once

#include "Core/Constants.h"

#include <atomic>
#include <vector>

struct DenoiseState;

namespace koe
{
/**
    RNNoise wrapper (F-03-1). RNNoise works on 480-sample frames at 48 kHz; a FIFO bridges any host
    block size, which adds one frame of delay on top of RNNoise's own. getLatencySamples() is the
    total, measured by the tests. Dry/wet mixing uses a dry path delayed by the same amount.
    Only valid at 48 kHz (isAvailable() is false otherwise; the caller then bypasses it).
*/
class NoiseSuppressor
{
public:
    NoiseSuppressor();
    ~NoiseSuppressor();

    /** blockSize = the host's (fixed) block size; multiples of 480 add no FIFO delay. */
    void prepare (double sampleRate, int blockSize);
    void reset();
    bool isAvailable() const noexcept { return available; }

    /** In place. mix 0 = dry (delayed), 1 = fully denoised. */
    void process (float* samples, int numSamples, float mix);
    int getLatencySamples() const noexcept;

    /** Last voice-activity probability reported by RNNoise (0..1). */
    float getVoiceProbability() const noexcept { return vad.load (std::memory_order_relaxed); }

    /** RNNoise v0.2's own delay beyond the FIFO, in samples (measured: EngineTests "RNNoise"). */
    static constexpr int kInternalDelay = 960;

private:
    DenoiseState* state = nullptr;
    bool available = false;
    int preroll = kRnnoiseFrame;
    std::vector<float> inFrame, outFrame, outFifo, dryLine;
    int inCount = 0, outRead = 0, outWrite = 0, outAvail = 0;
    int dryPos = 0;
    std::atomic<float> vad { 0.0f };
};
} // namespace koe

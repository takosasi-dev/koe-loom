#include "Dsp/NoiseSuppressor.h"

#include "Core/Constants.h"

#include <rnnoise.h>

#include <algorithm>
#include <cmath>
#include <numeric>

namespace koe
{
NoiseSuppressor::NoiseSuppressor() = default;

NoiseSuppressor::~NoiseSuppressor()
{
    if (state != nullptr) rnnoise_destroy (state);
}

void NoiseSuppressor::prepare (double sampleRate, int blockSize)
{
    available = std::abs (sampleRate - 48000.0) < 1.0 && rnnoise_get_frame_size() == kRnnoiseFrame;
    if (state == nullptr) state = rnnoise_create (nullptr); // built-in model (rnnoise_data.c)
    // Whole blocks go in before any output is taken, so the FIFO only needs enough pre-roll to cover
    // the worst block-end phase against the 480-sample frame grid: 0 for multiples of 480 (the default
    // buffer), 480 - gcd(B, 480) otherwise. Assumes a fixed host block size (WASAPI delivers one).
    blockSize = std::max (1, blockSize);
    preroll = blockSize % kRnnoiseFrame == 0 ? 0 : kRnnoiseFrame - std::gcd (blockSize, kRnnoiseFrame);
    inFrame.assign (size_t (kRnnoiseFrame), 0.0f);
    outFrame.assign (size_t (kRnnoiseFrame), 0.0f);
    outFifo.assign (size_t (kRnnoiseFrame * 2 + blockSize + preroll + 16), 0.0f);
    dryLine.assign (size_t (getLatencySamples()), 0.0f); // read-before-write: delay = size
    reset();
}

void NoiseSuppressor::reset()
{
    if (state != nullptr) rnnoise_init (state, nullptr);
    std::fill (inFrame.begin(), inFrame.end(), 0.0f);
    std::fill (outFifo.begin(), outFifo.end(), 0.0f);
    std::fill (dryLine.begin(), dryLine.end(), 0.0f);
    inCount = 0;
    outRead = 0;
    outAvail = preroll;
    outWrite = preroll;
    dryPos = 0;
}

int NoiseSuppressor::getLatencySamples() const noexcept { return preroll + kInternalDelay; }

void NoiseSuppressor::process (float* x, int n, float mix)
{
    if (! available || state == nullptr) return;
    const int fifoSize = int (outFifo.size());

    // 1) feed the whole block, denoising every completed frame
    for (int i = 0; i < n; ++i)
    {
        inFrame[size_t (inCount++)] = x[i] * 32768.0f; // RNNoise expects 16-bit scale
        if (inCount == kRnnoiseFrame)
        {
            vad.store (rnnoise_process_frame (state, outFrame.data(), inFrame.data()), std::memory_order_relaxed);
            for (int k = 0; k < kRnnoiseFrame; ++k)
            {
                outFifo[size_t (outWrite)] = outFrame[size_t (k)] * (1.0f / 32768.0f);
                outWrite = (outWrite + 1) % fifoSize;
            }
            outAvail += kRnnoiseFrame;
            inCount = 0;
        }
    }

    // 2) take the block back out, mixed with the equally delayed dry signal
    const int dryLen = int (dryLine.size());
    for (int i = 0; i < n; ++i)
    {
        float wet = 0.0f;
        if (outAvail > 0) // an underrun only happens if the host changes its block size
        {
            wet = outFifo[size_t (outRead)];
            outRead = (outRead + 1) % fifoSize;
            --outAvail;
        }
        const float in = x[i];
        float dry = in;
        if (dryLen > 0)
        {
            dry = dryLine[size_t (dryPos)];
            dryLine[size_t (dryPos)] = in;
            dryPos = (dryPos + 1) % dryLen;
        }
        const float y = dry + mix * (wet - dry);
        x[i] = std::isfinite (y) ? y : 0.0f;
    }
}
} // namespace koe

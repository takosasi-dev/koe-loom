#include "Engine/TakeRecorder.h"

#include <algorithm>
#include <cmath>

namespace koe
{
TakeRecorder::TakeRecorder (double sampleRate, float maxSeconds, float loopFadeMs)
    : rate (sampleRate),
      buffer (size_t (std::max (1, int (std::lround (sampleRate * maxSeconds)))), 0.0f),
      fadeLen (std::max (1, int (std::lround (sampleRate * loopFadeMs / 1000.0))))
{
}

void TakeRecorder::startPlayback() noexcept
{
    restart.store (true);
    playing.store (true);
}

void TakeRecorder::push (const float* samples, int numSamples)
{
    if (! recording.load (std::memory_order_relaxed)) return;
    const int len = length.load (std::memory_order_relaxed);
    const int n = std::min (numSamples, getCapacity() - len);
    for (int i = 0; i < n; ++i)
    {
        const float v = samples[i];
        buffer[size_t (len + i)] = std::isfinite (v) ? v : 0.0f;
    }
    if (n > 0) length.store (len + n, std::memory_order_release);
}

bool TakeRecorder::render (float* dest, int numSamples)
{
    if (! playing.load (std::memory_order_relaxed)) return false;
    const int len = length.load (std::memory_order_acquire);
    if (len <= 0) return false;
    int p = playPos.load (std::memory_order_relaxed);
    if (restart.exchange (false))
    {
        p = 0;
        startGain = 0.0f;
    }
    // loop of period len - x: the last x samples fade into the first x, then playback goes on from x
    const int x = std::min (fadeLen, len / 4);
    const float* b = buffer.data();
    const float step = 1.0f / float (fadeLen);
    for (int i = 0; i < numSamples; ++i)
    {
        if (p >= len) p = x;
        float v = b[p];
        if (p >= len - x)
        {
            const int k = p - (len - x);
            const float a = (float (k) + 0.5f) / float (x);
            v = v * (1.0f - a) + b[k] * a;
        }
        if (startGain < 1.0f) startGain = std::min (1.0f, startGain + step);
        dest[i] = v * startGain;
        ++p;
    }
    playPos.store (p >= len ? x : p, std::memory_order_relaxed);
    return true;
}
} // namespace koe

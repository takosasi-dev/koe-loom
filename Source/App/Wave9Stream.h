#pragma once

// 配信用の出力 (INTERFACES.md §11). Owner: wave9/stream. AppController keeps one StreamData as a private member;
// put this feature's state here (atomics, buffers, unique_ptrs to your own classes ...) so AppController.h stays untouched.
//
// The stream output is a second MonitorOutput (FIFO + clock-drift resampler) fed by the processor's sent tap 0: exactly
// what the virtual mic gets, so a muted virtual mic (試し録り playback) is silent here too.

#include "Core/Constants.h"
#include "Dsp/Building.h"
#include "Engine/MonitorOutput.h"
#include "Engine/VoiceProcessor.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <memory>

namespace koe
{
/** Sent tap 0 -> the stream's MonitorOutput, with the stream volume (kStreamVolumeDb reaches +6 dB; MonitorOutput's own
    volume stops at 0 dB, so it stays at 0 and the gain is applied here). Audio thread: no allocation, lock or I/O. */
class StreamTap final : public IAudioTap
{
public:
    explicit StreamTap (MonitorOutput& o) : out (o) {}

    void setGainDb (float db) noexcept { target.store (dsp::dbToGain (kStreamVolumeDb.clamp (db)), std::memory_order_relaxed); }

    void push (const float* x, int n) override
    {
        const float t = target.load (std::memory_order_relaxed);
        if (t == 1.0f && gain == 1.0f)
        {
            out.push (x, n);
            return;
        }
        for (int pos = 0; pos < n; pos += kChunk)
        {
            const int k = std::min (kChunk, n - pos);
            const float step = (t - gain) / float (k); // glides to the new volume within one block
            for (int i = 0; i < k; ++i)
            {
                gain += step;
                buf[size_t (i)] = x[pos + i] * gain;
            }
            gain = t;
            out.push (buf.data(), k);
        }
    }

private:
    static constexpr int kChunk = kMaxBlockSize; // the processor's blocks are never longer
    MonitorOutput& out;
    std::atomic<float> target { 1.0f };
    float gain = 1.0f; // audio thread
    std::array<float, kChunk> buf {};
};

struct StreamData
{
    // ---- tests: AppController (false) has no devices. While outputsForTests is non-empty, these outputs exist and opening
    // one of them "works" without a device (MonitorOutput::prepareForTest at 48 kHz / 480); failOpenForTests fails to open.
    static inline juce::StringArray outputsForTests;
    static inline juce::String failOpenForTests;
    /** The newest controller's stream MonitorOutput (pullForTest / simulateDeviceLostForTest). */
    static inline MonitorOutput* outputForTests = nullptr;

    std::unique_ptr<MonitorOutput> out;     // made on the first open, kept until the controller goes
    std::unique_ptr<StreamTap> tap;         // never deleted while attached: closeStream() only detaches it
    juce::String openName;                  // the device open now ("" = closed)
    juce::String error;                     // Japanese, for S-03 ("" = fine)
    bool lost = false;                      // Settings::streamDevice is set but stopped / failed: retrying
    int retryCountdown = 0;

    StreamData() = default;
    StreamData (const StreamData&) = delete;
    StreamData& operator= (const StreamData&) = delete;
    ~StreamData() { if (outputForTests == out.get()) outputForTests = nullptr; }
};
} // namespace koe

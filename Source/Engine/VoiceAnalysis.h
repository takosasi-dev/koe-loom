#pragma once

// 声の高さのメーター and the recording of 自分の声で音量合わせ (INTERFACES.md §10.3). Owner: wave8/analysis.
// The taps run on the audio thread (no allocation, lock or I/O); CalibrationJob has its own thread; the rest is
// message thread.

#include "Dsp/PitchDetector.h"
#include "Engine/VoiceProcessor.h"
#include "Model/Preset.h"

#include <juce_core/juce_core.h>

#include <array>
#include <atomic>
#include <map>
#include <memory>
#include <vector>

namespace koe
{
/** Keeps the newest samples of a tap point. The audio thread writes, the message thread copies the newest n. */
class TapRing final : public IAudioTap
{
public:
    explicit TapRing (int capacity = 1 << 16); // allocates; holds ~340 ms at 192 kHz
    void push (const float* samples, int numSamples) override; // non-finite -> 0
    /** While detached. */
    void clear() noexcept { written.store (0); }
    /** Copies the newest n samples (oldest first). False until n samples arrived since clear(), or n is too big. */
    bool readLatest (float* dest, int n) const noexcept;
    long long getWritten() const noexcept { return written.load (std::memory_order_acquire); }

private:
    std::unique_ptr<std::atomic<float>[]> buf;
    int mask = 0;
    std::atomic<long long> written { 0 };
};

/** Pitch of the newest ~40 ms of a ring, smoothed for display. Message thread. */
class PitchMeter
{
public:
    static constexpr float kQuietDb = -50.0f;     // below this (RMS of the newest 40 ms) there is no pitch
    /** Raw reading of x (newest last, about 48 ms): Hz, 0 when quiet or unvoiced (PitchDetector, MPM). */
    float measure (const float* x, int n, double sampleRate);
    /** measure() on the ring's newest 48 ms, then smoothed: 0 only after 2 of the last 3 readings had none,
        a median of the voiced ones, and half way towards it each call (in semitones). */
    float poll (const TapRing& ring, double sampleRate);
    void reset();

private:
    dsp::PitchDetector detector;
    double preparedRate = 0.0;
    std::vector<float> scratch;
    std::array<float, 3> history {};
    int historyPos = 0;
    float smoothed = 0.0f;
    long long lastWritten = -1;
};

/** "A3" for 220 Hz (nearest equal-tempered note, A4 = 440 Hz, C4 = middle C); "" for 0. */
juce::String noteNameForHz (float hz);

/** The calibration take: an input tap writing into a buffer allocated before it is attached. */
class TakeBuffer final : public IAudioTap
{
public:
    /** While detached: allocates room for `samples` and starts empty. */
    void start (int samples);
    void push (const float* samples, int numSamples) override; // until full; non-finite -> 0
    int recorded() const noexcept { return int (std::min<long long> (count.load(), (long long) data.size())); }
    int capacity() const noexcept { return int (data.size()); }
    bool full() const noexcept { return ! data.empty() && recorded() >= capacity(); }
    /** After full() and detached. */
    const std::vector<float>& samples() const noexcept { return data; }
    void release() { data = {}; count.store (0); }

private:
    std::vector<float> data;
    std::atomic<long long> count { 0 };
};

/** "" when the take can be measured, else why not (Japanese): too quiet, clipped, or under 3 s of voice. */
juce::String checkCalibrationTake (const std::vector<float>& x, double sampleRate);

/** Measures every preset against the reference (そのまま) on the take, on a low-priority background thread
    (tools::presetLevelDb, tools::calibratedTrimDb). Looks at cancel() between presets. */
class CalibrationJob final : private juce::Thread
{
public:
    CalibrationJob (std::vector<float> take, double sampleRate, Preset reference, std::vector<Preset> presets);
    ~CalibrationJob() override; // cancels and waits
    void start();
    void cancel() { signalThreadShouldExit(); }
    bool isFinished() const noexcept { return finished.load (std::memory_order_acquire); } // ran to the end or was cancelled
    bool wasCancelled() const noexcept { return cancelled.load(); }
    float getProgress() const noexcept { return float (done.load()) / float (juce::jmax (1, int (presets.size()) + 1)); }
    /** After isFinished() && ! wasCancelled(): preset id -> trim. */
    const std::map<juce::String, float>& getTrims() const noexcept { return trims; }

private:
    void run() override;
    const std::vector<float> take;
    const double rate;
    const Preset reference;
    const std::vector<Preset> presets;
    std::map<juce::String, float> trims;
    std::atomic<int> done { 0 };
    std::atomic<bool> finished { false }, cancelled { false };
};
} // namespace koe

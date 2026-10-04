#pragma once

// 加工した声の WAV 録音 (INTERFACES.md §10.3, owner wave8/capture). The audio thread only copies into a lock-free FIFO
// (allocated in create(), message thread); a low-priority background thread writes mono 24-bit WAV. Detached objects
// stay alive for a few ticks (the audio thread may still be in push()), so the controller retires them.

#include "Engine/VoiceProcessor.h"

#include <juce_audio_formats/juce_audio_formats.h>

#include <atomic>
#include <memory>
#include <vector>

namespace koe
{
class WavRecorder final : public IAudioTap, private juce::Thread
{
public:
    enum class Failure { none, write, overflow };

    /** Message thread. Opens file (must not exist) and starts the writer thread; nullptr + Japanese error on failure. */
    static std::unique_ptr<WavRecorder> create (const juce::File& file, double sampleRate, juce::String& error);
    ~WavRecorder() override;

    /** Message thread: stops accepting, writes what is left, closes the file. Idempotent. False if anything failed. */
    bool finish();

    const juce::File& getFile() const noexcept { return file; }
    double getSampleRate() const noexcept { return rate; }
    double getSeconds() const noexcept { return double (pushed.load (std::memory_order_relaxed)) / rate; }
    Failure getFailure() const noexcept { return Failure (failure.load()); }
    /** Tests: the next write on the background thread fails (as with a full disk). */
    void failNextWriteForTests() noexcept { failForTests.store (true); }

    void push (const float* samples, int numSamples) override; // audio thread

private:
    WavRecorder (const juce::File& f, double sampleRate, std::unique_ptr<juce::AudioFormatWriter> w);
    void run() override;
    void drain();

    const juce::File file;
    const double rate;
    std::unique_ptr<juce::AudioFormatWriter> writer;
    juce::AbstractFifo fifo;
    std::vector<float> ring, chunk;
    std::atomic<bool> accepting { true }, failForTests { false };
    std::atomic<int> failure { 0 };
    std::atomic<long long> pushed { 0 };
    bool finished = false;
};
} // namespace koe

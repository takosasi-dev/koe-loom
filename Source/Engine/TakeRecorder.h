#pragma once

// 試し録り (INTERFACES.md §10.3, owner wave8/capture): one take of the raw device input, then looped back in its place.
// One object = one take: the buffer is allocated in the constructor (message thread) and never resized, so the audio
// thread can keep using a detached object for the rest of its block. The controller retires old objects instead of
// deleting them at once.

#include "Engine/VoiceProcessor.h"

#include <atomic>
#include <vector>

namespace koe
{
class TakeRecorder final : public IAudioTap, public IInputSource
{
public:
    /** Message thread. Starts recording at once (attach it as the input tap). */
    TakeRecorder (double sampleRate, float maxSeconds, float loopFadeMs = 10.0f);

    double getSampleRate() const noexcept { return rate; }
    int getCapacity() const noexcept { return int (buffer.size()); }
    int getLength() const noexcept { return length.load (std::memory_order_acquire); }
    bool isFull() const noexcept { return getLength() >= getCapacity(); }

    // ---- message thread ----
    bool isRecording() const noexcept { return recording.load() && ! isFull(); }
    void stopRecording() noexcept { recording.store (false); }
    /** Loops the take from the start with a 10 ms fade-in; the seam is crossfaded. Call after stopRecording(). */
    void startPlayback() noexcept;
    void stopPlayback() noexcept { playing.store (false); }
    bool isPlaying() const noexcept { return playing.load(); }
    int getPlayPosition() const noexcept { return playPos.load (std::memory_order_relaxed); }
    /** Tests / snapshots, before playback: the take's samples. */
    const std::vector<float>& getSamples() const noexcept { return buffer; }

    // ---- audio thread ----
    void push (const float* samples, int numSamples) override;    // records (non-finite -> 0) until full
    bool render (float* dest, int numSamples) override;           // the loop while playing

private:
    const double rate;
    std::vector<float> buffer;
    const int fadeLen;
    std::atomic<int> length { 0 }, playPos { 0 };
    std::atomic<bool> recording { true }, playing { false }, restart { false };
    float startGain = 1.0f; // audio thread: fade-in after startPlayback()
};
} // namespace koe

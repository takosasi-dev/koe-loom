#pragma once

// 試し録り and WAV recording (INTERFACES.md §10). Owner: wave8/capture. AppController keeps one CaptureData as a private member;
// put this feature's state here (atomics, buffers, unique_ptrs to your own classes ...) so AppController.h stays untouched.

#include "Engine/TakeRecorder.h"
#include "Engine/WavRecorder.h"

#include <memory>
#include <vector>

namespace koe
{
struct CaptureData
{
    // ---- tests: AppController (false) has no device; these stand in for a running one / a full disk ----
    static inline bool assumeDevicesRunningForTests = false;
    static inline bool failNextWavWriteForTests = false; // the next started recording fails its first write

    std::unique_ptr<TakeRecorder> take;     // the current take (nullptr = none)
    bool takeTapOn = false;                 // take is the input tap 0 (recording)
    bool takeTurnedMonitorOn = false;       // playTestTake turned the monitor on: turn it off again when it stops
    std::unique_ptr<WavRecorder> wav;       // recording now (the output tap 0)
    juce::File lastWav;

    /** Detached objects the audio thread may still be inside for its current block: deleted a few ticks later. */
    struct Retired { std::unique_ptr<IAudioTap> object; int ticksLeft; };
    std::vector<Retired> retired;
    void retire (std::unique_ptr<IAudioTap> o) { if (o != nullptr) retired.push_back ({ std::move (o), 6 }); }
};
} // namespace koe

#pragma once

// マイクの癖の補正 (INTERFACES.md §11). Owner: wave9/voice. AppController keeps one MicEqData as a private member;
// put this feature's state here (atomics, buffers, unique_ptrs to your own classes ...) so AppController.h stays untouched.

#include "Engine/MicEq.h"
#include "Engine/VoiceAnalysis.h"

#include <memory>
#include <vector>

namespace koe
{
struct MicEqData
{
    // ---- measuring: input tap 3 while recording, then a MicEqJob ----
    enum class Phase { idle, recording, analysing, done, failed };
    Phase phase = Phase::idle;
    TakeBuffer take;                       // the same recorder as 音量合わせ (Engine/VoiceAnalysis.h)
    bool takeAttached = false, takeReleasePending = false;
    double takeRate = 0.0;
    std::unique_ptr<MicEqJob> job;         // working out the gains
    std::unique_ptr<MicEqJob> retired;     // cancelled, still finishing
    juce::String error;

    // ---- applying: the input filter (made on first need, lives until the controller goes: the audio thread may be in it) ----
    std::unique_ptr<MicEqFilter> filter;
    bool attached = false;                 // set as the processor's input filter
    std::vector<float> builtGains;         // what the filter was last given ...
    double builtRate = 0.0;                // ... at this rate

    // ---- tests (VoiceTests.cpp): message thread only ----
    static inline bool testDevicesRunning = false; // startMicEqMeasure without an audio device
};
} // namespace koe

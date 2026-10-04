#pragma once

// 声の高さのメーター and 自分の声で音量合わせ (INTERFACES.md §10). Owner: wave8/analysis. AppController keeps one AnalysisData as a private member;
// put this feature's state here (atomics, buffers, unique_ptrs to your own classes ...) so AppController.h stays untouched.

#include "Engine/VoiceAnalysis.h"

#include <memory>
#include <string>
#include <vector>

namespace koe
{
struct AnalysisData
{
    // ---- 声の高さ: input tap 1 and output tap 1, attached by pollPitch(), detached ~1 s after the last poll ----
    std::unique_ptr<TapRing> inRing, outRing;
    PitchMeter inMeter, outMeter;
    bool pitchAttached = false;
    int pitchIdleTicks = 0;

    // ---- 音量合わせ: input tap 2 while recording, then a CalibrationJob ----
    enum class Phase { idle, recording, analysing, done, failed };
    Phase phase = Phase::idle;
    TakeBuffer take;
    bool takeAttached = false, takeReleasePending = false;
    double takeRate = 0.0;
    std::unique_ptr<CalibrationJob> job;     // measuring
    std::unique_ptr<CalibrationJob> retired; // cancelled, still finishing its current preset
    int presetsAdjusted = 0;
    juce::String error;

    // ---- tests (AnalysisTests.cpp): message thread only ----
    static inline bool testDevicesRunning = false;        // startCalibration without an audio device
    static inline std::vector<std::string> testPresetIds; // non-empty: measure only these built-in presets
};
} // namespace koe

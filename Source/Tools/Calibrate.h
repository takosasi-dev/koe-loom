#pragma once

#include "Model/Preset.h"

#include <juce_core/juce_core.h>

#include <vector>

namespace koe::tools
{
/** Renders one preset offline through the full VoiceProcessor (noise suppression off, gate always open,
    output gain 0 dB: the AC-10 / AC-37 conditions). latencyOut = total processing latency in samples. */
std::vector<float> renderPreset (const Preset& preset, const std::vector<float>& input, int& latencyOut);

/** RMS (dB) of the output from 1 s after the start to the end of the input, latency-compensated
    (koeloom_presets.md §5.1). */
float measureLevelDb (const std::vector<float>& input, const std::vector<float>& output, int latency);

/** KoeLoom.exe --calibrate-presets [csvPath]: every built-in preset vs そのまま with the reference speech
    (%KOELOOM_REFERENCE_WAV%, else the synthetic voice). Writes "id,diffDb,suggestedTrimDb,usesReference"
    CSV (default: calibration.csv next to the exe). Returns 0 on success. */
int calibratePresets (const juce::String& csvPath);
} // namespace koe::tools

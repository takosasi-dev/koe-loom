#pragma once

#include "Core/Constants.h"
#include "Model/Preset.h"

#include <juce_core/juce_core.h>

#include <vector>

namespace koe::tools
{
/** Renders one preset offline through the full VoiceProcessor (noise suppression off, gate always open,
    output gain 0 dB: the AC-10 / AC-37 conditions). latencyOut = total processing latency in samples.
    Thread-safe (its own processor and chain): 自分の声で音量合わせ runs it on a background thread. */
std::vector<float> renderPreset (const Preset& preset, const std::vector<float>& input, int& latencyOut,
                                 double sampleRate = kSampleRate);

/** RMS (dB) of the output from 1 s after the start to the end of the input, latency-compensated
    (koeloom_presets.md §5.1). */
float measureLevelDb (const std::vector<float>& input, const std::vector<float>& output, int latency,
                      double sampleRate = kSampleRate);

/** The level (dB) of a preset on `speech`: padded with 1 s of silence, renderPreset, measureLevelDb. */
float presetLevelDb (const Preset& preset, const std::vector<float>& speech, double sampleRate = kSampleRate);

/** The trim for a preset whose level (with its shipped trim) differs from そのまま by diffDb: the shipped trim when
    |diff| <= 1 dB (tools/apply_trims.py), else shipped - diff in 0.5 dB steps within kTrimDb. */
float calibratedTrimDb (float shippedTrimDb, float diffDb);

/** KoeLoom.exe --calibrate-presets [csvPath]: every built-in preset vs そのまま with the reference speech
    (%KOELOOM_REFERENCE_WAV%, else the synthetic voice). Writes "id,diffDb,suggestedTrimDb,usesReference"
    CSV (default: calibration.csv next to the exe). Returns 0 on success. */
int calibratePresets (const juce::String& csvPath);
} // namespace koe::tools

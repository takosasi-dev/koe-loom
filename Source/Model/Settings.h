#pragma once

// Environment settings (%APPDATA%\KoeLoom\settings.json, F-11-1). Not part of presets (D-7).

#include "Core/Constants.h"

#include <juce_core/juce_core.h>

#include <vector>

namespace koe
{
inline constexpr int kAccentColours = 6, kBackgroundTones = 3; // S-03 外観 choices (ui::Theme holds the colours)

struct HotkeyBinding
{
    juce::String action; // see Hotkeys.h action ids, e.g. "voiceToggle", "favorite.3", "slot.7", "sound.12"
    int modifiers = 0;   // MOD_ALT=1, MOD_CONTROL=2, MOD_SHIFT=4, MOD_WIN=8 (Win32 RegisterHotKey values)
    int virtualKey = 0;  // Win32 VK code
    bool operator== (const HotkeyBinding&) const = default;
};

struct Settings
{
    // ---- devices (F-01) ----
    bool hasSavedDevices = false;          // false on first run -> F-01-2 auto-pick of CABLE Input
    juce::String deviceType;               // JUCE device type name; empty = WASAPI shared low-latency preferred
    juce::String inputDevice;              // empty = 未選択
    juce::String outputDevice;             // empty = 未選択
    juce::String monitorDevice;            // empty = none
    double sampleRate = kSampleRate;
    int bufferSize = kDefaultBufferSize;

    // ---- environment (F-03, F-12) ----
    float inputGainDb = kInputGainDb.def;
    bool noiseSuppressionOn = true;
    float noiseMix = kNoiseMix.def;        // 0..1
    bool gateOn = true;
    float gateThresholdDb = kGateThresholdDb.def;
    float gateAttackMs = kGateAttackMs.def;
    float gateHoldMs = kGateHoldMs.def;
    float gateReleaseMs = kGateReleaseMs.def;
    float outputGainDb = kOutputGainDb.def; // rounded to 0.5 dB steps

    // ---- monitor (F-08-5). Monitor itself always starts OFF; only volume/device persist. ----
    float monitorVolumeDb = kMonitorVolumeDb.def;

    // ---- voice ----
    juce::String currentPresetId = "natural-asis";
    juce::StringArray favorites;           // preset ids, max 9, registration order (F-05-8)
    bool voiceChangerOn = true;

    // ---- app (F-09, F-14, F-15) ----
    bool darkTheme = true;
    int accentColour = 0;                  // S-03 外観: 0 = シアン (default) .. kAccentColours - 1
    int backgroundTone = 0;                // S-03 外観: 0 = 標準 (default) .. kBackgroundTones - 1
    bool softwareRenderer = false;
    bool startMinimized = false;
    bool autoStart = false;
    bool confirmOnExit = true;             // F-09-6
    bool multicore = false;                // F-15-5 (default decided in Phase 0)
    std::vector<HotkeyBinding> hotkeys;    // default: none (F-07-4)

    // ---- first run / help (F-10, F-13) ----
    bool setupDone = false;
    int tourStep = -1;                     // -1 never started, 0..6 interrupted at step, 7 finished
    bool soundboardHintShown = false;

    // ---- soundboard global (F-06-7) ----
    float duckingDb = kDuckingDb.def;

    bool operator== (const Settings&) const = default;
};

struct SettingsLoadResult
{
    bool fileMissing = false;   // first run
    bool corrupted = false;     // invalid JSON -> defaults used, file kept as .bak (F-11-2)
    juce::File backupFile;
    juce::StringArray clampedKeys; // out-of-range values clamped (E-29), logged by the caller
};

Settings loadSettings (const juce::File& file, SettingsLoadResult& result);
/** Returns false if the file cannot be written (E-19). Writes atomically (temp file + replace). */
bool saveSettings (const Settings& settings, const juce::File& file);

/** Settings with every value clamped / normalised (output gain to 0.5 dB, favorites <= 9, unique). */
Settings clampSettings (const Settings& s, juce::StringArray* clampedKeys = nullptr);
} // namespace koe

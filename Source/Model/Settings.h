#pragma once

// Environment settings (%APPDATA%\KoeLoom\settings.json, F-11-1). Not part of presets (D-7).

#include "Core/Constants.h"

#include <juce_core/juce_core.h>

#include <map>
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

/** アプリごとの自動切り替え: while a program with this file name is in front, use this preset (INTERFACES.md §10). */
struct AppSwitchRule
{
    juce::String exe;       // program file name, e.g. "VALORANT.exe" (compared ignoring case)
    juce::String presetId;  // built-in or user preset id
    bool operator== (const AppSwitchRule&) const = default;
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

    // ======== detailed settings (S-03 「詳細な設定」, INTERFACES.md §7). Every default keeps the old behaviour. ========
    // ---- audio processing (owner: wave4/audio, applied in AppController::applyAudioSettings) ----
    int converterQuality = 1;              // 0 低遅延 / 1 標準 (today's phase vocoder) / 2 高品質
    float pitchMinHz = kPitchMinHz.def;    // pitch detection range; clamp keeps min < max
    float pitchMaxHz = kPitchMaxHz.def;
    bool highPassOn = false;               // input low cut before noise suppression
    float highPassHz = kHighPassHz.def;
    bool agcOn = false;                    // automatic input level before the gate
    float agcTargetDb = kAgcTargetDb.def;
    float agcMaxGainDb = kAgcMaxGainDb.def;
    float limiterCeilingDb = kLimiterCeilingSetDb.def;
    float limiterReleaseMs = kLimiterReleaseMs.def;
    float presetCrossfadeMs = kPresetCrossfadeMs.def;
    int soundboardMaxVoices = kSoundboardMaxVoices; // 1..kSoundboardMaxVoices
    float soundFadeMs = kSoundFadeMs.def;
    float duckAttackMs = kDuckAttackMs.def;
    float duckReleaseMs = kDuckReleaseMs.def;
    bool monitorIncludeSoundboard = true;  // global switch on top of each slot's toMonitor

    // ---- devices, monitor, hotkeys, app (owner: wave4/platform, applied in AppController::applyPlatformSettings) ----
    bool wasapiExclusive = false;          // exclusive mode for input and output (E-04 if refused)
    int inputChannel = 0;                  // 0 = as before: average of the open channels (the first two; a mono device's only one), 1 = left, 2 = right, 3 = average of both
    int monitorLatency = 1;                // 0 低遅延 / 1 標準 (today's target fill) / 2 安定
    int reconnectSeconds = 1;              // device retry interval, 1..10 (F-01-4)
    int pushToTalk = 0;                    // 0 off / 1 押している間だけ話す / 2 押している間だけミュート (hotkey action "pushToTalk")
    float pttReleaseMs = kPttReleaseMs.def;
    bool hotkeyToasts = false;             // show a toast when a hotkey changes something (false: hotkeys were silent before)
    bool favoriteWrap = true;              // favourite next / prev wraps around
    int startupVoice = 0;                  // 0 前回の状態 / 1 ON / 2 OFF
    bool startupLastPreset = true;         // false = start with natural-asis
    int closeAction = 0;                   // 0 = what the app did before (tray), 1 = quit
    bool trayNotifications = false;        // tray balloon for a new danger notice while not in front (false: none before)
    int logLevel = 1;                      // 0 エラーだけ / 1 標準 / 2 詳細
    int logKeepDays = 7;                   // 1..30

    // ---- screen (owner: wave4/ui, read by the UI on change messages) ----
    int uiScalePercent = 100;              // one of kUiScalePercents
    bool alwaysOnTop = false;
    int animations = 0;                    // 0 Windows に従う / 1 オン / 2 オフ
    int meterFps = 30;                     // 30 or 60
    float meterPeakHoldMs = kMeterPeakHoldMs.def;
    float tooltipDelayMs = kTooltipDelayMs.def;
    int knobSensitivity = 1;               // 0 ゆっくり / 1 標準 / 2 速い
    bool knobWheel = true;                 // mouse wheel turns knobs
    bool settingsShowDetails = false;      // S-03: 「詳細な設定」 open in every section

    // ---- updates from GitHub Releases (owner: wave4/update, INTERFACES.md §7.4). OFF = no network at all (F-11-4, AC-27) ----
    bool autoUpdate = false;               // check at startup, download, replace the exe when the app quits
    bool updateIncludePrerelease = true;   // only pre-releases exist while v0.x
    juce::String updateSkippedVersion;     // "0.2.0" the user chose to skip; empty = none

    // ---- looks (INTERFACES.md §8, owner request 2026-10-03: the mock's 案 B / 案 C as alternatives, own themes) ----
    int layoutStyle = 0;                   // S-01 page: 0 案 A Studio (approved) / 1 案 B Paper / 2 案 C Mono
    juce::String themeId;                  // "" = darkTheme + accentColour + backgroundTone (as before);
                                           // "builtin:paper", "builtin:mono", or "user:<file name>" in paths::themesDir()

    // ---- wave 8 (INTERFACES.md §10, owner request 2026-10-04). Every default keeps the old behaviour. ----
    juce::StringArray momentaryRecipes;    // 押している間だけのエフェクト: index i = hotkey "momentary.<i+1>", a recipe id ("" or missing = none, max kMomentarySlots)
    bool appSwitchOn = false;              // アプリごとの自動切り替え
    bool appSwitchRestore = true;          // when no rule matches any more, go back to the preset used before the switch
    std::vector<AppSwitchRule> appSwitchRules; // max kMaxAppSwitchRules, first match wins
    std::map<juce::String, float> calibratedTrimDb; // 自分の声で音量合わせ: built-in preset id -> output trim (replaces its outputTrimDb)
    juce::String calibratedAt;             // when that was measured (ISO 8601), "" = never

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

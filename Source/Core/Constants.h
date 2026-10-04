#pragma once

// Every limit and range from the spec lives here, in one place (spec §9.4: "重さの区分と上限は
// コード内の定数 1 か所に置く"). Values marked [暫定] in the spec are expected to be tuned.

namespace koe
{
struct Range
{
    float min, max, def;
    constexpr float clamp (float v) const noexcept { return v < min ? min : (v > max ? max : v); }
};

// ---- audio format (A-2, A-17) ----
inline constexpr double kSampleRate = 48000.0;
inline constexpr int kDefaultBufferSize = 480;          // [暫定] 10 ms
inline constexpr int kMaxBlockSize = 2048;              // processors split larger callbacks
inline constexpr int kRnnoiseFrame = 480;               // RNNoise works in 10 ms frames at 48 kHz

// ---- chain / layers / presets (A-9, F-04, F-05) ----
inline constexpr int kMaxSlots = 10;                    // [暫定]
inline constexpr int kMaxHeavyOn = 2;                   // [暫定]
inline constexpr int kMaxLayers = 2;                    // [暫定]
inline constexpr int kMaxFavorites = 9;
inline constexpr int kMaxEffectParams = 12;             // largest effect (vocoder) has 10
inline constexpr int kPresetSchemaVersion = 1;
inline constexpr int kPresetMaxBytes = 64 * 1024;
inline constexpr int kPresetNameMaxChars = 32;
inline constexpr float kIrMaxSeconds = 10.0f;           // longest impulse response for "convolution" (INTERFACES.md §9.3)
inline constexpr float kTestTakeMaxSeconds = 15.0f;     // 試し録り: longest take (INTERFACES.md §10)
inline constexpr float kCalibrationSeconds = 10.0f;     // 自分の声で音量合わせ: how long the user speaks
inline constexpr int kMomentarySlots = 4;               // 押している間だけのエフェクト: hotkey actions momentary.1..4
inline constexpr int kMaxAppSwitchRules = 20;           // アプリごとの自動切り替え

// ---- soundboard (A-4, F-06) ----
inline constexpr int kSoundboardSlots = 12;
inline constexpr int kSoundboardMaxVoices = 8;
inline constexpr double kSoundboardMaxSeconds = 60.0;
inline constexpr long long kSoundboardMaxTotalBytes = 200LL * 1024 * 1024;

// ---- parameter ranges ----
inline constexpr Range kPitchSt { -12.0f, 12.0f, 0.0f };          // F-02-1 (0.1 step)
inline constexpr Range kFormantSt { -6.0f, 6.0f, 0.0f };          // F-02-2
inline constexpr Range kLayerLevelDb { -24.0f, 0.0f, -6.0f };     // F-02-7
inline constexpr Range kInputGainDb { -24.0f, 24.0f, 0.0f };      // F-03-4
inline constexpr Range kOutputGainDb { -24.0f, 12.0f, 0.0f };     // F-12-1 (0.5 step)
inline constexpr Range kTrimDb { -12.0f, 6.0f, 0.0f };            // koeloom_presets §3 outputTrimDb
inline constexpr Range kNoiseMix { 0.0f, 1.0f, 1.0f };            // F-03-1 dry/wet
inline constexpr Range kGateThresholdDb { -80.0f, 0.0f, -45.0f }; // F-03-2
inline constexpr Range kGateAttackMs { 0.1f, 50.0f, 5.0f };
inline constexpr Range kGateHoldMs { 0.0f, 500.0f, 80.0f };
inline constexpr Range kGateReleaseMs { 10.0f, 1000.0f, 120.0f };
inline constexpr Range kMonitorVolumeDb { -40.0f, 0.0f, -6.0f };
inline constexpr Range kSoundboardVolumeDb { -24.0f, 6.0f, 0.0f }; // F-06-4
inline constexpr Range kDuckingDb { -24.0f, 0.0f, 0.0f };         // F-06-7
inline constexpr Range kLayerDegree { -7.0f, 7.0f, 2.0f };        // F-02-11 (0 excluded)
inline constexpr float kOutputGainStepDb = 0.5f;
inline constexpr float kPitchStep = 0.1f;

// ---- detailed settings (S-03 「詳細な設定」, owner request 2026-10-03). Defaults = the behaviour before;
//      an owner who finds the old behaviour used another value corrects the default here. ----
inline constexpr Range kPitchMinHz { 60.0f, 300.0f, 60.0f };        // pitch detection range (scale layers, autopitch)
inline constexpr Range kPitchMaxHz { 300.0f, 1000.0f, 1000.0f };
inline constexpr Range kHighPassHz { 20.0f, 300.0f, 80.0f };        // input low cut (off by default)
inline constexpr Range kAgcTargetDb { -30.0f, -10.0f, -18.0f };     // automatic input level (off by default)
inline constexpr Range kAgcMaxGainDb { 0.0f, 24.0f, 12.0f };
inline constexpr Range kLimiterCeilingSetDb { -6.0f, -0.1f, -1.0f }; // default = kLimiterCeilingDb
inline constexpr Range kLimiterReleaseMs { 10.0f, 1000.0f, 60.0f };
inline constexpr Range kPresetCrossfadeMs { 10.0f, 200.0f, 30.0f };  // default = kChainSwapFadeMs (F-04-6)
inline constexpr Range kSoundFadeMs { 0.0f, 500.0f, 5.0f };          // soundboard start / stop / stop-all fade
inline constexpr Range kDuckAttackMs { 1.0f, 500.0f, 20.0f };
inline constexpr Range kDuckReleaseMs { 10.0f, 2000.0f, 300.0f }; // the soundboard always used 0.3 s
inline constexpr Range kPttReleaseMs { 0.0f, 1000.0f, 200.0f };      // push-to-talk tail after the key is released
inline constexpr Range kMeterPeakHoldMs { 0.0f, 3000.0f, 1500.0f };  // LevelMeter held 45 frames at 30 fps (F-08-1)
inline constexpr Range kTooltipDelayMs { 200.0f, 2000.0f, 500.0f };  // F-13-6: 500 ms
inline constexpr int kUiScalePercents[] = { 90, 100, 110, 125, 150 };

// ---- output safety (§9.2) ----
inline constexpr float kLimiterCeilingDb = -1.0f;
inline constexpr float kOutputGainWarnDb = 6.0f;                  // F-12-4

// ---- timings, ms ----
inline constexpr float kParamSmoothMs = 30.0f;      // 20–50 ms (§5.3)
inline constexpr float kVoiceToggleFadeMs = 15.0f;  // 10–20 ms (F-02-3)
inline constexpr float kMuteFadeMs = 30.0f;         // F-08-8
inline constexpr float kChainSwapFadeMs = 30.0f;    // F-04-6
inline constexpr float kSlotToggleFadeMs = 20.0f;   // F-04-7
inline constexpr float kLayerFadeMs = 30.0f;        // F-05-5
inline constexpr float kScaleLayerCutMs = 20.0f;    // E-28 / AC-47: scale layer with no pitch detected
inline constexpr float kStartupFadeMs = 100.0f;     // F-09-5
inline constexpr float kShifterCrossfadeMs = 20.0f; // bypass <-> shifted (F-02-9)

// ---- watchdog (§5.6, F-02-8) ----
inline constexpr float kWatchdogLoad = 0.8f;
inline constexpr double kWatchdogHoldSeconds = 1.0;

// ---- parallel (F-15, E-33/E-34) ----
inline constexpr float kParallelDeadlineFraction = 0.5f;
inline constexpr int kParallelAutoOffMisses = 10;   // in 10 s
inline constexpr int kParallelMinLogicalCores = 3;

// ---- misc ----
inline constexpr float kLatencyWarnMs = 100.0f;     // F-08-3
inline constexpr int kXrunWarnCount = 10;           // E-06: 10 in 10 s
inline constexpr double kSilentInputSeconds = 10.0; // E-15
inline constexpr float kBpmMin = 40.0f, kBpmMax = 240.0f;
} // namespace koe

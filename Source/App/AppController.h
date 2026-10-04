#pragma once

// The single object the UI talks to. Owns settings, presets, the audio engine and the platform
// services, and turns UI requests into engine changes with the spec's rules applied
// (limits, warnings, persistence). Message thread only, unless noted.
//
// UI pattern: listen with addChangeListener() (sent after any state change), poll getStatus() /
// pollMeters() from a 30 fps timer (F-08-1), read the model with the getters, call setters.

#include "Engine/AudioEngine.h"
#include "Engine/CableProbe.h"
#include "Engine/MonitorOutput.h"
#include "Engine/ScaleLayerPitch.h"
#include "Engine/Soundboard.h"
#include "Engine/VoiceProcessor.h"
#include "Model/PresetLibrary.h"
#include "Model/Settings.h"
#include "Platform/Hotkeys.h"
#include "Platform/Updater.h"

#include <juce_events/juce_events.h>

#include <functional>
#include <memory>
#include <vector>

namespace koe
{
enum class NoticeLevel { danger, warning, info };

/** A banner at the top of the window (§8.3: max 2 visible, danger first, then "+n"). */
struct Notice
{
    juce::String key;           // stable id, e.g. "output.unset", "device.lost", "loop", "hotkey.fail"
    NoticeLevel level = NoticeLevel::warning;
    juce::String text;          // Japanese
    bool dismissible = true;    // × closes it (stays closed for this run, F-01-11)
    juce::String actionLabel;   // optional button, e.g. "設定で変更"
    juce::String actionId;      // what the button does: "openSettings.devices", "openSetup", ...
};

class AppController final : public juce::ChangeBroadcaster, private juce::Timer
{
public:
    /** openDevices = false: never touches audio hardware (snapshots, UI tests). */
    explicit AppController (bool openDevices = true);
    ~AppController() override;

    /** Loads settings/presets, scans devices, applies F-01-2/F-01-10 and opens the devices if both are set. */
    void startup();
    /** Saves everything and closes the devices. Called once before quitting. */
    void shutdown();

    // ================================================================ devices (F-01)
    juce::StringArray getInputDevices() const;
    juce::StringArray getOutputDevices() const;
    /** Outputs that may be used for monitoring (virtual cables excluded, F-01-6). */
    juce::StringArray getMonitorDevices() const;
    juce::String getInputDevice() const { return settings.inputDevice; }   // empty = 未選択
    juce::String getOutputDevice() const { return settings.outputDevice; }
    juce::String getMonitorDevice() const { return settings.monitorDevice; }
    void setInputDevice (const juce::String& name);
    void setOutputDevice (const juce::String& name);
    void setMonitorDevice (const juce::String& name);
    void rescanDevices();
    bool isVirtualCableInstalled() const;                 // an output containing "CABLE Input" exists (F-10-1)
    static bool isCableInputName (const juce::String&);   // "CABLE Input"  (play side)
    static bool isCableOutputName (const juce::String&);  // "CABLE Output" (record side)
    juce::Array<double> getAvailableSampleRates() const;
    juce::Array<int> getAvailableBufferSizes() const;
    void setSampleRate (double rate);                     // F-01-8
    void setBufferSize (int samples);

    struct Status
    {
        bool running = false;
        bool loopConfig = false;        // F-01-5
        bool deviceLost = false;        // F-01-4, waiting to come back
        juce::String error;             // last open error (Japanese), empty if none
        double sampleRate = 0.0;
        int bufferSize = 0;
        float latencyMs = 0.0f;         // F-08-3 estimate (device + algorithm + chain)
        bool latencyWarn = false;       // > 100 ms
        int xruns = 0;                  // F-08-4
        float cpuPercent = 0.0f;
        bool gateOpen = false;          // F-03-3
        bool limiterActive = false;     // F-08-2 (held ~300 ms for visibility)
        bool monitorOn = false;
    };
    Status getStatus() const;

    struct Meters { float inputDb = -100.0f, outputDb = -100.0f; bool inputClip = false, outputClip = false; };
    /** Peak since the previous call, dBFS (UI holds the 1.5 s peak itself). */
    Meters pollMeters();

    // ================================================================ voice (F-02, F-08-8)
    bool isVoiceChangerOn() const { return settings.voiceChangerOn; }
    void setVoiceChangerOn (bool on);
    bool isMicMuted() const { return micMuted; }          // never saved (F-08-8)
    void setMicMuted (bool on);
    /** Pitch/formant of the current (working) preset. Setting either turns the converter on (hasShifter). */
    float getPitch() const { return current.pitchSt; }
    float getFormant() const { return current.formantSt; }
    bool hasShifter() const { return current.hasShifter; }
    void setPitch (float semitones);                      // clamped, 0.1 st steps
    void setFormant (float semitones);
    /** 「変換 ON/OFF」 on card 02. OFF = R-P3 (delayed dry main voice); layers are silent while OFF
        and are not saved with the preset (E-30), but stay in the working preset until it changes. */
    void setShifterEnabled (bool on);

    // ---- layers (F-02-7) ----
    int getNumLayers() const { return int (current.layers.size()); }
    LayerDef getLayer (int index) const;
    bool addLayer (juce::String& whyNot);                 // max 2; turns the converter on
    void removeLayer (int index);
    void setLayer (int index, const LayerDef& def);
    void setLayerEnabled (int index, bool on);            // per-voice ON (30 ms fade, F-05-5)
    bool areLayersAutoStopped() const;                   // watchdog (F-02-8, F-08-7)
    void resumeLayers();                                  // manual re-enable

    // ================================================================ chain (F-04)
    const std::vector<SlotDef>& getChain() const { return current.chain; }
    /** F-04-1 (10 max), F-04-3 (freeze/looper one each), F-04-17 (3rd heavy is added OFF). */
    bool addEffect (const std::string& type, juce::String& whyNot);
    void removeSlot (int slot);
    bool moveSlot (int from, int to);                     // F-04-4 / F-04-5
    /** Refuses a 3rd heavy effect (F-04-17). Clears "自動停止" when the user turns a slot back on. */
    bool setSlotEnabled (int slot, bool on, juce::String& whyNot);
    void setSlotParam (int slot, int paramIndex, float value); // clamped to the ParamSpec
    bool isSlotAutoStopped (int slot) const;
    void triggerSlot (int slot, EffectTrigger action);    // freeze / looper buttons (F-04-18/19)
    int getSlotUiState (int slot) const;
    bool hasLooperRecording() const;                      // E-27: ask before structure changes

    // ---- impulse-response files for "convolution" (INTERFACES.md §9.3, AppController_Ir.cpp, owner wave7/ir)
    /** Copies the picked WAV/FLAC/AIFF into paths::irDir() (keeps an existing same-name file if identical,
        otherwise adds " (2)" ...), sets the slot's file and rebuilds the chain. Refuses (Japanese whyNot) when
        the slot is not "convolution", the file is unreadable, or longer than kIrMaxSeconds. */
    bool setSlotFile (int slot, const juce::File& picked, juce::String& whyNot);
    /** Use a file already in paths::irDir() by name ("" = none). */
    void setSlotFileName (int slot, const juce::String& fileName);
    /** The slot's file name ("" = none) and whether it is missing / unreadable. */
    juce::String getSlotFileName (int slot) const;
    bool isSlotFileMissing (int slot) const;
    /** Audio files in paths::irDir(), sorted by name. */
    static juce::StringArray listIrFiles();

    // ---- compare and random (INTERFACES.md §9.4, AppController_Extra.cpp, owner wave7/extra)
    /** While held, the output is the voice-changer-OFF sound (shifter, layers and chain bypassed) with the
        usual crossfade. Never saved, does not mark the preset modified, released on preset load. */
    void setCompareHold (bool held);
    bool isCompareHeld() const { return compareHeld; }
    /** おまかせ生成: replaces the working copy with a random but safe voice (pitch/formant + 2..4 effects).
        Same seed -> same result. Refuses when the looper has a recording (E-27). Marks the preset modified. */
    bool randomizeCurrent (uint32_t seed, juce::String& whyNot);

    // ================================================================ presets (F-05)
    PresetLibrary& getPresetLibrary() { return *library; }
    const Preset& getCurrentPreset() const { return current; } // working copy including edits
    bool isCurrentPresetModified() const { return modified; }
    /** fromHotkey: E-27 refuses (with a notice) instead of asking when the looper has a recording. */
    void loadPreset (const std::string& id, bool fromHotkey = false);
    bool saveCurrentAsNew (const juce::String& name, juce::String& error);
    bool overwriteCurrent (juce::String& error);          // user presets only (F-05-2)
    /** Library edits through here so listeners hear about them, the working preset's name follows a
        rename, and a deleted preset leaves the favourites. */
    bool duplicatePreset (const std::string& id, std::string& newIdOut, juce::String& error);
    bool renamePreset (const std::string& id, const juce::String& newName, juce::String& error);
    bool removePreset (const std::string& id, juce::String& error);
    bool importPreset (const juce::File& file, std::string& newIdOut, PresetLoadReport& report);
    juce::StringArray getFavorites() const { return settings.favorites; }
    bool isFavorite (const std::string& id) const;
    bool toggleFavorite (const std::string& id, juce::String& whyNot); // max 9 (F-05-8)
    void loadFavorite (int index, bool fromHotkey = false);            // 0..8
    void nextFavorite (bool fromHotkey = false);
    void prevFavorite (bool fromHotkey = false);

    // ================================================================ environment (F-03, F-12)
    const Settings& getSettings() const { return settings; }
    void setInputGainDb (float db);
    void setNoiseSuppression (bool on, float mix);
    void setGate (bool on, float thresholdDb, float attackMs, float holdMs, float releaseMs);
    void setOutputGainDb (float db);                      // 0.5 dB steps; warning notice above +6 dB (F-12-4)
    // ---- monitor (F-08-5) ----
    /** False while the monitor device is lost (F-01-4: shown stopped, notice "monitor.lost"); it comes
        back by itself when the device returns. setMonitorOn (false) stops waiting, (true) retries now. */
    bool isMonitorOn() const { return monitorOn && ! monitorLost; }
    void setMonitorOn (bool on);                          // never saved; starts OFF
    void setMonitorVolumeDb (float db);
    /** True if the monitor device looks like speakers (UI shows the howling warning). */
    bool isMonitorDeviceSpeaker() const;

    /** Generic setter for the app options (theme, renderer, start minimized, confirm on exit, multicore, tour...)
        and every detailed setting (INTERFACES.md §7): clamps, saves, applies, sends a change message. */
    void updateSettings (const std::function<void (Settings&)>& change);
    bool setAutoStart (bool on, juce::String& error);     // F-09-3
    // ---- settings file (S-03 詳細): owner wave4/platform ----
    /** Writes settings.json's content (hotkeys and devices included) to a file of the user's choice. */
    bool exportSettings (const juce::File& file, juce::String& error) const;
    /** Loads, clamps and applies a settings file; refuses invalid JSON (error in Japanese). Devices are reopened. */
    bool importSettings (const juce::File& file, juce::String& error);
    /** Every setting back to its default except devices, favourites, hotkeys, setupDone and tourStep. */
    void resetSettings();

    // ================================================================ updates (INTERFACES.md §7.4, owner wave4/update)
    struct UpdateState
    {
        enum class Status { idle, checking, upToDate, downloading, ready, failed };
        Status status = Status::idle;
        juce::String version;      // newest version found, e.g. "0.2.0"
        juce::String releaseUrl;   // the release page (notes)
        float progress = 0.0f;     // 0..1 while downloading
        juce::String error;        // Japanese, when failed
    };
    /** Looks at GitHub Releases on a background thread. Automatic checks run only when settings.autoUpdate;
        userInitiated (S-03 「今すぐ確認」) checks even when it is off. Never blocks the message thread. */
    void checkForUpdates (bool userInitiated);
    UpdateState getUpdateState() const;
    /** Ready update: replace the exe and restart now (S-03 / the "update.ready" notice). */
    void applyUpdateNow();
    /** Skip the found version (no notice until a newer one). */
    void skipUpdateVersion();
    /** Tests: use this updater (fake network, exe in a temp folder); the app makes the real one on first use. */
    void setUpdaterForTests (std::unique_ptr<Updater> u);

    // ================================================================ hotkeys (F-07)
    /** Refuses a key already used by another action (F-07-4). */
    bool setHotkey (const juce::String& action, int modifiers, int virtualKey, juce::String& whyNot);
    void clearHotkey (const juce::String& action);
    juce::String getHotkeyText (const juce::String& action) const; // "" when unassigned

    // ================================================================ soundboard (F-06) & setup (F-10)
    Soundboard& getSoundboard() { return *soundboard; }
    void saveSoundboard();
    CableProbe& getCableProbe() { return *cableProbe; }
    void setTestTone (bool on);

    // ================================================================ notices / toasts
    /** Active banners, most severe first, without dismissed ones. */
    std::vector<Notice> getNotices() const;
    void dismissNotice (const juce::String& key);
    /** Short-lived messages (refusals, auto-stops); the UI shows and drops them. */
    juce::StringArray takeToasts();

    // ================================================================ misc
    /** Diagnostics text for the clipboard, user name in paths masked (F-11-3). */
    juce::String buildDiagnostics() const;
    /** Window/tray hooks set by the app shell. */
    std::function<void()> onShowWindowRequest;
    std::function<void()> onQuitRequest;
    /** Called by hotkeys and the tray with an action id (see Hotkeys.h). */
    void performAction (const juce::String& action);

    VoiceProcessor& getProcessorForTests() { return processor; }
    MonitorOutput& getMonitorForTests() { return *monitor; }
    void tickForTests() { timerCallback(); }              // one 30 fps controller tick
    /** Tests: what openDevicesIfReady() does around a device (re)open at this rate / buffer, without a device. */
    void reprepareForTests (double sampleRate, int bufferSize);

private:
    void timerCallback() override;
    void applyEnvironment();
    // Detailed settings (INTERFACES.md §7). before == nullptr: apply everything (startup, device reopen).
    void applyAudioSettings (const Settings* before);     // AppController_Audio.cpp, owner wave4/audio
    void applyPlatformSettings (const Settings* before);  // AppController_Platform.cpp, owner wave4/platform
    void applyPresetToEngine (bool rebuildChain);
    void rebuildChain();
    void openDevicesIfReady();
    void closeDevices();
    void reapplyHotkeys();
    void saveSettingsSoon();
    void addNotice (Notice n);
    void removeNotice (const juce::String& key);
    void toast (const juce::String& text);
    void markModified();
    void refreshOutputNotice();
    void monitorDeviceGone (const juce::String& openError); // F-01-4 / E-04 for the monitor device
    void reopenMonitor();
    // ---- wave4/platform helpers (AppController_Platform.cpp) ----
    void applyMicMute();                                  // user mute OR push-to-talk mute -> processor
    void tickPushToTalk();                                // timer: key up -> pttReleaseMs tail -> restore
    void hotkeyToast (const juce::String& text);          // a toast only when hotkeyToasts is on
    void replaceSettings (const Settings& next, bool reopenDevices); // import / reset
    int reconnectTicks() const { return settings.reconnectSeconds * 30; } // timer ticks between device retries
    // updates (AppController_Update.cpp, owner wave4/update)
    void pollUpdater();             // every timer tick: the automatic check, notices
    void finishUpdateOnQuit();      // shutdown(): swap in a ready update (and restart after applyUpdateNow)
    std::unique_ptr<Updater> updater;
    Updater::Status shownUpdateStatus = Updater::Status::idle;
    bool updateAutoChecked = false, restartAfterUpdate = false;

    const bool allowDevices;
    Settings settings;
    std::unique_ptr<PresetLibrary> library;
    Preset current;                 // working copy
    std::string currentBaseId;      // the preset it came from
    bool modified = false;
    bool compareHeld = false;       // INTERFACES.md §9.4
    bool micMuted = false, monitorOn = false;

    ScaleLayerPitch scalePitch;     // declared before the processor, which points at it
    VoiceProcessor processor;
    std::unique_ptr<AudioEngine> engine;
    std::unique_ptr<MonitorOutput> monitor;
    std::unique_ptr<Soundboard> soundboard;
    std::unique_ptr<CableProbe> cableProbe;
    std::unique_ptr<Hotkeys> hotkeys;

    juce::StringArray inputs, outputs;
    juce::String lastError;
    bool loopConfig = false, deviceLost = false;
    std::vector<Notice> notices;
    juce::StringArray dismissed, toasts;
    int saveCountdown = 0;
    int limiterHold = 0;
    int reopenCountdown = 0;
    bool monitorLost = false;       // monitorOn, but its device stopped or failed to open: retrying
    int monitorRetryCountdown = 0;
    int ticks = 0;
    int xrunBase = 0, xrunWindowStart = 0;
    // ---- wave4/platform state ----
    bool pttHeld = false;           // the push-to-talk key is down
    int pttKey = 0;                 // its virtual key (0 = no binding: treated as released at the next tick)
    float pttTailMs = 0.0f;         // > 0: released, still active for this long
    std::unique_ptr<juce::Logger> logFilter; // logging::FilterLogger in front of the app's logger (logLevel)
    juce::Logger* logTarget = nullptr;
};
} // namespace koe

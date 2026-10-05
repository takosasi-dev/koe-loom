#pragma once

// The single object the UI talks to. Owns settings, presets, the audio engine and the platform
// services, and turns UI requests into engine changes with the spec's rules applied
// (limits, warnings, persistence). Message thread only, unless noted.
//
// UI pattern: listen with addChangeListener() (sent after any state change), poll getStatus() /
// pollMeters() from a 30 fps timer (F-08-1), read the model with the getters, call setters.

#include "App/Wave8Analysis.h"
#include "App/Wave8Automation.h"
#include "App/Wave8Capture.h"
#include "App/Wave8Morph.h"
#include "App/Wave9MicEq.h"
#include "App/Wave9Stream.h"
#include "App/Wave10Edit.h"
#include "App/Wave10Viz.h"
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

#include <array>
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
    bool isVirtualCableInstalled() const;                 // an output that is VB-CABLE's play side exists (F-10-1)
    static bool isCableInputName (const juce::String&);   // play side: "CABLE Input", "CABLE In 16 Ch", "スピーカー (VB-Audio Virtual Cable)"
    static bool isCableOutputName (const juce::String&);  // record side: "CABLE Output", or another input named after the driver
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

    // ================================================================ wave 8 (INTERFACES.md §10, owner request 2026-10-04)
    // ---- 試し録り and WAV recording (AppController_Capture.cpp, owner wave8/capture) ----
    enum class TakeState { empty, recording, ready, playing };
    /** 試し録り: records the device input (before any processing) for up to kTestTakeMaxSeconds, replacing the old take.
        Refuses (Japanese whyNot) while the devices are not running or a take is playing. */
    bool startTestTake (juce::String& whyNot);
    /** Stops recording (the take keeps what was recorded) or playback. */
    void stopTestTake();
    /** Loops the take through the whole path instead of the microphone, so presets and knobs can be tried on it.
        The virtual mic is silent meanwhile (setOutputMuted) and the monitor is turned on; refuses without a monitor
        device. Keeps playing across preset changes. A notice with 「止める」 (action "takeStop") shows while it plays. */
    bool playTestTake (juce::String& whyNot);
    void clearTestTake();
    TakeState getTestTakeState() const;
    float getTestTakeSeconds() const;    // length (while recording: so far)
    float getTestTakePosition() const;   // playback position, seconds
    /** Records what goes to the virtual mic into a new WAV in paths::recordingsDir() (written on a background thread).
        A notice with 「止める」 (action "wavStop") shows while recording. Hotkey "recordToggle". */
    bool startWavRecording (juce::String& whyNot);
    void stopWavRecording();
    bool isWavRecording() const;
    double getWavRecordingSeconds() const;
    juce::File getLastWavFile() const;   // the last finished recording (juce::File() = none yet)

    // ---- 声の高さのメーター and 自分の声で音量合わせ (AppController_Analysis.cpp, owner wave8/analysis) ----
    struct PitchReading { float inputHz = 0.0f, outputHz = 0.0f; }; // 0 = no pitch (quiet or unvoiced)
    /** Message thread, from a UI timer. Analysis runs only while polled (stops about 1 s after the last call). */
    PitchReading pollPitch();
    struct CalibrationState
    {
        enum class Phase { idle, recording, analysing, done, failed };
        Phase phase = Phase::idle;
        float progress = 0.0f;       // 0..1 within the phase
        int presetsAdjusted = 0;     // done: built-in presets whose trim now differs from the shipped one
        juce::String error;          // failed: Japanese
    };
    /** Records kCalibrationSeconds of the user's voice, then measures every built-in preset on a background thread and
        stores Settings::calibratedTrimDb / calibratedAt. Refuses while the devices are not running or already busy. */
    bool startCalibration (juce::String& whyNot);
    void cancelCalibration();
    CalibrationState getCalibrationState() const;
    /** Back to the shipped trims (clears Settings::calibratedTrimDb). */
    void clearCalibration();
    /** The trim the engine uses for the working preset: the calibrated one for a built-in base preset, else its own. */
    float getEffectiveTrimDb() const;

    // ---- プリセットを混ぜる (AppController_Morph.cpp, owner wave8/morph) ----
    /** Makes the working preset a blend of two presets (amount 0 = A). Refuses (whyNot) when the blend breaks the chain
        rules (10 slots, heavy limit, one freeze / looper) or the looper has a recording (E-27). Rebuilds the chain once. */
    bool beginMorph (const std::string& idA, const std::string& idB, juce::String& whyNot);
    /** 0 = A .. 1 = B. Live: parameters glide, slots only one side has fade by SlotDef::wet, no chain rebuild.
        Marks the working preset modified (it can be saved as new; wet is saved with it). */
    void setMorphAmount (float amount);
    float getMorphAmount() const;
    /** False once the chain was rebuilt by anything else (preset load, effect added / removed / moved ...). */
    bool isMorphing() const;
    void endMorph();                     // keeps the blend as the working preset
    /** The two presets of the running blend (for a tool rebuilt mid-blend); false when not morphing. */
    bool getMorphPresets (std::string& idA, std::string& idB, juce::String& nameA, juce::String& nameB) const;

    // ---- 押している間だけのエフェクト and アプリごとの自動切り替え (AppController_Automation.cpp, owner wave8/automation) ----
    struct MomentaryRecipe { juce::String id, nameJa; };
    static std::vector<MomentaryRecipe> getMomentaryRecipes();
    /** "" = none. Stored in Settings::momentaryRecipes[index]. */
    void setMomentaryRecipe (int index, const juce::String& recipeId);
    /** Hotkey "momentary.<index+1>" pressed; the release is polled in tickAutomation (like push-to-talk). */
    void setMomentaryHeld (int index, bool held);
    bool isMomentaryHeld (int index) const;
    /** File name of the program in front, e.g. "VALORANT.exe" ("" = unknown). */
    juce::String getForegroundProgram() const;
    /** Programs that have a visible window now (file names, sorted, unique), for the rule editor. */
    static juce::StringArray listRunningPrograms();

    // ================================================================ wave 9 (INTERFACES.md §11, owner request 2026-10-04)
    // ---- 声の大きさで変わる効果 (lead; the slot editor UI is wave9/voice) ----
    /** target "" = none, "wet", or the id of a numeric param of that slot's type (else refused, false). depth -1..1.
        Live (no chain rebuild); marks the working preset modified. */
    bool setSlotMod (int slot, const std::string& target, float depth);
    /** 0..1, the voice level the modulated slots follow right now (0 while no slot is modulated). */
    float getModLevel() const;

    // ---- マイクの癖の補正 (AppController_MicEq.cpp, owner wave9/voice) ----
    struct MicEqState
    {
        enum class Phase { idle, recording, analysing, done, failed };
        Phase phase = Phase::idle;
        float progress = 0.0f;       // 0..1 within the phase
        juce::String error;          // failed: Japanese
    };
    /** Records kMicEqSeconds of the user's voice, then works out Settings::micEqGainsDb (and micEqAt) on a background thread
        and turns micEqOn on. Refuses (whyNot) while the devices are not running or already busy. */
    bool startMicEqMeasure (juce::String& whyNot);
    void cancelMicEqMeasure();
    MicEqState getMicEqState() const;
    void setMicEqOn (bool on);           // only audible when micEqGainsDb is set
    void clearMicEq();                   // forgets the measurement (micEqGainsDb, micEqAt) and turns it off

    // ---- プリセットの共有コード and サウンドボードに録音を登録 (AppController_Share.cpp, owner wave9/share) ----
    /** A short text ("KL1:" + ...) that carries the whole preset, to paste in a chat. whyNot when the preset is unknown. */
    juce::String makeShareCode (const std::string& presetId, juce::String& whyNot) const;
    /** Reads a share code (surrounding spaces / line breaks and text around it are fine) into a new user preset, like
        importPreset (same E-21..E-30 report). False with report.rejectReason in Japanese when it is not a valid code. */
    bool importShareCode (const juce::String& text, std::string& newIdOut, PresetLoadReport& report);
    /** Renders the 試し録り take through the working preset (offline, not real time) into a new WAV in
        paths::recordingsDir(). File() and whyNot when there is no take or it cannot be written. */
    juce::File renderTestTakeToFile (juce::String& whyNot);

    // ---- 配信用の出力 (AppController_Stream.cpp, owner wave9/stream) ----
    /** Devices the stream output may use: every output except the one the virtual mic goes to. */
    juce::StringArray getStreamDevices() const;
    /** "" = OFF. Saved in Settings::streamDevice and opened at once (and whenever the main devices open). */
    void setStreamDevice (const juce::String& name);
    void setStreamVolumeDb (float db);
    bool isStreamRunning() const;
    juce::String getStreamError() const; // Japanese, "" = fine

    // ================================================================ wave 10 (INTERFACES.md §12, owner request 2026-10-05)
    // ---- 元に戻す / やり直し and A/B 聞き比べ (AppController_Edit.cpp, owner wave10/edit) ----
    /** Every edit of the working preset (everything that goes through markModified) is one undo step; edits closer than
        kUndoMergeMs together merge into one (a knob drag). At most kUndoSteps. Loading a preset clears the history. */
    bool canUndo() const;
    bool canRedo() const;
    /** False + whyNot (Japanese) when there is nothing to undo / redo or the looper holds a recording a rebuild would lose (E-27). */
    bool undo (juce::String& whyNot);
    bool redo (juce::String& whyNot);
    /** A/B: while on, the engine plays the saved version of the working preset (the library copy of the preset it came
        from); getCurrentPreset() and the screen keep the edited one. Any edit, preset load, save or morph turns it off. */
    bool canAbCompare() const;           // modified and the preset it came from still exists
    bool setAbCompare (bool on, juce::String& whyNot);
    bool isAbCompare() const;

    // ---- スロットの右クリックメニュー (lead; the menus are wave10/ui) ----
    /** A copy right after the slot (same params, mod, ON/OFF; a heavy copy over kMaxHeavyOn comes OFF). Refused with
        whyNot at kMaxSlots or for one-per-chain types (freeze, looper, tapestop). Rebuilds the chain (E-27 is the UI's ask). */
    bool duplicateSlot (int slot, juce::String& whyNot);
    /** Every param back to its default and the level modulation off; ON/OFF and the IR file name stay. Live (no rebuild). */
    void resetSlot (int slot);

    // ---- 声の見える化 (AppController_Viz.cpp, owner wave10/viz) ----
    /** The latest long-ish average spectra (dB, kSpectrumBands log-spaced bands kSpectrumLowHz..kSpectrumHighHz) of the
        input (input tap 4, before any processing) and the output (output tap 2). Call it from a UI timer: the taps are
        attached on the first call and detached ~1 s after the last. False while there is no sound data yet. */
    bool pollSpectrum (std::array<float, kSpectrumBands>& inDb, std::array<float, kSpectrumBands>& outDb);

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
    // wave 8 (INTERFACES.md §10): each owner's file. tick = every timer tick, shutdown = before the devices close
    void tickCapture();      void shutdownCapture();      // AppController_Capture.cpp
    void tickAnalysis();     void shutdownAnalysis();     // AppController_Analysis.cpp
    void tickAutomation();   void shutdownAutomation();   // AppController_Automation.cpp
    // wave 9 (INTERFACES.md §11), same idea
    void tickMicEq();        void shutdownMicEq();        // AppController_MicEq.cpp
    void applyMicEq();                                    //   end of applyEnvironment(): settings -> the input filter
    void tickStream();       void shutdownStream();       // AppController_Stream.cpp
    void openStream();                                    //   after the main devices opened (openDevicesIfReady)
    void closeStream();                                   //   closeDevices()
    // wave 10 (INTERFACES.md §12)
    void noteEdit();         // markModified(): records an undo step (or merges into the last), ends A/B    AppController_Edit.cpp
    void editReset();        // loadPreset(): clears the history, ends A/B
    void editBeforeSave();   // first thing in saveCurrentAsNew / overwriteCurrent: ends A/B (engine back on current), history stays
    const Preset& playedPreset() const;                   //   what applyPresetToEngine / rebuildChain play: current, or the saved copy during A/B
    float trimFor (const Preset& p) const;                //   getEffectiveTrimDb() for any preset that came from currentBaseId
    void tickViz();          void shutdownViz();          // AppController_Viz.cpp
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
    int chainBuilds = 0;            // +1 on every rebuildChain() (INTERFACES.md §10: morph notices other rebuilds)
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
    // ---- wave 8 state, one struct per owner (INTERFACES.md §10). Declared last: destroyed before the engine parts above.
    CaptureData capture;            // App/Wave8Capture.h, wave8/capture
    AnalysisData analysis;          // App/Wave8Analysis.h, wave8/analysis
    MorphData morph;                // App/Wave8Morph.h, wave8/morph
    AutomationData automation;      // App/Wave8Automation.h, wave8/automation
    MicEqData micEq;                // App/Wave9MicEq.h, wave9/voice
    StreamData stream;              // App/Wave9Stream.h, wave9/stream
    EditData edit;                  // App/Wave10Edit.h, wave10/edit
    VizData viz;                    // App/Wave10Viz.h, wave10/viz
};
} // namespace koe

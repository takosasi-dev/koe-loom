// Detailed device / monitor / hotkey / app settings and the settings file (INTERFACES.md §7, owner wave4/platform):
// exclusive mode, input channel, monitor latency, reconnect interval, push-to-talk, toasts, favourite wrap,
// startup state, close action, tray notifications, log level / retention, export / import / reset.
// (startupVoice / startupLastPreset are read once in startup(), favoriteWrap in next/prevFavorite(),
// reconnectSeconds in reconnectTicks(), closeAction in performAction ("closeWindow"), trayNotifications in Tray.cpp.)
#include "App/AppController.h"

#include "Core/Paths.h"
#include "Platform/Log.h"

namespace koe
{
namespace
{
juce::String u8 (const char* s) { return juce::String::fromUTF8 (s); }

/** A JSON object that is not a settings file (a preset, say) would silently reset everything. */
bool looksLikeSettings (const juce::var& root)
{
    for (auto* key : { "currentPresetId", "inputDevice", "outputDevice", "voiceChangerOn", "darkTheme", "favorites", "hotkeys" })
        if (root.hasProperty (key)) return true;
    return false;
}
} // namespace

void AppController::applyPlatformSettings (const Settings* before)
{
    // before == nullptr only at startup (startup()); import / reset go through replaceSettings()
    const bool all = before == nullptr;
    logging::setLevel (settings.logLevel);
    if (all)
    {
        // the app's file logger (Main.cpp); tests and tools have none
        if (logFilter == nullptr && juce::Logger::getCurrentLogger() != nullptr)
        {
            logTarget = juce::Logger::getCurrentLogger();
            logFilter = std::make_unique<logging::FilterLogger> (logTarget);
            juce::Logger::setCurrentLogger (logFilter.get());
        }
        if (const int n = logging::deleteOldFiles (paths::logsDir(), settings.logKeepDays); n > 0)
            juce::Logger::writeToLog ("logs: deleted " + juce::String (n) + " file(s) older than " + juce::String (settings.logKeepDays) + " days");
    }
    monitor->setLatencyMode (settings.monitorLatency);
    if (engine != nullptr) engine->setInputChannel (settings.inputChannel);
    if (all || before->pushToTalk != settings.pushToTalk)
    {
        if (settings.pushToTalk == 0)
        {
            pttHeld = false;
            pttTailMs = 0.0f;
        }
        applyMicMute();
    }
    if (! all && before->wasapiExclusive != settings.wasapiExclusive) openDevicesIfReady();
}

// ============================================================================ push-to-talk
void AppController::applyMicMute()
{
    const bool active = pttHeld || pttTailMs > 0.0f;
    const bool pttMute = (settings.pushToTalk == 1 && ! active) || (settings.pushToTalk == 2 && active);
    processor.setMicMute (micMuted || pttMute); // the usual 30 ms mute fade (F-08-8)
}

void AppController::tickPushToTalk()
{
    if (pttHeld)
    {
        if (pttKey != 0 && Hotkeys::isKeyDown (pttKey)) return;
        pttHeld = false;
        pttTailMs = settings.pttReleaseMs;
    }
    else if (pttTailMs > 0.0f)
        pttTailMs -= 1000.0f / 30.0f; // one controller tick
    else
        return;
    if (pttTailMs < 1.0f) // a sub-millisecond rest of the tail counts as done
    {
        pttTailMs = 0.0f;
        applyMicMute();
        sendChangeMessage();
    }
}

void AppController::hotkeyToast (const juce::String& text)
{
    if (settings.hotkeyToasts) toast (text);
}

// ============================================================================ settings file
bool AppController::exportSettings (const juce::File& file, juce::String& error) const
{
    if (saveSettings (settings, file)) return true;
    error = u8 ("書き出せませんでした。保存先のフォルダに書き込めるか確認してください。");
    return false;
}

bool AppController::importSettings (const juce::File& file, juce::String& error)
{
    if (! file.existsAsFile())
    {
        error = u8 ("ファイルが見つかりません。");
        return false;
    }
    // checked here first: loadSettings() moves a broken file aside as .bak (F-11-2), never do that to the user's file
    juce::var root;
    if (juce::JSON::parse (file.loadFileAsString(), root).failed() || ! root.isObject())
    {
        error = u8 ("読み込めませんでした。ファイルが壊れているか、JSON の形式ではありません。");
        return false;
    }
    if (! looksLikeSettings (root))
    {
        error = u8 ("KoeLoom の設定ファイルではありません。");
        return false;
    }
    SettingsLoadResult lr;
    auto next = loadSettings (file, lr); // out-of-range values clamped (E-29)
    for (auto& k : lr.clampedKeys) juce::Logger::writeToLog ("settings import: clamped " + k);
    next.autoStart = settings.autoStart; // mirrors the registry, which only setAutoStart() changes
    replaceSettings (next, true);
    return true;
}

void AppController::resetSettings()
{
    Settings d;
    // kept (§7.3): devices, favourites, hotkeys, setupDone, tourStep; and state that is not a setting
    // (the current voice, autostart = the registry, the one-time soundboard hint)
    d.hasSavedDevices = settings.hasSavedDevices;
    d.deviceType = settings.deviceType;
    d.inputDevice = settings.inputDevice;
    d.outputDevice = settings.outputDevice;
    d.monitorDevice = settings.monitorDevice;
    d.sampleRate = settings.sampleRate;
    d.bufferSize = settings.bufferSize;
    d.favorites = settings.favorites;
    d.hotkeys = settings.hotkeys;
    d.setupDone = settings.setupDone;
    d.tourStep = settings.tourStep;
    d.currentPresetId = settings.currentPresetId;
    d.voiceChangerOn = settings.voiceChangerOn;
    d.autoStart = settings.autoStart;
    d.soundboardHintShown = settings.soundboardHintShown;
    d.momentaryRecipes = settings.momentaryRecipes; // they go with the hotkeys
    d.calibratedTrimDb = settings.calibratedTrimDb; // a measurement, not a preference
    d.calibratedAt = settings.calibratedAt;
    replaceSettings (d, false);
}

void AppController::replaceSettings (const Settings& next, bool reopenDevices)
{
    const Settings before = settings;
    settings = clampSettings (next);
    if (isCableInputName (settings.monitorDevice)) settings.monitorDevice = {}; // F-01-6
    if (allowDevices)
    {
        // F-01-10 as at startup: a device of another PC is not kept
        if (settings.inputDevice.isNotEmpty() && ! inputs.contains (settings.inputDevice))
        {
            addNotice ({ "input.missing", NoticeLevel::warning,
                         u8 ("保存していた入力デバイス「") + settings.inputDevice + u8 ("」が見つかりません。選び直してください。") });
            settings.inputDevice = {};
        }
        if (settings.outputDevice.isNotEmpty() && ! outputs.contains (settings.outputDevice)) settings.outputDevice = {};
        if (settings.monitorDevice.isNotEmpty() && ! outputs.contains (settings.monitorDevice)) settings.monitorDevice = {};
    }
    soundboard->setDuckingDb (settings.duckingDb);
    setOutputGainDb (settings.outputGainDb); // F-12-4 notice either way
    applyEnvironment();                      // ... and applyAudioSettings (nullptr)
    processor.setTrimDb (getEffectiveTrimDb()); // calibratedTrimDb may differ
    auto platformBefore = before;
    platformBefore.wasapiExclusive = settings.wasapiExclusive; // the devices are reopened once, below
    applyPlatformSettings (&platformBefore);
    reapplyHotkeys();
    if (settings.currentPresetId != before.currentPresetId && library->find (settings.currentPresetId.toStdString()) != nullptr)
        loadPreset (settings.currentPresetId.toStdString());
    const bool devicesChanged = settings.inputDevice != before.inputDevice || settings.outputDevice != before.outputDevice
                                || settings.monitorDevice != before.monitorDevice || settings.sampleRate != before.sampleRate
                                || settings.bufferSize != before.bufferSize || settings.wasapiExclusive != before.wasapiExclusive;
    if (reopenDevices || devicesChanged) openDevicesIfReady();
    refreshOutputNotice();
    saveSettingsSoon();
    sendChangeMessage();
}
} // namespace koe

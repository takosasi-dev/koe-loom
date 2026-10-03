#include "Model/Settings.h"

#include "Model/Preset.h"

#include <algorithm>
#include <cmath>

namespace koe
{
namespace
{
/** Reads keys that exist with the right JSON type; wrong types keep the default and are reported. */
struct Reader
{
    const juce::var& o;
    juce::StringArray& bad;

    const juce::var* at (const char* k)
    {
        const auto& v = o[k];
        return v.isVoid() ? nullptr : &v;
    }
    void get (const char* k, bool& out)
    {
        if (auto* v = at (k)) { if (v->isBool()) out = bool (*v); else bad.addIfNotAlreadyThere (k); }
    }
    void get (const char* k, double& out)
    {
        if (auto* v = at (k)) { if (v->isInt() || v->isInt64() || v->isDouble()) out = double (*v); else bad.addIfNotAlreadyThere (k); }
    }
    void get (const char* k, float& out)
    {
        double d = out;
        get (k, d);
        out = float (d);
    }
    void get (const char* k, int& out)
    {
        double d = out;
        get (k, d);
        out = int (std::lround (juce::jlimit (-1.0e9, 1.0e9, d)));
    }
    void get (const char* k, juce::String& out)
    {
        if (auto* v = at (k)) { if (v->isString()) out = v->toString(); else bad.addIfNotAlreadyThere (k); }
    }
};
} // namespace

Settings clampSettings (const Settings& in, juce::StringArray* clampedKeys)
{
    Settings s = in;
    juce::StringArray changed;
    auto range = [&changed] (const char* key, float& v, const Range& r) {
        const float c = std::isfinite (v) ? r.clamp (v) : r.def;
        if (c != v) { v = c; changed.add (key); }
    };
    range ("inputGainDb", s.inputGainDb, kInputGainDb);
    range ("noiseMix", s.noiseMix, kNoiseMix);
    range ("gateThresholdDb", s.gateThresholdDb, kGateThresholdDb);
    range ("gateAttackMs", s.gateAttackMs, kGateAttackMs);
    range ("gateHoldMs", s.gateHoldMs, kGateHoldMs);
    range ("gateReleaseMs", s.gateReleaseMs, kGateReleaseMs);
    range ("outputGainDb", s.outputGainDb, kOutputGainDb);
    range ("monitorVolumeDb", s.monitorVolumeDb, kMonitorVolumeDb);
    range ("duckingDb", s.duckingDb, kDuckingDb);
    range ("pitchMinHz", s.pitchMinHz, kPitchMinHz);
    range ("pitchMaxHz", s.pitchMaxHz, kPitchMaxHz);
    range ("highPassHz", s.highPassHz, kHighPassHz);
    range ("agcTargetDb", s.agcTargetDb, kAgcTargetDb);
    range ("agcMaxGainDb", s.agcMaxGainDb, kAgcMaxGainDb);
    range ("limiterCeilingDb", s.limiterCeilingDb, kLimiterCeilingSetDb);
    range ("limiterReleaseMs", s.limiterReleaseMs, kLimiterReleaseMs);
    range ("presetCrossfadeMs", s.presetCrossfadeMs, kPresetCrossfadeMs);
    range ("soundFadeMs", s.soundFadeMs, kSoundFadeMs);
    range ("duckAttackMs", s.duckAttackMs, kDuckAttackMs);
    range ("duckReleaseMs", s.duckReleaseMs, kDuckReleaseMs);
    range ("pttReleaseMs", s.pttReleaseMs, kPttReleaseMs);
    range ("meterPeakHoldMs", s.meterPeakHoldMs, kMeterPeakHoldMs);
    range ("tooltipDelayMs", s.tooltipDelayMs, kTooltipDelayMs);
    if (s.pitchMinHz >= s.pitchMaxHz) { s.pitchMinHz = kPitchMinHz.def; s.pitchMaxHz = kPitchMaxHz.def; changed.addIfNotAlreadyThere ("pitchMinHz"); }

    auto choice = [&changed] (const char* key, int& v, int lo, int hi, int def) {
        if (v < lo || v > hi) { v = def; changed.add (key); }
    };
    const Settings d;
    choice ("converterQuality", s.converterQuality, 0, 2, d.converterQuality);
    choice ("soundboardMaxVoices", s.soundboardMaxVoices, 1, kSoundboardMaxVoices, d.soundboardMaxVoices);
    choice ("inputChannel", s.inputChannel, 0, 3, d.inputChannel);
    choice ("monitorLatency", s.monitorLatency, 0, 2, d.monitorLatency);
    choice ("reconnectSeconds", s.reconnectSeconds, 1, 10, d.reconnectSeconds);
    choice ("pushToTalk", s.pushToTalk, 0, 2, d.pushToTalk);
    choice ("startupVoice", s.startupVoice, 0, 2, d.startupVoice);
    choice ("closeAction", s.closeAction, 0, 1, d.closeAction);
    choice ("logLevel", s.logLevel, 0, 2, d.logLevel);
    choice ("logKeepDays", s.logKeepDays, 1, 30, d.logKeepDays);
    choice ("animations", s.animations, 0, 2, d.animations);
    choice ("knobSensitivity", s.knobSensitivity, 0, 2, d.knobSensitivity);
    choice ("layoutStyle", s.layoutStyle, 0, 2, d.layoutStyle);
    if (s.meterFps != 30 && s.meterFps != 60) { s.meterFps = d.meterFps; changed.add ("meterFps"); }
    if (std::find (std::begin (kUiScalePercents), std::end (kUiScalePercents), s.uiScalePercent) == std::end (kUiScalePercents))
    {
        s.uiScalePercent = d.uiScalePercent;
        changed.add ("uiScalePercent");
    }

    if (const float g = std::round (s.outputGainDb / kOutputGainStepDb) * kOutputGainStepDb; g != s.outputGainDb)
    {
        s.outputGainDb = g;
        changed.addIfNotAlreadyThere ("outputGainDb");
    }
    if (! (std::isfinite (s.sampleRate) && s.sampleRate > 0.0)) { s.sampleRate = kSampleRate; changed.add ("sampleRate"); }
    if (s.bufferSize <= 0) { s.bufferSize = kDefaultBufferSize; changed.add ("bufferSize"); }
    if (const int t = juce::jlimit (-1, 7, s.tourStep); t != s.tourStep) { s.tourStep = t; changed.add ("tourStep"); }
    if (s.accentColour < 0 || s.accentColour >= kAccentColours) { s.accentColour = 0; changed.add ("accentColour"); }
    if (s.backgroundTone < 0 || s.backgroundTone >= kBackgroundTones) { s.backgroundTone = 0; changed.add ("backgroundTone"); }
    if (! isValidPresetId (s.currentPresetId.toStdString())) { s.currentPresetId = Settings().currentPresetId; changed.add ("currentPresetId"); }

    auto fav = s.favorites;
    fav.removeEmptyStrings();
    fav.removeDuplicates (false);
    while (fav.size() > kMaxFavorites) fav.remove (fav.size() - 1);
    if (fav != s.favorites) { s.favorites = fav; changed.add ("favorites"); }

    std::vector<HotkeyBinding> keys;
    for (auto h : s.hotkeys)
    {
        h.modifiers &= 0xF;
        if (h.action.isNotEmpty() && h.virtualKey > 0 && h.virtualKey < 255) keys.push_back (h);
    }
    if (keys != s.hotkeys) { s.hotkeys = std::move (keys); changed.add ("hotkeys"); }

    if (clampedKeys != nullptr)
        for (auto& k : changed) clampedKeys->addIfNotAlreadyThere (k);
    return s;
}

Settings loadSettings (const juce::File& file, SettingsLoadResult& result)
{
    result = {};
    Settings s;
    if (! file.existsAsFile())
    {
        result.fileMissing = true;
        return s;
    }

    juce::var root;
    if (juce::JSON::parse (file.loadFileAsString(), root).failed() || ! root.isObject())
    {
        // F-11-2: defaults, broken file kept as settings.json.bak
        result.corrupted = true;
        result.backupFile = file.getSiblingFile (file.getFileName() + ".bak");
        if (! file.moveFileTo (result.backupFile)) result.backupFile = juce::File();
        return s;
    }

    Reader r { root, result.clampedKeys };
    r.get ("hasSavedDevices", s.hasSavedDevices);
    r.get ("deviceType", s.deviceType);
    r.get ("inputDevice", s.inputDevice);
    r.get ("outputDevice", s.outputDevice);
    r.get ("monitorDevice", s.monitorDevice);
    r.get ("sampleRate", s.sampleRate);
    r.get ("bufferSize", s.bufferSize);
    r.get ("inputGainDb", s.inputGainDb);
    r.get ("noiseSuppressionOn", s.noiseSuppressionOn);
    r.get ("noiseMix", s.noiseMix);
    r.get ("gateOn", s.gateOn);
    r.get ("gateThresholdDb", s.gateThresholdDb);
    r.get ("gateAttackMs", s.gateAttackMs);
    r.get ("gateHoldMs", s.gateHoldMs);
    r.get ("gateReleaseMs", s.gateReleaseMs);
    r.get ("outputGainDb", s.outputGainDb);
    r.get ("monitorVolumeDb", s.monitorVolumeDb);
    r.get ("currentPresetId", s.currentPresetId);
    r.get ("voiceChangerOn", s.voiceChangerOn);
    r.get ("darkTheme", s.darkTheme);
    r.get ("accentColour", s.accentColour);
    r.get ("backgroundTone", s.backgroundTone);
    r.get ("softwareRenderer", s.softwareRenderer);
    r.get ("startMinimized", s.startMinimized);
    r.get ("autoStart", s.autoStart);
    r.get ("confirmOnExit", s.confirmOnExit);
    r.get ("multicore", s.multicore);
    r.get ("setupDone", s.setupDone);
    r.get ("tourStep", s.tourStep);
    r.get ("soundboardHintShown", s.soundboardHintShown);
    r.get ("duckingDb", s.duckingDb);
    r.get ("converterQuality", s.converterQuality);
    r.get ("pitchMinHz", s.pitchMinHz);
    r.get ("pitchMaxHz", s.pitchMaxHz);
    r.get ("highPassOn", s.highPassOn);
    r.get ("highPassHz", s.highPassHz);
    r.get ("agcOn", s.agcOn);
    r.get ("agcTargetDb", s.agcTargetDb);
    r.get ("agcMaxGainDb", s.agcMaxGainDb);
    r.get ("limiterCeilingDb", s.limiterCeilingDb);
    r.get ("limiterReleaseMs", s.limiterReleaseMs);
    r.get ("presetCrossfadeMs", s.presetCrossfadeMs);
    r.get ("soundboardMaxVoices", s.soundboardMaxVoices);
    r.get ("soundFadeMs", s.soundFadeMs);
    r.get ("duckAttackMs", s.duckAttackMs);
    r.get ("duckReleaseMs", s.duckReleaseMs);
    r.get ("monitorIncludeSoundboard", s.monitorIncludeSoundboard);
    r.get ("wasapiExclusive", s.wasapiExclusive);
    r.get ("inputChannel", s.inputChannel);
    r.get ("monitorLatency", s.monitorLatency);
    r.get ("reconnectSeconds", s.reconnectSeconds);
    r.get ("pushToTalk", s.pushToTalk);
    r.get ("pttReleaseMs", s.pttReleaseMs);
    r.get ("hotkeyToasts", s.hotkeyToasts);
    r.get ("favoriteWrap", s.favoriteWrap);
    r.get ("startupVoice", s.startupVoice);
    r.get ("startupLastPreset", s.startupLastPreset);
    r.get ("closeAction", s.closeAction);
    r.get ("trayNotifications", s.trayNotifications);
    r.get ("logLevel", s.logLevel);
    r.get ("logKeepDays", s.logKeepDays);
    r.get ("uiScalePercent", s.uiScalePercent);
    r.get ("alwaysOnTop", s.alwaysOnTop);
    r.get ("animations", s.animations);
    r.get ("meterFps", s.meterFps);
    r.get ("meterPeakHoldMs", s.meterPeakHoldMs);
    r.get ("tooltipDelayMs", s.tooltipDelayMs);
    r.get ("knobSensitivity", s.knobSensitivity);
    r.get ("knobWheel", s.knobWheel);
    r.get ("settingsShowDetails", s.settingsShowDetails);
    r.get ("autoUpdate", s.autoUpdate);
    r.get ("updateIncludePrerelease", s.updateIncludePrerelease);
    r.get ("updateSkippedVersion", s.updateSkippedVersion);
    r.get ("layoutStyle", s.layoutStyle);
    r.get ("themeId", s.themeId);

    if (auto* fav = root["favorites"].getArray())
    {
        for (auto& v : *fav)
            if (v.isString()) s.favorites.add (v.toString());
    }
    if (auto* hk = root["hotkeys"].getArray())
    {
        for (auto& v : *hk)
        {
            HotkeyBinding h;
            juce::StringArray ignored;
            Reader hr { v, ignored };
            hr.get ("action", h.action);
            hr.get ("modifiers", h.modifiers);
            hr.get ("virtualKey", h.virtualKey);
            s.hotkeys.push_back (h);
        }
    }
    return clampSettings (s, &result.clampedKeys);
}

bool saveSettings (const Settings& s, const juce::File& file)
{
    auto* o = new juce::DynamicObject();
    juce::var root (o);
    o->setProperty ("hasSavedDevices", s.hasSavedDevices);
    o->setProperty ("deviceType", s.deviceType);
    o->setProperty ("inputDevice", s.inputDevice);
    o->setProperty ("outputDevice", s.outputDevice);
    o->setProperty ("monitorDevice", s.monitorDevice);
    o->setProperty ("sampleRate", s.sampleRate);
    o->setProperty ("bufferSize", s.bufferSize);
    o->setProperty ("inputGainDb", s.inputGainDb);
    o->setProperty ("noiseSuppressionOn", s.noiseSuppressionOn);
    o->setProperty ("noiseMix", s.noiseMix);
    o->setProperty ("gateOn", s.gateOn);
    o->setProperty ("gateThresholdDb", s.gateThresholdDb);
    o->setProperty ("gateAttackMs", s.gateAttackMs);
    o->setProperty ("gateHoldMs", s.gateHoldMs);
    o->setProperty ("gateReleaseMs", s.gateReleaseMs);
    o->setProperty ("outputGainDb", s.outputGainDb);
    o->setProperty ("monitorVolumeDb", s.monitorVolumeDb);
    o->setProperty ("currentPresetId", s.currentPresetId);
    juce::Array<juce::var> fav;
    for (auto& f : s.favorites) fav.add (f);
    o->setProperty ("favorites", fav);
    o->setProperty ("voiceChangerOn", s.voiceChangerOn);
    o->setProperty ("darkTheme", s.darkTheme);
    o->setProperty ("accentColour", s.accentColour);
    o->setProperty ("backgroundTone", s.backgroundTone);
    o->setProperty ("softwareRenderer", s.softwareRenderer);
    o->setProperty ("startMinimized", s.startMinimized);
    o->setProperty ("autoStart", s.autoStart);
    o->setProperty ("confirmOnExit", s.confirmOnExit);
    o->setProperty ("multicore", s.multicore);
    juce::Array<juce::var> keys;
    for (auto& h : s.hotkeys)
    {
        auto* ho = new juce::DynamicObject();
        ho->setProperty ("action", h.action);
        ho->setProperty ("modifiers", h.modifiers);
        ho->setProperty ("virtualKey", h.virtualKey);
        keys.add (juce::var (ho));
    }
    o->setProperty ("hotkeys", keys);
    o->setProperty ("setupDone", s.setupDone);
    o->setProperty ("tourStep", s.tourStep);
    o->setProperty ("soundboardHintShown", s.soundboardHintShown);
    o->setProperty ("duckingDb", s.duckingDb);
    o->setProperty ("converterQuality", s.converterQuality);
    o->setProperty ("pitchMinHz", s.pitchMinHz);
    o->setProperty ("pitchMaxHz", s.pitchMaxHz);
    o->setProperty ("highPassOn", s.highPassOn);
    o->setProperty ("highPassHz", s.highPassHz);
    o->setProperty ("agcOn", s.agcOn);
    o->setProperty ("agcTargetDb", s.agcTargetDb);
    o->setProperty ("agcMaxGainDb", s.agcMaxGainDb);
    o->setProperty ("limiterCeilingDb", s.limiterCeilingDb);
    o->setProperty ("limiterReleaseMs", s.limiterReleaseMs);
    o->setProperty ("presetCrossfadeMs", s.presetCrossfadeMs);
    o->setProperty ("soundboardMaxVoices", s.soundboardMaxVoices);
    o->setProperty ("soundFadeMs", s.soundFadeMs);
    o->setProperty ("duckAttackMs", s.duckAttackMs);
    o->setProperty ("duckReleaseMs", s.duckReleaseMs);
    o->setProperty ("monitorIncludeSoundboard", s.monitorIncludeSoundboard);
    o->setProperty ("wasapiExclusive", s.wasapiExclusive);
    o->setProperty ("inputChannel", s.inputChannel);
    o->setProperty ("monitorLatency", s.monitorLatency);
    o->setProperty ("reconnectSeconds", s.reconnectSeconds);
    o->setProperty ("pushToTalk", s.pushToTalk);
    o->setProperty ("pttReleaseMs", s.pttReleaseMs);
    o->setProperty ("hotkeyToasts", s.hotkeyToasts);
    o->setProperty ("favoriteWrap", s.favoriteWrap);
    o->setProperty ("startupVoice", s.startupVoice);
    o->setProperty ("startupLastPreset", s.startupLastPreset);
    o->setProperty ("closeAction", s.closeAction);
    o->setProperty ("trayNotifications", s.trayNotifications);
    o->setProperty ("logLevel", s.logLevel);
    o->setProperty ("logKeepDays", s.logKeepDays);
    o->setProperty ("uiScalePercent", s.uiScalePercent);
    o->setProperty ("alwaysOnTop", s.alwaysOnTop);
    o->setProperty ("animations", s.animations);
    o->setProperty ("meterFps", s.meterFps);
    o->setProperty ("meterPeakHoldMs", s.meterPeakHoldMs);
    o->setProperty ("tooltipDelayMs", s.tooltipDelayMs);
    o->setProperty ("knobSensitivity", s.knobSensitivity);
    o->setProperty ("knobWheel", s.knobWheel);
    o->setProperty ("settingsShowDetails", s.settingsShowDetails);
    o->setProperty ("autoUpdate", s.autoUpdate);
    o->setProperty ("updateIncludePrerelease", s.updateIncludePrerelease);
    o->setProperty ("updateSkippedVersion", s.updateSkippedVersion);
    o->setProperty ("layoutStyle", s.layoutStyle);
    o->setProperty ("themeId", s.themeId);

    // replaceWithText writes a temporary file next to the target and then replaces it (atomic save)
    file.getParentDirectory().createDirectory();
    return file.replaceWithText (juce::JSON::toString (root), false, false, "\n");
}
} // namespace koe

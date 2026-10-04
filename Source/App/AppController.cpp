#include "App/AppController.h"

#include "Core/Paths.h"
#include "Effects/EffectRegistry.h"
#include "Platform/AutoStart.h"
#include "Platform/Log.h"

#include <algorithm>
#include <cmath>

namespace koe
{
namespace
{
juce::String u8 (const char* s) { return juce::String::fromUTF8 (s); }

int heavyOnCount (const std::vector<SlotDef>& chain, int except = -1)
{
    int n = 0;
    for (int i = 0; i < int (chain.size()); ++i)
    {
        if (i == except || ! chain[size_t (i)].enabled) continue;
        if (auto* info = findEffectInfo (chain[size_t (i)].type); info != nullptr && info->weight == EffectWeight::heavy) ++n;
    }
    return n;
}

juce::String effectName (const std::string& type)
{
    auto* info = findEffectInfo (type);
    return info != nullptr ? u8 (info->nameJa) : juce::String (type);
}

bool looksLikeSpeaker (const juce::String& name)
{
    return name.containsIgnoreCase ("speaker") || name.contains (u8 ("スピーカー")) || name.containsIgnoreCase ("HDMI")
           || name.containsIgnoreCase ("SPDIF") || name.containsIgnoreCase ("Digital Output");
}
} // namespace

// The chain object only contains slots whose type has an implementation; this maps model slot
// indices to chain slot indices (identical once every type is implemented).
static int chainIndexFor (const std::vector<SlotDef>& chain, int modelIndex)
{
    if (modelIndex < 0 || modelIndex >= int (chain.size())) return -1;
    if (! hasEffectFactory (chain[size_t (modelIndex)].type)) return -1;
    int idx = 0;
    for (int i = 0; i < modelIndex; ++i)
        if (hasEffectFactory (chain[size_t (i)].type)) ++idx;
    return idx;
}

AppController::AppController (bool openDevices) : allowDevices (openDevices)
{
    library = std::make_unique<PresetLibrary> (paths::presetsDir());
    monitor = std::make_unique<MonitorOutput>();
    soundboard = std::make_unique<Soundboard>();
    soundboard->onStateChanged = [this] { sendChangeMessage(); }; // loaded / failed / started / stopped
    cableProbe = std::make_unique<CableProbe>();
    if (allowDevices)
    {
        engine = std::make_unique<AudioEngine> (processor);
        engine->onDeviceListChanged = [this]
        {
            rescanDevices();
            if (deviceLost) reopenCountdown = 1;
            if (monitorLost) monitorRetryCountdown = 1;
        };
        engine->onDeviceError = [this] (const juce::String& msg)
        {
            logging::write (logging::error, "device error: " + msg);
            deviceLost = true;
            reopenCountdown = reconnectTicks(); // try again in reconnectSeconds (1 s by default)
            addNotice ({ "device.lost", NoticeLevel::danger, u8 ("オーディオデバイスが止まりました（抜かれた可能性があります）。戻ると自動で再開します。"), false });
            sendChangeMessage();
        };
        hotkeys = std::make_unique<Hotkeys>();
        hotkeys->onHotkey = [this] (const juce::String& action) { performAction (action); };
    }
    processor.setAuxSource (soundboard.get());
    processor.setMonitorSink (monitor.get());
    processor.prepare (kSampleRate, kDefaultBufferSize); // offline until a device opens
    scalePitch.prepare (kSampleRate, kDefaultBufferSize);
    processor.setScaleLayerPitch (&scalePitch);
}

AppController::~AppController()
{
    stopTimer();
    shutdownCapture();   // wave 8 (INTERFACES.md §10): idempotent, shutdown() may have run them already
    shutdownAnalysis();
    shutdownAutomation();
    shutdownMicEq();     // wave 9 (INTERFACES.md §11)
    shutdownStream();
    processor.setAuxSource (nullptr);
    processor.setMonitorSink (nullptr);
    if (engine != nullptr) engine->close();
    if (monitor != nullptr) monitor->close();
    if (logFilter != nullptr && juce::Logger::getCurrentLogger() == logFilter.get()) juce::Logger::setCurrentLogger (logTarget);
}

// ============================================================================ startup / shutdown
void AppController::startup()
{
    SettingsLoadResult lr;
    settings = loadSettings (paths::settingsFile(), lr);
    if (lr.corrupted)
        addNotice ({ "settings.corrupt", NoticeLevel::warning,
                     u8 ("設定ファイルが壊れていたため、初期設定で起動しました（元のファイルは settings.json.bak に残しました）。") });
    for (auto& k : lr.clampedKeys) juce::Logger::writeToLog ("settings: clamped " + k);
    if (isCableInputName (settings.monitorDevice)) settings.monitorDevice = {}; // F-01-6, also for a hand-edited file
    if (settings.outputGainDb > kOutputGainWarnDb) setOutputGainDb (settings.outputGainDb); // F-12-4 at startup
    if (settings.startupVoice != 0) settings.voiceChangerOn = settings.startupVoice == 1; // S-03 詳細 (0 = as left)
    if (! settings.startupLastPreset) settings.currentPresetId = "natural-asis";
    applyPlatformSettings (nullptr);

    for (auto& n : library->reload())
        addNotice ({ "presets.load", NoticeLevel::info, n });

    micMuted = false;
    monitorOn = false;
    const auto* p = library->find (settings.currentPresetId.toStdString());
    if (p == nullptr) p = library->find ("natural-asis");
    if (p != nullptr)
    {
        current = *p;
        currentBaseId = p->id;
    }
    soundboard->load (paths::soundboardFile());
    soundboard->setDuckingDb (settings.duckingDb);
    applyEnvironment();
    applyPresetToEngine (true);

    if (allowDevices)
    {
        engine->scanDevices();
        inputs = engine->getInputNames();
        outputs = engine->getOutputNames();
        // F-01-10: keep each saved device only if it exists; never fall back to Windows' default
        if (settings.inputDevice.isNotEmpty() && ! inputs.contains (settings.inputDevice))
        {
            addNotice ({ "input.missing", NoticeLevel::warning, u8 ("保存していた入力デバイス「") + settings.inputDevice + u8 ("」が見つかりません。選び直してください。") });
            settings.inputDevice = {};
        }
        if (settings.outputDevice.isNotEmpty() && ! outputs.contains (settings.outputDevice))
            settings.outputDevice = {};
        if (! settings.hasSavedDevices && settings.outputDevice.isEmpty())
        {
            // F-01-2: first run only, pick the cable's play side if there is exactly one
            juce::StringArray cables;
            for (auto& o : outputs) if (isCableInputName (o)) cables.add (o);
            if (cables.size() > 1) // VB-CABLE also adds "CABLE In 16 Ch": prefer the ordinary play side
                for (int i = cables.size(); --i >= 0;)
                    if (cables[i].containsIgnoreCase ("16 Ch") && cables.size() > 1) cables.remove (i);
            if (cables.size() == 1) settings.outputDevice = cables[0];
        }
        if (settings.monitorDevice.isNotEmpty() && ! outputs.contains (settings.monitorDevice)) settings.monitorDevice = {};
        openDevicesIfReady();
        reapplyHotkeys();
    }
    refreshOutputNotice();
    startTimerHz (30);
    sendChangeMessage();
}

void AppController::shutdown()
{
    stopTimer();
    settings.hasSavedDevices = true;
    if (! saveSettings (settings, paths::settingsFile())) logging::write (logging::error, "settings: save failed");
    soundboard->save (paths::soundboardFile());
    cableProbe->stop();
    shutdownCapture();   // wave 8 (INTERFACES.md §10): finish files and background threads while the devices still run
    shutdownAnalysis();
    shutdownAutomation();
    shutdownMicEq();     // wave 9 (INTERFACES.md §11)
    shutdownStream();
    closeDevices();
    if (hotkeys != nullptr) hotkeys->unregisterAll();
    finishUpdateOnQuit(); // §7.4: a ready update replaces the exe when the app quits
}

// ============================================================================ devices
juce::StringArray AppController::getInputDevices() const { return inputs; }
juce::StringArray AppController::getOutputDevices() const { return outputs; }

juce::StringArray AppController::getMonitorDevices() const
{
    juce::StringArray r;
    for (auto& o : outputs) if (! isCableInputName (o)) r.add (o); // F-01-6
    return r;
}

// Japanese Windows can name VB-CABLE's play side "スピーカー (VB-Audio Virtual Cable)" (seen 2026-10-04), so the
// driver name counts too. The play side is only ever looked up among outputs and the record side among inputs.
bool AppController::isCableInputName (const juce::String& n)
{
    return n.containsIgnoreCase ("CABLE In") || (n.containsIgnoreCase ("VB-Audio Virtual Cable") && ! n.containsIgnoreCase ("CABLE Output"));
}
bool AppController::isCableOutputName (const juce::String& n)
{
    return n.containsIgnoreCase ("CABLE Output") || (n.containsIgnoreCase ("VB-Audio Virtual Cable") && ! n.containsIgnoreCase ("CABLE In"));
}

bool AppController::isVirtualCableInstalled() const
{
    for (auto& o : outputs) if (isCableInputName (o)) return true;
    return false;
}

void AppController::rescanDevices()
{
    if (engine == nullptr) return;
    engine->scanDevices();
    inputs = engine->getInputNames();
    outputs = engine->getOutputNames();
    refreshOutputNotice();
    sendChangeMessage();
}

void AppController::setInputDevice (const juce::String& name)
{
    if (settings.inputDevice == name) return;
    settings.inputDevice = name;
    settings.hasSavedDevices = true;
    removeNotice ("input.missing");
    saveSettingsSoon();
    openDevicesIfReady();
    sendChangeMessage();
}

void AppController::setOutputDevice (const juce::String& name)
{
    if (settings.outputDevice == name) return;
    settings.outputDevice = name;
    settings.hasSavedDevices = true;
    dismissed.removeString ("output.unset");    // F-01-11: a new output may warn again
    dismissed.removeString ("output.notcable");
    saveSettingsSoon();
    openDevicesIfReady();
    refreshOutputNotice();
    sendChangeMessage();
}

void AppController::setMonitorDevice (const juce::String& name)
{
    if (isCableInputName (name)) return; // F-01-6
    settings.monitorDevice = name;
    saveSettingsSoon();
    if (monitorOn)
    {
        setMonitorOn (false);
        monitor->close(); // OFF only mutes it: without this, ON finds the old device still open and keeps it
        setMonitorOn (true);
    }
    sendChangeMessage();
}

juce::Array<double> AppController::getAvailableSampleRates() const
{
    if (engine == nullptr || settings.inputDevice.isEmpty() || settings.outputDevice.isEmpty()) return { 44100.0, 48000.0 };
    return engine->getCaps (settings.inputDevice, settings.outputDevice).sampleRates;
}

juce::Array<int> AppController::getAvailableBufferSizes() const
{
    if (engine == nullptr || settings.inputDevice.isEmpty() || settings.outputDevice.isEmpty()) return { 128, 256, 480, 512, 960 };
    return engine->getCaps (settings.inputDevice, settings.outputDevice).bufferSizes;
}

void AppController::setSampleRate (double rate)
{
    settings.sampleRate = rate;
    saveSettingsSoon();
    openDevicesIfReady();
}

void AppController::setBufferSize (int samples)
{
    settings.bufferSize = samples;
    saveSettingsSoon();
    openDevicesIfReady();
}

void AppController::closeDevices()
{
    closeStream();       // wave 9 (INTERFACES.md §11)
    if (monitor != nullptr) monitor->close();
    if (engine != nullptr) engine->close();
}

void AppController::openDevicesIfReady()
{
    if (engine == nullptr) return;
    closeDevices();
    lastError = {};
    removeNotice ("device.open");
    loopConfig = isCableOutputName (settings.inputDevice) && isCableInputName (settings.outputDevice);
    if (loopConfig)
    {
        // F-01-5: never start a loop (the mic would hear its own output)
        addNotice ({ "loop", NoticeLevel::danger, u8 ("ループ構成のため開始できません（入力が CABLE Output、出力が仮想ケーブルの再生側になっています）。入力をマイクにしてください。"), false });
        return;
    }
    removeNotice ("loop");
    if (inputs.isEmpty())
        addNotice ({ "input.none", NoticeLevel::danger, u8 ("入力デバイスが見つかりません。マイクを接続してください。"), false });
    else
        removeNotice ("input.none");
    if (settings.inputDevice.isEmpty() || settings.outputDevice.isEmpty()) return; // F-01-10

    // the audio thread starts inside open(): hide the pitch provider until it is prepared for the new rate
    processor.setScaleLayerPitch (nullptr);
    auto err = engine->open (settings.inputDevice, settings.outputDevice, settings.sampleRate, settings.bufferSize, settings.wasapiExclusive);
    removeNotice ("device.exclusive");
    if (err.isNotEmpty() && settings.wasapiExclusive)
    {
        // S-03 詳細: exclusive refused -> E-04 notice, carry on in shared mode
        logging::write (logging::error, "exclusive open failed: " + err);
        addNotice ({ "device.exclusive", NoticeLevel::warning,
                     u8 ("排他モードで開けなかったため、共有モードで開きました（") + err
                         + u8 ("）。他のアプリがデバイスを使っていないか確認してください。"), true,
                     u8 ("設定で変更"), "openSettings.devices" });
        err = engine->open (settings.inputDevice, settings.outputDevice, settings.sampleRate, settings.bufferSize, false);
    }
    scalePitch.prepare (processor.getSampleRate(), processor.getMaxBlockSize());
    processor.setScaleLayerPitch (&scalePitch);
    if (err.isNotEmpty())
    {
        lastError = err;
        // E-04: most often another app holds the device in exclusive mode
        addNotice ({ "device.open", NoticeLevel::danger,
                     u8 ("デバイスを開けませんでした（") + err + u8 ("）。他のアプリが排他モードで使っていないか確認してください。"), true,
                     u8 ("設定で変更"), "openSettings.devices" });
        logging::write (logging::error, "open failed: " + err);
        return;
    }
    deviceLost = false;
    removeNotice ("device.lost");
    const double rate = engine->getSampleRate();
    if (std::abs (rate - settings.sampleRate) > 1.0)
        toast (juce::String (settings.sampleRate, 0) + u8 (" Hz に対応していないため、") + juce::String (rate, 0) + u8 (" Hz で開きました。")); // E-05
    if (settings.noiseSuppressionOn && std::abs (rate - 48000.0) > 1.0)
        toast (u8 ("ノイズ抑制は 48000 Hz でだけ使えます。"));
    juce::Logger::writeToLog ("opened: in=" + settings.inputDevice + " out=" + settings.outputDevice + " rate=" + juce::String (rate)
                              + " buffer=" + juce::String (engine->getBufferSize()) + (engine->isExclusive() ? " exclusive" : ""));
    applyEnvironment();
    applyPresetToEngine (true); // the processor was re-prepared: build the chain for its block size
    soundboard->prepare (rate);
    if (monitorOn && settings.monitorDevice.isNotEmpty())
    {
        if (const auto monitorErr = monitor->open (settings.monitorDevice, rate); monitorErr.isEmpty()) monitor->setEnabled (true);
        else monitorDeviceGone (monitorErr);
    }
    openStream();        // wave 9 (INTERFACES.md §11)
}

void AppController::reprepareForTests (double rate, int buffer)
{
    // keep in step with openDevicesIfReady() (engine->open() prepares the processor)
    processor.setScaleLayerPitch (nullptr);
    processor.prepare (rate, buffer);
    scalePitch.prepare (processor.getSampleRate(), processor.getMaxBlockSize());
    processor.setScaleLayerPitch (&scalePitch);
    applyEnvironment();
    applyPresetToEngine (true);
    soundboard->prepare (rate);
}

AppController::Status AppController::getStatus() const
{
    Status s;
    s.loopConfig = loopConfig;
    s.deviceLost = deviceLost;
    s.error = lastError;
    s.monitorOn = isMonitorOn();
    s.gateOpen = processor.isGateOpen();
    s.limiterActive = limiterHold > 0;
    if (engine != nullptr && engine->isRunning())
    {
        s.running = true;
        s.sampleRate = engine->getSampleRate();
        s.bufferSize = engine->getBufferSize();
        const int total = engine->getDeviceLatencySamples() + processor.getLatencySamples();
        s.latencyMs = float (1000.0 * total / s.sampleRate);
        s.xruns = engine->getXRunCount();
        s.cpuPercent = engine->getCpuLoad() * 100.0f;
    }
    else
    {
        s.sampleRate = processor.getSampleRate();
        s.latencyMs = float (1000.0 * processor.getLatencySamples() / s.sampleRate);
    }
    s.latencyWarn = s.latencyMs > kLatencyWarnMs;
    return s;
}

AppController::Meters AppController::pollMeters()
{
    const auto m = processor.fetchMeters();
    if (processor.fetchLimiterActive()) limiterHold = 9; // ~300 ms at 30 fps
    return { dsp::gainToDb (m.inputPeak), dsp::gainToDb (m.outputPeak), m.inputClip, m.outputClip };
}

// ============================================================================ voice
void AppController::setVoiceChangerOn (bool on)
{
    settings.voiceChangerOn = on;
    processor.setVoiceChangerOn (on && ! compareHeld); // §9.4: OFF while 聞き比べ is held
    saveSettingsSoon();
    sendChangeMessage();
}

void AppController::setMicMuted (bool on)
{
    micMuted = on;
    applyMicMute();
    sendChangeMessage();
}

void AppController::setPitch (float st)
{
    current.pitchSt = std::round (kPitchSt.clamp (st) * 10.0f) / 10.0f;
    current.hasShifter = true;
    processor.setShifter (true, current.pitchSt, current.formantSt);
    markModified();
}

void AppController::setFormant (float st)
{
    current.formantSt = std::round (kFormantSt.clamp (st) * 10.0f) / 10.0f;
    current.hasShifter = true;
    processor.setShifter (true, current.pitchSt, current.formantSt);
    markModified();
}

void AppController::setShifterEnabled (bool on)
{
    if (current.hasShifter == on) return;
    current.hasShifter = on;
    processor.setShifter (on, current.pitchSt, current.formantSt);
    markModified();
}

LayerDef AppController::getLayer (int i) const
{
    return i >= 0 && i < int (current.layers.size()) ? current.layers[size_t (i)] : LayerDef();
}

bool AppController::addLayer (juce::String& whyNot)
{
    if (int (current.layers.size()) >= kMaxLayers)
    {
        whyNot = u8 ("重ねる声は 2 声までです。");
        return false;
    }
    LayerDef l;
    l.pitchSt = -12.0f;
    l.formantSt = -6.0f;
    l.levelDb = -10.0f;
    current.layers.push_back (l);
    current.hasShifter = true;
    applyPresetToEngine (false);
    markModified();
    return true;
}

void AppController::removeLayer (int i)
{
    if (i < 0 || i >= int (current.layers.size())) return;
    current.layers.erase (current.layers.begin() + i);
    applyPresetToEngine (false);
    markModified();
}

void AppController::setLayer (int i, const LayerDef& def)
{
    if (i < 0 || i >= int (current.layers.size())) return;
    auto d = def;
    d.pitchSt = kPitchSt.clamp (d.pitchSt);
    d.formantSt = kFormantSt.clamp (d.formantSt);
    d.levelDb = kLayerLevelDb.clamp (d.levelDb);
    d.degree = std::clamp (d.degree, -7, 7);
    if (d.degree == 0) d.degree = 1;
    d.key = std::clamp (d.key, 0, 11);
    current.layers[size_t (i)] = d;
    applyPresetToEngine (false);
    markModified();
}

void AppController::setLayerEnabled (int i, bool on)
{
    if (i < 0 || i >= int (current.layers.size()) || current.layers[size_t (i)].enabled == on) return;
    current.layers[size_t (i)].enabled = on;
    applyPresetToEngine (false);
    markModified();
}

bool AppController::areLayersAutoStopped() const { return processor.areLayersAutoStopped(); }

void AppController::resumeLayers()
{
    processor.clearLayerAutoStop();
    sendChangeMessage();
}

// ============================================================================ chain
bool AppController::addEffect (const std::string& type, juce::String& whyNot)
{
    auto* info = findEffectInfo (type);
    if (info == nullptr || ! hasEffectFactory (type))
    {
        whyNot = u8 ("このエフェクトはまだ使えません。");
        return false;
    }
    if (int (current.chain.size()) >= kMaxSlots)
    {
        whyNot = u8 ("スロットは 10 個までです。"); // F-04-1
        return false;
    }
    if (type == "freeze" || type == "looper")
        for (auto& s : current.chain)
            if (s.type == type)
            {
                whyNot = effectName (type) + u8 (" はチェーンに 1 つまでです。"); // F-04-3
                return false;
            }
    auto slot = makeDefaultSlot (type);
    if (! slot) return false;
    if (info->weight == EffectWeight::heavy && heavyOnCount (current.chain) >= kMaxHeavyOn)
    {
        slot->enabled = false; // F-04-17: added OFF
        toast (u8 ("重いエフェクトは同時に 2 つまでしか ON にできないため、OFF の状態で追加しました。"));
    }
    current.chain.push_back (*slot);
    rebuildChain();
    markModified();
    return true;
}

void AppController::removeSlot (int i)
{
    if (i < 0 || i >= int (current.chain.size())) return;
    current.chain.erase (current.chain.begin() + i);
    rebuildChain();
    markModified();
}

bool AppController::moveSlot (int from, int to)
{
    const int n = int (current.chain.size());
    if (from < 0 || from >= n || to < 0 || to >= n || from == to) return false;
    auto s = current.chain[size_t (from)];
    current.chain.erase (current.chain.begin() + from);
    current.chain.insert (current.chain.begin() + to, s);
    rebuildChain();
    markModified();
    return true;
}

bool AppController::setSlotEnabled (int i, bool on, juce::String& whyNot)
{
    if (i < 0 || i >= int (current.chain.size())) return false;
    auto& s = current.chain[size_t (i)];
    auto* info = findEffectInfo (s.type);
    if (on && info != nullptr && info->weight == EffectWeight::heavy && heavyOnCount (current.chain, i) >= kMaxHeavyOn)
    {
        whyNot = u8 ("重いエフェクトは同時に 2 つまでしか ON にできません。ほかの重いエフェクトを OFF にしてください。");
        return false;
    }
    s.enabled = on;
    if (auto* chain = processor.getRequestedChain(); chain != nullptr)
        if (const int ci = chainIndexFor (current.chain, i); ci >= 0 && ci < chain->size())
        {
            if (on) chain->slot (ci).autoStopped.store (false); // manual re-enable clears 自動停止
            chain->slot (ci).enabled.store (on);
        }
    markModified();
    return true;
}

void AppController::setSlotParam (int i, int p, float v)
{
    if (i < 0 || i >= int (current.chain.size())) return;
    auto& s = current.chain[size_t (i)];
    auto* info = findEffectInfo (s.type);
    if (info == nullptr || p < 0 || p >= int (info->params.size())) return;
    v = info->params[size_t (p)].clamp (v);
    if (int (s.params.size()) <= p) s.params.resize (info->params.size());
    s.params[size_t (p)] = v;
    if (auto* chain = processor.getRequestedChain(); chain != nullptr)
        if (const int ci = chainIndexFor (current.chain, i); ci >= 0 && ci < chain->size())
            chain->slot (ci).params[size_t (p)].store (v);
    markModified();
}

bool AppController::setSlotMod (int i, const std::string& target, float depth)
{
    if (i < 0 || i >= int (current.chain.size())) return false;
    auto& s = current.chain[size_t (i)];
    auto* info = findEffectInfo (s.type);
    if (info == nullptr) return false;
    const int mi = target.empty() ? EffectChain::kModNone : EffectChain::modIndexFor (*info, target);
    if (! target.empty() && mi == EffectChain::kModNone) return false;
    depth = std::isfinite (depth) ? std::clamp (depth, -1.0f, 1.0f) : 0.0f;
    s.modTarget = target;
    s.modDepth = target.empty() ? 0.0f : depth;
    if (auto* chain = processor.getRequestedChain(); chain != nullptr)
        if (const int ci = chainIndexFor (current.chain, i); ci >= 0 && ci < chain->size())
        {
            chain->slot (ci).modDepth.store (s.modDepth);
            chain->slot (ci).modIndex.store (mi);
        }
    markModified();
    return true;
}

float AppController::getModLevel() const
{
    auto* chain = processor.getRequestedChain();
    return chain != nullptr ? chain->getModLevel() : 0.0f;
}

bool AppController::isSlotAutoStopped (int i) const
{
    auto* chain = processor.getRequestedChain();
    const int ci = chainIndexFor (current.chain, i);
    return chain != nullptr && ci >= 0 && ci < chain->size() && chain->slot (ci).autoStopped.load();
}

void AppController::triggerSlot (int i, EffectTrigger action)
{
    auto* chain = processor.getRequestedChain();
    const int ci = chainIndexFor (current.chain, i);
    if (chain != nullptr && ci >= 0 && ci < chain->size()) chain->slot (ci).pendingTrigger.store (int (action));
}

int AppController::getSlotUiState (int i) const
{
    auto* chain = processor.getRequestedChain();
    const int ci = chainIndexFor (current.chain, i);
    return chain != nullptr && ci >= 0 && ci < chain->size() ? chain->slot (ci).fx->getUiState() : 0;
}

bool AppController::hasLooperRecording() const
{
    for (int i = 0; i < int (current.chain.size()); ++i)
        if (current.chain[size_t (i)].type == "looper" && getSlotUiState (i) != 0) return true;
    return false;
}

void AppController::rebuildChain()
{
    std::vector<SlotDef> buildable;
    for (auto& s : current.chain)
        if (hasEffectFactory (s.type)) buildable.push_back (s);
    ++chainBuilds;
    processor.requestChain (EffectChain::create (buildable, processor.getSampleRate(), processor.getMaxBlockSize()));
}

void AppController::applyPresetToEngine (bool rebuild)
{
    processor.setShifter (current.hasShifter, current.pitchSt, current.formantSt);
    for (int i = 0; i < kMaxLayers; ++i)
    {
        VoiceProcessor::LayerParams lp;
        if (i < int (current.layers.size()))
        {
            const auto& l = current.layers[size_t (i)];
            lp.active = l.enabled;
            lp.scale = l.mode == LayerDef::Mode::scale;
            lp.pitchSt = l.pitchSt;
            lp.formantSt = l.formantSt;
            lp.levelDb = l.levelDb;
            lp.key = l.key;
            lp.minor = l.minor;
            lp.degree = l.degree;
        }
        processor.setLayer (i, lp);
    }
    processor.setTrimDb (getEffectiveTrimDb());
    if (rebuild) rebuildChain();
}

float AppController::getEffectiveTrimDb() const
{
    // 自分の声で音量合わせ (INTERFACES.md §10): a measured trim replaces a built-in preset's shipped one. A blend
    // (プリセットを混ぜる) clears current.builtin and carries its own outputTrimDb.
    if (current.builtin)
        if (auto it = settings.calibratedTrimDb.find (juce::String (currentBaseId)); it != settings.calibratedTrimDb.end())
            return it->second;
    return current.outputTrimDb;
}

// ============================================================================ presets
void AppController::loadPreset (const std::string& id, bool fromHotkey)
{
    const auto* p = library->find (id);
    if (p == nullptr) return;
    if (fromHotkey && hasLooperRecording())
    {
        toast (u8 ("ルーパーに録音があるため、プリセットを切り替えません。")); // E-27
        return;
    }
    setCompareHold (false); // §9.4: a preset change releases 聞き比べ
    current = *p;
    currentBaseId = id;
    modified = false;
    settings.currentPresetId = juce::String (id);
    applyPresetToEngine (true); // F-05-5: pitch glides, layers fade, chain crossfades (E-20: last one wins)
    saveSettingsSoon();
    sendChangeMessage();
}

bool AppController::saveCurrentAsNew (const juce::String& name, juce::String& error)
{
    std::string newId;
    if (! library->saveNew (current, name, newId, error)) return false;
    if (auto* p = library->find (newId))
    {
        current = *p; // with the converter OFF the saved copy has no layers (E-30): the engine must drop them too
        applyPresetToEngine (false);
        currentBaseId = newId;
        modified = false;
        settings.currentPresetId = juce::String (newId);
        saveSettingsSoon();
    }
    sendChangeMessage();
    return true;
}

bool AppController::overwriteCurrent (juce::String& error)
{
    auto* base = library->find (currentBaseId);
    if (base == nullptr)
    {
        error = u8 ("元のプリセットは削除されています。「新しく保存」を使ってください。");
        return false;
    }
    if (base->builtin)
    {
        error = u8 ("内蔵プリセットは上書きできません。「複製」か「新しく保存」を使ってください。");
        return false;
    }
    auto p = current;
    p.id = currentBaseId;
    p.name = base->name;
    p.builtin = false;
    if (! library->overwrite (p, error)) return false;
    modified = false;
    sendChangeMessage();
    return true;
}

bool AppController::duplicatePreset (const std::string& id, std::string& newIdOut, juce::String& error)
{
    if (! library->duplicate (id, newIdOut, error)) return false;
    sendChangeMessage();
    return true;
}

bool AppController::renamePreset (const std::string& id, const juce::String& newName, juce::String& error)
{
    if (! library->rename (id, newName, error)) return false;
    if (id == currentBaseId)
        if (const auto* p = library->find (id)) current.name = p->name;
    sendChangeMessage();
    return true;
}

bool AppController::removePreset (const std::string& id, juce::String& error)
{
    if (! library->remove (id, error)) return false;
    if (isFavorite (id))
    {
        juce::String unused;
        toggleFavorite (id, unused);
    }
    sendChangeMessage();
    return true;
}

bool AppController::importPreset (const juce::File& file, std::string& newIdOut, PresetLoadReport& report)
{
    if (! library->importFile (file, newIdOut, report)) return false;
    sendChangeMessage();
    return true;
}

bool AppController::isFavorite (const std::string& id) const { return settings.favorites.contains (juce::String (id)); }

bool AppController::toggleFavorite (const std::string& id, juce::String& whyNot)
{
    const juce::String s (id);
    if (settings.favorites.contains (s)) settings.favorites.removeString (s);
    else
    {
        if (settings.favorites.size() >= kMaxFavorites)
        {
            whyNot = u8 ("お気に入りは 9 件までです。"); // F-05-8
            return false;
        }
        settings.favorites.add (s);
    }
    saveSettingsSoon();
    sendChangeMessage();
    return true;
}

void AppController::loadFavorite (int index, bool fromHotkey)
{
    if (index >= 0 && index < settings.favorites.size()) loadPreset (settings.favorites[index].toStdString(), fromHotkey);
}

void AppController::nextFavorite (bool fromHotkey)
{
    if (settings.favorites.isEmpty()) return;
    const int i = settings.favorites.indexOf (juce::String (currentBaseId));
    if (! settings.favoriteWrap && i == settings.favorites.size() - 1) return; // at the last one
    loadFavorite (i < 0 ? 0 : (i + 1) % settings.favorites.size(), fromHotkey);
}

void AppController::prevFavorite (bool fromHotkey)
{
    if (settings.favorites.isEmpty()) return;
    const int n = settings.favorites.size();
    const int i = settings.favorites.indexOf (juce::String (currentBaseId));
    if (! settings.favoriteWrap && i == 0) return; // at the first one
    loadFavorite (i < 0 ? n - 1 : (i + n - 1) % n, fromHotkey);
}

// ============================================================================ environment
void AppController::applyEnvironment()
{
    processor.setInputGainDb (settings.inputGainDb);
    processor.setNoiseSuppression (settings.noiseSuppressionOn, settings.noiseMix);
    processor.setGate (settings.gateOn, settings.gateThresholdDb, settings.gateAttackMs, settings.gateHoldMs, settings.gateReleaseMs);
    processor.setOutputGainDb (settings.outputGainDb);
    processor.setVoiceChangerOn (settings.voiceChangerOn && ! compareHeld); // §9.4
    applyMicMute();
    monitor->setVolumeDb (settings.monitorVolumeDb);
    applyAudioSettings (nullptr);
    applyMicEq();        // wave 9 (INTERFACES.md §11)
}

void AppController::setInputGainDb (float db)
{
    settings.inputGainDb = kInputGainDb.clamp (db);
    processor.setInputGainDb (settings.inputGainDb);
    saveSettingsSoon();
    sendChangeMessage();
}

void AppController::setNoiseSuppression (bool on, float mix)
{
    settings.noiseSuppressionOn = on;
    settings.noiseMix = kNoiseMix.clamp (mix);
    processor.setNoiseSuppression (on, settings.noiseMix);
    saveSettingsSoon();
    sendChangeMessage();
}

void AppController::setGate (bool on, float thr, float att, float hold, float rel)
{
    settings.gateOn = on;
    settings.gateThresholdDb = kGateThresholdDb.clamp (thr);
    settings.gateAttackMs = kGateAttackMs.clamp (att);
    settings.gateHoldMs = kGateHoldMs.clamp (hold);
    settings.gateReleaseMs = kGateReleaseMs.clamp (rel);
    processor.setGate (on, settings.gateThresholdDb, settings.gateAttackMs, settings.gateHoldMs, settings.gateReleaseMs);
    saveSettingsSoon();
    sendChangeMessage();
}

void AppController::setOutputGainDb (float db)
{
    settings.outputGainDb = std::round (kOutputGainDb.clamp (db) / kOutputGainStepDb) * kOutputGainStepDb;
    processor.setOutputGainDb (settings.outputGainDb);
    if (settings.outputGainDb > kOutputGainWarnDb)
        addNotice ({ "output.gain", NoticeLevel::warning, u8 ("出力ゲインが +6 dB を超えています。リミッターが働きやすくなり、音が歪むことがあります。") });
    else
        removeNotice ("output.gain");
    saveSettingsSoon();
    sendChangeMessage();
}

void AppController::setMonitorOn (bool on)
{
    if (on == monitorOn && ! monitorLost) return;
    if (on)
    {
        if (settings.monitorDevice.isEmpty())
        {
            toast (u8 ("モニターに使うデバイスを、設定のデバイスで選んでください。"));
            return;
        }
        if (allowDevices && ! monitor->isOpen())
        {
            const auto err = monitor->open (settings.monitorDevice, processor.getSampleRate());
            if (err.isNotEmpty())
            {
                toast (u8 ("モニターのデバイスを開けませんでした（") + err + u8 ("）。"));
                return;
            }
        }
        monitor->setEnabled (true);
        monitorOn = true;
        monitorLost = false;
        removeNotice ("monitor.lost");
    }
    else
    {
        monitor->setEnabled (false);
        monitorOn = false;
        monitorLost = false; // stop waiting for the device
        removeNotice ("monitor.lost");
    }
    sendChangeMessage();
}

void AppController::monitorDeviceGone (const juce::String& openError)
{
    logging::write (logging::error, "monitor device " + (openError.isEmpty() ? juce::String ("stopped") : "open failed: " + openError));
    monitor->close();
    monitorLost = true;
    monitorRetryCountdown = reconnectTicks(); // try again in reconnectSeconds (1 s by default)
    addNotice ({ "monitor.lost", NoticeLevel::warning,
                 openError.isEmpty()
                     ? u8 ("モニターのデバイスが止まりました（抜かれた可能性があります）。戻ると自動で再開します。") // F-01-4
                     : u8 ("モニターのデバイスを開けませんでした（") + openError + u8 ("）。他のアプリが排他モードで使っていないか確認してください。使えるようになると自動で再開します。"), // E-04
                 true, u8 ("設定で変更"), "openSettings.devices" });
    sendChangeMessage();
}

void AppController::reopenMonitor()
{
    if (allowDevices)
        if (const auto err = monitor->open (settings.monitorDevice, processor.getSampleRate()); err.isNotEmpty())
        {
            monitorRetryCountdown = reconnectTicks();
            return;
        }
    monitor->setEnabled (true);
    monitorLost = false;
    removeNotice ("monitor.lost");
    sendChangeMessage();
}

void AppController::setMonitorVolumeDb (float db)
{
    settings.monitorVolumeDb = kMonitorVolumeDb.clamp (db);
    monitor->setVolumeDb (settings.monitorVolumeDb);
    saveSettingsSoon();
    sendChangeMessage();
}

bool AppController::isMonitorDeviceSpeaker() const { return looksLikeSpeaker (settings.monitorDevice); }

void AppController::updateSettings (const std::function<void (Settings&)>& change)
{
    const Settings before = settings;
    change (settings);
    settings = clampSettings (settings);
    soundboard->setDuckingDb (settings.duckingDb);
    applyAudioSettings (&before);
    applyPlatformSettings (&before);
    processor.setTrimDb (getEffectiveTrimDb()); // calibratedTrimDb may have changed
    saveSettingsSoon();
    sendChangeMessage();
}

bool AppController::setAutoStart (bool on, juce::String& error)
{
    if (! autostart::setEnabled (on, error)) return false;
    settings.autoStart = on;
    saveSettingsSoon();
    sendChangeMessage();
    return true;
}

// ============================================================================ hotkeys
bool AppController::setHotkey (const juce::String& action, int modifiers, int vk, juce::String& whyNot)
{
    for (auto& h : settings.hotkeys)
        if (h.action != action && h.modifiers == modifiers && h.virtualKey == vk && vk != 0)
        {
            whyNot = u8 ("このキーは「") + Hotkeys::actionLabel (h.action) + u8 ("」に割り当て済みです。"); // F-07-4
            return false;
        }
    clearHotkey (action);
    settings.hotkeys.push_back ({ action, modifiers, vk });
    reapplyHotkeys();
    saveSettingsSoon();
    sendChangeMessage();
    return true;
}

void AppController::clearHotkey (const juce::String& action)
{
    auto& hk = settings.hotkeys;
    hk.erase (std::remove_if (hk.begin(), hk.end(), [&] (const HotkeyBinding& b) { return b.action == action; }), hk.end());
    reapplyHotkeys();
    saveSettingsSoon();
}

juce::String AppController::getHotkeyText (const juce::String& action) const
{
    for (auto& h : settings.hotkeys)
        if (h.action == action) return Hotkeys::describe (h.modifiers, h.virtualKey);
    return {};
}

void AppController::reapplyHotkeys()
{
    if (hotkeys == nullptr) return;
    const auto failed = hotkeys->apply (settings.hotkeys);
    if (failed.empty())
    {
        removeNotice ("hotkey.fail");
        return;
    }
    juce::StringArray names;
    for (auto& f : failed) names.add (Hotkeys::describe (f.modifiers, f.virtualKey));
    addNotice ({ "hotkey.fail", NoticeLevel::warning,
                 u8 ("ホットキーを登録できませんでした: ") + names.joinIntoString (", ") + u8 ("（他のアプリが使っている可能性があります）") }); // F-07-3
}

void AppController::performAction (const juce::String& a)
{
    logging::write (logging::detail, "action: " + a);
    const auto presetBefore = currentBaseId;
    if (a == "voiceToggle")
    {
        setVoiceChangerOn (! settings.voiceChangerOn);
        hotkeyToast (settings.voiceChangerOn ? u8 ("ボイチェン ON") : u8 ("ボイチェン OFF"));
    }
    else if (a == "muteToggle")
    {
        setMicMuted (! micMuted);
        hotkeyToast (micMuted ? u8 ("マイクミュート ON") : u8 ("マイクミュート OFF"));
    }
    else if (a == "pushToTalk")
    {
        if (settings.pushToTalk == 0) return; // S-03 詳細: off = the key does nothing
        pttKey = 0;
        for (auto& h : settings.hotkeys)
            if (h.action == a) pttKey = h.virtualKey;
        pttHeld = true;
        pttTailMs = 0.0f;
        applyMicMute();
        sendChangeMessage();
    }
    else if (a == "favoriteNext") nextFavorite (true);
    else if (a == "favoritePrev") prevFavorite (true);
    else if (a.startsWith ("favorite.")) loadFavorite (a.fromFirstOccurrenceOf (".", false, false).getIntValue() - 1, true);
    else if (a.startsWith ("slot."))
    {
        const int i = a.fromFirstOccurrenceOf (".", false, false).getIntValue() - 1;
        if (i >= 0 && i < int (current.chain.size()))
        {
            juce::String why;
            if (! setSlotEnabled (i, ! current.chain[size_t (i)].enabled, why)) toast (why);
            else
                hotkeyToast (u8 ("スロット ") + juce::String (i + 1) + u8 ("（") + effectName (current.chain[size_t (i)].type) + u8 ("）")
                             + (current.chain[size_t (i)].enabled ? " ON" : " OFF"));
            sendChangeMessage();
        }
    }
    else if (a.startsWith ("sound.")) soundboard->trigger (a.fromFirstOccurrenceOf (".", false, false).getIntValue() - 1);
    else if (a.startsWith ("soundStop.")) soundboard->stopSlot (a.fromFirstOccurrenceOf (".", false, false).getIntValue() - 1);
    else if (a == "soundStopAll") soundboard->stopAll();
    else if (a == "freezeToggle" || a == "looperRecPlay" || a == "looperClear")
    {
        const std::string type = a == "freezeToggle" ? "freeze" : "looper";
        const auto action = a == "freezeToggle" ? EffectTrigger::freezeToggle : (a == "looperRecPlay" ? EffectTrigger::looperRecordPlay : EffectTrigger::looperClear);
        for (int i = 0; i < int (current.chain.size()); ++i)
            if (current.chain[size_t (i)].type == type) triggerSlot (i, action);
    }
    else if (a == "recordToggle") // wave 8 (INTERFACES.md §10)
    {
        juce::String why;
        if (isWavRecording()) { stopWavRecording(); hotkeyToast (u8 ("録音を止めました")); }
        else if (startWavRecording (why)) hotkeyToast (u8 ("録音を始めました"));
        else toast (why);
    }
    else if (a == "wavStop") stopWavRecording();   // notice buttons
    else if (a == "takeStop") stopTestTake();
    else if (a.startsWith ("momentary."))
    {
        const int i = a.fromFirstOccurrenceOf (".", false, false).getIntValue() - 1;
        if (i >= 0 && i < kMomentarySlots) setMomentaryHeld (i, true);
    }
    else if (a == "show") { if (onShowWindowRequest) onShowWindowRequest(); }
    else if (a == "quit") { if (onQuitRequest) onQuitRequest(); }
    else if (a == "closeWindow") { if (settings.closeAction == 1 && onQuitRequest) onQuitRequest(); } // × button; 0 = stay in the tray

    if (currentBaseId != presetBefore) hotkeyToast (u8 ("プリセット: ") + current.name);
}

// ============================================================================ soundboard / setup
void AppController::saveSoundboard() { soundboard->save (paths::soundboardFile()); }
void AppController::setTestTone (bool on) { soundboard->setTestTone (on); }

// ============================================================================ notices
void AppController::addNotice (Notice n)
{
    for (auto& e : notices)
        if (e.key == n.key) { e = n; return; }
    notices.push_back (n);
}

void AppController::removeNotice (const juce::String& key)
{
    notices.erase (std::remove_if (notices.begin(), notices.end(), [&] (const Notice& n) { return n.key == key; }), notices.end());
}

std::vector<Notice> AppController::getNotices() const
{
    std::vector<Notice> r;
    for (auto& n : notices)
        if (! dismissed.contains (n.key)) r.push_back (n);
    std::stable_sort (r.begin(), r.end(), [] (const Notice& a, const Notice& b) { return int (a.level) < int (b.level); });
    return r;
}

void AppController::dismissNotice (const juce::String& key)
{
    dismissed.addIfNotAlreadyThere (key);
    sendChangeMessage();
}

void AppController::toast (const juce::String& text)
{
    toasts.add (text);
    juce::Logger::writeToLog ("notice: " + text);
    sendChangeMessage();
}

juce::StringArray AppController::takeToasts()
{
    auto t = toasts;
    toasts.clear();
    return t;
}

void AppController::refreshOutputNotice()
{
    removeNotice ("output.unset");
    removeNotice ("output.notcable");
    if (! allowDevices) return;
    if (settings.outputDevice.isEmpty())
        addNotice ({ "output.unset", NoticeLevel::warning, u8 ("出力先が未選択です。設定で選んでください。"), true, u8 ("設定で変更"), "openSettings.devices" });
    else if (! isCableInputName (settings.outputDevice))
    {
        auto text = u8 ("出力先が仮想ケーブルではありません。Discord に声が届かない可能性があります。");
        if (looksLikeSpeaker (settings.outputDevice)) text << u8 ("自分の声がスピーカーから流れます。"); // E-03
        addNotice ({ "output.notcable", NoticeLevel::warning, text, true, u8 ("設定で変更"), "openSettings.devices" });
    }
    if (! isVirtualCableInstalled())
        addNotice ({ "cable.missing", NoticeLevel::info, u8 ("仮想ケーブル（VB-CABLE）が見つかりません。導入の手順を確認してください。"), true,
                     u8 ("手順を見る"), "openSetup" }); // E-02
    else
        removeNotice ("cable.missing");
}

// ============================================================================ timer
void AppController::markModified()
{
    modified = true;
    sendChangeMessage();
}

void AppController::saveSettingsSoon() { saveCountdown = 15; } // ~0.5 s debounce

void AppController::timerCallback()
{
    ++ticks;
    processor.collectGarbage();
    pollUpdater(); // §7.4
    if (limiterHold > 0) --limiterHold;

    // F-04-11: NaN auto-bypass reported by the chain -> keep the model in sync and tell the user
    if (auto* chain = processor.getRequestedChain())
    {
        int ci = -1;
        while (chain->popAutoStopEvent (ci))
        {
            int mi = -1;
            for (int i = 0; i < int (current.chain.size()); ++i)
                if (chainIndexFor (current.chain, i) == ci) mi = i;
            if (mi < 0) continue;
            auto* info = findEffectInfo (current.chain[size_t (mi)].type);
            const bool heavy = info != nullptr && info->weight == EffectWeight::heavy;
            if (! chain->slot (ci).enabled.load())
            {
                current.chain[size_t (mi)].enabled = false;
                toast (u8 ("スロット ") + juce::String (mi + 1) + u8 ("（") + effectName (current.chain[size_t (mi)].type)
                       + u8 ("）の出力が異常になったため、自動で OFF にしました。"));
            }
            else if (heavy)
                toast (u8 ("CPU の負荷が高いため、スロット ") + juce::String (mi + 1) + u8 ("（") + effectName (current.chain[size_t (mi)].type) + u8 ("）を自動停止しました。"));
            sendChangeMessage();
        }
    }

    if (engine != nullptr)
    {
        if (engine->fetchLayersAutoStopped()) toast (u8 ("CPU の負荷が高いため、重ねる声を自動停止しました。手動で再開できます。"));
        if (engine->fetchOverloadNotice()) toast (u8 ("CPU の負荷が高い状態が続いています。エフェクトを減らすか、バッファサイズを大きくしてください。"));
        engine->fetchHeavySlotStopped(); // the chain event above carries the details

        // E-15: completely silent input for 10 s
        if (engine->isRunning() && engine->isInputSilent())
            addNotice ({ "input.silent", NoticeLevel::info,
                         u8 ("マイクの音が届いていません。Windows の［設定］>［プライバシーとセキュリティ］>［マイク］で、デスクトップアプリのマイクへのアクセスが ON か確認してください。") });
        else
            removeNotice ("input.silent");

        // F-01-4: try to come back after a device loss
        if (deviceLost && reopenCountdown > 0 && --reopenCountdown == 0)
        {
            rescanDevices();
            if (inputs.contains (settings.inputDevice) && outputs.contains (settings.outputDevice)) openDevicesIfReady();
            if (deviceLost) reopenCountdown = reconnectTicks();
        }

        // E-06: 10 XRUNs within 10 s
        const int x = engine->getXRunCount();
        if (ticks - xrunWindowStart >= 300) { xrunWindowStart = ticks; xrunBase = x; }
        if (x - xrunBase >= kXrunWarnCount)
        {
            addNotice ({ "xrun", NoticeLevel::warning, u8 ("音切れが続いています。設定の詳細で、バッファサイズを大きくしてみてください。"), true,
                         u8 ("設定で変更"), "openSettings.advanced" });
            xrunBase = x;
        }
    }

    // F-01-4 for the monitor device: stopped -> notice and shown OFF; back -> resumes by itself
    if (monitorOn && ! monitorLost && monitor->fetchDeviceLost()) monitorDeviceGone ({});
    if (monitorLost && monitorRetryCountdown > 0 && --monitorRetryCountdown == 0)
    {
        rescanDevices();
        if (! allowDevices || outputs.contains (settings.monitorDevice)) reopenMonitor();
        else monitorRetryCountdown = reconnectTicks();
    }
    tickPushToTalk();
    tickCapture();       // wave 8 (INTERFACES.md §10)
    tickMicEq();         // wave 9 (INTERFACES.md §11)
    tickStream();
    tickAnalysis();
    tickAutomation();

    if (saveCountdown > 0 && --saveCountdown == 0)
    {
        if (saveSettings (settings, paths::settingsFile())) removeNotice ("settings.save");
        else addNotice ({ "settings.save", NoticeLevel::warning, u8 ("設定を保存できません。フォルダに書き込めるか確認してください。") }); // E-19
        sendChangeMessage();
    }
}

juce::String AppController::buildDiagnostics() const
{
    const auto s = getStatus();
    juce::String t;
    t << "KoeLoom " << KOELOOM_VERSION_STRING << "\n"
      << juce::SystemStats::getOperatingSystemName() << " / " << juce::SystemStats::getCpuModel() << " / "
      << juce::SystemStats::getNumCpus() << " logical cores\n"
      << "device type: " << (engine != nullptr ? engine->getTypeName() : juce::String ("-")) << "\n"
      << "input: " << settings.inputDevice << "\noutput: " << settings.outputDevice << "\nmonitor: " << settings.monitorDevice << "\n"
      << "running: " << (s.running ? "yes" : "no") << ", rate " << s.sampleRate << ", buffer " << s.bufferSize << "\n"
      << "latency (estimate): " << juce::String (s.latencyMs, 1) << " ms, xruns " << s.xruns << ", cpu " << juce::String (s.cpuPercent, 1) << " %\n"
      << "non-finite input samples: " << processor.getNonFiniteInputCount() << "\n"
      << "preset: " << juce::String (currentBaseId) << (modified ? " (modified)" : "") << ", slots " << int (current.chain.size())
      << ", layers " << int (current.layers.size()) << "\n"
      << "noise suppression " << (settings.noiseSuppressionOn ? "on" : "off") << ", gate " << (settings.gateOn ? "on" : "off")
      << ", output gain " << settings.outputGainDb << " dB\n"
      << "data dir: " << paths::dataDir().getFullPathName() << "\n";
    // F-11-3: hide the user name
    const auto home = juce::File::getSpecialLocation (juce::File::userHomeDirectory).getFullPathName();
    return t.replace (home, "%USERPROFILE%");
}
} // namespace koe

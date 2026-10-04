// 配信用の出力 (INTERFACES.md §11.3). Owner: wave9/stream.
// Port: sent tap 0 (exactly what the virtual mic gets) -> StreamTap (volume) -> a second MonitorOutput on its own device.
// A stopped / unplugged / missing device works like the monitor's (F-01-4): a notice, then a retry every reconnectSeconds.

#include "App/AppController.h"
#include "Platform/Log.h"

namespace koe
{
namespace
{
juce::String u8 (const char* s) { return juce::String::fromUTF8 (s); }

Notice streamNotice (const juce::String& text)
{
    return { "stream.lost", NoticeLevel::warning, text, true, u8 ("設定で変更"), "openSettings.devices" };
}
} // namespace

juce::StringArray AppController::getStreamDevices() const
{
    juce::StringArray r;
    for (auto& o : StreamData::outputsForTests.isEmpty() ? outputs : StreamData::outputsForTests)
        if (o != settings.outputDevice) r.add (o); // the virtual mic's own device would hear itself twice
    return r;
}

void AppController::setStreamDevice (const juce::String& name)
{
    if (name.isNotEmpty() && name == settings.outputDevice)
    {
        toast (u8 ("仮想マイクの出力先と同じデバイスは、配信用の出力に選べません。"));
        return;
    }
    settings.streamDevice = name;
    saveSettingsSoon();
    closeStream();
    openStream(); // also the retry when the same device is picked again
    sendChangeMessage();
}

void AppController::setStreamVolumeDb (float db)
{
    updateSettings ([db] (Settings& s) { s.streamVolumeDb = kStreamVolumeDb.clamp (db); });
    if (stream.tap != nullptr) stream.tap->setGainDb (settings.streamVolumeDb);
}

bool AppController::isStreamRunning() const { return stream.openName.isNotEmpty(); }
juce::String AppController::getStreamError() const { return stream.error; }

void AppController::openStream()
{
    auto& st = stream;
    const auto name = settings.streamDevice;
    if (st.openName.isNotEmpty()) return;
    if (name.isEmpty() || name == settings.outputDevice)
    {
        st.lost = false;
        st.error = name.isEmpty() ? juce::String() : u8 ("仮想マイクの出力先と同じデバイスです。別のデバイスを選んでください。");
        removeNotice ("stream.lost");
        return;
    }
    const bool fake = ! allowDevices;
    if (fake && StreamData::outputsForTests.isEmpty()) return;         // snapshots / UI tests: never a device
    if (! fake && ! (engine != nullptr && engine->isRunning())) return; // opens with the main devices (openDevicesIfReady)

    if (st.out == nullptr)
    {
        st.out = std::make_unique<MonitorOutput>();
        st.tap = std::make_unique<StreamTap> (*st.out);
        StreamData::outputForTests = st.out.get();
    }
    const auto& known = fake ? StreamData::outputsForTests : outputs;
    juce::String err;
    if (! known.contains (name))
        err = u8 ("配信用の出力のデバイス「") + name + u8 ("」が見つかりません。つながると自動で再開します。");
    else if (fake && name == StreamData::failOpenForTests)
        err = u8 ("配信用の出力のデバイスを開けませんでした（テスト）。使えるようになると自動で再開します。");
    else if (fake)
        st.out->prepareForTest (processor.getSampleRate(), kSampleRate, kDefaultBufferSize);
    else if (const auto e = st.out->open (name, processor.getSampleRate()); e.isNotEmpty())
        err = u8 ("配信用の出力のデバイスを開けませんでした（") + e + u8 ("）。他のアプリが排他モードで使っていないか確認してください。使えるようになると自動で再開します。");

    if (err.isNotEmpty())
    {
        logging::write (logging::error, "stream device open failed: " + name);
        st.out->close();
        if (st.error != err) addNotice (streamNotice (err)); // once per reason, not on every retry
        st.error = err;
        st.lost = true;
        st.retryCountdown = reconnectTicks();
        return;
    }
    st.out->setVolumeDb (0.0f); // the volume is the tap's
    st.tap->setGainDb (settings.streamVolumeDb);
    st.out->setEnabled (true);
    processor.setTap (VoiceProcessor::TapPoint::sent, 0, st.tap.get());
    st.openName = name;
    st.lost = false;
    st.error = {};
    removeNotice ("stream.lost");
}

void AppController::closeStream()
{
    auto& st = stream;
    processor.setTap (VoiceProcessor::TapPoint::sent, 0, nullptr);
    if (st.out != nullptr) st.out->close(); // waits for a push in flight; the tap object stays alive
    st.openName = {};
}

void AppController::tickStream()
{
    auto& st = stream;
    if (st.tap != nullptr) st.tap->setGainDb (settings.streamVolumeDb); // S-03's slider goes through updateSettings
    if (st.openName.isNotEmpty() && st.out->fetchDeviceLost())
    {
        logging::write (logging::error, "stream device stopped");
        closeStream();
        st.error = u8 ("配信用の出力のデバイスが止まりました（抜かれた可能性があります）。戻ると自動で再開します。");
        addNotice (streamNotice (st.error));
        st.lost = true;
        st.retryCountdown = reconnectTicks();
        sendChangeMessage();
    }
    else if (st.lost)
    {
        if (--st.retryCountdown > 0) return;
        st.retryCountdown = reconnectTicks();
        if (allowDevices) rescanDevices();
        openStream();
        if (isStreamRunning()) sendChangeMessage();
    }
    else if (st.openName != settings.streamDevice || (st.openName.isNotEmpty() && st.openName == settings.outputDevice))
    {
        // another device chosen behind our back (settings import / reset), not opened yet, or now the virtual mic's
        const auto before = st.error;
        closeStream();
        openStream();
        if (isStreamRunning() || st.error != before) sendChangeMessage();
    }
}

void AppController::shutdownStream()
{
    closeStream();
    stream.lost = false; // no retries after this
}
} // namespace koe

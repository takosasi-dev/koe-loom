// 配信用の出力 and --bench-presets (INTERFACES.md §11.3, owner wave9/stream). Category "Stream".
// No window, no sound, no audio device: AppController (false) with StreamData::outputsForTests standing in for the output
// devices (opening one prepares the stream's MonitorOutput for pullForTest instead of a device); audio goes through
// getProcessorForTests().process() on this thread.

#include "App/AppController.h"
#include "Core/Paths.h"
#include "Dsp/Building.h"
#include "Tests/TestUtil.h"
#include "Tools/Bench.h"
#include "UI/MainComponent.h"
#include "UI/Screens.h"
#include "UI/main/Common.h"
#include "UI/screens/Common.h"

#include <cmath>
#include <cstring>

namespace koe
{
namespace
{
using namespace test;
constexpr int kBlock = 480;
const juce::String kCable ("CABLE Input (VB-Audio Virtual Cable)");
const juce::String kCable16 ("CABLE In 16 Ch (VB-Audio Virtual Cable)");
const juce::String kPhones ("Headphones (test)");
const juce::String kSpeakers ("Speakers (test)");

void freshDataDir()
{
    auto d = paths::dataDir();
    if (d.getFullPathName().contains ("KoeLoomTests")) d.deleteRecursively();
    d.createDirectory();
}

/** The test hooks are global: always back to "no devices" when a test ends, whatever happens. */
struct FakeOutputs
{
    FakeOutputs() { StreamData::outputsForTests = { kCable, kCable16, kPhones, kSpeakers }; }
    ~FakeOutputs()
    {
        StreamData::outputsForTests.clear();
        StreamData::failOpenForTests = {};
    }
};

std::unique_ptr<AppController> makeController()
{
    auto c = std::make_unique<AppController> (false);
    c->startup();
    c->updateSettings ([] (Settings& s)
    {
        s.setupDone = true;
        s.tourStep = 7;
    });
    c->setOutputDevice (kCable);
    c->setVoiceChangerOn (true);
    c->reprepareForTests (kSr, kBlock);
    c->dispatchPendingMessages();
    return c;
}

/** Runs in through the controller's processor block by block; returns the virtual mic (left) and, if asked, the stream. */
std::vector<float> run (AppController& c, const std::vector<float>& in, std::vector<float>* streamOut = nullptr)
{
    auto& vp = c.getProcessorForTests();
    std::vector<float> out (in.size(), 0.0f);
    if (streamOut != nullptr) streamOut->assign (in.size(), 0.0f);
    for (size_t pos = 0; pos + kBlock <= in.size(); pos += kBlock)
    {
        vp.process (in.data() + pos, out.data() + pos, nullptr, kBlock);
        if (streamOut != nullptr && StreamData::outputForTests != nullptr) StreamData::outputForTests->pullForTest (streamOut->data() + pos, kBlock);
    }
    return out;
}

std::vector<float> tail (const std::vector<float>& v, double seconds)
{
    const auto n = std::min (v.size(), size_t (seconds * kSr));
    return { v.end() - long (n), v.end() };
}

void ticks (AppController& c, int n)
{
    for (int i = 0; i < n; ++i) c.tickForTests();
}

bool hasNotice (AppController& c, const char* key)
{
    for (auto& n : c.getNotices())
        if (n.key == key) return true;
    return false;
}
} // namespace

class StreamTests : public juce::UnitTest
{
public:
    StreamTests() : juce::UnitTest ("Stream output and preset CPU bench (wave9/stream)", "Stream") {}

    void runTest() override
    {
        deviceList();
        soundAndVolume();
        virtualMicUntouched();
        missingAndLostDevices();
        bench();
        fmodExactIsFmod();
        settingsCard();
    }

private:
    // ---------------------------------------------------------------------------------------------
    void deviceList()
    {
        beginTest ("Devices: every output except the virtual mic's (other cables are fine); that one is refused; \"\" = OFF");
        freshDataDir();
        FakeOutputs fake;
        auto c = makeController();
        const auto list = c->getStreamDevices();
        expect (! list.contains (kCable), "the virtual mic's output is not offered");
        expect (list.contains (kCable16) && list.contains (kPhones) && list.contains (kSpeakers), list.joinIntoString (" | "));
        c->takeToasts();
        c->setStreamDevice (kCable);
        expect (c->getSettings().streamDevice.isEmpty() && ! c->isStreamRunning(), "refused");
        expect (c->takeToasts().size() == 1, "told why");
        c->setStreamDevice (kPhones);
        expect (c->isStreamRunning() && c->getSettings().streamDevice == kPhones && c->getStreamError().isEmpty());
        c->setStreamDevice (kCable16);
        expect (c->isStreamRunning() && c->getSettings().streamDevice == kCable16, "a second virtual cable (OBS) works");
        c->setStreamDevice ({});
        expect (! c->isStreamRunning() && c->getStreamError().isEmpty() && c->getSettings().streamDevice.isEmpty());
        c->setStreamVolumeDb (20.0f);
        expectEquals (c->getSettings().streamVolumeDb, kStreamVolumeDb.max, "volume clamped");
        c->setStreamVolumeDb (-100.0f);
        expectEquals (c->getSettings().streamVolumeDb, kStreamVolumeDb.min);
    }

    // ---------------------------------------------------------------------------------------------
    void soundAndVolume()
    {
        beginTest ("Sent tap 0 reaches the stream device at the stream volume; the muted virtual mic (試し録り) is silent there too");
        freshDataDir();
        FakeOutputs fake;
        auto c = makeController();
        c->setStreamDevice (kPhones);
        expect (c->isStreamRunning() && StreamData::outputForTests != nullptr);
        const auto tone = sine (440.0, 2.0, 0.1f);
        for (float db : { 0.0f, -12.0f, 6.0f, -24.0f })
        {
            c->setStreamVolumeDb (db);
            std::vector<float> st;
            const auto mic = run (*c, tone, &st);
            const float diff = rmsDb (tail (st, 1.0)) - rmsDb (tail (mic, 1.0));
            expectWithinAbsoluteError (diff, db, 0.5f, "stream level vs the virtual mic at " + juce::String (db) + " dB");
            expect (allFinite (st));
        }
        c->setStreamVolumeDb (0.0f);
        c->getProcessorForTests().setOutputMuted (true);
        std::vector<float> st;
        const auto mic = run (*c, tone, &st);
        expect (peakDb (tail (mic, 1.0)) <= -120.0f && peakDb (tail (st, 1.0)) <= -120.0f, "both silent while muted");
        c->getProcessorForTests().setOutputMuted (false);
    }

    // ---------------------------------------------------------------------------------------------
    void virtualMicUntouched()
    {
        beginTest ("The stream on / off / volume never changes the virtual mic by a sample; no allocation on the audio side");
        freshDataDir();
        FakeOutputs fake;
        auto plain = makeController();
        auto streamed = makeController();
        plain->loadPreset ("character-demon-king");
        streamed->loadPreset ("character-demon-king");
        plain->dispatchPendingMessages();
        streamed->dispatchPendingMessages();
        const auto voice = synthVoice (3.0);
        const auto a = run (*plain, voice);

        std::vector<float> b (voice.size(), 0.0f), st (static_cast<size_t> (kBlock));
        auto& vp = streamed->getProcessorForTests();
        long long allocs = 0;
        for (size_t pos = 0; pos + kBlock <= voice.size(); pos += kBlock)
        {
            if (pos == size_t (0.5 * kSr)) streamed->setStreamDevice (kPhones);
            if (pos == size_t (1.0 * kSr)) streamed->setStreamVolumeDb (-6.0f);
            if (pos == size_t (1.5 * kSr)) streamed->setStreamVolumeDb (6.0f);
            if (pos == size_t (2.5 * kSr)) streamed->setStreamDevice ({});
            AllocationCounter count;
            vp.process (voice.data() + pos, b.data() + pos, nullptr, kBlock);
            if (StreamData::outputForTests != nullptr) StreamData::outputForTests->pullForTest (st.data(), kBlock);
            allocs += count.count();
        }
        int diff = 0;
        for (size_t i = 0; i < a.size(); ++i) diff += a[i] != b[i];
        expectEquals (diff, 0, "samples that differ");
        expect (peakAbs (a) > 0.01f, "there is sound to compare");
        expectEquals (allocs, 0LL, "allocations in process() / the stream's pull");
    }

    static float peakAbs (const std::vector<float>& v)
    {
        float m = 0.0f;
        for (auto x : v) m = std::max (m, std::abs (x));
        return m;
    }

    // ---------------------------------------------------------------------------------------------
    void missingAndLostDevices()
    {
        beginTest ("Missing / failing / unplugged stream device: notice, shown stopped, back by itself after reconnectSeconds");
        freshDataDir();
        FakeOutputs fake;
        auto c = makeController();
        const int retry = c->getSettings().reconnectSeconds * 30;

        c->setStreamDevice ("Gone (test)");
        expect (! c->isStreamRunning() && c->getStreamError().isNotEmpty() && hasNotice (*c, "stream.lost"), "missing");
        StreamData::outputsForTests.add ("Gone (test)");
        ticks (*c, retry);
        expect (c->isStreamRunning() && c->getStreamError().isEmpty() && ! hasNotice (*c, "stream.lost"), "plugged in: runs");

        StreamData::outputForTests->simulateDeviceLostForTest();
        ticks (*c, 1);
        expect (! c->isStreamRunning() && c->getStreamError().contains (juce::String::fromUTF8 ("止まりました")) && hasNotice (*c, "stream.lost"),
                "unplugged");
        ticks (*c, retry);
        expect (c->isStreamRunning() && ! hasNotice (*c, "stream.lost"), "back");

        StreamData::failOpenForTests = kSpeakers;
        c->setStreamDevice (kSpeakers);
        expect (! c->isStreamRunning() && c->getStreamError().isNotEmpty(), "open fails");
        ticks (*c, retry);
        expect (! c->isStreamRunning(), "still failing");
        StreamData::failOpenForTests = {};
        ticks (*c, retry);
        expect (c->isStreamRunning(), "opens once it can");

        c->setOutputDevice (kSpeakers); // the virtual mic moves onto the stream's device
        ticks (*c, 1);
        expect (! c->isStreamRunning() && c->getStreamError().isNotEmpty(), "never the virtual mic's device");
        expect (! c->getStreamDevices().contains (kSpeakers));
        c->setOutputDevice (kCable);
        ticks (*c, 1);
        expect (c->isStreamRunning(), "free again");

        c->setStreamDevice ("Gone again (test)");
        expect (hasNotice (*c, "stream.lost"));
        c->setStreamDevice ({});
        ticks (*c, retry * 2);
        expect (! c->isStreamRunning() && c->getStreamError().isEmpty() && ! hasNotice (*c, "stream.lost"), "OFF stops retrying");

        c->setStreamDevice (kPhones);
        c->shutdown();
        expect (! c->isStreamRunning(), "closed by shutdown (the destructor runs it again)");
    }

    // ---------------------------------------------------------------------------------------------
    void bench()
    {
        beginTest ("--bench-presets: one row per preset and converter quality, sane times, the CSV shape");
        freshDataDir();
        const auto rows = tools::runBench (2, 1.0, 1);
        expectEquals (int (rows.size()), 6, "2 presets x qualities 0..2");
        for (size_t i = 0; i < rows.size(); ++i)
        {
            const auto& r = rows[i];
            expect (! r.id.empty() && r.quality == int (i % 3), juce::String (r.id));
            expect (r.meanUs > 0.0 && r.p95Us > 0.0 && r.meanUs <= r.maxUs && r.p95Us <= r.maxUs, juce::String (r.id) + " times");
            expectWithinAbsoluteError (r.realtimePct, r.meanUs / 10000.0 * 100.0, 1.0e-6);
        }
        juce::StringArray lines;
        lines.addLines (tools::benchCsv (rows).trimEnd());
        expectEquals (lines.size(), 7);
        expectEquals (lines[0], juce::String ("id,quality,meanUs,p95Us,maxUs,realtimePct,cpu"));
        for (int i = 1; i < lines.size(); ++i)
        {
            juce::StringArray f;
            f.addTokens (lines[i], ",", {});
            expectEquals (f.size(), 7, lines[i]);
            expect (f[0] == juce::String (rows[size_t (i - 1)].id) && f[1].getIntValue() == rows[size_t (i - 1)].quality);
        }
    }

    // ---------------------------------------------------------------------------------------------
    void fmodExactIsFmod()
    {
        beginTest ("dsp::fmodExact (the converter's faster phase wrap) is bit-identical to std::fmod, so the voice does not change");
        const float y = 6.283185307179586f;
        auto same = [] (float a, float b) { return std::memcmp (&a, &b, sizeof (float)) == 0; };
        juce::Random rng (12345);
        int bad = 0, n = 0;
        auto check = [&] (float x)
        {
            ++n;
            if (! same (dsp::fmodExact (x, y), std::fmod (x, y)) && ++bad <= 3)
                logMessage ("differs at " + juce::String (x, 9));
        };
        for (int i = 0; i < 2000000; ++i) check ((rng.nextFloat() * 2.0f - 1.0f) * 4000.0f); // the vocoder's range is about +-1700
        for (int k = -700; k <= 700; ++k)
        {
            float x = float (k) * y; // around every multiple, where the quotient is easy to get wrong
            for (int j = 0; j < 8; ++j)
            {
                check (x);
                check (std::nextafter (x, 1.0e9f));
                x = std::nextafter (x, -1.0e9f);
            }
        }
        for (float x : { 0.0f, -0.0f, y, -y, 3.14159265f, -3.14159265f, 1.0e-30f, -1.0e-30f }) check (x);
        expectEquals (bad, 0, juce::String (n) + " values");

        // the speed, logged only (other programs share the CPU): the per-bin phase wraps of one converter frame
        std::vector<float> xs (1 << 16);
        for (auto& x : xs) x = (rng.nextFloat() * 2.0f - 1.0f) * 1700.0f;
        auto timeNs = [&xs] (auto f)
        {
            double best = 1.0e9;
            volatile float sink = 0.0f;
            for (int r = 0; r < 5; ++r)
            {
                const auto t0 = juce::Time::getHighResolutionTicks();
                float acc = 0.0f;
                for (auto x : xs) acc += f (x);
                sink = sink + acc;
                best = std::min (best, double (juce::Time::getHighResolutionTicks() - t0) * 1.0e9
                                           / double (juce::Time::getHighResolutionTicksPerSecond()) / double (xs.size()));
            }
            return best;
        };
        const double a = timeNs ([y] (float x) { return std::fmod (x, y); });
        const double b = timeNs ([y] (float x) { return dsp::fmodExact (x, y); });
        logMessage ("std::fmod " + juce::String (a, 1) + " ns, fmodExact " + juce::String (b, 1) + " ns per call");
    }

    // ---------------------------------------------------------------------------------------------
    void settingsCard()
    {
        using namespace ui;
        const bool wasOff = mainui::animationsOff();
        mainui::animationsOff() = true;
        beginTest ("S-03 devices: 配信用の出力 card picks the device, shows the state, the volume follows; search finds it");
        {
            freshDataDir();
            FakeOutputs fake;
            auto c = makeController();
            MainComponent mc (*c);
            mc.setSize (Theme::defaultWidth, Theme::defaultHeight);
            mc.showSettings (Navigator::SettingsSection::devices);
            auto* combo = dynamic_cast<juce::ComboBox*> (mainui::findById (&mc, "settings.stream.device"));
            auto* status = mainui::findById (&mc, "settings.stream.device.status");
            auto* volume = mainui::findById (&mc, "settings.streamVolumeDb");
            expect (combo != nullptr && status != nullptr && volume != nullptr, "card parts");
            if (combo != nullptr && status != nullptr && volume != nullptr)
            {
                expectEquals (combo->getNumItems(), 1 + c->getStreamDevices().size(), "使わない + the devices");
                expect (! volume->isEnabled(), "volume off while there is no device");
                combo->setSelectedId (1 + 1 + c->getStreamDevices().indexOf (kPhones), juce::sendNotificationSync);
                c->dispatchPendingMessages();
                expect (c->getSettings().streamDevice == kPhones && c->isStreamRunning());
                expect (volume->isEnabled());
                if (auto* label = dynamic_cast<screens::TextLabel*> (status))
                    expectEquals (label->getText(), juce::String::fromUTF8 ("動いています"));
                c->updateSettings ([] (Settings& s) { s.streamVolumeDb = -12.0f; }); // the slider's way
                ticks (*c, 1);
                std::vector<float> st;
                const auto mic = run (*c, sine (440.0, 2.0, 0.1f), &st);
                expectWithinAbsoluteError (rmsDb (tail (st, 1.0)) - rmsDb (tail (mic, 1.0)), -12.0f, 0.5f, "volume from the settings");
                combo->setSelectedId (1, juce::sendNotificationSync);
                c->dispatchPendingMessages();
                expect (c->getSettings().streamDevice.isEmpty() && ! c->isStreamRunning());
            }
            if (auto* v = dynamic_cast<SettingsView*> (mainui::findById (&mc, "page.settings")))
                for (auto* q : { "配信", "OBS" })
                {
                    v->setSearchText (juce::String::fromUTF8 (q));
                    auto* card = mainui::findById (&mc, "settings.stream");
                    expect (card != nullptr && mainui::visibleWithin (card, &mc), juce::String::fromUTF8 (q) + ": card found by the search");
                }
            else
                expect (false, "settings view");
        }
        mainui::animationsOff() = wasOff;
    }
};

static StreamTests streamTests;
} // namespace koe

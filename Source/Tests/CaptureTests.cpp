// 試し録り and the WAV recording of the processed voice (INTERFACES.md §10.3, owner wave8/capture). Category "Capture".
// No window, no sound, no audio device: AppController (false) with CaptureData::assumeDevicesRunningForTests standing in
// for running devices; audio goes through getProcessorForTests().process() on this thread; the monitor is pulled with
// MonitorOutput::pullForTest. WAV files land in the test data folder (KOELOOM_DATA_DIR).

#include "App/AppController.h"
#include "Core/Paths.h"
#include "Engine/TakeRecorder.h"
#include "Engine/WavRecorder.h"
#include "Tests/TestUtil.h"
#include "UI/MainComponent.h"
#include "UI/main/Common.h"
#include "UI/main/ToolsView.h"

namespace koe
{
namespace
{
using namespace test;
constexpr int kBlock = 480;

void freshDataDir()
{
    auto d = paths::dataDir();
    if (d.getFullPathName().contains ("KoeLoomTests")) d.deleteRecursively();
    d.createDirectory();
}

/** The test hooks are global: always back to "no device" when a test ends, whatever happens. */
struct AssumeRunning
{
    explicit AssumeRunning (bool on = true) { CaptureData::assumeDevicesRunningForTests = on; }
    ~AssumeRunning()
    {
        CaptureData::assumeDevicesRunningForTests = false;
        CaptureData::failNextWavWriteForTests = false;
    }
};

std::unique_ptr<AppController> makeController (const juce::String& monitorDevice = "Headphones (test)")
{
    auto c = std::make_unique<AppController> (false);
    c->startup();
    c->updateSettings ([monitorDevice] (Settings& s)
    {
        s.setupDone = true;
        s.tourStep = 7;
        s.monitorDevice = monitorDevice;
    });
    c->setVoiceChangerOn (true);
    c->loadPreset ("character-demon-king");
    c->reprepareForTests (kSr, kBlock);
    c->dispatchPendingMessages();
    return c;
}

/** Runs in through the controller's processor block by block; returns the virtual mic (left). */
std::vector<float> run (AppController& c, const std::vector<float>& in, std::vector<float>* monitorOut = nullptr)
{
    auto& vp = c.getProcessorForTests();
    std::vector<float> out (in.size(), 0.0f);
    if (monitorOut != nullptr) monitorOut->assign (in.size(), 0.0f);
    for (size_t pos = 0; pos + kBlock <= in.size(); pos += kBlock)
    {
        vp.process (in.data() + pos, out.data() + pos, nullptr, kBlock);
        if (monitorOut != nullptr) c.getMonitorForTests().pullForTest (monitorOut->data() + pos, kBlock);
    }
    return out;
}

std::vector<float> slice (const std::vector<float>& v, double fromS, double toS)
{
    const auto a = std::min (v.size(), size_t (fromS * kSr)), b = std::min (v.size(), size_t (toS * kSr));
    return { v.begin() + long (a), v.begin() + long (b) };
}

float peakAbs (const std::vector<float>& v)
{
    float m = 0.0f;
    for (auto x : v) m = std::max (m, std::abs (x));
    return m;
}

const Notice* findNotice (const std::vector<Notice>& ns, const juce::String& key)
{
    for (auto& n : ns)
        if (n.key == key) return &n;
    return nullptr;
}

bool toastsContain (AppController& c, const char* utf8)
{
    for (auto& t : c.takeToasts())
        if (t.contains (juce::String::fromUTF8 (utf8))) return true;
    return false;
}

struct WavInfo { bool ok = false; double rate = 0.0; int channels = 0, bits = 0; std::vector<float> samples; };
WavInfo readWav (const juce::File& f)
{
    WavInfo w;
    juce::WavAudioFormat fmt;
    std::unique_ptr<juce::AudioFormatReader> r (fmt.createReaderFor (f.createInputStream().release(), true));
    if (r == nullptr) return w;
    w.ok = true;
    w.rate = r->sampleRate;
    w.channels = int (r->numChannels);
    w.bits = int (r->bitsPerSample);
    juce::AudioBuffer<float> b (int (r->numChannels), int (r->lengthInSamples));
    r->read (&b, 0, int (r->lengthInSamples), 0, true, true);
    w.samples.assign (b.getReadPointer (0), b.getReadPointer (0) + b.getNumSamples());
    return w;
}
} // namespace

class CaptureTests : public juce::UnitTest
{
public:
    CaptureTests() : juce::UnitTest ("Test take and WAV recording (wave8/capture)", "Capture") {}

    void runTest() override
    {
        tapsLeaveTheSoundAlone();
        takeRecorderLoop();
        takeRefusals();
        takeRecordAndPlay();
        takeStopsAtMaximum();
        takeRateChangeAndDeviceStop();
        wavRecording();
        wavFailureAndShutdown();
        noAllocationOnTheAudioThread();
        tools();
    }

private:
    // ---------------------------------------------------------------------------------------------
    void tapsLeaveTheSoundAlone()
    {
        beginTest ("Taps on and off: the virtual mic is the same to the sample as without them");
        freshDataDir();
        AssumeRunning running;
        auto plain = makeController();
        auto tapped = makeController();
        const auto voice = synthVoice (3.0);
        const auto a = run (*plain, voice);
        juce::String why;
        std::vector<float> b (voice.size(), 0.0f);
        auto& vp = tapped->getProcessorForTests();
        for (size_t pos = 0; pos + kBlock <= voice.size(); pos += kBlock)
        {
            if (pos == size_t (0.5 * kSr)) { expect (tapped->startTestTake (why), why); expect (tapped->startWavRecording (why), why); }
            if (pos == size_t (1.5 * kSr)) { tapped->stopTestTake(); tapped->stopWavRecording(); }
            if (pos == size_t (2.0 * kSr)) { expect (tapped->startTestTake (why), why); }
            if (pos == size_t (2.5 * kSr)) { tapped->clearTestTake(); }
            vp.process (voice.data() + pos, b.data() + pos, nullptr, kBlock);
        }
        int diff = 0;
        for (size_t i = 0; i < a.size(); ++i) diff += a[i] != b[i];
        expectEquals (diff, 0, "samples that differ");
        expect (peakAbs (a) > 0.01f, "there is sound to compare");
    }

    // ---------------------------------------------------------------------------------------------
    void takeRecorderLoop()
    {
        beginTest ("TakeRecorder: records until full (non-finite -> 0), loops with a crossfaded seam and a fade-in");
        TakeRecorder t (kSr, 1.0f);
        expect (t.isRecording());
        const auto in = sine (220.0, 0.4567, 0.5f);
        for (size_t pos = 0; pos < in.size(); pos += kBlock) t.push (in.data() + pos, int (std::min<size_t> (kBlock, in.size() - pos)));
        expectEquals (t.getLength(), int (in.size()));
        t.stopRecording();
        std::vector<float> extra (kBlock, 0.3f);
        t.push (extra.data(), kBlock);
        expectEquals (t.getLength(), int (in.size()), "nothing more after stopRecording");

        std::vector<float> out (size_t (3.0 * kSr), 0.0f);
        expect (! t.render (out.data(), kBlock), "not playing: the device input is used");
        t.startPlayback();
        for (size_t pos = 0; pos < out.size(); pos += kBlock) expect (t.render (out.data() + pos, kBlock));
        expect (allFinite (out));
        expect (std::abs (out[0]) < 0.01f, "fades in");
        const int len = int (in.size()), x = int (0.010 * kSr);
        for (int seam = len - x, k = 0; k < 3; ++k, seam += len - x)
            expectLessOrEqual (clickRatio (out, seam, seam + x), 2.0, "seam " + juce::String (k + 1) + " has no click");
        // after the first seam the loop goes on from x: sample len + j is the take's sample x + j
        expectWithinAbsoluteError (out[size_t (len + 200)], t.getSamples()[size_t (x + 200)], 1.0e-6f);
        t.stopPlayback();
        expect (! t.render (out.data(), kBlock));

        TakeRecorder full (kSr, 0.1f);
        auto longer = sine (220.0, 0.3, 0.5f);
        longer[100] = std::numeric_limits<float>::quiet_NaN();
        longer[101] = std::numeric_limits<float>::infinity();
        for (size_t pos = 0; pos + kBlock <= longer.size(); pos += kBlock) full.push (longer.data() + pos, kBlock);
        expect (full.isFull() && ! full.isRecording(), "stops by itself when full");
        expectEquals (full.getLength(), int (0.1 * kSr));
        expect (full.getSamples()[100] == 0.0f && full.getSamples()[101] == 0.0f && full.getSamples()[102] == longer[102], "non-finite samples recorded as 0");
    }

    // ---------------------------------------------------------------------------------------------
    void takeRefusals()
    {
        beginTest ("試し録り refusals: no device, nothing recorded, mic muted, no monitor device");
        freshDataDir();
        juce::String why;
        {
            AssumeRunning notRunning (false);
            auto c = makeController();
            expect (! c->startTestTake (why) && why.isNotEmpty(), "refused while the devices do not run");
            expect (c->getTestTakeState() == AppController::TakeState::empty);
            expect (! c->playTestTake (why) && why.isNotEmpty(), "nothing to play");
            expect (! c->startWavRecording (why) && why.isNotEmpty(), "WAV refused while the devices do not run");
            expect (! c->isWavRecording());
        }
        AssumeRunning running;
        auto c = makeController ({});
        expect (c->startTestTake (why), why);
        run (*c, synthVoice (0.5));
        c->stopTestTake();
        why = {};
        expect (! c->playTestTake (why) && why.contains (juce::String::fromUTF8 ("モニターの出力先")), "no monitor device: " + why);
        c->updateSettings ([] (Settings& s) { s.monitorDevice = "Headphones (test)"; });
        c->setMicMuted (true);
        why = {};
        expect (! c->playTestTake (why) && why.contains (juce::String::fromUTF8 ("ミュート")), "mic muted: " + why);
        expect (c->getTestTakeState() == AppController::TakeState::ready && ! c->isMonitorOn());
        c->setMicMuted (false);
        expect (c->playTestTake (why), why);
        why = {};
        expect (! c->startTestTake (why) && why.isNotEmpty(), "no new take while one plays");
        c->stopTestTake();
        c->clearTestTake();
        expect (c->getTestTakeState() == AppController::TakeState::empty);
        expectEquals (c->getTestTakeSeconds(), 0.0f);
        // stopping a take that recorded nothing leaves no take
        expect (c->startTestTake (why), why);
        expect (c->getTestTakeState() == AppController::TakeState::recording);
        c->stopTestTake();
        expect (c->getTestTakeState() == AppController::TakeState::empty);
    }

    // ---------------------------------------------------------------------------------------------
    void takeRecordAndPlay()
    {
        beginTest ("試し録り: records the raw input, plays it through the path; virtual mic silent, monitor hears it, presets switch live");
        freshDataDir();
        AssumeRunning running;
        auto c = makeController();
        c->setInputGainDb (6.0f); // the take is before the input gain
        juce::String why;
        expect (c->startTestTake (why), why);
        expect (c->getTestTakeState() == AppController::TakeState::recording);
        const auto voice = synthVoice (1.0);
        run (*c, voice);
        expectWithinAbsoluteError (c->getTestTakeSeconds(), 1.0f, 0.001f);
        c->stopTestTake();
        expect (c->getTestTakeState() == AppController::TakeState::ready);
        run (*c, synthVoice (0.5, 3)); // not recording any more
        expectWithinAbsoluteError (c->getTestTakeSeconds(), 1.0f, 0.001f);

        c->getMonitorForTests().prepareForTest (kSr, kSr, kBlock);
        expect (! c->isMonitorOn());
        expect (c->playTestTake (why), why);
        expect (c->getTestTakeState() == AppController::TakeState::playing);
        expect (c->isMonitorOn(), "the monitor was turned on");
        const auto notices = c->getNotices();
        const auto* n = findNotice (notices, "take.playing");
        expect (n != nullptr && ! n->dismissible && n->actionId == "takeStop" && n->actionLabel.isNotEmpty(), "notice with 止める");

        std::vector<float> mon;
        const auto quiet = silence (1.5);
        const auto out = run (*c, quiet, &mon);
        expectEquals (peakAbs (slice (out, 0.05, 1.5)), 0.0f, "the virtual mic is silent while the take plays");
        expect (rmsDb (slice (mon, 0.3, 1.5)) > -40.0f, "the monitor hears the take: " + juce::String (rmsDb (slice (mon, 0.3, 1.5))));
        expect (allFinite (mon));
        const float pos = c->getTestTakePosition();
        expect (pos > 0.0f && pos < 1.0f, "playback position loops inside the take: " + juce::String (pos));

        c->loadPreset ("natural-asis");
        expect (c->getTestTakeState() == AppController::TakeState::playing, "keeps playing across a preset change");
        const auto out2 = run (*c, quiet, &mon);
        expectEquals (peakAbs (out2), 0.0f, "still silent after the preset change");
        expect (rmsDb (slice (mon, 0.3, 1.5)) > -40.0f, "and the monitor still hears it");

        c->performAction ("takeStop"); // the notice's button
        expect (c->getTestTakeState() == AppController::TakeState::ready);
        expect (! c->isMonitorOn(), "the monitor is back off (it was off before)");
        expect (findNotice (c->getNotices(), "take.playing") == nullptr);
        const auto back = run (*c, synthVoice (1.0));
        expect (peakAbs (slice (back, 0.2, 1.0)) > 0.01f, "the virtual mic gets the voice again");

        // a monitor the user had on stays on
        c->setMonitorOn (true);
        expect (c->playTestTake (why), why);
        c->stopTestTake();
        expect (c->isMonitorOn(), "monitor left on when it was on before");
    }

    // ---------------------------------------------------------------------------------------------
    void takeStopsAtMaximum()
    {
        beginTest ("試し録り: stops by itself at kTestTakeMaxSeconds (noticed by the tick), a new take replaces the old");
        freshDataDir();
        AssumeRunning running;
        auto c = makeController();
        juce::String why;
        expect (c->startTestTake (why), why);
        run (*c, synthVoice (kTestTakeMaxSeconds + 1.0));
        expectWithinAbsoluteError (c->getTestTakeSeconds(), kTestTakeMaxSeconds, 0.001f);
        c->takeToasts();
        c->tickForTests();
        expect (c->getTestTakeState() == AppController::TakeState::ready);
        expect (toastsContain (*c, "試し録りを止めました"), "told it stopped");
        expect (c->startTestTake (why), why);
        expectEquals (c->getTestTakeSeconds(), 0.0f, "the old take is gone");
        run (*c, synthVoice (0.5));
        c->stopTestTake();
        expectWithinAbsoluteError (c->getTestTakeSeconds(), 0.5f, 0.011f);
        for (int i = 0; i < 10; ++i) c->tickForTests(); // retired takes are deleted
    }

    // ---------------------------------------------------------------------------------------------
    void takeRateChangeAndDeviceStop()
    {
        beginTest ("試し録り: a reopen at another rate deletes the take; stopped devices stop playback and restore everything");
        freshDataDir();
        AssumeRunning running;
        auto c = makeController();
        juce::String why;
        expect (c->startTestTake (why), why);
        run (*c, synthVoice (0.5));
        c->stopTestTake();
        c->getMonitorForTests().prepareForTest (kSr, kSr, kBlock);
        expect (c->playTestTake (why), why);
        c->reprepareForTests (44100.0, 441);
        c->takeToasts();
        c->tickForTests();
        expect (c->getTestTakeState() == AppController::TakeState::empty, "the take is deleted");
        expect (toastsContain (*c, "サンプルレート"), "and the user is told");
        expect (! c->isMonitorOn() && findNotice (c->getNotices(), "take.playing") == nullptr);
        auto& vp = c->getProcessorForTests();
        const auto voice = synthVoice (1.0, 7, 44100.0);
        std::vector<float> out (voice.size(), 0.0f);
        for (size_t pos = 0; pos + 441 <= voice.size(); pos += 441) vp.process (voice.data() + pos, out.data() + pos, nullptr, 441);
        expect (peakAbs (out) > 0.01f, "the virtual mic is not muted any more");

        c->reprepareForTests (kSr, kBlock);
        expect (c->startTestTake (why), why);
        run (*c, synthVoice (0.5));
        c->stopTestTake();
        expect (c->playTestTake (why), why);
        CaptureData::assumeDevicesRunningForTests = false; // the device stopped
        c->tickForTests();
        expect (c->getTestTakeState() == AppController::TakeState::ready, "playback stopped, the take stays");
        expect (! c->isMonitorOn());
        CaptureData::assumeDevicesRunningForTests = true;
        expect (c->playTestTake (why), why);
        c->setMonitorOn (false); // the user turns the monitor off
        c->tickForTests();
        expect (c->getTestTakeState() == AppController::TakeState::ready, "playback stops with the monitor");
    }

    // ---------------------------------------------------------------------------------------------
    void wavRecording()
    {
        beginTest ("WAV: mono 24-bit at the device rate, exactly what went to the virtual mic, named by the time, (2) on a clash");
        freshDataDir();
        AssumeRunning running;
        auto c = makeController();
        juce::String why;
        expect (c->getLastWavFile() == juce::File());
        expect (c->startWavRecording (why), why);
        expect (c->isWavRecording());
        const auto notices = c->getNotices();
        const auto* n = findNotice (notices, "wav.recording");
        expect (n != nullptr && ! n->dismissible && n->actionId == "wavStop", "notice with 止める");
        const auto voice = synthVoice (2.0);
        const auto out = run (*c, voice);
        expectWithinAbsoluteError (c->getWavRecordingSeconds(), 2.0, 1.0e-6);
        c->stopWavRecording();
        expect (! c->isWavRecording() && findNotice (c->getNotices(), "wav.recording") == nullptr);
        const auto f = c->getLastWavFile();
        expect (f.existsAsFile(), f.getFullPathName());
        expect (f.getParentDirectory() == paths::recordingsDir());
        const auto stem = f.getFileNameWithoutExtension();
        expect (stem.length() == juce::String ("KoeLoom 2026-10-04 12-34-56").length()
                    && stem.matchesWildcard ("KoeLoom ????-??-?? ??-??-??", false) && f.hasFileExtension ("wav"), stem);
        const auto w = readWav (f);
        expect (w.ok && w.rate == kSr && w.channels == 1 && w.bits == 24, "format");
        expectEquals (int (w.samples.size()), int (voice.size()), "length");
        float err = 0.0f;
        for (size_t i = 0; i < std::min (w.samples.size(), out.size()); ++i) err = std::max (err, std::abs (w.samples[i] - out[i]));
        expectLessThan (err, 1.0e-6f, "the samples are what went to the virtual mic");

        // the hotkey; a second file in the same second gets " (2)"; the take's playback is recorded although the mic is muted
        c->performAction ("recordToggle");
        expect (c->isWavRecording(), "recordToggle starts");
        expect (c->startTestTake (why), why); // the take records the raw input meanwhile
        run (*c, synthVoice (0.5));
        c->stopTestTake();
        c->getMonitorForTests().prepareForTest (kSr, kSr, kBlock);
        expect (c->playTestTake (why), why);
        const auto mic = run (*c, silence (1.0));
        c->stopTestTake();
        c->performAction ("recordToggle");
        expect (! c->isWavRecording(), "recordToggle stops");
        const auto g = c->getLastWavFile();
        expect (g.existsAsFile() && g != f, g.getFullPathName());
        if (g.getFileName().substring (0, 27) == f.getFileName().substring (0, 27))
            expect (g.getFileName().endsWith (" (2).wav"), g.getFileName());
        const auto w2 = readWav (g);
        expectEquals (int (w2.samples.size()), int (1.5 * kSr));
        expectEquals (peakAbs (slice (mic, 0.05, 1.0)), 0.0f, "virtual mic muted during the take");
        expect (rmsDb (slice (w2.samples, 0.8, 1.5)) > -40.0f, "the WAV has the take's processed sound (before the mute)");

        // nothing recorded -> no file is left behind
        const int before = paths::recordingsDir().getNumberOfChildFiles (juce::File::findFiles);
        expect (c->startWavRecording (why), why);
        c->stopWavRecording();
        expectEquals (paths::recordingsDir().getNumberOfChildFiles (juce::File::findFiles), before);
        expect (c->getLastWavFile() == g);
    }

    // ---------------------------------------------------------------------------------------------
    void wavFailureAndShutdown()
    {
        beginTest ("WAV: a failed write stops it with a Japanese notice; closing the app closes the file first");
        freshDataDir();
        juce::String why;
        {
            AssumeRunning running;
            auto c = makeController();
            CaptureData::failNextWavWriteForTests = true;
            expect (c->startWavRecording (why), why);
            run (*c, synthVoice (0.5));
            for (int i = 0; i < 200 && c->isWavRecording(); ++i)
            {
                juce::Thread::sleep (10);
                c->tickForTests();
            }
            expect (! c->isWavRecording(), "stopped by the failure");
            const auto notices = c->getNotices();
            const auto* n = findNotice (notices, "wav.failed");
            expect (n != nullptr && n->text.contains (juce::String::fromUTF8 ("録音を止めました")), n != nullptr ? n->text : juce::String());
            expect (c->startWavRecording (why), why);
            expect (findNotice (c->getNotices(), "wav.failed") == nullptr, "a new recording clears it");
            c->stopWavRecording();
        }
        freshDataDir();
        {
            AssumeRunning running;
            auto c = makeController();
            expect (c->startWavRecording (why), why);
            run (*c, synthVoice (1.0));
            c->shutdown();
            expect (! c->isWavRecording());
            c->shutdown(); // twice is fine
        }
        auto files = paths::recordingsDir().findChildFiles (juce::File::findFiles, false, "*.wav");
        expectEquals (files.size(), 1);
        if (files.size() == 1)
        {
            const auto w = readWav (files[0]);
            expect (w.ok);
            expectEquals (int (w.samples.size()), int (kSr), "every sample is in the closed file");
        }
        {
            AssumeRunning running;
            auto c = makeController();
            expect (c->startWavRecording (why), why);
            run (*c, synthVoice (0.5));
            // destroyed while recording (no shutdown()): the destructor closes the file
        }
        files = paths::recordingsDir().findChildFiles (juce::File::findFiles, false, "*.wav");
        juce::StringArray names;
        for (auto& f : files) names.add (f.getFileName() + " " + juce::String (f.getSize()));
        expectEquals (files.size(), 2, names.joinIntoString (", "));
        for (auto& f : files) expect (readWav (f).ok, f.getFileName());
    }

    // ---------------------------------------------------------------------------------------------
    void noAllocationOnTheAudioThread()
    {
        beginTest ("No allocation in process() while the take records / plays and the WAV records (AllocHook)");
        freshDataDir();
        AssumeRunning running;
        auto c = makeController();
        auto& vp = c->getProcessorForTests();
        const auto voice = synthVoice (1.0);
        std::vector<float> out (voice.size());
        juce::String why;
        run (*c, voice); // warm up
        expect (c->startTestTake (why), why);
        expect (c->startWavRecording (why), why);
        {
            long long n = 0;
            {
                AllocationCounter count;
                for (size_t pos = 0; pos + kBlock <= voice.size(); pos += kBlock) vp.process (voice.data() + pos, out.data() + pos, nullptr, kBlock);
                n = count.count();
            }
            expectEquals (n, 0LL, "recording");
        }
        c->stopTestTake();
        c->getMonitorForTests().prepareForTest (kSr, kSr, kBlock);
        expect (c->playTestTake (why), why);
        run (*c, voice); // the mute fade and the chain settle
        {
            long long n = 0;
            {
                AllocationCounter count;
                for (size_t pos = 0; pos + kBlock <= voice.size(); pos += kBlock) vp.process (voice.data() + pos, out.data() + pos, nullptr, kBlock);
                n = count.count();
            }
            expectEquals (n, 0LL, "playing");
        }
        expect (allFinite (out));
        c->stopTestTake();
        c->stopWavRecording();
    }

    // ---------------------------------------------------------------------------------------------
    void tools()
    {
        using namespace ui;
        beginTest ("ツール 試し録り / 録音: buttons follow the state, everything fits the narrow window");
        freshDataDir();
        AssumeRunning running;
        auto c = makeController();
        const bool wasOff = mainui::animationsOff();
        mainui::animationsOff() = true;
        {
            MainComponent mc (*c);
            for (auto size : { juce::Point<int> (Theme::defaultWidth, Theme::defaultHeight), juce::Point<int> (Theme::minWidth, Theme::minHeight) })
            {
                mc.setSize (size.x, size.y);
                mc.showPage (Navigator::Page::tools);
                auto* view = dynamic_cast<ToolsView*> (mainui::findById (&mc, "page.tools"));
                expect (view != nullptr);
                if (view == nullptr) return;
                const auto label = juce::String (size.x) + "x" + juce::String (size.y);
                for (auto [tool, id, buttons] : { std::tuple { ToolsView::Tool::take, "tools.take", juce::StringArray { "take.record", "take.play", "take.clear" } },
                                                  std::tuple { ToolsView::Tool::record, "tools.record", juce::StringArray { "record.toggle", "record.folder", "record.open" } } })
                {
                    view->showTool (tool);
                    auto* page = mainui::findById (&mc, id);
                    expect (page != nullptr && page->isVisible(), label + " " + id);
                    if (page == nullptr) continue;
                    for (auto& b : buttons)
                    {
                        auto* comp = mainui::findById (page, b);
                        expect (comp != nullptr, b);
                        if (comp == nullptr) continue;
                        expect (page->getLocalBounds().contains (comp->getBounds()), label + ": " + b + " inside the card");
                        expect (comp->getHeight() >= Theme::touchMin, label + ": " + b + " tall enough");
                    }
                }
            }

            auto button = [&mc] (const char* id) { return dynamic_cast<juce::Button*> (mainui::findById (&mc, id)); };
            dynamic_cast<ToolsView*> (mainui::findById (&mc, "page.tools"))->showTool (ToolsView::Tool::take);
            expect (! button ("take.play")->isEnabled() && ! button ("take.clear")->isEnabled(), "nothing to play or clear yet");
            button ("take.record")->onClick();
            expect (c->getTestTakeState() == AppController::TakeState::recording);
            expect (button ("take.record")->getButtonText() == juce::String::fromUTF8 ("止める"));
            run (*c, synthVoice (0.5));
            button ("take.record")->onClick();
            expect (c->getTestTakeState() == AppController::TakeState::ready);
            expect (button ("take.play")->isEnabled() && button ("take.clear")->isEnabled());
            c->getMonitorForTests().prepareForTest (kSr, kSr, kBlock);
            button ("take.play")->onClick();
            expect (c->getTestTakeState() == AppController::TakeState::playing);
            expect (! button ("take.record")->isEnabled(), "no recording while playing");
            button ("take.play")->onClick();
            expect (c->getTestTakeState() == AppController::TakeState::ready);
            button ("take.clear")->onClick();
            expect (c->getTestTakeState() == AppController::TakeState::empty);

            dynamic_cast<ToolsView*> (mainui::findById (&mc, "page.tools"))->showTool (ToolsView::Tool::record);
            expect (! button ("record.open")->isEnabled(), "no file to open yet");
            button ("record.toggle")->onClick();
            expect (c->isWavRecording() && button ("record.toggle")->getButtonText() == juce::String::fromUTF8 ("停止"));
            run (*c, synthVoice (0.3));
            button ("record.toggle")->onClick();
            expect (! c->isWavRecording() && c->getLastWavFile().existsAsFile());
            expect (button ("record.open")->isEnabled(), "the last file can be opened");
        }
        mainui::animationsOff() = wasOff;
    }
};

static CaptureTests captureTests;
} // namespace koe

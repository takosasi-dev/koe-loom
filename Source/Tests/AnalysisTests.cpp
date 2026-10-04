// 声の高さのメーター and 自分の声で音量合わせ (INTERFACES.md §10.3 / §10.4, owner wave8/analysis). Category "Analysis".
// No window, no audio device: AppController (false), audio pushed through getProcessorForTests(), ticks by tickForTests(),
// AnalysisData::testDevicesRunning instead of a running device. Levels are measured on the synthetic voice (合成音声で代用).

#include "App/AppController.h"
#include "Core/Paths.h"
#include "Engine/VoiceAnalysis.h"
#include "Tests/TestUtil.h"
#include "Tools/Calibrate.h"
#include "UI/MainComponent.h"
#include "UI/main/Common.h"
#include "UI/main/ToolsView.h"

#include <cmath>
#include <functional>

namespace koe
{
namespace
{
using namespace test;
using Phase = AppController::CalibrationState::Phase;
constexpr int kBlock = 480;
constexpr int kPollSamples = 1600; // 1/30 s at 48 kHz: one UI frame

void freshDataDir()
{
    auto d = paths::dataDir();
    if (d.getFullPathName().contains ("KoeLoomTests")) d.deleteRecursively();
    d.createDirectory();
}

std::unique_ptr<AppController> makeController (const char* preset = "natural-asis")
{
    auto c = std::make_unique<AppController> (false);
    c->startup();
    c->updateSettings ([] (Settings& s) { s.setupDone = true; s.tourStep = 7; });
    c->setVoiceChangerOn (true);
    c->loadPreset (preset);
    c->dispatchPendingMessages();
    return c;
}

std::vector<float> feed (VoiceProcessor& vp, const std::vector<float>& in)
{
    std::vector<float> out (in.size(), 0.0f);
    for (size_t pos = 0; pos + kBlock <= in.size(); pos += kBlock) vp.process (in.data() + pos, out.data() + pos, nullptr, kBlock);
    return out;
}

/** Feeds `signal` one UI frame at a time and polls after each; returns the last reading. */
AppController::PitchReading pollWhileFeeding (AppController& c, const std::vector<float>& signal, int frames, std::vector<AppController::PitchReading>* all = nullptr)
{
    AppController::PitchReading r;
    std::vector<float> out (kPollSamples);
    for (int f = 0; f < frames; ++f)
    {
        const size_t pos = size_t (f) * kPollSamples % (signal.size() - kPollSamples);
        c.getProcessorForTests().process (signal.data() + pos, out.data(), nullptr, kPollSamples);
        r = c.pollPitch();
        if (all != nullptr) all->push_back (r);
    }
    return r;
}

bool waitFor (AppController& c, const std::function<bool()>& done, double seconds)
{
    const double end = juce::Time::getMillisecondCounterHiRes() + seconds * 1000.0;
    while (! done())
    {
        if (juce::Time::getMillisecondCounterHiRes() > end) return false;
        c.tickForTests();
        juce::Thread::sleep (5);
    }
    return true;
}

float shippedTrim (AppController& c, const juce::String& id)
{
    auto* p = c.getPresetLibrary().find (id.toStdString());
    return p != nullptr ? p->outputTrimDb : -999.0f;
}

void resetHooks()
{
    AnalysisData::testDevicesRunning = false;
    AnalysisData::testPresetIds.clear();
}
} // namespace

class AnalysisTests : public juce::UnitTest
{
public:
    AnalysisTests() : juce::UnitTest ("Pitch meter and voice calibration (wave8/analysis)", "Analysis") {}

    void runTest() override
    {
        pitchTests();
        calibrationRuleTests();
        tapTests();
        calibrationTests();
        uiTests();
        fullCalibrationTest(); // the slow one last
        resetHooks();
    }

private:
    void pitchTests()
    {
        beginTest ("声の高さ: synthetic 220 Hz within 1 % (input and そのまま output), silence / quiet / noise read 0");
        freshDataDir();
        auto c = makeController();
        const auto tone = sine (220.0, 2.0, 0.3f);
        auto r = pollWhileFeeding (*c, tone, 20);
        expectWithinAbsoluteError (r.inputHz, 220.0f, 2.2f, "input 220 Hz");
        expectWithinAbsoluteError (r.outputHz, 220.0f, 2.2f, "output (そのまま) 220 Hz");
        logMessage ("220 Hz -> in " + juce::String (r.inputHz, 2) + " Hz, out " + juce::String (r.outputHz, 2) + " Hz");

        r = pollWhileFeeding (*c, silence (1.0), 10);
        expectEquals (r.inputHz, 0.0f, "silence: input 0");
        expectEquals (r.outputHz, 0.0f, "silence: output 0");
        r = pollWhileFeeding (*c, sine (220.0, 1.0, 0.002f), 10); // about -57 dBFS
        expectEquals (r.inputHz, 0.0f, "below -50 dBFS: 0");
        std::vector<AppController::PitchReading> noise;
        pollWhileFeeding (*c, whiteNoise (2.0, 0.3f), 40, &noise);
        int voiced = 0;
        for (auto& n : noise) voiced += n.inputHz > 0.0f ? 1 : 0;
        expect (voiced <= 8, "white noise reads as no pitch (mostly): " + juce::String (voiced) + " / 40");

        beginTest ("声の高さ: 魔王 (-9 st) lowers the processed voice; the input reading stays");
        c->loadPreset ("character-demon-king");
        c->dispatchPendingMessages();
        r = pollWhileFeeding (*c, tone, 40);
        expectWithinAbsoluteError (r.inputHz, 220.0f, 2.2f, "input still 220 Hz");
        logMessage ("魔王: out " + juce::String (r.outputHz, 1) + " Hz (220 Hz -9 st = 130.8 Hz)");
        expect (r.outputHz > 100.0f && r.outputHz < 165.0f, "processed voice lower: " + juce::String (r.outputHz, 1) + " Hz");

        beginTest ("声の高さ: the taps come off ~1 s after the last poll and go back on with the next one");
        c->loadPreset ("natural-asis");
        c->dispatchPendingMessages();
        r = pollWhileFeeding (*c, tone, 10);
        expect (r.inputHz > 0.0f, "reading while polled");
        for (int i = 0; i < 31; ++i) c->tickForTests();
        feed (c->getProcessorForTests(), tone); // nothing listens now
        r = c->pollPitch();                     // re-attaches with an empty ring
        expectEquals (r.inputHz, 0.0f, "detached: the audio fed meanwhile was not kept");
        r = pollWhileFeeding (*c, tone, 10);
        expectWithinAbsoluteError (r.inputHz, 220.0f, 2.2f, "reading again after re-attaching");
        for (int i = 0; i < 20; ++i) c->tickForTests();
        r = c->pollPitch(); // still attached (20 ticks < 1 s), but nothing new arrived: no pitch rather than the last one
        r = c->pollPitch();
        expectEquals (r.inputHz, 0.0f, "no new audio (device stopped): 0");

        beginTest ("noteNameForHz");
        expectEquals (noteNameForHz (220.0f), juce::String ("A3"));
        expectEquals (noteNameForHz (440.0f), juce::String ("A4"));
        expectEquals (noteNameForHz (261.63f), juce::String ("C4"));
        expectEquals (noteNameForHz (123.47f), juce::String ("B2"));
        expectEquals (noteNameForHz (0.0f), juce::String());
    }

    void calibrationRuleTests()
    {
        beginTest ("音量合わせ: the trim rule of tools/apply_trims.py (|diff| <= 1 dB keeps the shipped trim), 0.5 dB steps, kTrimDb");
        expectEquals (tools::calibratedTrimDb (0.0f, 0.9f), 0.0f);
        expectEquals (tools::calibratedTrimDb (0.0f, -1.0f), 0.0f);
        expectEquals (tools::calibratedTrimDb (2.0f, 1.0f), 2.0f);
        expectEquals (tools::calibratedTrimDb (-1.5f, 0.3f), -1.5f);
        expectEquals (tools::calibratedTrimDb (0.0f, 1.2f), -1.0f);
        expectEquals (tools::calibratedTrimDb (0.0f, -3.3f), 3.5f);
        expectEquals (tools::calibratedTrimDb (-1.0f, 5.0f), -6.0f);
        expectEquals (tools::calibratedTrimDb (0.0f, -20.0f), kTrimDb.max);
        expectEquals (tools::calibratedTrimDb (0.0f, 30.0f), kTrimDb.min);

        beginTest ("音量合わせ: the take check (too quiet / clipped / under 3 s of voice -> a Japanese reason)");
        expect (checkCalibrationTake (synthVoice (10.0), kSr).isEmpty(), "synthetic speech is usable");
        expect (checkCalibrationTake (silence (10.0), kSr).contains (juce::String::fromUTF8 ("小さすぎ")), "silence: too quiet");
        expect (checkCalibrationTake (sine (200.0, 10.0, 0.001f), kSr).contains (juce::String::fromUTF8 ("小さすぎ")), "-63 dBFS: too quiet");
        auto loud = synthVoice (10.0);
        for (auto& v : loud) v = juce::jlimit (-1.0f, 1.0f, v * 8.0f);
        expect (checkCalibrationTake (loud, kSr).contains (juce::String::fromUTF8 ("割れて")), "clipped: too loud");
        const auto shortVoice = concat ({ synthVoice (2.0), silence (8.0) });
        expect (checkCalibrationTake (shortVoice, kSr).contains (juce::String::fromUTF8 ("3 秒")), "2 s of voice: too little");
        auto noisy = concat ({ synthVoice (2.0), silence (8.0) });
        const auto hiss = whiteNoise (10.0, 0.0005f); // room noise about -72 dBFS does not count as voice
        for (size_t i = 0; i < noisy.size(); ++i) noisy[i] += hiss[i];
        expect (checkCalibrationTake (noisy, kSr).contains (juce::String::fromUTF8 ("3 秒")), "2 s of voice over room noise: too little");
    }

    void tapTests()
    {
        beginTest ("Taps attached (pitch + calibration take): no allocation on the audio thread, the virtual mic bit-identical");
        const auto voice = synthVoice (3.0);
        std::vector<float> plain;
        {
            freshDataDir();
            auto c = makeController ("character-demon-king");
            plain = feed (c->getProcessorForTests(), voice);
        }
        freshDataDir();
        auto c = makeController ("character-demon-king");
        c->pollPitch();
        AnalysisData::testDevicesRunning = true;
        juce::String why;
        expect (c->startCalibration (why), "recording starts: " + why);
        std::vector<float> tapped (voice.size(), 0.0f);
        {
            AllocationCounter counter;
            for (size_t pos = 0; pos + kBlock <= voice.size(); pos += kBlock)
                c->getProcessorForTests().process (voice.data() + pos, tapped.data() + pos, nullptr, kBlock);
            const auto allocations = counter.count(); // before building the message string
            expectEquals (allocations, 0LL, "no allocation with the taps on");
        }
        float diff = 0.0f;
        for (size_t i = 0; i < plain.size(); ++i) diff = std::max (diff, std::abs (plain[i] - tapped[i]));
        expectEquals (diff, 0.0f, "same output sample for sample");
        expect (allFinite (tapped), "finite");
        c->cancelCalibration();
        resetHooks();
    }

    void calibrationTests()
    {
        beginTest ("音量合わせ: refused without a running device; while recording; 10 s of synthetic speech -> trims stored and used at once");
        freshDataDir();
        auto c = makeController();
        juce::String why;
        expect (! c->startCalibration (why) && why.isNotEmpty(), "no device: refused (" + why + ")");
        expect (c->getCalibrationState().phase == Phase::idle, "still idle");

        AnalysisData::testDevicesRunning = true;
        AnalysisData::testPresetIds = { "natural-asis", "natural-clear", "character-demon-king", "character-helium", "character-robot" };
        expect (c->startCalibration (why), "starts: " + why);
        expect (c->getCalibrationState().phase == Phase::recording, "recording");
        expect (! c->startCalibration (why) && why.isNotEmpty(), "a second start is refused while recording");
        feed (c->getProcessorForTests(), synthVoice (4.0));
        c->tickForTests();
        const auto mid = c->getCalibrationState();
        expect (mid.phase == Phase::recording && mid.progress > 0.3f && mid.progress < 0.5f, "progress " + juce::String (mid.progress, 2));
        feed (c->getProcessorForTests(), synthVoice (6.5, 11));
        c->tickForTests();
        expect (c->getCalibrationState().phase == Phase::analysing, "measuring once 10 s are in");
        const double t0 = juce::Time::getMillisecondCounterHiRes();
        expect (waitFor (*c, [&] { return c->getCalibrationState().phase != Phase::analysing; }, 120.0), "finishes");
        logMessage ("5 presets measured in " + juce::String ((juce::Time::getMillisecondCounterHiRes() - t0) / 1000.0, 1) + " s");
        const auto st = c->getCalibrationState();
        expect (st.phase == Phase::done, "done (" + st.error + ")");
        const auto& s = c->getSettings();
        expectEquals (int (s.calibratedTrimDb.size()), 5, "one trim per measured preset");
        expect (s.calibratedAt.isNotEmpty() && juce::Time::fromISO8601 (s.calibratedAt).toMilliseconds() > 0, "calibratedAt: " + s.calibratedAt);
        int adjusted = 0;
        for (auto& [id, trim] : s.calibratedTrimDb)
        {
            const float shipped = shippedTrim (*c, id);
            const bool same = trim == shipped;
            adjusted += std::abs (trim - shipped) >= 0.1f ? 1 : 0;
            expect (same || (std::abs (trim - shipped) >= 0.5f && std::abs (trim * 2.0f - std::round (trim * 2.0f)) < 1.0e-4f),
                    id + ": shipped, or moved by more than 1 dB in 0.5 dB steps (" + juce::String (shipped) + " -> " + juce::String (trim) + ")");
            expect (trim >= kTrimDb.min && trim <= kTrimDb.max, id + " within kTrimDb");
            logMessage ("  " + id + ": shipped " + juce::String (shipped, 1) + " -> " + juce::String (trim, 1));
        }
        expectEquals (s.calibratedTrimDb.at ("natural-asis"), shippedTrim (*c, "natural-asis"), "そのまま keeps its trim");
        expectEquals (st.presetsAdjusted, adjusted, "presetsAdjusted = trims 0.1 dB or more away from the shipped one");

        c->loadPreset ("character-demon-king");
        expectEquals (c->getEffectiveTrimDb(), s.calibratedTrimDb.at ("character-demon-king"), "the loaded built-in uses the measured trim");
        c->clearCalibration();
        expect (c->getSettings().calibratedTrimDb.empty() && c->getSettings().calibratedAt.isEmpty(), "元に戻す clears both");
        expectEquals (c->getEffectiveTrimDb(), shippedTrim (*c, "character-demon-king"), "back to the shipped trim");
        expect (c->getCalibrationState().phase == Phase::idle, "idle after 元に戻す");

        beginTest ("音量合わせ: やめる while recording or measuring changes nothing");
        c->updateSettings ([] (Settings& x) { x.calibratedTrimDb["character-helium"] = -3.0f; x.calibratedAt = "2026-01-02T03:04:05.000+09:00"; });
        const auto before = c->getSettings();
        expect (c->startCalibration (why), "starts");
        feed (c->getProcessorForTests(), synthVoice (3.0));
        c->cancelCalibration();
        expect (c->getCalibrationState().phase == Phase::idle, "idle after cancelling the recording");
        feed (c->getProcessorForTests(), synthVoice (8.0)); // the tap is off: this must not finish a take
        c->tickForTests();
        expect (c->getCalibrationState().phase == Phase::idle, "still idle");

        expect (c->startCalibration (why), "starts again");
        feed (c->getProcessorForTests(), synthVoice (10.5));
        c->tickForTests();
        expect (c->getCalibrationState().phase == Phase::analysing, "measuring");
        c->cancelCalibration();
        expect (c->getCalibrationState().phase == Phase::idle, "idle at once");
        expect (waitFor (*c, [&] { return c->startCalibration (why); }, 30.0), "the cancelled job winds down, then a new start works");
        c->cancelCalibration();
        for (int i = 0; i < 5; ++i) c->tickForTests();
        expect (c->getSettings().calibratedTrimDb == before.calibratedTrimDb && c->getSettings().calibratedAt == before.calibratedAt,
                "the earlier calibration is untouched");

        beginTest ("音量合わせ: a silent take fails with a Japanese reason and stores nothing; the device stopping ends a recording");
        expect (c->startCalibration (why), "starts");
        feed (c->getProcessorForTests(), silence (10.5));
        c->tickForTests();
        auto f = c->getCalibrationState();
        expect (f.phase == Phase::failed && f.error.contains (juce::String::fromUTF8 ("小さすぎ")), "failed: " + f.error);
        expect (c->getSettings().calibratedTrimDb == before.calibratedTrimDb, "nothing stored");
        expect (c->startCalibration (why), "can start again after a failure");
        AnalysisData::testDevicesRunning = false;
        c->tickForTests();
        f = c->getCalibrationState();
        expect (f.phase == Phase::failed && f.error.isNotEmpty(), "device stopped: failed (" + f.error + ")");
        c->clearCalibration();

        beginTest ("音量合わせ: the input gain is applied to the take");
        AnalysisData::testDevicesRunning = true;
        c->setInputGainDb (20.0f); // synthetic speech peaks at -6 dBFS: +20 dB clips (the tap itself is before the gain)
        expect (c->startCalibration (why), "starts");
        feed (c->getProcessorForTests(), synthVoice (10.5));
        c->tickForTests();
        f = c->getCalibrationState();
        expect (f.phase == Phase::failed && f.error.contains (juce::String::fromUTF8 ("割れて")), "clipped after +20 dB of input gain: " + f.error);
        c->setInputGainDb (0.0f);
        resetHooks();
    }

    void uiTests()
    {
        using namespace ui;
        beginTest ("Tools: 声の高さ and 音量合わせ fit the narrow card; start / やめる / 元に戻す follow the state");
        const bool wasOff = mainui::animationsOff();
        mainui::animationsOff() = true;
        freshDataDir();
        auto c = makeController();
        MainComponent mc (*c);
        mc.setSize (Theme::minWidth, Theme::minHeight);
        mc.showPage (Navigator::Page::tools);
        auto* tools = dynamic_cast<ToolsView*> (mainui::findById (&mc, "page.tools"));
        expect (tools != nullptr, "tools page");
        if (tools == nullptr) return;

        tools->showTool (ToolsView::Tool::pitch);
        auto* pitch = mainui::findById (&mc, "tools.pitch");
        expect (pitch != nullptr && pitch->isVisible() && pitch->getWidth() >= 400 && pitch->getHeight() >= 300,
                "pitch tool size " + (pitch != nullptr ? pitch->getBounds().toString() : juce::String()));
        auto* pitchTimer = dynamic_cast<juce::Timer*> (pitch);
        expect (pitchTimer != nullptr, "the pitch tool is a timer");
        if (auto* t = pitchTimer)
        {
            expect (t->isTimerRunning(), "pitch tool polls while shown");
            const auto tone = sine (220.0, 1.0, 0.3f);
            std::vector<float> out (kPollSamples);
            for (int i = 0; i < 5; ++i)
            {
                c->getProcessorForTests().process (tone.data() + i * kPollSamples, out.data(), nullptr, kPollSamples);
                t->timerCallback();
            }
            juce::Image img (juce::Image::ARGB, pitch->getWidth(), pitch->getHeight(), true);
            juce::Graphics g (img);
            pitch->paintEntireComponent (g, false); // draws without throwing; the snapshot shows the result
        }

        tools->showTool (ToolsView::Tool::calibrate);
        auto* cal = mainui::findById (&mc, "tools.calibrate");
        auto* start = dynamic_cast<juce::Button*> (mainui::findById (&mc, "calibrate.start"));
        auto* cancel = dynamic_cast<juce::Button*> (mainui::findById (&mc, "calibrate.cancel"));
        auto* undo = dynamic_cast<juce::Button*> (mainui::findById (&mc, "calibrate.undo"));
        expect (cal != nullptr && start != nullptr && cancel != nullptr && undo != nullptr, "calibrate parts");
        if (cal == nullptr || start == nullptr || cancel == nullptr || undo == nullptr) return;
        auto inside = [cal] (juce::Component* b) { return cal->getLocalBounds().contains (b->getBounds()); };
        expect (start->isVisible() && ! cancel->isVisible() && ! undo->isVisible(), "idle: 始める only");
        expect (inside (start) && start->getHeight() >= Theme::touchMin, "始める inside the card " + start->getBounds().toString());
        start->onClick(); // no device: refused, shown in the card
        expect (c->getCalibrationState().phase == Phase::idle, "refused without a device");
        AnalysisData::testDevicesRunning = true;
        start->onClick();
        expect (c->getCalibrationState().phase == Phase::recording, "recording");
        expect (cancel->isVisible() && ! start->isVisible() && inside (cancel), "recording: やめる");
        cancel->onClick();
        expect (c->getCalibrationState().phase == Phase::idle && start->isVisible(), "やめる -> idle");
        c->updateSettings ([] (Settings& x) { x.calibratedTrimDb["character-helium"] = -3.0f; x.calibratedAt = juce::Time::getCurrentTime().toISO8601 (true); });
        auto* calTimer = dynamic_cast<juce::Timer*> (cal);
        expect (calTimer != nullptr, "the calibrate tool is a timer");
        if (calTimer != nullptr) calTimer->timerCallback();
        expect (undo->isVisible() && inside (undo) && ! undo->getBounds().intersects (start->getBounds()), "元に戻す shown with a calibration " + undo->getBounds().toString());
        undo->onClick();
        expect (c->getSettings().calibratedTrimDb.empty(), "元に戻す clears");
        expect (! undo->isVisible(), "and hides");
        resetHooks();
        mainui::animationsOff() = wasOff;
    }

    void fullCalibrationTest()
    {
        beginTest ("音量合わせ: every built-in preset on 10 s of synthetic speech (合成音声で代用), in the background");
        freshDataDir();
        auto c = makeController();
        int builtins = 0;
        for (auto& p : c->getPresetLibrary().all()) builtins += p.builtin ? 1 : 0;
        AnalysisData::testDevicesRunning = true;
        juce::String why;
        expect (c->startCalibration (why), "starts: " + why);
        feed (c->getProcessorForTests(), synthVoice (10.5));
        c->tickForTests();
        expect (c->getCalibrationState().phase == Phase::analysing, "measuring");
        const double t0 = juce::Time::getMillisecondCounterHiRes();
        expect (waitFor (*c, [&] { return c->getCalibrationState().phase != Phase::analysing; }, 600.0), "finishes");
        const double secs = (juce::Time::getMillisecondCounterHiRes() - t0) / 1000.0;
        const auto st = c->getCalibrationState();
        expect (st.phase == Phase::done, "done (" + st.error + ")");
        const auto& m = c->getSettings().calibratedTrimDb;
        expectEquals (int (m.size()), builtins, "every built-in preset has a trim");
        int kept = 0;
        for (auto& [id, trim] : m)
        {
            expect (std::isfinite (trim) && trim >= kTrimDb.min && trim <= kTrimDb.max, id);
            kept += trim == shippedTrim (*c, id) ? 1 : 0;
        }
        logMessage (juce::String (builtins) + " presets in " + juce::String (secs, 1) + " s; " + juce::String (st.presetsAdjusted)
                    + " adjusted, " + juce::String (kept) + " kept the shipped trim (synthetic speech)");
        resetHooks();
    }
};

static AnalysisTests analysisTests;
} // namespace koe

// 声の大きさで変わる効果の UI and マイクの癖の補正 (INTERFACES.md §11.3 / §11.4, owner wave9/voice). Category "Voice".
// No window, no audio device: AppController (false), audio pushed through getProcessorForTests(), ticks by tickForTests(),
// MicEqData::testDevicesRunning instead of a running device. Measurements use the synthetic voice (合成音声で代用).

#include "App/AppController.h"
#include "Core/Paths.h"
#include "Effects/EffectRegistry.h"
#include "Engine/MicEq.h"
#include "Tests/TestUtil.h"
#include "UI/MainComponent.h"
#include "UI/main/Common.h"
#include "UI/main/Panels.h"
#include "UI/main/ToolsView.h"

#include <cmath>
#include <functional>

namespace koe
{
namespace
{
using namespace test;
using Phase = AppController::MicEqState::Phase;
constexpr int kBlock = 480;

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

float meanOf (const std::vector<float>& v)
{
    float m = 0.0f;
    for (float x : v) m += x;
    return v.empty() ? 0.0f : m / float (v.size());
}

/** RMS (dB) of a filter's output for a sine at hz, after it settled. */
float filteredSineDb (MicEqFilter& f, double hz, double rate)
{
    auto x = sine (hz, 0.6, 0.25f, rate);
    for (size_t pos = 0; pos < x.size(); pos += kBlock) f.process (x.data() + pos, int (std::min<size_t> (kBlock, x.size() - pos)));
    const int tail = int (rate * 0.2);
    return rmsDb (x.data() + x.size() - size_t (tail), tail) - rmsDb (sine (hz, 0.2, 0.25f, rate));
}

void resetHooks()
{
    MicEqData::testDevicesRunning = false;
    AnalysisData::testDevicesRunning = false;
}
} // namespace

class VoiceTests : public juce::UnitTest
{
public:
    VoiceTests() : juce::UnitTest ("Level modulation UI and mic correction (wave9/voice)", "Voice") {}

    void runTest() override
    {
        gainRuleTests();
        filterTests();
        offTests();
        measureTests();
        rateTests();
        modUiTests();
        toolTests();
        resetHooks();
    }

private:
    void gainRuleTests()
    {
        beginTest ("補正量: the target is ANSI S3.5-1997 Table 3 (normal effort), interpolated, NaN outside 160 Hz .. 8 kHz");
        const auto target = micEqTargetDb();
        expect (std::isnan (target[0]) && std::isnan (target[13]), "125 Hz and 11.2 kHz are outside the table");
        expectWithinAbsoluteError (target[2], 34.75f, 0.01f, "250 Hz is a table point");
        expectWithinAbsoluteError (target[6], 25.01f, 0.01f, "1 kHz is a table point");
        expectWithinAbsoluteError (target[10], 9.33f, 0.01f, "4 kHz is a table point");
        expectWithinAbsoluteError (target[12], 1.13f, 0.01f, "8 kHz is a table point");
        expect (target[7] < target[6] && target[7] > target[8], "1.4 kHz lies between its neighbours");

        beginTest ("補正量: measured = target + D gives about -D (mean 0, smoothed), a constant D gives nothing");
        auto levelsWith = [&target] (const std::function<float (int)>& d)
        {
            MicEqBands l;
            for (int b = 0; b < kMicEqBands; ++b) l[size_t (b)] = (std::isfinite (target[size_t (b)]) ? target[size_t (b)] : 20.0f) + d (b);
            return l;
        };
        auto flat = micEqGainsFromLevels (levelsWith ([] (int) { return 7.0f; }));
        expectEquals (int (flat.size()), kMicEqBands, "14 gains");
        for (float g : flat) expectWithinAbsoluteError (g, 0.0f, 1.0e-4f, "a level offset is not a colour");

        auto tilt = [] (int b) { return 4.0f * (float (b) - 6.5f) / 6.5f; }; // the mic is +-4 dB brighter / darker
        auto g = micEqGainsFromLevels (levelsWith (tilt));
        for (int b = 1; b < kMicEqBands - 1; ++b)
        {
            expectWithinAbsoluteError (g[size_t (b)], -tilt (b), 0.6f, "band " + juce::String (b));
        }
        expectWithinAbsoluteError (g[0], g[1], 0.6f, "125 Hz continues 180 Hz (no guessed target)");
        expectWithinAbsoluteError (meanOf (g), 0.0f, 0.02f, "mean 0: the loudness stays");

        beginTest ("補正量: a strong colour is held within +-6 dB with mean 0; a narrow peak is spread to its neighbours");
        auto big = micEqGainsFromLevels (levelsWith ([] (int b) { return b < 7 ? 15.0f : -15.0f; }));
        for (float v : big) expect (v >= -kMicEqMaxDb - 1.0e-4f && v <= kMicEqMaxDb + 1.0e-4f, "within +-6: " + juce::String (v));
        expectWithinAbsoluteError (meanOf (big), 0.0f, 0.05f, "mean 0 after the limit");
        expect (big[0] < -5.0f && big[13] > 5.0f, "lows cut, highs lifted");
        auto peak = micEqGainsFromLevels (levelsWith ([] (int b) { return b == 9 ? 4.0f : 0.0f; })); // +4 dB at 2.8 kHz
        expect (peak[9] < -1.5f && peak[8] < -0.5f && peak[10] < -0.5f && peak[9] < peak[8], "the 2.8 kHz peak is pulled down, softly: "
                + juce::String (peak[8], 2) + " " + juce::String (peak[9], 2) + " " + juce::String (peak[10], 2));

        beginTest ("補正量: the band levels of a coloured voice (a +8 dB presence peak) lead to a cut there");
        const auto voice = synthVoice (6.0);
        auto coloured = voice;
        dsp::Biquad peq;
        peq.setPeak (kSr, 2800.0f, 1.4f, 8.0f);
        for (auto& v : coloured) v = peq.process (v);
        const auto plainLevels = micEqBandLevelsDb (voice, kSr), colouredLevels = micEqBandLevelsDb (coloured, kSr);
        for (int b = 0; b < kMicEqBands; ++b) expect (std::isfinite (plainLevels[size_t (b)]), "band " + juce::String (b) + " measured");
        expectWithinAbsoluteError (colouredLevels[9] - plainLevels[9], 8.0f, 1.0f, "the band levels see the +8 dB at 2.8 kHz");
        const auto plainGains = micEqGainsFromLevels (plainLevels), colouredGains = micEqGainsFromLevels (colouredLevels);
        expect (colouredGains[9] - plainGains[9] < -2.0f, "2.8 kHz is cut by more than 2 dB compared with the plain voice ("
                + juce::String (colouredGains[9] - plainGains[9], 2) + " dB)");
        for (int b = 0; b < kMicEqBands; ++b) logMessage ("  " + juce::String (kMicEqBandHz[b]) + " Hz: plain " + juce::String (plainGains[size_t (b)], 1)
                                                          + " dB, coloured " + juce::String (colouredGains[size_t (b)], 1) + " dB (合成音声で代用)");
        const auto none = micEqBandLevelsDb (silence (3.0), kSr);
        expect (std::isnan (none[5]), "silence: nothing measured");
    }

    void filterTests()
    {
        beginTest ("フィルタ: the cascade hits each band centre within 0.5 dB (48 kHz and 44.1 kHz)");
        const std::vector<std::vector<float>> patterns = {
            { -2.5f, -2.0f, -1.2f, -0.6f, -0.4f, -0.5f, -0.2f, 0.4f, 1.3f, 2.6f, 3.1f, 1.8f, 0.4f, -1.0f },
            { 0, 0, 0, 0, 0, 0, 6, 0, 0, 0, 0, 0, 0, 0 },
            { -6, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 6 },
            { 3, -3, 3, -3, 3, -3, 3, -3, 3, -3, 3, -3, 3, -3 },
            { 6, 6, 6, 6, 6, 6, 6, -6, -6, -6, -6, -6, -6, -6 },
        };
        for (const double rate : { 48000.0, 44100.0 })
            for (size_t k = 0; k < patterns.size(); ++k)
            {
                float worst = 0.0f;
                for (int b = 0; b < kMicEqBands; ++b)
                    worst = std::max (worst, std::abs (micEqResponseDb (patterns[k], rate, kMicEqBandHz[b]) - patterns[k][size_t (b)]));
                expect (worst <= 0.5f, "pattern " + juce::String (int (k)) + " at " + juce::String (rate) + ": worst " + juce::String (worst, 3) + " dB");
            }

        beginTest ("フィルタ: MicEqFilter itself (sines at band centres, after the 30 ms fade-in)");
        for (const int b : { 0, 4, 6, 9, 12 })
        {
            MicEqFilter f;
            f.setGains (patterns[0], kSr);
            f.setEnabled (true);
            const float got = filteredSineDb (f, kMicEqBandHz[b], kSr);
            expectWithinAbsoluteError (got, patterns[0][size_t (b)], 0.5f, juce::String (kMicEqBandHz[b]) + " Hz");
        }

        beginTest ("フィルタ: ON / OFF and a new set of gains glide (no clicks), no allocation, finite after a NaN");
        MicEqFilter f;
        f.setGains (patterns[1], kSr);
        auto x = sine (1000.0, 1.0, 0.3f);
        const auto dry = x;
        {
            AllocationCounter counter;
            for (size_t pos = 0; pos + kBlock <= x.size(); pos += kBlock)
            {
                const int blk = int (pos / kBlock);
                if (blk == 20) f.setEnabled (true);     // fade in
                if (blk == 60) f.setEnabled (false);    // fade out
                f.process (x.data() + pos, kBlock);
            }
            const auto allocations = counter.count(); // before building the message string
            expectEquals (allocations, 0LL, "no allocation in process (fades included)");
        }
        float steady = 0.0f, worstStep = 0.0f;
        for (size_t i = 1; i < 9000; ++i) steady = std::max (steady, std::abs (dry[i] - dry[i - 1]));
        for (size_t i = 1; i < x.size(); ++i) worstStep = std::max (worstStep, std::abs (x[i] - x[i - 1]));
        expect (worstStep < steady * 2.1f, "no step bigger than the +6 dB sine itself makes: " + juce::String (worstStep / steady, 2) + "x");
        const int doneAt = 60 * kBlock + int (kSr * 0.04);
        bool same = true;
        for (size_t i = size_t (doneAt); i < x.size(); ++i) same = same && x[i] == dry[i];
        expect (same && f.isIdle(), "faded out: the samples are untouched bit for bit");

        f.setEnabled (true);
        auto y = sine (1000.0, 0.5, 0.3f);
        for (size_t pos = 0; pos + kBlock <= y.size(); pos += kBlock)
        {
            if (pos == size_t (kBlock) * 20) f.setGains (patterns[0], kSr); // message thread: a new measurement
            AllocationCounter counter;
            f.process (y.data() + pos, kBlock);
            const auto allocations = counter.count(); // before building the message string
            expectEquals (allocations, 0LL, "no allocation taking the new gains");
        }
        float jump = 0.0f;
        for (size_t i = 1; i < y.size(); ++i) jump = std::max (jump, std::abs (y[i] - y[i - 1]));
        expect (jump < steady * 2.1f, "the swap crossfades: " + juce::String (jump / steady, 2) + "x");
        f.collectGarbage();

        std::vector<float> bad (kBlock, 0.1f);
        bad[10] = std::numeric_limits<float>::quiet_NaN();
        f.process (bad.data(), kBlock);
        auto after = sine (300.0, 0.1, 0.3f);
        f.process (after.data(), kBlock);
        expect (allFinite (after.data(), kBlock), "a NaN does not stay in the filter");
    }

    void offTests()
    {
        beginTest ("OFF: a stored measurement with micEqOn off changes nothing (the same samples as a fresh controller)");
        const auto voice = synthVoice (2.0);
        std::vector<float> plain;
        {
            freshDataDir();
            auto c = makeController ("character-demon-king");
            plain = feed (c->getProcessorForTests(), voice);
        }
        freshDataDir();
        auto c = makeController ("character-demon-king");
        c->updateSettings ([] (Settings& s)
        {
            s.micEqGainsDb.assign (size_t (kMicEqBands), 3.0f);
            s.micEqGainsDb[0] = -6.0f;
            s.micEqAt = "2026-10-04T12:00:00.000+09:00";
            s.micEqOn = false;
        });
        c->tickForTests();
        auto off = feed (c->getProcessorForTests(), voice);
        expect (off == plain, "bit-identical with the measurement stored but off");

        beginTest ("ON: the input is corrected (the output differs), no allocation on the audio thread, finite");
        c->setMicEqOn (true);
        c->tickForTests();
        std::vector<float> on (voice.size(), 0.0f);
        {
            AllocationCounter counter;
            for (size_t pos = 0; pos + kBlock <= voice.size(); pos += kBlock)
                c->getProcessorForTests().process (voice.data() + pos, on.data() + pos, nullptr, kBlock);
            const auto allocations = counter.count(); // before building the message string
            expectEquals (allocations, 0LL, "no allocation with the filter on");
        }
        expect (on != plain && allFinite (on), "corrected and finite");
        c->setMicEqOn (false);
        feed (c->getProcessorForTests(), silence (0.1));
        c->tickForTests();
        expect (! c->getSettings().micEqOn && c->getSettings().micEqGainsDb.size() == size_t (kMicEqBands), "OFF keeps the measurement");
        c->clearMicEq();
        expect (c->getSettings().micEqGainsDb.empty() && c->getSettings().micEqAt.isEmpty() && ! c->getSettings().micEqOn, "消す forgets it");
    }

    void measureTests()
    {
        beginTest ("測る: refused without a running device, while 音量合わせ runs, and twice");
        freshDataDir();
        auto c = makeController();
        juce::String why;
        expect (! c->startMicEqMeasure (why) && why.isNotEmpty(), "no device: " + why);
        AnalysisData::testDevicesRunning = true;
        MicEqData::testDevicesRunning = true;
        expect (c->startCalibration (why), "音量合わせ starts");
        why = {};
        expect (! c->startMicEqMeasure (why) && why.contains (juce::String::fromUTF8 ("音量合わせ")), "refused during 音量合わせ: " + why);
        c->cancelCalibration();
        expect (c->startMicEqMeasure (why), "starts: " + why);
        expect (c->getMicEqState().phase == Phase::recording, "recording");
        expect (! c->startMicEqMeasure (why) && why.isNotEmpty(), "a second start is refused");

        beginTest ("測る: no allocation with the recording tap on, the virtual mic unchanged");
        const auto voice = synthVoice (4.0);
        std::vector<float> plain;
        {
            auto ref = makeController();
            plain = feed (ref->getProcessorForTests(), voice);
        }
        std::vector<float> tapped (voice.size(), 0.0f);
        {
            AllocationCounter counter;
            for (size_t pos = 0; pos + kBlock <= voice.size(); pos += kBlock)
                c->getProcessorForTests().process (voice.data() + pos, tapped.data() + pos, nullptr, kBlock);
            const auto allocations = counter.count(); // before building the message string
            expectEquals (allocations, 0LL, "no allocation while recording");
        }
        expect (tapped == plain, "same output sample for sample while recording");
        c->tickForTests();
        const auto mid = c->getMicEqState();
        expect (mid.phase == Phase::recording && mid.progress > 0.35f && mid.progress < 0.45f, "progress " + juce::String (mid.progress, 2));

        beginTest ("測る: 10 s of synthetic voice -> 14 gains within +-6 dB, mean 0, micEqOn, micEqAt (合成音声で代用)");
        feed (c->getProcessorForTests(), synthVoice (6.5, 11));
        c->tickForTests();
        expect (c->getMicEqState().phase == Phase::analysing || c->getMicEqState().phase == Phase::done, "working it out once 10 s are in");
        expect (waitFor (*c, [&] { return c->getMicEqState().phase != Phase::analysing; }, 60.0), "finishes");
        auto st = c->getMicEqState();
        expect (st.phase == Phase::done, "done (" + st.error + ")");
        const auto& s = c->getSettings();
        expectEquals (int (s.micEqGainsDb.size()), kMicEqBands, "14 gains");
        for (float v : s.micEqGainsDb) expect (v >= -kMicEqMaxDb && v <= kMicEqMaxDb, "within +-6: " + juce::String (v));
        expectWithinAbsoluteError (meanOf (s.micEqGainsDb), 0.0f, 0.05f, "mean 0");
        expect (s.micEqOn && juce::Time::fromISO8601 (s.micEqAt).toMilliseconds() > 0, "on, at " + s.micEqAt);
        juce::String line;
        for (float v : s.micEqGainsDb) line << juce::String (v, 1) << " ";
        logMessage ("  synthetic voice gains: " + line);
        const auto stored = s.micEqGainsDb;

        beginTest ("測る: やめる changes nothing; a silent / clipped take fails with a Japanese reason; the device stopping ends it");
        expect (c->startMicEqMeasure (why), "starts again");
        feed (c->getProcessorForTests(), synthVoice (3.0));
        c->cancelMicEqMeasure();
        expect (c->getMicEqState().phase == Phase::idle, "idle after やめる");
        feed (c->getProcessorForTests(), synthVoice (8.0)); // the tap is off
        c->tickForTests();
        expect (c->getMicEqState().phase == Phase::idle && c->getSettings().micEqGainsDb == stored, "still idle, nothing changed");

        expect (c->startMicEqMeasure (why), "starts");
        feed (c->getProcessorForTests(), silence (10.5));
        c->tickForTests();
        st = c->getMicEqState();
        expect (st.phase == Phase::failed && st.error.contains (juce::String::fromUTF8 ("小さすぎ")), "silence: " + st.error);
        expect (c->getSettings().micEqGainsDb == stored, "nothing stored");

        c->setInputGainDb (20.0f);
        expect (c->startMicEqMeasure (why), "starts after a failure");
        feed (c->getProcessorForTests(), synthVoice (10.5));
        c->tickForTests();
        st = c->getMicEqState();
        expect (st.phase == Phase::failed && st.error.contains (juce::String::fromUTF8 ("割れて")), "clipped after +20 dB of input gain: " + st.error);
        c->setInputGainDb (0.0f);

        expect (c->startMicEqMeasure (why), "starts");
        MicEqData::testDevicesRunning = false;
        c->tickForTests();
        st = c->getMicEqState();
        expect (st.phase == Phase::failed && st.error.isNotEmpty(), "device stopped: " + st.error);
        expect (c->getSettings().micEqGainsDb == stored, "nothing stored");
        resetHooks();
    }

    void rateTests()
    {
        beginTest ("Device reopened at 44.1 kHz: the coefficients follow the rate (+6 dB at 2 kHz stays +6 dB)");
        auto setup = [] (bool micEq)
        {
            auto c = makeController();
            c->updateSettings ([micEq] (Settings& s)
            {
                s.noiseSuppressionOn = false;
                s.gateOn = false;
                s.micEqGainsDb.assign (size_t (kMicEqBands), 0.0f);
                s.micEqGainsDb[8] = 6.0f; // 2 kHz
                s.micEqOn = micEq;
            });
            c->tickForTests();
            c->reprepareForTests (44100.0, kBlock);
            c->tickForTests();
            return c;
        };
        freshDataDir();
        auto with = setup (true);
        auto without = setup (false);
        const auto tone = sine (2000.0, 1.0, 0.05f, 44100.0);
        const auto a = feed (with->getProcessorForTests(), tone), b = feed (without->getProcessorForTests(), tone);
        const int tail = 44100 / 4;
        const float gain = rmsDb (a.data() + a.size() - size_t (tail), tail) - rmsDb (b.data() + b.size() - size_t (tail), tail);
        expectWithinAbsoluteError (gain, 6.0f, 0.5f, "2 kHz at 44.1 kHz: " + juce::String (gain, 2) + " dB");
        expect (allFinite (a), "finite");
    }

    void modUiTests()
    {
        using namespace ui;
        beginTest ("声の大きさで動かす: the S-09 row sets SlotDef::mod through setSlotMod (target, depth, なし)");
        const bool wasOff = mainui::animationsOff();
        mainui::animationsOff() = true;
        freshDataDir();
        auto c = makeController ("character-demon-king");
        for (const int layout : { 0, 1, 2 }) // the three layouts share showSlotDetail
        {
            c->updateSettings ([layout] (Settings& s) { s.layoutStyle = layout; });
            c->loadPreset ("character-demon-king");
            MainComponent mc (*c);
            mc.setSize (Theme::minWidth, Theme::minHeight);
            mc.showSlotDetail (0);
            auto* combo = dynamic_cast<juce::ComboBox*> (mainui::findById (&mc, "slotDetail.modTarget"));
            auto* depth = dynamic_cast<juce::Slider*> (mainui::findById (&mc, "slotDetail.modDepth"));
            expect (combo != nullptr && depth != nullptr, "layout " + juce::String (layout) + ": the row is there");
            if (combo == nullptr || depth == nullptr) continue;
            const auto* info = findEffectInfo (c->getChain()[0].type);
            int numeric = 0;
            for (auto& spec : info->params) numeric += spec.isChoice() ? 0 : 1;
            expectEquals (combo->getNumItems(), 2 + numeric, "なし + かかり具合 + every numeric knob (no choices)");
            expect (! depth->isEnabled(), "depth is off while なし");
            combo->setSelectedId (2, juce::sendNotificationSync); // かかり具合
            expect (c->getChain()[0].modTarget == "wet" && std::abs (c->getChain()[0].modDepth - 0.5f) < 1.0e-4f, "wet at +50 % to start with");
            expect (depth->isEnabled() && c->isCurrentPresetModified(), "depth on, preset modified");
            depth->setValue (-30.0, juce::sendNotificationSync);
            expect (std::abs (c->getChain()[0].modDepth + 0.3f) < 1.0e-4f, "depth -30 %");
            if (numeric > 0)
            {
                combo->setSelectedId (3, juce::sendNotificationSync);
                std::string firstNumeric;
                for (auto& spec : info->params)
                    if (! spec.isChoice()) { firstNumeric = spec.id; break; }
                expect (c->getChain()[0].modTarget == firstNumeric, "a knob: " + juce::String (firstNumeric));
            }
            depth->setValue (5.0, juce::sendNotificationSync);
            expect (std::abs (depth->snapValue (5.0, juce::Slider::notDragging)) < 1.0e-9, "sticks at 0 near the middle");
            combo->setSelectedId (1, juce::sendNotificationSync);
            expect (c->getChain()[0].modTarget.empty() && c->getChain()[0].modDepth == 0.0f, "なし clears it");
        }
        c->updateSettings ([] (Settings& s) { s.layoutStyle = 0; });

        beginTest ("声の大きさで動かす: every effect's S-09 fits the narrow window; the row overlaps nothing; the meter follows the voice");
        for (auto& info : allEffectInfos())
        {
            if (! hasEffectFactory (info.type)) continue;
            c->loadPreset ("natural-asis");
            while (! c->getChain().empty()) c->removeSlot (0);
            juce::String why;
            if (! c->addEffect (info.type, why)) { logMessage (juce::String (info.type) + ": not added (" + why + ")"); continue; }
            MainComponent mc (*c);
            mc.setSize (Theme::minWidth, Theme::minHeight);
            mc.showSlotDetail (0);
            auto* panel = mainui::findById (&mc, "panel.slotDetail");
            auto* row = mainui::findById (&mc, "slotDetail.mod");
            if (panel == nullptr || row == nullptr) { expect (false, juce::String (info.type) + ": panel and row"); continue; }
            bool ok = panel->getLocalBounds().contains (row->getBounds()) && row->getBottom() <= panel->getHeight() - Theme::space2;
            for (auto* child : panel->getChildren())
                if (child != row && child->isVisible() && child->getBounds().intersects (row->getBounds())) ok = false;
            for (auto* child : row->getChildren())
                ok = ok && row->getLocalBounds().contains (child->getBounds()) && child->getWidth() > 40;
            expect (ok, juce::String (info.type) + ": row " + row->getBounds().toString() + " in panel " + panel->getBounds().toString());
        }
        c->loadPreset ("natural-asis");
        while (! c->getChain().empty()) c->removeSlot (0);
        juce::String why;
        c->addEffect ("tremolo", why);
        expect (c->setSlotMod (0, "wet", 1.0f), "mod on");
        feed (c->getProcessorForTests(), sine (200.0, 0.5, 0.3f));
        expect (c->getModLevel() > 0.5f, "loud voice: level " + juce::String (c->getModLevel(), 2));
        {
            MainComponent mc (*c);
            mc.setSize (Theme::minWidth, Theme::minHeight);
            mc.showSlotDetail (0);
            auto* meter = mainui::findById (&mc, "slotDetail.modMeter");
            expect (meter != nullptr && meter->isEnabled(), "meter on while a target is set");
        }
        mainui::animationsOff() = wasOff;
    }

    void toolTests()
    {
        using namespace ui;
        beginTest ("Tool: マイク補正 fits the narrow card; 測る / やめる / ON-OFF / 消す follow the state");
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
        tools->showTool (ToolsView::Tool::micEq);
        auto* tool = mainui::findById (&mc, "tools.miceq");
        auto* start = dynamic_cast<juce::Button*> (mainui::findById (&mc, "miceq.start"));
        auto* cancel = dynamic_cast<juce::Button*> (mainui::findById (&mc, "miceq.cancel"));
        auto* toggle = dynamic_cast<juce::Button*> (mainui::findById (&mc, "miceq.toggle"));
        auto* clear = dynamic_cast<juce::Button*> (mainui::findById (&mc, "miceq.clear"));
        expect (tool != nullptr && start != nullptr && cancel != nullptr && toggle != nullptr && clear != nullptr, "parts");
        if (tool == nullptr || start == nullptr || cancel == nullptr || toggle == nullptr || clear == nullptr) return;
        auto* timer = dynamic_cast<juce::Timer*> (tool);
        auto inside = [tool] (juce::Component* b) { return tool->getLocalBounds().contains (b->getBounds()); };
        expect (start->isVisible() && inside (start) && ! cancel->isVisible() && ! toggle->isVisible() && ! clear->isVisible(), "unmeasured: 測る only");
        start->onClick();
        expect (c->getMicEqState().phase == Phase::idle, "refused without a device");
        MicEqData::testDevicesRunning = true;
        start->onClick();
        expect (c->getMicEqState().phase == Phase::recording && cancel->isVisible() && inside (cancel) && ! start->isVisible(), "recording: やめる");
        cancel->onClick();
        expect (c->getMicEqState().phase == Phase::idle && start->isVisible(), "やめる -> idle");

        c->updateSettings ([] (Settings& s)
        {
            s.micEqGainsDb = { -2, -1, 0, 0, 1, 1, 0, 0, 2, 3, 2, 0, -1, -2 };
            s.micEqAt = juce::Time::getCurrentTime().toISO8601 (true);
            s.micEqOn = true;
        });
        if (timer != nullptr) timer->timerCallback();
        expect (toggle->isVisible() && clear->isVisible() && inside (toggle) && inside (clear) && toggle->getToggleState(), "measured: ON/OFF and 消す");
        expect (! clear->getBounds().intersects (toggle->getBounds()) && ! start->getBounds().intersects (clear->getBounds()), "no overlap");
        expect (start->getButtonText() == juce::String::fromUTF8 ("測り直す"), "測り直す");
        toggle->setToggleState (false, juce::dontSendNotification);
        toggle->onClick();
        expect (! c->getSettings().micEqOn, "OFF from the switch");
        juce::Image img (juce::Image::ARGB, tool->getWidth(), tool->getHeight(), true);
        juce::Graphics g (img);
        tool->paintEntireComponent (g, false);
        clear->onClick();
        expect (c->getSettings().micEqGainsDb.empty() && ! clear->isVisible() && ! toggle->isVisible(), "消す clears and hides");
        resetHooks();
        mainui::animationsOff() = wasOff;
    }
};

static VoiceTests voiceTests;
} // namespace koe

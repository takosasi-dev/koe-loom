// 声の見える化 (INTERFACES.md §12.3 / §12.4, owner wave10/viz). Category "Viz".
// No window, no audio device: AppController (false), audio pushed through getProcessorForTests(), ticks by tickForTests().
// Noise suppression and the gate are off where a steady tone must come through unchanged.

#include "App/AppController.h"
#include "Core/Paths.h"
#include "Effects/EffectRegistry.h"
#include "Engine/Spectrum.h"
#include "Tests/TestUtil.h"
#include "UI/MainComponent.h"
#include "UI/SpectrumView.h"
#include "UI/main/Common.h"
#include "UI/main/Panels.h"
#include "UI/main/ToolsView.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace koe
{
namespace
{
using namespace test;
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
    const auto& s = c->getSettings();
    c->setNoiseSuppression (false, s.noiseMix); // a steady tone is "noise" to it
    c->setGate (false, s.gateThresholdDb, s.gateAttackMs, s.gateHoldMs, s.gateReleaseMs);
    c->setVoiceChangerOn (true);
    c->loadPreset (preset);
    c->dispatchPendingMessages();
    return c;
}

struct Reading
{
    bool ok = false;
    SpectrumBands in {}, out {};
};

/** Feeds `signal` (from `start`, one UI frame at a time) and polls after each frame; the last reading. */
Reading pollWhileFeeding (AppController& c, const std::vector<float>& signal, int frames, size_t start = 0)
{
    Reading r;
    std::vector<float> out (kPollSamples);
    for (int f = 0; f < frames; ++f)
    {
        const size_t pos = (start + size_t (f) * kPollSamples) % (signal.size() - kPollSamples);
        c.getProcessorForTests().process (signal.data() + pos, out.data(), nullptr, kPollSamples);
        r.ok = c.pollSpectrum (r.in, r.out);
    }
    return r;
}

void feed (VoiceProcessor& vp, const std::vector<float>& in, std::vector<float>* outAll = nullptr)
{
    std::vector<float> out (kBlock);
    for (size_t pos = 0; pos + kBlock <= in.size(); pos += kBlock)
    {
        vp.process (in.data() + pos, out.data(), nullptr, kBlock);
        if (outAll != nullptr) outAll->insert (outAll->end(), out.begin(), out.end());
    }
}

int peakBand (const SpectrumBands& b) { return int (std::max_element (b.begin(), b.end()) - b.begin()); }
float peakDb (const SpectrumBands& b) { return *std::max_element (b.begin(), b.end()); }
bool finite (const SpectrumBands& b)
{
    return std::all_of (b.begin(), b.end(), [] (float v) { return std::isfinite (v); });
}
float amplitudeDb (float a) { return 20.0f * std::log10 (a); }
} // namespace

class VizTests : public juce::UnitTest
{
public:
    VizTests() : juce::UnitTest ("Voice spectrum view (wave10/viz)", "Viz") {}

    void runTest() override
    {
        analyserTests();
        controllerTests();
        tapTests();
        uiTests();
        VizData::testSpectra = nullptr;
    }

private:
    void analyserTests()
    {
        beginTest ("Bands: log-spaced kSpectrumLowHz..kSpectrumHighHz, bandFor() matches bandCentreHz(), window ~85 ms");
        expectWithinAbsoluteError (SpectrumAnalyser::bandCentreHz (0), kSpectrumLowHz * std::pow (kSpectrumHighHz / kSpectrumLowHz, 0.5f / kSpectrumBands), 0.01f);
        for (int b = 0; b < kSpectrumBands; ++b) expectEquals (SpectrumAnalyser::bandFor (SpectrumAnalyser::bandCentreHz (b)), b);
        expectEquals (SpectrumAnalyser::bandFor (kSpectrumLowHz), 0);
        expectEquals (SpectrumAnalyser::bandFor (kSpectrumLowHz * 0.9f), -1);
        expectEquals (SpectrumAnalyser::bandFor (kSpectrumHighHz), -1);
        expectEquals (SpectrumAnalyser::windowSize (48000.0), 4096);
        expectEquals (SpectrumAnalyser::windowSize (44100.0), 4096);
        expectEquals (SpectrumAnalyser::windowSize (96000.0), 8192);
        expectEquals (SpectrumAnalyser::windowSize (192000.0), 16384);

        beginTest ("measure(): a sine peaks in its band (+-1) at its level (+-2 dB) at 44.1 / 48 / 96 kHz; an octave away is 40 dB down");
        for (const double rate : { 44100.0, 48000.0, 96000.0 })
            for (const double hz : { 60.0, 100.0, 440.0, 1000.0, 5000.0, 12000.0 })
            {
                SpectrumAnalyser a;
                SpectrumBands db;
                const auto x = sine (hz, 0.3, 0.25f, rate);
                expect (a.measure (x.data(), int (x.size()), rate, db), "measured");
                const int want = SpectrumAnalyser::bandFor (float (hz));
                const int got = peakBand (db);
                const auto what = juce::String (hz) + " Hz at " + juce::String (rate) + ": band " + juce::String (got) + " (want " + juce::String (want)
                                  + "), " + juce::String (peakDb (db), 1) + " dB";
                expect (std::abs (got - want) <= 1, what);
                expectWithinAbsoluteError (peakDb (db), amplitudeDb (0.25f), 2.0f, what);
                for (const int far : { want - 12, want + 12 })
                    if (far >= 0 && far < kSpectrumBands) expect (db[size_t (far)] < peakDb (db) - 40.0f, what + ", an octave away " + juce::String (db[size_t (far)], 1));
                expect (finite (db), what + " finite");
            }
        {
            SpectrumAnalyser a;
            SpectrumBands db;
            const auto x = sine (1000.0, 0.3, 0.25f, 22050.0);
            a.measure (x.data(), int (x.size()), 22050.0, db);
            expectEquals (db[size_t (SpectrumAnalyser::bandFor (14000.0f))], SpectrumAnalyser::kFloorDb, "above Nyquist (22.05 kHz): floor");
            const std::vector<float> zeros (8192, 0.0f);
            a.measure (zeros.data(), int (zeros.size()), 48000.0, db);
            expectEquals (peakDb (db), SpectrumAnalyser::kFloorDb, "silence: floor everywhere");
            const std::vector<float> shortOne (100, 0.5f);
            expect (! a.measure (shortOne.data(), int (shortOne.size()), 48000.0, db), "shorter than a window: refused");
        }

        beginTest ("poll(): fast up, slow down; no data until a window arrived and ~0.5 s after the samples stop");
        {
            TapRing ring;
            SpectrumAnalyser a;
            SpectrumBands db;
            const auto loud = sine (1000.0, 0.2, 0.5f);
            const auto quiet = silence (0.2);
            const auto band = size_t (SpectrumAnalyser::bandFor (1000.0f));
            ring.push (loud.data(), 2000);
            expect (! a.poll (ring, kSr, db), "half a window: no data");
            expectEquals (db[band], SpectrumAnalyser::kFloorDb);
            ring.push (loud.data() + 2000, 4096);
            expect (a.poll (ring, kSr, db), "a full window: data");
            const float top = db[band];
            expectWithinAbsoluteError (top, amplitudeDb (0.5f), 2.0f, "the first frame is taken as is");
            for (int i = 0; i < 4; ++i)
            {
                ring.push (quiet.data(), 4096);
                a.poll (ring, kSr, db);
            }
            expect (db[band] > top - 60.0f, "4 silent frames: still on the way down (" + juce::String (db[band], 1) + " dB)");
            for (int i = 0; i < 60; ++i)
            {
                ring.push (quiet.data(), 4096);
                a.poll (ring, kSr, db);
            }
            expect (db[band] < -100.0f, "2 s of silence: down (" + juce::String (db[band], 1) + " dB)");
            for (int i = 0; i < 4; ++i)
            {
                ring.push (loud.data(), 4096);
                a.poll (ring, kSr, db);
            }
            expect (db[band] > top - 3.0f, "4 loud frames: up again (" + juce::String (db[band], 1) + " dB)");
            for (int i = 0; i < SpectrumAnalyser::kStalePolls; ++i) expect (a.poll (ring, kSr, db), "no new samples, still held");
            expect (! a.poll (ring, kSr, db), "no new samples for kStalePolls polls: no data");
            ring.push (loud.data(), 1600);
            expect (a.poll (ring, kSr, db), "samples again: data at once");
            ring.clear();
            expect (! a.poll (ring, kSr, db), "the ring cleared: no data");
        }

        beginTest ("measure() / demoSpectra(): finite with NaN / Inf in the input; the demo is finite and inside the scale");
        {
            SpectrumAnalyser a;
            SpectrumBands db;
            auto x = sine (300.0, 0.2, 0.3f);
            for (size_t i = 0; i < x.size(); i += 7) x[i] = (i % 2 == 0) ? std::numeric_limits<float>::quiet_NaN() : std::numeric_limits<float>::infinity();
            a.measure (x.data(), int (x.size()), kSr, db);
            expect (finite (db), "finite");
            SpectrumBands in, out;
            SpectrumAnalyser::demoSpectra (in, out);
            expect (finite (in) && finite (out), "demo finite");
            expect (peakDb (in) < 0.0f && peakDb (in) > -60.0f && peakDb (out) < 0.0f && peakDb (out) > -60.0f, "demo peaks inside the scale");
            expect (peakBand (out) != peakBand (in) || peakDb (out) != peakDb (in), "the demo's two curves differ");
        }
    }

    void controllerTests()
    {
        beginTest ("pollSpectrum(): a sine through そのまま peaks in its band (+-1) in the input and the output");
        freshDataDir();
        auto c = makeController();
        SpectrumBands in, out;
        expect (! c->pollSpectrum (in, out), "no sound yet: false");
        expectEquals (peakDb (in), SpectrumAnalyser::kFloorDb, "no sound yet: floor");
        for (const double hz : { 100.0, 250.0, 1000.0, 3150.0, 8000.0 })
        {
            const auto tone = sine (hz, 2.0, 0.3f);
            const auto r = pollWhileFeeding (*c, tone, 40);
            const int want = SpectrumAnalyser::bandFor (float (hz));
            const auto what = juce::String (hz) + " Hz: in band " + juce::String (peakBand (r.in)) + " " + juce::String (peakDb (r.in), 1)
                              + " dB, out band " + juce::String (peakBand (r.out)) + " " + juce::String (peakDb (r.out), 1) + " dB (want " + juce::String (want) + ")";
            logMessage (what);
            expect (r.ok, "data");
            expect (std::abs (peakBand (r.in) - want) <= 1, what);
            expect (std::abs (peakBand (r.out) - want) <= 1, what);
            expectWithinAbsoluteError (peakDb (r.in), amplitudeDb (0.3f), 2.0f, what);
        }

        beginTest ("Input and output come from their own taps: the input gain moves only the output; 魔王 lowers only the output");
        {
            const auto tone = sine (1000.0, 3.0, 0.3f);
            const auto base = pollWhileFeeding (*c, tone, 40);
            const auto band = size_t (SpectrumAnalyser::bandFor (1000.0f));
            c->setInputGainDb (-12.0f); // after the input tap, before everything else
            const auto quieter = pollWhileFeeding (*c, tone, 40, 40 * kPollSamples);
            const float dIn = quieter.in[band] - base.in[band], dOut = quieter.out[band] - base.out[band];
            logMessage ("input gain -12 dB: input " + juce::String (dIn, 2) + " dB, output " + juce::String (dOut, 2) + " dB");
            expectWithinAbsoluteError (dIn, 0.0f, 0.5f, "the input stays");
            expectWithinAbsoluteError (dOut, -12.0f, 1.5f, "the output follows the gain");
            c->setInputGainDb (0.0f);

            c->loadPreset ("character-demon-king");
            c->dispatchPendingMessages();
            const auto voice = sine (220.0, 3.0, 0.3f);
            const auto r = pollWhileFeeding (*c, voice, 60);
            const int want = SpectrumAnalyser::bandFor (220.0f);
            logMessage ("魔王 on 220 Hz: input band " + juce::String (peakBand (r.in)) + ", output band " + juce::String (peakBand (r.out))
                        + " (" + juce::String (SpectrumAnalyser::bandCentreHz (peakBand (r.out)), 0) + " Hz)");
            expect (std::abs (peakBand (r.in) - want) <= 1, "the input still peaks at 220 Hz");
            expect (peakBand (r.out) <= want - 4, "the output peaks lower");
        }

        beginTest ("NaN / Inf from the device: the spectra stay finite");
        {
            c->loadPreset ("natural-asis");
            c->dispatchPendingMessages();
            auto bad = sine (500.0, 2.0, 0.3f);
            for (size_t i = 0; i < bad.size(); i += 5) bad[i] = (i % 2 == 0) ? std::numeric_limits<float>::quiet_NaN() : -std::numeric_limits<float>::infinity();
            const auto r = pollWhileFeeding (*c, bad, 30);
            expect (r.ok, "data");
            expect (finite (r.in) && finite (r.out), "finite");
        }

        beginTest ("The device stops (no new samples): data for ~0.5 s, then false");
        {
            const auto tone = sine (500.0, 2.0, 0.3f);
            pollWhileFeeding (*c, tone, 10);
            bool all = true;
            for (int i = 0; i < SpectrumAnalyser::kStalePolls; ++i) all = c->pollSpectrum (in, out) && all;
            expect (all, "held for kStalePolls polls");
            expect (! c->pollSpectrum (in, out), "then no data");
        }
    }

    void tapTests()
    {
        beginTest ("The taps come off ~1 s after the last poll and go back on with the next one");
        {
            freshDataDir();
            auto c = makeController();
            const auto tone = sine (440.0, 2.0, 0.3f);
            SpectrumBands in, out;
            auto r = pollWhileFeeding (*c, tone, 10);
            expect (r.ok, "data while polled");
            for (int i = 0; i < 20; ++i) c->tickForTests();
            feed (c->getProcessorForTests(), std::vector<float> (tone.begin(), tone.begin() + 3 * kPollSamples));
            expect (c->pollSpectrum (in, out), "20 ticks (< 1 s): still attached, the audio fed meanwhile arrived");
            for (int i = 0; i < 31; ++i) c->tickForTests();
            feed (c->getProcessorForTests(), std::vector<float> (tone.begin(), tone.begin() + 3 * kPollSamples));
            expect (! c->pollSpectrum (in, out), "31 ticks: detached, the audio fed meanwhile was not kept (re-attached empty)");
            r = pollWhileFeeding (*c, tone, 10);
            expect (r.ok && std::abs (peakBand (r.in) - SpectrumAnalyser::bandFor (440.0f)) <= 1, "data again after re-attaching");
        }

        beginTest ("Taps attached: no allocation on the audio thread, the virtual mic bit-identical");
        {
            const auto voice = synthVoice (3.0);
            std::vector<float> plain, tapped;
            {
                freshDataDir();
                auto c = makeController ("character-demon-king");
                feed (c->getProcessorForTests(), voice, &plain);
            }
            freshDataDir();
            auto c2 = makeController ("character-demon-king");
            SpectrumBands in, out;
            c2->pollSpectrum (in, out); // attaches
            tapped.assign (voice.size(), 0.0f);
            {
                AllocationCounter counter;
                for (size_t pos = 0; pos + kBlock <= voice.size(); pos += kBlock)
                    c2->getProcessorForTests().process (voice.data() + pos, tapped.data() + pos, nullptr, kBlock);
                const auto allocations = counter.count(); // before building the message string
                expectEquals (allocations, 0LL, "no allocation with the taps on");
            }
            tapped.resize (plain.size());
            float diff = 0.0f;
            for (size_t i = 0; i < std::min (plain.size(), tapped.size()); ++i) diff = std::max (diff, std::abs (plain[i] - tapped[i]));
            expectEquals (diff, 0.0f, "same output sample for sample");
            expect (allFinite (tapped), "finite");
            c2->pollSpectrum (in, out);
            expect (finite (in) && finite (out), "spectra finite");
        }
    }

    void uiTests()
    {
        using namespace ui;
        const bool wasOff = mainui::animationsOff();
        mainui::animationsOff() = true;

        beginTest ("Tool: fits the narrow card, polls only while shown, says when the device is not running, draws the spectra");
        {
            freshDataDir();
            auto c = makeController();
            MainComponent mc (*c);
            mc.setSize (Theme::minWidth, Theme::minHeight);
            mc.showPage (Navigator::Page::tools);
            auto* tools = dynamic_cast<ToolsView*> (mainui::findById (&mc, "page.tools"));
            expect (tools != nullptr, "tools page");
            if (tools == nullptr) return;
            tools->showTool (ToolsView::Tool::spectrum);
            auto* tool = mainui::findById (&mc, "tools.spectrum");
            auto* view = dynamic_cast<SpectrumView*> (mainui::findById (&mc, "spectrum.view"));
            expect (tool != nullptr && view != nullptr, "tool and view");
            if (tool == nullptr || view == nullptr) return;
            expect (tool->isVisible() && view->isVisible(), "shown");
            expect (tool->getLocalBounds().contains (view->getBounds()), "the view inside the tool " + view->getBounds().toString());
            expect (view->getWidth() >= 400 && view->getHeight() >= 200, "big enough in 800x560: " + view->getBounds().toString());
            expect (view->isTimerRunning(), "polls while shown");
            expect (! view->hasData(), "no sound: no data");
            expectEquals (view->statusText(), ja ("入力デバイスが動いていません"));
            {
                juce::Image img (juce::Image::ARGB, tool->getWidth(), tool->getHeight(), true);
                juce::Graphics g (img);
                tool->paintEntireComponent (g, false);
            }
            const auto tone = sine (1000.0, 1.0, 0.3f);
            std::vector<float> out (kPollSamples);
            for (int i = 0; i < 5; ++i)
            {
                c->getProcessorForTests().process (tone.data() + i * kPollSamples, out.data(), nullptr, kPollSamples);
                view->timerCallback();
            }
            expect (view->hasData() && view->statusText().isEmpty(), "data after sound");
            expect (std::abs (peakBand (view->inputDb()) - SpectrumAnalyser::bandFor (1000.0f)) <= 1, "the view shows the input peak");
            {
                juce::Image img (juce::Image::ARGB, tool->getWidth(), tool->getHeight(), true);
                juce::Graphics g (img);
                tool->paintEntireComponent (g, false); // the curves draw without throwing; the snapshot shows them
            }
            tools->showTool (ToolsView::Tool::pitch);
            expect (! view->isTimerRunning(), "another tool shown: the timer stops");
            tools->showTool (ToolsView::Tool::spectrum);
            expect (view->isTimerRunning(), "back: polls again");

            VizData::Spectra fake;
            SpectrumAnalyser::demoSpectra (fake.in, fake.out);
            VizData::testSpectra = &fake;
            view->timerCallback();
            expect (view->hasData() && view->inputDb() == fake.in && view->outputDb() == fake.out, "the snapshot hook feeds the view");
            VizData::testSpectra = nullptr;
        }

        beginTest ("Detail panel strip: above 声の大きさで動かす, under every control, and the panel fits 800x560 for every effect type");
        {
            freshDataDir();
            auto c = makeController();
            int tallest = 0;
            juce::String tallestType;
            for (const auto& info : allEffectInfos())
            {
                c->loadPreset ("natural-asis");
                c->dispatchPendingMessages();
                juce::String why;
                if (! c->addEffect (info.type, why))
                {
                    expect (false, juce::String (info.type) + " could not be added: " + why);
                    continue;
                }
                c->dispatchPendingMessages();
                const int slot = int (c->getChain().size()) - 1;
                MainComponent mc (*c);
                mc.setSize (Theme::minWidth, Theme::minHeight);
                mc.showSlotDetail (slot);
                auto* panel = dynamic_cast<mainui::SlotDetailPanel*> (mainui::findById (&mc, "panel.slotDetail"));
                auto* strip = dynamic_cast<SpectrumView*> (panel != nullptr ? mainui::findById (panel, "slotDetail.spectrum") : nullptr);
                const juce::String type (info.type);
                expect (panel != nullptr && strip != nullptr, type + ": panel with a strip");
                if (panel == nullptr || strip == nullptr) continue;
                const auto pb = mc.getLocalArea (panel, panel->getLocalBounds());
                if (pb.getHeight() > tallest) { tallest = pb.getHeight(); tallestType = type; }
                expect (pb.getY() >= 0 && pb.getBottom() <= mc.getHeight() && pb.getRight() <= mc.getWidth(), type + ": fits 800x560 " + pb.toString());
                expect (strip->isVisible() && strip->getHeight() == kSpectrumStripH && panel->getLocalBounds().contains (strip->getBounds()),
                        type + ": strip " + strip->getBounds().toString());
                expect (strip->isTimerRunning(), type + ": the strip polls while open");
                if (auto* mod = mainui::findById (panel, "slotDetail.modTarget"))
                    expect (panel->getLocalArea (mod, mod->getLocalBounds()).getY() >= strip->getBottom(), type + ": above 声の大きさで動かす");
                for (int p = 0; p < int (info.params.size()); ++p)
                    if (auto* ctl = panel->controlFor (p))
                        expect (panel->getLocalArea (ctl, ctl->getLocalBounds()).getBottom() <= strip->getY(),
                                type + ": control " + juce::String (p) + " above the strip");
            }
            logMessage ("tallest detail panel with the strip: " + tallestType + " " + juce::String (tallest) + " px (window 560)");

            for (const int layout : { 1, 2 }) // B Paper / C Mono share the panel
            {
                c->updateSettings ([layout] (Settings& s) { s.layoutStyle = layout; });
                c->loadPreset ("character-demon-king");
                c->dispatchPendingMessages();
                MainComponent mc (*c);
                mc.setSize (Theme::minWidth, Theme::minHeight);
                mc.showSlotDetail (0);
                auto* panel = mainui::findById (&mc, "panel.slotDetail");
                auto* strip = panel != nullptr ? mainui::findById (panel, "slotDetail.spectrum") : nullptr;
                expect (panel != nullptr && strip != nullptr && strip->isVisible(), "layout " + juce::String (layout) + ": strip");
                if (panel != nullptr) expect (mc.getLocalArea (panel, panel->getLocalBounds()).getBottom() <= mc.getHeight(), "layout " + juce::String (layout) + ": fits");
            }
            c->updateSettings ([] (Settings& s) { s.layoutStyle = 0; });
        }
        mainui::animationsOff() = wasOff;
    }
};

static VizTests vizTests;
} // namespace koe

// Cross-module checks (lead). Category "Integration".

#include "App/AppController.h"
#include "Effects/EffectRegistry.h"
#include "Engine/Watchdog.h"
#include "Model/PresetLibrary.h"
#include "Tests/TestUtil.h"
#include "Tools/Calibrate.h"
#include "UI/Theme.h"

#include "Core/Paths.h"

namespace koe
{
namespace
{
using namespace test;
constexpr int kBlock = 480;

/** Each AppController test starts from an empty data dir (the runner points KOELOOM_DATA_DIR at a temp folder). */
void freshDataDir()
{
    auto d = paths::dataDir();
    if (d.getFullPathName().contains ("KoeLoomTests")) d.deleteRecursively();
    d.createDirectory();
}

void renderBlocks (VoiceProcessor& vp, const std::vector<float>& in, std::vector<float>& out, size_t& pos, int blocks, long long* allocs = nullptr)
{
    for (int b = 0; b < blocks && pos + kBlock <= in.size(); ++b, pos += kBlock)
    {
        AllocationCounter counter;
        vp.process (in.data() + pos, out.data() + pos, nullptr, kBlock);
        if (allocs != nullptr) *allocs += counter.count();
    }
}
} // namespace

class IntegrationTests : public juce::UnitTest
{
public:
    IntegrationTests() : juce::UnitTest ("Integration", "Integration") {}

    void runTest() override
    {
        beginTest ("AC-44: every Phase 2 effect type has an implementation");
        {
            int phase2 = 0;
            for (auto& i : allEffectInfos())
                if (i.phase == 2)
                {
                    ++phase2;
                    expect (hasEffectFactory (i.type), juce::String ("missing ") + i.type);
                }
            expectEquals (phase2, 24);
        }

        beginTest ("AC-10: built-in presets within +-2 dB of そのまま (reference speech or synthetic)");
        {
            bool real = false;
            const auto speech = referenceSpeechOrSynth (&real);
            auto input = speech;
            input.resize (input.size() + size_t (kSr), 0.0f);
            PresetLibrary lib (paths::presetsDir());
            lib.reload();
            const auto* asis = lib.find ("natural-asis");
            expect (asis != nullptr);
            int lat = 0;
            const auto ref = tools::renderPreset (*asis, input, lat);
            const float refDb = tools::measureLevelDb (speech, ref, lat);
            int checked = 0;
            for (auto& p : lib.all())
            {
                if (! p.builtin) continue;
                bool complete = true; // presets using effects that are not implemented yet are skipped
                for (auto& s : p.chain) complete = complete && hasEffectFactory (s.type);
                if (! complete) continue;
                int l = 0;
                const auto out = tools::renderPreset (p, input, l);
                expect (allFinite (out), juce::String (p.id) + " non-finite");
                const float d = tools::measureLevelDb (speech, out, l) - refDb;
                expectLessOrEqual (std::abs (d), 2.0f, juce::String (p.id) + " level " + juce::String (d, 2) + " dB" + (real ? "" : " (synthetic voice)"));
                ++checked;
            }
            logMessage ("  checked " + juce::String (checked) + " presets with " + (real ? "the reference speech" : "the synthetic voice"));
        }

        for (const char* startPreset : { "natural-asis", "character-helium" })
        {
            beginTest (juce::String ("AC-39: add / remove / move / toggle real effects 20x each from ") + startPreset + ": click-free, no audio-thread allocation, 11th slot refused");
            freshDataDir();
            AppController c (false);
            c.startup();
            c.loadPreset (startPreset);
            auto& vp = c.getProcessorForTests();
            const auto in = synthVoice (60.0, 31, kSr, false);
            std::vector<float> out (in.size());
            size_t pos = 0;
            long long allocs = 0;
            renderBlocks (vp, in, out, pos, 100, &allocs);
            const char* types[] = { "eq", "distortion", "echo", "reverb", "modulation" };
            std::vector<std::pair<int, int>> ops; // sample position of each operation
            juce::StringArray labels;
            juce::String why;
            auto op = [&] (std::function<void()> f, const juce::String& label)
            {
                f();
                juce::String chainText;
                for (auto& s : c.getChain()) chainText << s.type << (s.enabled ? "" : "(off)") << " ";
                labels.add (label + " -> " + chainText);
                ops.push_back ({ int (pos), int (pos) + int (kChainSwapFadeMs * 0.001 * kSr) });
                renderBlocks (vp, in, out, pos, 25, &allocs);
                vp.collectGarbage();
            };
            for (int r = 0; r < 20; ++r)
            {
                op ([&] { c.addEffect (types[r % 5], why); }, "add");
                if (c.getChain().size() > 3) op ([&] { c.removeSlot (0); }, "remove");
                if (c.getChain().size() > 1) op ([&] { c.moveSlot (0, int (c.getChain().size()) - 1); }, "move");
                if (! c.getChain().empty()) op ([&] { c.setSlotEnabled (0, ! c.getChain()[0].enabled, why); }, "toggle");
            }
            expectEquals (allocs, 0LL);
            double worst = 0.0;
            for (size_t k = 0; k < ops.size(); ++k)
            {
                auto [a, b] = ops[k];
                if (b + int (0.15 * kSr) >= int (pos)) continue;
                const double r = clickRatio (out, a, b);
                if (r > 2.0) logMessage ("  op " + juce::String (int (k)) + " " + labels[int (k)] + ": " + juce::String (r, 2));
                worst = std::max (worst, r);
            }
            logMessage ("  worst click ratio over " + juce::String (int (ops.size())) + " operations: " + juce::String (worst, 2));
            expectLessOrEqual (worst, 2.0);

            while (c.getChain().size() < size_t (kMaxSlots)) c.addEffect ("eq", why);
            expect (! c.addEffect ("eq", why));
            expect (why.contains ("10"));
            c.shutdown();
        }

        beginTest ("AC-50: 10 slots of mixed effects, 60 s: finite, peak <= -1 dBFS");
        {
            freshDataDir();
            AppController c (false);
            c.startup();
            juce::String why;
            for (const char* t : { "compressor", "distortion", "ringmod", "modulation", "phaser", "ensemble", "echo", "reverb", "bitcrusher", "voicechar" })
                expect (c.addEffect (t, why), why);
            c.setOutputGainDb (12.0f);
            c.setPitch (5.0f);
            juce::String w2;
            c.addLayer (w2);
            c.addLayer (w2);
            auto& vp = c.getProcessorForTests();
            vp.collectGarbage();
            auto in = concat ({ synthVoice (40.0, 5), whiteNoise (10.0, 0.5f, 3), sine (220.0, 10.0, 0.9f) });
            std::vector<float> out (in.size());
            size_t pos = 0;
            renderBlocks (vp, in, out, pos, int (in.size() / kBlock));
            expect (allFinite (out));
            expectLessOrEqual (peakDb (out), -1.0f + 1.0e-4f);
            c.shutdown();
        }

        beginTest ("AC-12 / §5.6: watchdog stops layers after 1 s over 80 %, then heavy slots, then only notifies");
        {
            Watchdog w;
            const double blk = 0.01;
            int acted = -1;
            for (int i = 0; i < 99; ++i) expect (w.update (0.9, blk, true, true) == Watchdog::Action::none);
            expect (w.update (0.9, blk, true, true) == Watchdog::Action::stopLayers);
            for (int i = 0; i < 99; ++i) expect (w.update (0.9, blk, false, true) == Watchdog::Action::none);
            expect (w.update (0.9, blk, false, true) == Watchdog::Action::stopHeavySlot);
            for (int i = 0; i < 100; ++i) if (w.update (0.9, blk, false, false) == Watchdog::Action::notifyOnly) acted = i;
            expectEquals (acted, 99);
            for (int i = 0; i < 50; ++i) w.update (0.9, blk, true, true);
            expect (w.update (0.5, blk, true, true) == Watchdog::Action::none); // dropping below resets
            for (int i = 0; i < 99; ++i) expect (w.update (0.9, blk, true, true) == Watchdog::Action::none);

            // processor side: auto-stopped layers fade out and stay off until resumed
            VoiceProcessor vp;
            vp.setNoiseSuppression (false, 1);
            vp.setGate (false, -45, 5, 80, 120);
            vp.setShifterFactory ([] { return createIdentityShifter (256); });
            vp.prepare (kSr, kBlock);
            vp.setShifter (true, 0, 0);
            VoiceProcessor::LayerParams lp;
            lp.active = true;
            lp.levelDb = 0;
            vp.setLayer (0, lp);
            auto in = sine (300.0, 2.0, 0.2f);
            std::vector<float> out (in.size());
            size_t pos = 0;
            renderBlocks (vp, in, out, pos, 50);
            expect (vp.anyLayerRunning());
            vp.autoStopLayers();
            renderBlocks (vp, in, out, pos, 20);
            expect (! vp.anyLayerRunning());
            expect (vp.areLayersAutoStopped());
            vp.clearLayerAutoStop();
            renderBlocks (vp, in, out, pos, 20);
            expect (vp.anyLayerRunning());
        }

        beginTest ("AppController rules: favourites <= 9, layers <= 2, output gain warning, duplicate hotkeys, modified flag");
        {
            freshDataDir();
            AppController c (false);
            c.startup();
            juce::String why;
            int added = 0;
            for (auto& p : c.getPresetLibrary().all())
                if (c.toggleFavorite (p.id, why)) ++added;
                else break;
            expectEquals (added, kMaxFavorites);
            expect (why.isNotEmpty());

            expect (c.addLayer (why));
            expect (c.addLayer (why));
            expect (! c.addLayer (why));

            c.setOutputGainDb (6.5f);
            bool warned = false;
            for (auto& n : c.getNotices()) warned = warned || n.key == "output.gain";
            expect (warned);
            c.setOutputGainDb (6.0f);
            warned = false;
            for (auto& n : c.getNotices()) warned = warned || n.key == "output.gain";
            expect (! warned);
            c.setOutputGainDb (3.3f);
            expectEquals (c.getSettings().outputGainDb, 3.5f);

            expect (c.setHotkey ("voiceToggle", 2, 0x70, why));
            expect (! c.setHotkey ("muteToggle", 2, 0x70, why));
            expect (why.isNotEmpty());

            c.loadPreset ("character-helium");
            expect (! c.isCurrentPresetModified());
            c.setPitch (3.24f);
            expect (c.isCurrentPresetModified());
            expectWithinAbsoluteError (c.getPitch(), 3.2f, 1.0e-5f);
            c.shutdown();
        }

        beginTest ("AppController: converter ON/OFF and per-voice ON survive a preset save -> load");
        {
            freshDataDir();
            AppController c (false);
            c.startup();
            juce::String why;
            expect (c.addLayer (why));
            c.setLayerEnabled (0, false);
            expect (! c.getLayer (0).enabled);
            PresetLoadReport report;
            auto back = parsePreset (serializePreset (c.getCurrentPreset()), report);
            expect (back.has_value() && back->layers.size() == 1 && ! back->layers[0].enabled);

            c.setShifterEnabled (false);
            expect (! c.hasShifter());
            expectEquals (c.getNumLayers(), 1); // kept in the working preset, silent while OFF
            back = parsePreset (serializePreset (c.getCurrentPreset()), report);
            expect (back.has_value() && ! back->hasShifter && back->layers.empty()); // E-30
            c.setPitch (2.0f);
            expect (c.hasShifter());
            c.shutdown();
        }

        beginTest ("AppController: a rename follows into the working preset, a delete leaves the favourites");
        {
            freshDataDir();
            AppController c (false);
            c.startup();
            juce::String err;
            std::string id;
            expect (c.duplicatePreset ("character-helium", id, err));
            c.loadPreset (id);
            expect (c.toggleFavorite (id, err));
            const auto name = juce::String::fromUTF8 ("テスト声");
            expect (c.renamePreset (id, name, err));
            expect (c.getCurrentPreset().name == name);
            expect (c.removePreset (id, err));
            expect (! c.isFavorite (id));
            expect (! c.overwriteCurrent (err));
            expect (err.contains (juce::String::fromUTF8 ("削除")));
            c.shutdown();
        }

        beginTest ("AC-53 (automatic part): theme contrast, dark and light");
        for (bool dark : { true, false })
        {
            const auto& p = dark ? ui::Theme::dark() : ui::Theme::light();
            for (auto bgc : { p.bg, p.surface, p.raised })
            {
                expectGreaterOrEqual (ui::contrastRatio (p.text, bgc), 4.5);
                expectGreaterOrEqual (ui::contrastRatio (p.textSub, bgc), 4.5);
                expectGreaterOrEqual (ui::contrastRatio (p.border, bgc), 3.0);
            }
            for (auto status : { p.ok, p.warn, p.danger, p.accent })
                expectGreaterOrEqual (ui::contrastRatio (status, p.surface), 3.0);
            for (auto status : { p.ok, p.warn, p.danger })
                expectGreaterOrEqual (ui::contrastRatio (status, p.surface), 4.5, dark ? "dark status text" : "light status text");
            expectGreaterOrEqual (ui::contrastRatio (p.onAccent, p.accent), 4.5);
            expectGreaterOrEqual (ui::contrastRatio (p.onAccent, p.danger), 4.5);
        }
    }
};

static IntegrationTests integrationTests;
} // namespace koe

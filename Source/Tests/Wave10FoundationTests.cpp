// Wave 10 foundation (INTERFACES.md §12.1, lead): knob style setting, slot duplicate / reset, the tapestop trigger and
// hotkey, the extra taps and the edit hooks' defaults. Category "Engine". No window, no audio device: AppController (false).

#include "App/AppController.h"
#include "Core/Paths.h"
#include "Effects/EffectRegistry.h"
#include "Model/Settings.h"
#include "Platform/Hotkeys.h"
#include "Tests/TestUtil.h"

namespace koe
{
namespace
{
void freshDataDir()
{
    auto d = paths::dataDir();
    if (d.getFullPathName().contains ("KoeLoomTests")) d.deleteRecursively();
    d.createDirectory();
}

std::unique_ptr<AppController> makeController()
{
    auto c = std::make_unique<AppController> (false);
    c->startup();
    c->updateSettings ([] (Settings& s)
    {
        s.setupDone = true;
        s.tourStep = 7;
    });
    c->loadPreset ("character-demon-king"); // 魔王: 4 slots (distortion, ringmod, echo, reverb)
    c->dispatchPendingMessages();
    return c;
}
} // namespace

class Wave10FoundationTests : public juce::UnitTest
{
public:
    Wave10FoundationTests() : juce::UnitTest ("Wave 10 foundation", "Engine") {}

    void runTest() override
    {
        beginTest ("settings: knobStyle round-trips, out of range goes back to the knob");
        {
            Settings s;
            expectEquals (s.knobStyle, 0);
            s.knobStyle = 1;
            auto f = juce::File::createTempFile (".json");
            expect (saveSettings (s, f));
            SettingsLoadResult lr;
            expectEquals (loadSettings (f, lr).knobStyle, 1);
            f.deleteFile();
            s.knobStyle = 7;
            expectEquals (clampSettings (s).knobStyle, 0);
        }

        beginTest ("taps: 6 per point (input 4 and output 2 for 声の見える化)");
        expectEquals (VoiceProcessor::kTapsPerPoint, 6);

        beginTest ("hotkeys: tapeStopToggle exists with a Japanese label");
        {
            expect (Hotkeys::allActions().contains ("tapeStopToggle"));
            expect (Hotkeys::actionLabel ("tapeStopToggle") != "tapeStopToggle");
        }

        freshDataDir();
        auto c = makeController();
        const auto chain0 = c->getChain();
        expectEquals (int (chain0.size()), 4);

        beginTest ("duplicateSlot: a copy right after it, marks modified; refused at 10 slots");
        {
            c->setSlotParam (1, 0, 300.0f);
            juce::String why;
            expect (c->duplicateSlot (1, why), why);
            const auto& ch = c->getChain();
            expectEquals (int (ch.size()), 5);
            expect (ch[2] == ch[1]);
            expect (ch[3].type == chain0[2].type);
            expect (c->isCurrentPresetModified());
            while (int (c->getChain().size()) < kMaxSlots) expect (c->duplicateSlot (0, why), why);
            expect (! c->duplicateSlot (0, why));
            expect (why.isNotEmpty());
            expect (! c->duplicateSlot (99, why));
        }

        beginTest ("resetSlot: params back to defaults, mod off, ON/OFF kept");
        {
            c->loadPreset ("character-demon-king");
            juce::String why;
            const auto* info = findEffectInfo (c->getChain()[0].type);
            expect (info != nullptr);
            expect (c->setSlotEnabled (0, false, why));
            c->setSlotParam (0, 0, info->params[0].max);
            expect (c->setSlotMod (0, "wet", 0.5f));
            c->resetSlot (0);
            const auto& s = c->getChain()[0];
            for (size_t p = 0; p < info->params.size(); ++p) expectEquals (s.params[p], info->params[p].def);
            expect (s.modTarget.empty());
            expect (! s.enabled);
        }

        beginTest ("edit hooks: by default the engine plays the working preset");
        {
            expect (! c->isAbCompare());
            expectEquals (c->getEffectiveTrimDb(), c->getCurrentPreset().outputTrimDb);
        }

        beginTest ("A/B: a slot button reaches the saved copy's slot of the same once-per-chain type, not the same index");
        {
            c->loadPreset ("character-demon-king");
            juce::String why;
            expect (c->addEffect ("freeze", why), why);
            expect (c->saveCurrentAsNew ("AB test", why), why);  // saved: 4 effects, then freeze (slot 4)
            const int last = int (c->getChain().size()) - 1;
            expect (c->moveSlot (last, 0));                       // edited: freeze first
            expect (c->setAbCompare (true, why), why);
            auto& vp = c->getProcessorForTests();
            std::vector<float> in (480, 0.1f), out (480);
            vp.process (in.data(), out.data(), nullptr, 480);
            c->triggerSlot (0, EffectTrigger::freezeToggle);     // the edited freeze's index
            vp.process (in.data(), out.data(), nullptr, 480);
            expect (c->getSlotUiState (0) != 0, "the saved copy's freeze took the trigger");
            c->triggerSlot (1, EffectTrigger::freezeToggle);     // distortion in the edit: nothing to reach in the saved copy
            vp.process (in.data(), out.data(), nullptr, 480);
            expect (c->getSlotUiState (0) != 0);
            expect (c->setAbCompare (false, why), why);
        }

        beginTest ("tapestop is one per chain (when the type exists)");
        {
            c->loadPreset ("character-demon-king");
            juce::String why;
            if (hasEffectFactory ("tapestop"))
            {
                expect (c->addEffect ("tapestop", why), why);
                expect (! c->addEffect ("tapestop", why));
                expect (! c->duplicateSlot (int (c->getChain().size()) - 1, why));
            }
            else
                expect (! c->addEffect ("tapestop", why));
        }
        c->shutdown();
    }
};

static Wave10FoundationTests wave10FoundationTests;
} // namespace koe

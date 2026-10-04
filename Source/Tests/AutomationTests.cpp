// 押している間だけのエフェクト and アプリごとの自動切り替え (INTERFACES.md §10.3, owner wave8/automation). Category "Automation".
// No window, no audio device, no real keyboard or windows: AppController (false), Hotkeys::keyStateForTests, the
// foreground::*ForTests hooks, synthetic speech (test::synthVoice, 合成音声で代用).

#include "App/AppController.h"
#include "Core/Paths.h"
#include "Effects/EffectRegistry.h"
#include "Engine/MomentaryFx.h"
#include "Platform/ForegroundApp.h"
#include "Tests/TestUtil.h"
#include "UI/MainComponent.h"
#include "UI/Screens.h"
#include "UI/main/Common.h"

#include <set>

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

std::unique_ptr<AppController> makeController()
{
    auto c = std::make_unique<AppController> (false);
    c->startup();
    c->updateSettings ([] (Settings& s)
    {
        s.setupDone = true;
        s.tourStep = 7;
    });
    c->setVoiceChangerOn (true);
    c->loadPreset ("natural-asis");
    c->dispatchPendingMessages();
    return c;
}

std::vector<float> render (VoiceProcessor& vp, const std::vector<float>& in)
{
    std::vector<float> out (in.size(), 0.0f);
    for (size_t pos = 0; pos + kBlock <= in.size(); pos += kBlock) vp.process (in.data() + pos, out.data() + pos, nullptr, kBlock);
    return out;
}

std::vector<float> run (MomentaryFx& fx, std::vector<float> x)
{
    for (size_t pos = 0; pos < x.size(); pos += kBlock) fx.process (x.data() + pos, int (std::min<size_t> (kBlock, x.size() - pos)));
    return x;
}

float maxDiff (const std::vector<float>& a, const std::vector<float>& b, size_t from = 0, size_t to = SIZE_MAX)
{
    float d = 0.0f;
    for (size_t i = from; i < std::min ({ a.size(), b.size(), to }); ++i) d = std::max (d, std::abs (a[i] - b[i]));
    return d;
}

std::vector<float> slice (const std::vector<float>& v, double fromS, double toS)
{
    return { v.begin() + int (fromS * kSr), v.begin() + int (std::min (double (v.size()), toS * kSr)) };
}

void setRecipe (MomentaryFx& fx, int slot, const char* id)
{
    const auto* def = findMomentaryRecipe (id);
    fx.setChain (slot, EffectChain::create (momentaryRecipeChain (*def), kSr, kBlock), kSr, def->tailSeconds);
}

void ticks (AppController& c, int n)
{
    for (int i = 0; i < n; ++i) c.tickForTests();
}
} // namespace

class AutomationTests : public juce::UnitTest
{
public:
    AutomationTests() : juce::UnitTest ("Momentary effects and per-app switching (wave8/automation)", "Automation") {}

    void runTest() override
    {
        recipes();
        momentaryFx();
        momentaryInController();
        appSwitch();
        settingsCards();
        Hotkeys::keyStateForTests = nullptr;
        foreground::programInFrontForTests = nullptr;
        foreground::visibleProgramsForTests = nullptr;
    }

private:
    void recipes()
    {
        beginTest ("recipes: 6..8, unique ids, Japanese names, light / medium effects with no latency; held 2 s of speech: finite, <= +6 dBFS, no allocation");
        const auto list = AppController::getMomentaryRecipes();
        expect (list.size() >= 6 && list.size() <= 8, "count " + juce::String (int (list.size())));
        std::set<juce::String> ids;
        for (auto& r : list)
        {
            ids.insert (r.id);
            expect (r.nameJa.isNotEmpty() && ! r.nameJa.containsOnly ("abcdefghijklmnopqrstuvwxyz"), r.id + ": Japanese name");
            const auto* def = findMomentaryRecipe (r.id);
            expect (def != nullptr, r.id);
            if (def == nullptr) continue;
            const auto* info = findEffectInfo (def->type);
            expect (info != nullptr && info->weight != EffectWeight::heavy, r.id + ": light / medium");
            const auto chainDef = momentaryRecipeChain (*def);
            expect (chainDef.size() == 1 && chainDef[0].params.size() == info->params.size(), r.id + ": one full slot");
            for (auto& [pid, v] : def->params) expect (info->paramIndex (pid) >= 0, r.id + ": knob " + pid);
            auto chain = EffectChain::create (chainDef, kSr, kBlock);
            expect (chain != nullptr && chain->size() == 1 && chain->slot (0).fx->getLatencySamples() == 0, r.id + ": built, latency 0");

            MomentaryFx fx;
            setRecipe (fx, 0, def->id);
            fx.setHeld (0, true);
            auto x = synthVoice (2.0, 3);
            fx.process (x.data(), kBlock); // takes the chain
            AllocationCounter counter;
            for (size_t pos = kBlock; pos < x.size(); pos += kBlock) fx.process (x.data() + pos, kBlock);
            const auto allocs = counter.count();
            expectEquals (allocs, 0LL, r.id + ": allocations");
            expect (allFinite (x), r.id + ": finite");
            expectLessOrEqual (peakDb (x), 6.0f, r.id + ": peak");
        }
        expectEquals (int (ids.size()), int (list.size()), "unique ids");
        expect (findMomentaryRecipe ("") == nullptr && findMomentaryRecipe ("nope") == nullptr);
    }

    void momentaryFx()
    {
        beginTest ("MomentaryFx: idle passes the voice bit for bit; held changes it; released it fades back and stops (telephone)");
        {
            MomentaryFx fx;
            setRecipe (fx, 0, "telephone");
            const auto in = synthVoice (3.0, 5, kSr, false);
            auto out = run (fx, slice (in, 0.0, 0.5));
            expect (out == slice (in, 0.0, 0.5), "idle: untouched");
            expect (! fx.isActive (0));

            fx.setHeld (0, true);
            out = run (fx, slice (in, 0.5, 1.5));
            expect (fx.isActive (0));
            expectGreaterThan (maxDiff (out, slice (in, 0.5, 1.5), size_t (0.05 * kSr)), 0.02f, "held: the effect is on");
            expectLessOrEqual (std::abs (out[0] - in[size_t (0.5 * kSr)]), 0.01f, "the first sample still mostly dry (30 ms fade)");

            fx.setHeld (0, false);
            out = run (fx, slice (in, 1.5, 3.0)); // 30 ms fade + 0.5 s tail, then idle
            expect (! fx.isActive (0), "stopped after the tail");
            const auto ref = slice (in, 1.5, 3.0);
            expect (maxDiff (out, ref, size_t (0.6 * kSr)) == 0.0f, "back to the voice bit for bit");
        }

        beginTest ("MomentaryFx: press / release are click-free (30 ms, spec §13.2 判定)");
        {
            MomentaryFx fx;
            setRecipe (fx, 0, "radio");
            auto x = synthVoice (2.0, 11, kSr, false);
            const int press = int (0.5 * kSr), release = int (1.2 * kSr);
            for (int pos = 0; pos < int (x.size()); pos += kBlock)
            {
                if (pos == press) fx.setHeld (0, true);
                if (pos == release) fx.setHeld (0, false);
                fx.process (x.data() + pos, kBlock);
            }
            expectLessOrEqual (clickRatio (x, press, press + int (0.03 * kSr)), 2.0, "press");
            expectLessOrEqual (clickRatio (x, release, release + int (0.03 * kSr)), 2.0, "release");
        }

        beginTest ("MomentaryFx: an echo keeps ringing after the release (input closed only) and then stops");
        {
            MomentaryFx fx;
            setRecipe (fx, 0, "yamabiko");
            fx.setHeld (0, true);
            run (fx, synthVoice (1.0, 2));
            fx.setHeld (0, false);
            auto tail = run (fx, silence (1.0));
            expectGreaterThan (rmsDb (slice (tail, 0.1, 1.0)), -50.0f, "echoes after the release");
            auto later = run (fx, silence (4.0));
            expect (! fx.isActive (0), "stopped after the tail time");
            expect (maxDiff (slice (later, 3.5, 4.0), silence (0.5)) == 0.0f, "silent once stopped");
        }

        beginTest ("MomentaryFx: two slots at once; a new chain waits while running and is taken when idle; a rate change swaps at once");
        {
            MomentaryFx fx;
            setRecipe (fx, 0, "telephone");
            setRecipe (fx, 1, "robot");
            fx.setHeld (0, true);
            fx.setHeld (1, true);
            auto x = run (fx, synthVoice (0.5, 4));
            expect (fx.isActive (0) && fx.isActive (1) && allFinite (x));
            setRecipe (fx, 0, "megaphone");            // while running: waits
            run (fx, synthVoice (0.1, 4));
            fx.collectGarbage();
            fx.setChain (1, EffectChain::create (momentaryRecipeChain (*findMomentaryRecipe ("robot")), 44100.0, 256), 44100.0, 0.5f);
            run (fx, synthVoice (0.1, 4));             // rate changed: slot 1 swaps while held, the old chain is retired
            fx.collectGarbage();
            fx.setHeld (0, false);
            fx.setHeld (1, false);
            run (fx, silence (1.0));
            expect (! fx.isActive (0) && ! fx.isActive (1));
            fx.setChain (0, nullptr, kSr, 0.0f);       // recipe removed
            fx.setHeld (0, true);
            const auto v = synthVoice (0.2, 4);
            expect (run (fx, v) == v, "a slot without a chain does nothing");
        }
    }

    void momentaryInController()
    {
        const auto in = synthVoice (2.0, 8);

        beginTest ("controller: without a recipe nothing is attached; a recipe that is not held leaves the output identical (bit for bit)");
        std::vector<float> plain;
        {
            freshDataDir();
            auto c = makeController();
            c->performAction ("momentary.1"); // no recipe: nothing
            expect (! c->isMomentaryHeld (0));
            plain = render (c->getProcessorForTests(), in);
        }
        {
            freshDataDir();
            auto c = makeController();
            c->setMomentaryRecipe (0, "cathedral");
            expectEquals (c->getSettings().momentaryRecipes[0], juce::String ("cathedral"));
            ticks (*c, 2);
            expect (render (c->getProcessorForTests(), in) == plain, "attached and idle: same output");
            c->setMomentaryRecipe (1, "bogus");
            expect (c->getSettings().momentaryRecipes[1].isEmpty(), "unknown recipe ids are not stored");
        }

        beginTest ("controller: the hotkey holds while its key is down (Hotkeys::isKeyDown), released at the next tick when it is up");
        {
            freshDataDir();
            auto c = makeController();
            c->setMomentaryRecipe (0, "telephone");
            juce::String why;
            expect (c->setHotkey ("momentary.1", 2, 'M', why), why);
            bool down = true;
            Hotkeys::keyStateForTests = [&down] (int vk) { return vk == 'M' && down; };
            c->performAction ("momentary.1");
            expect (c->isMomentaryHeld (0));
            ticks (*c, 5);
            expect (c->isMomentaryHeld (0), "still held while the key is down");
            auto& vp = c->getProcessorForTests();
            auto held = render (vp, in);
            expectGreaterThan (maxDiff (held, plain, size_t (0.2 * kSr)), 0.02f, "the effect is heard");
            std::vector<float> o (kBlock);
            AllocationCounter counter;
            for (size_t pos = 0; pos + kBlock <= in.size(); pos += kBlock) vp.process (in.data() + pos, o.data(), nullptr, kBlock);
            const auto allocs = counter.count();
            expectEquals (allocs, 0LL, "no allocation on the audio thread while held");
            down = false;
            c->tickForTests();
            expect (! c->isMomentaryHeld (0), "released with the key");
            Hotkeys::keyStateForTests = nullptr;
        }

        beginTest ("controller: no key bound -> released at the next tick; 「押して試す」 holds until let go; shutdown releases (twice is fine)");
        {
            freshDataDir();
            auto c = makeController();
            c->setMomentaryRecipe (2, "robot");
            Hotkeys::keyStateForTests = [] (int) { return false; };
            c->performAction ("momentary.3");
            expect (c->isMomentaryHeld (2));
            c->tickForTests();
            expect (! c->isMomentaryHeld (2), "no binding: released");
            setMomentaryUiHold (2, true);
            c->setMomentaryHeld (2, true);
            ticks (*c, 10);
            expect (c->isMomentaryHeld (2), "held by the button");
            setMomentaryUiHold (2, false);
            c->setMomentaryHeld (2, false);
            expect (! c->isMomentaryHeld (2));
            c->setMomentaryHeld (2, true);
            c->setMomentaryRecipe (2, "");
            expect (! c->isMomentaryHeld (2), "clearing the recipe releases");
            c->setMomentaryRecipe (2, "robot");
            setMomentaryUiHold (2, true);
            c->setMomentaryHeld (2, true);
            c->shutdown();
            expect (! c->isMomentaryHeld (2), "shutdown releases");
            Hotkeys::keyStateForTests = nullptr;
        }

        beginTest ("controller: a device reopen at another rate / block rebuilds the chains; finite output, no allocation");
        {
            freshDataDir();
            auto c = makeController();
            c->setMomentaryRecipe (0, "yamabiko");
            setMomentaryUiHold (0, true);
            c->setMomentaryHeld (0, true);
            auto& vp = c->getProcessorForTests();
            render (vp, in);
            c->reprepareForTests (44100.0, 256);
            c->tickForTests();
            std::vector<float> o (256);
            const auto in44 = synthVoice (1.0, 8, 44100.0);
            vp.process (in44.data(), o.data(), nullptr, 256); // takes the new chain
            AllocationCounter counter;
            bool finite = true;
            for (size_t pos = 256; pos + 256 <= in44.size(); pos += 256)
            {
                vp.process (in44.data() + pos, o.data(), nullptr, 256);
                finite = finite && allFinite (o);
            }
            const auto allocs = counter.count();
            expectEquals (allocs, 0LL);
            expect (finite);
            setMomentaryUiHold (0, false);
        }
    }

    void appSwitch()
    {
        juce::String front;
        foreground::programInFrontForTests = [&front] { return front; };

        auto setup = [] (AppController& c, bool restore)
        {
            c.updateSettings ([restore] (Settings& s)
            {
                s.appSwitchOn = true;
                s.appSwitchRestore = restore;
                s.appSwitchRules = { { "Game.exe", "character-demon-king" }, { "Chat.exe", "device-telephone" } };
            });
            c.takeToasts();
        };
        auto second = [] (AppController& c) { ticks (c, 30); };

        beginTest ("switching: off does nothing; a matching program switches (ignoring case) once a second; KoeLoom / unknown do nothing");
        {
            freshDataDir();
            auto c = makeController();
            front = "Game.exe";
            second (*c);
            expect (c->getCurrentPreset().id == "natural-asis", "appSwitchOn is off by default");
            setup (*c, true);
            front = "game.EXE";
            ticks (*c, 29);
            expect (c->getCurrentPreset().id == "natural-asis", "not before a second");
            c->tickForTests();
            expect (c->getCurrentPreset().id == "character-demon-king", "switched");
            const auto t = c->takeToasts();
            expect (t.size() == 1 && t[0].contains ("game.EXE") && t[0].contains (juce::String::fromUTF8 ("→")), t.joinIntoString ("|"));
            front = "KoeLoom.exe";
            second (*c);
            front = {};
            second (*c);
            expect (c->getCurrentPreset().id == "character-demon-king", "KoeLoom in front / unknown: unchanged");
            front = "Chat.exe";
            second (*c);
            expect (c->getCurrentPreset().id == "device-telephone", "another rule");
            front = "notepad.exe";
            second (*c);
            expect (c->getCurrentPreset().id == "natural-asis", "no rule any more: back to the preset from before the switches");
            front = "Game.exe";
            second (*c);
            c->loadPreset ("natural-calm"); // picked by hand while the game is in front
            second (*c);
            expect (c->getCurrentPreset().id == "natural-calm", "a preset picked by hand is not overridden");
            front = "notepad.exe";
            second (*c);
            expect (c->getCurrentPreset().id == "natural-calm", "changed by hand: not restored");
        }

        beginTest ("switching: a modified preset is not switched (told once), then switches once saved; edited after a switch -> not restored");
        {
            freshDataDir();
            auto c = makeController();
            setup (*c, true);
            c->setPitch (3.0f);
            expect (c->isCurrentPresetModified());
            front = "Game.exe";
            for (int i = 0; i < 3; ++i) second (*c);
            expect (c->getCurrentPreset().id == "natural-asis" && c->isCurrentPresetModified(), "not switched");
            expectEquals (c->takeToasts().size(), 1, "told once");
            c->loadPreset ("natural-asis"); // drops the edit
            second (*c);
            expect (c->getCurrentPreset().id == "character-demon-king", "switches once nothing is modified");
            c->setPitch (-5.0f);
            front = "notepad.exe";
            second (*c);
            expect (c->getCurrentPreset().id == "character-demon-king" && c->isCurrentPresetModified(), "edited: not restored");
        }

        beginTest ("switching: appSwitchRestore off keeps the switched preset; turning the feature off forgets the switch");
        {
            freshDataDir();
            auto c = makeController();
            setup (*c, false);
            front = "Game.exe";
            second (*c);
            front = "notepad.exe";
            second (*c);
            expect (c->getCurrentPreset().id == "character-demon-king");
            c->updateSettings ([] (Settings& s) { s.appSwitchRestore = true; });
            front = "Game.exe";
            second (*c);
            c->updateSettings ([] (Settings& s) { s.appSwitchOn = false; });
            c->tickForTests();
            c->updateSettings ([] (Settings& s) { s.appSwitchOn = true; });
            front = "notepad.exe";
            second (*c);
            expect (c->getCurrentPreset().id == "character-demon-king", "off and on again: no stale restore");
        }

        beginTest ("listRunningPrograms: sorted, unique, without KoeLoom (hooked, no real windows)");
        {
            foreground::visibleProgramsForTests = [] { return juce::StringArray { "zoom.exe", "KoeLoom.exe", "Discord.exe", "zoom.exe", "chrome.exe" }; };
            const auto l = AppController::listRunningPrograms();
            expect (l == juce::StringArray { "chrome.exe", "Discord.exe", "zoom.exe" }, l.joinIntoString (","));
            expect (foreground::isSelf ("koeloom.EXE"));
            foreground::visibleProgramsForTests = nullptr;
        }
        foreground::programInFrontForTests = nullptr;
    }

    void settingsCards()
    {
        using namespace ui;
        const bool wasOff = mainui::animationsOff();
        mainui::animationsOff() = true;
        beginTest ("S-03: 押している間のエフェクト (hotkeys) and アプリごとの自動切り替え (startup) cards: set, hold, add / remove rules, search");
        {
            freshDataDir();
            auto c = makeController();
            foreground::visibleProgramsForTests = [] { return juce::StringArray { "Game.exe" }; };
            MainComponent mc (*c);
            mc.setSize (Theme::defaultWidth, Theme::defaultHeight);
            mc.showSettings (Navigator::SettingsSection::hotkeys);

            auto* combo = dynamic_cast<juce::ComboBox*> (mainui::findById (&mc, "settings.momentary.1"));
            auto* tryB = dynamic_cast<juce::Button*> (mainui::findById (&mc, "settings.momentary.1.try"));
            expect (combo != nullptr && tryB != nullptr, "row 1");
            if (combo != nullptr && tryB != nullptr)
            {
                expectEquals (combo->getNumItems(), 1 + int (AppController::getMomentaryRecipes().size()));
                expect (! tryB->isEnabled(), "nothing to try without a recipe");
                combo->setSelectedId (2, juce::sendNotificationSync);
                c->dispatchPendingMessages();
                expectEquals (c->getSettings().momentaryRecipes[0], AppController::getMomentaryRecipes()[0].id);
                expect (tryB->isEnabled());
                tryB->setState (juce::Button::buttonDown);
                ticks (*c, 3);
                expect (c->isMomentaryHeld (0), "held while pressed");
                tryB->setState (juce::Button::buttonNormal);
                expect (! c->isMomentaryHeld (0), "released");
            }

            mc.showSettings (Navigator::SettingsSection::startup);
            auto* program = dynamic_cast<juce::ComboBox*> (mainui::findById (&mc, "settings.appSwitch.program"));
            auto* preset = dynamic_cast<juce::ComboBox*> (mainui::findById (&mc, "settings.appSwitch.preset"));
            auto* add = dynamic_cast<juce::Button*> (mainui::findById (&mc, "settings.appSwitch.add"));
            expect (program != nullptr && preset != nullptr && add != nullptr, "add line");
            if (program != nullptr && preset != nullptr && add != nullptr)
            {
                program->setText ("Game", juce::dontSendNotification);
                const auto& all = c->getPresetLibrary().all();
                int demon = 0;
                for (int i = 0; i < int (all.size()); ++i) if (all[size_t (i)].id == "character-demon-king") demon = i + 1;
                preset->setSelectedId (demon, juce::dontSendNotification);
                add->onClick();
                c->dispatchPendingMessages();
                expect (c->getSettings().appSwitchRules == std::vector<AppSwitchRule> { { "Game.exe", "character-demon-king" } }, "rule added (.exe appended)");
                auto* remove = dynamic_cast<juce::Button*> (mainui::findById (&mc, "settings.appSwitch.rule.1.remove"));
                expect (remove != nullptr, "rule line");
                if (remove != nullptr) remove->onClick();
                c->dispatchPendingMessages();
                expect (c->getSettings().appSwitchRules.empty(), "removed");
            }

            if (auto* v = dynamic_cast<SettingsView*> (mainui::findById (&mc, "page.settings")))
                for (auto [q, id] : { std::pair { "自動切り替え", "settings.appSwitch" }, std::pair { "押している間", "settings.momentary" } })
                {
                    v->setSearchText (juce::String::fromUTF8 (q));
                    auto* card = mainui::findById (&mc, id);
                    expect (card != nullptr && mainui::visibleWithin (card, &mc), juce::String::fromUTF8 (q) + ": card found by the search");
                }
            else
                expect (false, "settings view");
            foreground::visibleProgramsForTests = nullptr;
        }
        mainui::animationsOff() = wasOff;
    }
};

static AutomationTests automationTests;
} // namespace koe

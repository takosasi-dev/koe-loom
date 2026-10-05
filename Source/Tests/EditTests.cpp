// 元に戻す / やり直し and A/B 聞き比べ (INTERFACES.md §12.3, owner wave10/edit). Category "Edit".
// No window, no audio device: AppController (false), MainComponent offscreen, synthetic speech (test::synthVoice).
// Steps are spaced past kUndoMergeMs with EditData::clockOffsetMsForTests instead of sleeping.

#include "App/AppController.h"
#include "Core/Paths.h"
#include "Effects/EffectRegistry.h"
#include "Tests/TestUtil.h"
#include "UI/MainComponent.h"
#include "UI/main/Common.h"

namespace koe
{
namespace
{
using namespace test;
constexpr int kBlock = 480;
constexpr const char* kDemon = "character-demon-king"; // 魔王: pitch -9, 1 voice, 4 slots (distortion, ringmod, echo, reverb)

void freshDataDir()
{
    auto d = paths::dataDir();
    if (d.getFullPathName().contains ("KoeLoomTests")) d.deleteRecursively();
    d.createDirectory();
}

std::unique_ptr<AppController> makeController (int layoutStyle = 0)
{
    auto c = std::make_unique<AppController> (false);
    c->startup();
    c->updateSettings ([layoutStyle] (Settings& s)
    {
        s.setupDone = true;
        s.tourStep = 7;
        s.layoutStyle = layoutStyle;
        s.favorites = juce::StringArray { "natural-asis", kDemon, "natural-ikebo", "character-helium", "character-robot", "space-cave" };
    });
    c->setVoiceChangerOn (true);
    c->loadPreset (kDemon);
    c->dispatchPendingMessages();
    return c;
}

/** The next edit is a step of its own. */
void nextStep() { EditData::clockOffsetMsForTests += kUndoMergeMs + 100.0; }

std::vector<float> render (VoiceProcessor& vp, const std::vector<float>& in)
{
    std::vector<float> out (in.size(), 0.0f);
    for (size_t pos = 0; pos + kBlock <= in.size(); pos += kBlock) vp.process (in.data() + pos, out.data() + pos, nullptr, kBlock);
    return out;
}

float maxDiff (const std::vector<float>& a, const std::vector<float>& b, size_t from)
{
    float d = 0.0f;
    for (size_t i = from; i < std::min (a.size(), b.size()); ++i) d = std::max (d, std::abs (a[i] - b[i]));
    return d;
}

/** Index of the first numeric (non-choice) parameter of slot i, or -1. */
int numericParam (const AppController& c, int slot)
{
    const auto* info = findEffectInfo (c.getChain()[size_t (slot)].type);
    for (int p = 0; info != nullptr && p < int (info->params.size()); ++p)
        if (! info->params[size_t (p)].isChoice()) return p;
    return -1;
}

/** A value of that parameter different from the current one. */
float otherValue (const AppController& c, int slot, int p)
{
    const auto& spec = findEffectInfo (c.getChain()[size_t (slot)].type)->params[size_t (p)];
    const float now = c.getChain()[size_t (slot)].params[size_t (p)];
    return std::abs (now - spec.max) > std::abs (now - spec.min) ? spec.max : spec.min;
}

juce::Component* shown (juce::Component* root, const juce::String& id)
{
    std::function<juce::Component* (juce::Component*)> find = [&] (juce::Component* comp) -> juce::Component*
    {
        if (comp != root && ! comp->isVisible()) return nullptr;
        if (comp->getComponentID() == id) return comp;
        for (auto* child : comp->getChildren())
            if (auto* f = find (child)) return f;
        return nullptr;
    };
    return find (root);
}

juce::Rectangle<int> boundsIn (juce::Component& root, juce::Component* comp) { return root.getLocalArea (comp, comp->getLocalBounds()); }
} // namespace

class EditTests : public juce::UnitTest
{
public:
    EditTests() : juce::UnitTest ("Undo, redo and A/B (wave10/edit)", "Edit") {}

    void runTest() override
    {
        freshDataDir();
        everyKindOfEdit();
        history();
        inPlaceOrRebuild();
        looper();
        identity();
        abCompare();
        keysAndButtons();
        layoutFits();
    }

private:
    // ---------------------------------------------------------------------------------------------
    void everyKindOfEdit()
    {
        beginTest ("each kind of edit is one step: undo gives the preset back exactly, redo the edited one, 変更あり follows");
        auto c = makeController();
        juce::String why;
        struct Edit { const char* name; std::function<void (AppController&)> setup, edit; };
        const std::vector<Edit> edits = {
            { "knob", {}, [] (AppController& a) { const int p = numericParam (a, 0); a.setSlotParam (0, p, otherValue (a, 0, p)); } },
            { "slot OFF", {}, [] (AppController& a) { juce::String w; a.setSlotEnabled (1, false, w); } },
            { "add an effect", {}, [] (AppController& a) { juce::String w; a.addEffect ("compressor", w); } },
            { "remove a slot", {}, [] (AppController& a) { a.removeSlot (2); } },
            { "move a slot", {}, [] (AppController& a) { a.moveSlot (0, 2); } },
            { "duplicate a slot", {}, [] (AppController& a) { juce::String w; a.duplicateSlot (1, w); } },
            { "reset a slot", [] (AppController& a) { const int p = numericParam (a, 3); a.setSlotParam (3, p, otherValue (a, 3, p)); },
              [] (AppController& a) { a.resetSlot (3); } },
            { "声の大きさで動かす", {}, [] (AppController& a) { a.setSlotMod (0, "wet", 0.6f); } },
            { "pitch", {}, [] (AppController& a) { a.setPitch (3.0f); } },
            { "formant", {}, [] (AppController& a) { a.setFormant (2.5f); } },
            { "converter OFF", {}, [] (AppController& a) { a.setShifterEnabled (false); } },
            { "add a voice", {}, [] (AppController& a) { juce::String w; a.addLayer (w); } },
            { "remove a voice", {}, [] (AppController& a) { a.removeLayer (0); } },
            { "change a voice", {}, [] (AppController& a) { auto l = a.getLayer (0); l.pitchSt = 7.0f; a.setLayer (0, l); } },
            { "voice OFF", {}, [] (AppController& a) { a.setLayerEnabled (0, false); } },
            { "おまかせ", {}, [] (AppController& a) { juce::String w; a.randomizeCurrent (42, w); } },
            { "混ぜる", {}, [] (AppController& a) { juce::String w; a.beginMorph (kDemon, "character-alien", w); a.setMorphAmount (0.4f); } },
            { "IR file", [] (AppController& a) { juce::String w; a.addEffect ("convolution", w); },
              [] (AppController& a) { a.setSlotFileName (int (a.getChain().size()) - 1, "hall.wav"); } },
        };
        for (auto& e : edits)
        {
            c->loadPreset (kDemon);
            const auto loaded = c->getCurrentPreset();
            if (e.setup)
            {
                nextStep();
                e.setup (*c);
            }
            const auto before = c->getCurrentPreset();
            nextStep();
            e.edit (*c);
            const auto after = c->getCurrentPreset();
            const juce::String n (e.name);
            expect (! (after == before), n + ": the edit changed the preset");
            expect (c->canUndo() && ! c->canRedo(), n + ": one step to undo, nothing to redo");
            expect (c->undo (why), n + ": " + why);
            expect (c->getCurrentPreset() == before, n + ": undo gives the preset back exactly");
            expect (c->isCurrentPresetModified() == ! (before == loaded), n + ": 変更あり only if it differs from the saved one");
            expect (c->canRedo(), n + ": redo possible");
            expect (c->redo (why), n + ": " + why);
            expect (c->getCurrentPreset() == after, n + ": redo gives the edit back");
            expect (c->isCurrentPresetModified(), n + ": 変更あり again");
        }
        c->shutdown();
    }

    // ---------------------------------------------------------------------------------------------
    void history()
    {
        auto c = makeController();
        juce::String why;
        const auto loaded = c->getCurrentPreset();
        const int p = numericParam (*c, 0);
        const auto& spec = findEffectInfo (c->getChain()[0].type)->params[size_t (p)];

        beginTest ("a knob drag (edits closer than kUndoMergeMs) is one step");
        nextStep();
        for (int i = 0; i <= 20; ++i) c->setSlotParam (0, p, spec.min + (spec.max - spec.min) * float (i) / 20.0f);
        expect (c->undo (why), why);
        expect (c->getCurrentPreset() == loaded, "back before the whole drag");
        expect (! c->canUndo(), "one step only");

        beginTest ("at most kUndoSteps steps: the oldest ones drop");
        c->loadPreset (kDemon);
        std::vector<Preset> states;
        for (int i = 0; i < kUndoSteps + 5; ++i)
        {
            nextStep();
            c->setPitch (-10.0f + 0.1f * float (i));
            states.push_back (c->getCurrentPreset());
        }
        int undone = 0;
        while (c->undo (why)) ++undone;
        expectEquals (undone, kUndoSteps);
        expect (c->getCurrentPreset() == states[4], "the oldest kept step");
        expect (why.isNotEmpty(), "a reason when nothing is left");

        beginTest ("a new edit clears redo; refusals give a reason");
        c->loadPreset (kDemon);
        nextStep();
        c->setPitch (1.0f);
        nextStep();
        c->setPitch (2.0f);
        expect (c->undo (why) && c->canRedo());
        nextStep();
        c->setFormant (1.0f);
        expect (! c->canRedo(), "redo gone");
        why = {};
        expect (! c->redo (why) && why.isNotEmpty());

        beginTest ("undoing every step clears 変更あり, redo sets it again; an edit that changes nothing is no step");
        c->loadPreset (kDemon);
        for (float st : { 1.0f, 2.0f, 3.0f })
        {
            nextStep();
            c->setPitch (st);
        }
        while (c->undo (why)) {}
        expect (! c->isCurrentPresetModified() && c->getCurrentPreset() == loaded);
        expect (c->redo (why) && c->isCurrentPresetModified());
        c->loadPreset (kDemon);
        nextStep();
        c->setPitch (c->getPitch()); // same value
        expect (! c->canUndo(), "nothing really changed");

        beginTest ("loading a preset empties the history and ends A/B");
        nextStep();
        c->setPitch (4.0f);
        nextStep();
        c->setPitch (5.0f);
        c->undo (why);
        expect (c->setAbCompare (true, why), why);
        c->loadPreset ("natural-asis");
        expect (! c->canUndo() && ! c->canRedo() && ! c->isAbCompare());
        c->shutdown();
    }

    // ---------------------------------------------------------------------------------------------
    void inPlaceOrRebuild()
    {
        beginTest ("same chain shape: undo writes into the running chain (no rebuild); another shape rebuilds");
        auto c = makeController();
        auto& vp = c->getProcessorForTests();
        const auto in = synthVoice (0.3);
        render (vp, in);
        juce::String why;
        auto* chain0 = vp.getRequestedChain();
        const int p = numericParam (*c, 0);
        const float v0 = c->getChain()[0].params[size_t (p)];
        nextStep();
        c->setSlotParam (0, p, otherValue (*c, 0, p));
        nextStep();
        juce::String w;
        c->setSlotEnabled (1, false, w);
        nextStep();
        c->setSlotMod (0, "wet", -0.5f);
        nextStep();
        c->setPitch (2.0f);
        for (int i = 0; i < 4; ++i) expect (c->undo (why), why);
        expect (vp.getRequestedChain() == chain0, "the same chain object (effects keep their state)");
        expectEquals (chain0->slot (0).params[size_t (p)].load(), v0);
        expect (chain0->slot (1).enabled.load(), "slot 1 back ON in the engine");
        expectEquals (chain0->slot (0).modIndex.load(), int (EffectChain::kModNone));
        expectEquals (c->getPitch(), -9.0f);
        {
            const auto tone = synthVoice (0.1);
            std::vector<float> out (tone.size());
            AllocationCounter counter;
            for (size_t pos = 0; pos + kBlock <= tone.size(); pos += kBlock) vp.process (tone.data() + pos, out.data() + pos, nullptr, kBlock);
            const auto allocations = counter.count(); // before building the message string
            expectEquals (allocations, 0LL, "no allocation while playing after an in-place undo");
        }
        for (int i = 0; i < 4; ++i) expect (c->redo (why), why);
        expect (vp.getRequestedChain() == chain0, "redo in place too");

        nextStep();
        c->addEffect ("compressor", why);
        auto* chain1 = vp.getRequestedChain();
        expect (chain1 != chain0);
        expect (c->undo (why), why);
        expect (vp.getRequestedChain() != chain1, "rebuilt for the old shape");
        expectEquals (vp.getRequestedChain()->size(), 4);
        auto* chain2 = vp.getRequestedChain();
        expect (c->redo (why), why);
        expect (vp.getRequestedChain() != chain2 && vp.getRequestedChain()->size() == 5, "redo rebuilds too");
        c->shutdown();
    }

    // ---------------------------------------------------------------------------------------------
    void looper()
    {
        beginTest ("the looper's recording: an in-place step keeps it, a step that rebuilds is refused (E-27)");
        auto c = makeController();
        auto& vp = c->getProcessorForTests();
        juce::String why;
        nextStep();
        expect (c->addEffect ("looper", why), why);
        const auto in = synthVoice (0.5);
        render (vp, in);
        nextStep();
        const int p = numericParam (*c, 0);
        c->setSlotParam (0, p, otherValue (*c, 0, p));
        c->triggerSlot (int (c->getChain().size()) - 1, EffectTrigger::looperRecordPlay);
        render (vp, in);
        expect (c->hasLooperRecording(), "recording");
        expect (c->undo (why), why);
        expect (c->hasLooperRecording(), "the in-place undo kept the recording");
        const auto before = c->getCurrentPreset();
        why = {};
        expect (! c->undo (why), "undoing the looper's addition would lose the recording");
        expect (why.isNotEmpty() && c->getCurrentPreset() == before && c->canUndo());
        expect (! c->setAbCompare (true, why), "A/B rebuilds too");
        c->shutdown();
    }

    // ---------------------------------------------------------------------------------------------
    void identity()
    {
        beginTest ("「新しく保存」 and a rename: the history stays and a step back keeps the new name");
        auto c = makeController();
        juce::String why;
        const auto loaded = c->getCurrentPreset();
        nextStep();
        c->setPitch (1.0f);
        expect (c->saveCurrentAsNew (juce::String::fromUTF8 ("わたしの声"), why), why);
        const auto savedId = c->getCurrentPreset().id;
        expect (c->canUndo(), "the history stays across a save");
        nextStep();
        c->setFormant (1.0f);
        expect (c->undo (why), why);
        expect (! c->isCurrentPresetModified(), "back to what was just saved");
        expect (c->undo (why), why);
        expect (c->getCurrentPreset().name == juce::String::fromUTF8 ("わたしの声") && c->getCurrentPreset().id == savedId, "the new name stays");
        expectEquals (c->getPitch(), loaded.pitchSt);
        expect (c->isCurrentPresetModified(), "differs from the saved one");
        expect (c->renamePreset (savedId, juce::String::fromUTF8 ("改名"), why), why);
        expect (c->redo (why), why);
        expect (c->getCurrentPreset().name == juce::String::fromUTF8 ("改名"), "a rename is followed too");
        expect (! c->isCurrentPresetModified());

        beginTest ("undoing a blend gives the preset back, name included");
        c->loadPreset (kDemon);
        nextStep();
        expect (c->beginMorph (kDemon, "character-alien", why), why);
        expect (c->getCurrentPreset().name != loaded.name);
        expect (c->undo (why), why);
        expect (c->getCurrentPreset() == loaded && ! c->isCurrentPresetModified());
        c->shutdown();
    }

    // ---------------------------------------------------------------------------------------------
    void abCompare()
    {
        juce::String why;
        // through a rebuild, so the controllers compared below all start from freshly built chains (no parameter glides)
        auto edit = [] (AppController& a)
        {
            a.setPitch (4.0f);
            juce::String w;
            a.addEffect ("compressor", w);
        };

        beginTest ("A/B: not without a change; the engine plays the saved preset while the screen keeps the edit");
        {
            auto ab = makeController();
            expect (! ab->canAbCompare());
            expect (! ab->setAbCompare (true, why) && why.isNotEmpty() && ! ab->isAbCompare());
            auto ref = makeController();
            auto edited = makeController();
            edit (*ab);
            edit (*edited);
            const auto shownPreset = ab->getCurrentPreset();
            expect (ab->canAbCompare());
            expect (ab->setAbCompare (true, why), why);
            expect (ab->isAbCompare() && ab->getCurrentPreset() == shownPreset && ab->isCurrentPresetModified(), "the screen keeps the edit");
            expectEquals (ab->getPitch(), 4.0f);
            const auto in = synthVoice (2.0, 7, kSr, false);
            const auto a = render (ab->getProcessorForTests(), in);
            const auto r = render (ref->getProcessorForTests(), in);
            const auto e = render (edited->getProcessorForTests(), in);
            expect (allFinite (a) && rmsDb (a) > -40.0f);
            const float d = maxDiff (a, r, size_t (kSr * 0.3));
            expect (d < 1.0e-5f, "same as the saved preset, max diff " + juce::String (d, 8));
            expect (maxDiff (a, e, size_t (kSr * 0.3)) > 0.01f, "not the edited voice");
            ab->shutdown();
            ref->shutdown();
            edited->shutdown();
        }

        beginTest ("A/B: touching anything ends it and the engine plays the edited voice again (the edit counts)");
        {
            auto ab = makeController();
            auto edited = makeController();
            edit (*ab);
            edit (*edited);
            expect (ab->setAbCompare (true, why), why);
            ab->setFormant (-2.0f);
            edited->setFormant (-2.0f);
            expect (! ab->isAbCompare());
            expect (ab->getCurrentPreset() == edited->getCurrentPreset());
            const auto in = synthVoice (1.5, 7, kSr, false);
            const auto a = render (ab->getProcessorForTests(), in);
            const auto e = render (edited->getProcessorForTests(), in);
            const float d = maxDiff (a, e, size_t (kSr * 0.3));
            expect (d < 1.0e-5f, "the edited voice, max diff " + juce::String (d, 8));
            ab->shutdown();
            edited->shutdown();
        }

        beginTest ("A/B: off again plays the edit; a save ends it (history kept); undo ends it; a blend keeps going");
        {
            auto c = makeController();
            auto& vp = c->getProcessorForTests();
            nextStep();
            edit (*c);
            expect (c->setAbCompare (true, why), why);
            expect (vp.getRequestedChain()->size() == 4, "the engine runs the saved chain");
            expect (c->setAbCompare (false, why), why);
            expect (! c->isAbCompare() && vp.getRequestedChain()->size() == 5, "and the edited one again");
            expect (c->setAbCompare (true, why), why);
            expect (c->saveCurrentAsNew ("AB", why), why);
            expect (! c->isAbCompare() && c->canUndo() && ! c->canAbCompare());
            nextStep();
            c->setPitch (-3.0f);
            expect (c->setAbCompare (true, why), why);
            expect (c->undo (why), why);
            expect (! c->isAbCompare() && c->getPitch() == 4.0f, "undo ends A/B and steps back");

            c->loadPreset (kDemon);
            expect (c->beginMorph (kDemon, "character-alien", why), why);
            c->setMorphAmount (0.5f);
            expect (c->setAbCompare (true, why), why);
            expect (c->isMorphing(), "A/B keeps the blend's controls");
            c->setMorphAmount (0.6f);
            expect (! c->isAbCompare() && c->isMorphing(), "moving the blend ends A/B, the blend goes on");
            expect (c->setAbCompare (true, why), why);
            expect (c->beginMorph (kDemon, "character-helium", why), why);
            expect (c->isMorphing() && ! c->isAbCompare(), "a blend begun during A/B works");
            c->shutdown();
        }
    }

    // ---------------------------------------------------------------------------------------------
    void keysAndButtons()
    {
        using namespace ui;
        beginTest ("Ctrl+Z / Ctrl+Y / Ctrl+Shift+Z on the voice page (not on S-03); the buttons follow and act");
        const bool wasOff = mainui::animationsOff();
        mainui::animationsOff() = true;
        auto c = makeController();
        MainComponent mc (*c);
        mc.setSize (Theme::defaultWidth, Theme::defaultHeight);
        const juce::KeyPress ctrlZ ('z', juce::ModifierKeys::commandModifier, 0), ctrlY ('y', juce::ModifierKeys::commandModifier, 0),
            ctrlShiftZ ('Z', juce::ModifierKeys::commandModifier | juce::ModifierKeys::shiftModifier, 0);
        auto* undo = dynamic_cast<juce::Button*> (shown (&mc, "voice.undo"));
        auto* redo = dynamic_cast<juce::Button*> (shown (&mc, "voice.redo"));
        auto* ab = dynamic_cast<juce::Button*> (shown (&mc, "voice.ab"));
        expect (undo != nullptr && redo != nullptr && ab != nullptr, "voice.undo / voice.redo / voice.ab");
        if (undo == nullptr || redo == nullptr || ab == nullptr) return;
        expect (undo->getTooltip().contains ("Ctrl+Z") && redo->getTooltip().contains ("Ctrl+Y") && ab->getTooltip().startsWith ("A/B"));
        expect (! undo->isEnabled() && ! redo->isEnabled() && ! ab->isEnabled(), "nothing to do right after loading");

        nextStep();
        c->setPitch (2.0f);
        c->dispatchPendingMessages();
        expect (undo->isEnabled() && ! redo->isEnabled() && ab->isEnabled());
        ab->onClick();
        c->dispatchPendingMessages();
        expect (c->isAbCompare() && ab->getTitle().contains (juce::String::fromUTF8 ("保存版")), "A/B ON: pressed, 「保存版」");
        ab->onClick();
        expect (! c->isAbCompare());

        expect (mc.keyPressed (ctrlZ), "Ctrl+Z taken");
        c->dispatchPendingMessages();
        expect (! c->canUndo() && c->canRedo() && ! undo->isEnabled() && redo->isEnabled());
        expect (mc.keyPressed (ctrlY) && c->canUndo(), "Ctrl+Y redoes");
        expect (mc.keyPressed (ctrlZ) && mc.keyPressed (ctrlShiftZ) && c->canUndo() && ! c->canRedo(), "Ctrl+Shift+Z redoes");
        expect (mc.keyPressed (ctrlY), "nothing to redo: still taken (a toast says why)");
        undo->onClick();
        expect (c->canRedo());
        redo->onClick();
        expect (! c->canRedo());

        mc.showPage (Navigator::Page::settings);
        expect (! mc.keyPressed (ctrlZ) && c->canUndo(), "S-03: not an undo");
        mc.showPage (Navigator::Page::voice);
        mainui::animationsOff() = wasOff;
        c->shutdown();
    }

    // ---------------------------------------------------------------------------------------------
    void layoutFits()
    {
        using namespace ui;
        const bool wasOff = mainui::animationsOff();
        mainui::animationsOff() = true;
        for (int style : { 0, 1, 2 })
        {
            const auto look = juce::String ("layout ") + juce::String (style);
            beginTest ("元に戻す / やり直し / A/B fit beside 聞き比べ / おまかせ (" + look + "), wide and narrow; Studio's favourites keep their width");
            auto c = makeController (style);
            nextStep();
            c->setPitch (1.0f);
            MainComponent mc (*c);
            for (auto size : { juce::Point<int> (Theme::defaultWidth, Theme::defaultHeight), juce::Point<int> (Theme::minWidth, Theme::minHeight),
                               juce::Point<int> (Theme::narrowWidth - 1, Theme::minHeight) })
            {
                mc.setSize (size.x, size.y);
                c->dispatchPendingMessages();
                const auto label = look + " " + juce::String (size.x) + "x" + juce::String (size.y);
                std::vector<std::pair<juce::String, juce::Rectangle<int>>> rects;
                for (auto* id : { "voice.undo", "voice.redo", "voice.ab", "voice.compare", "voice.random", "voice.presetSelector",
                                  "voice.fav.0", "voice.fav.1", "voice.fav.2", "voice.fav.3", "voice.fav.more" })
                {
                    auto* comp = shown (&mc, id);
                    const bool mine = juce::String (id).startsWith ("voice.undo") || juce::String (id) == "voice.redo" || juce::String (id) == "voice.ab";
                    if (mine) expect (comp != nullptr, label + ": " + id + " shown");
                    if (comp == nullptr) continue;
                    const auto b = boundsIn (mc, comp);
                    rects.emplace_back (id, b);
                    expect (mc.getLocalBounds().contains (b), label + ": " + id + " inside the window");
                    if (mine) expect (b.getWidth() >= 20 && b.getHeight() >= 24, label + ": " + id + " big enough " + b.toString());
                    if (auto* chip = dynamic_cast<mainui::ChipButton*> (comp))
                        expect (chip->getWidth() >= chip->preferredWidth(), label + ": " + id + " not squeezed (AC-60) "
                                                                                + juce::String (chip->getWidth()) + " < " + juce::String (chip->preferredWidth()));
                }
                for (size_t i = 0; i < rects.size(); ++i)
                    for (size_t j = i + 1; j < rects.size(); ++j)
                        expect (! rects[i].second.intersects (rects[j].second), label + ": " + rects[i].first + " overlaps " + rects[j].first);
                if (style == 2 && size.x < Theme::narrowWidth)
                    if (auto* picker = shown (&mc, "tour.presets"))
                        expect (picker->getWidth() >= 150, label + ": the preset picker keeps room " + juce::String (picker->getWidth()));
            }
            c->shutdown();
        }
        mainui::animationsOff() = wasOff;
    }
};

static EditTests editTests;
} // namespace koe

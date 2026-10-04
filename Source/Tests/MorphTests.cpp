// プリセットを混ぜる (INTERFACES.md §10.3 / §10.4, owner wave8/morph). Category "Morph".
// No window, no audio device: AppController (false), MainComponent offscreen, synthetic speech (test::synthVoice).
// The t = 0 / t = 1 identity runs the blended chain alone through EffectChain::create (§10.4).

#include "App/AppController.h"
#include "Core/Paths.h"
#include "Dsp/Building.h"
#include "Effects/EffectRegistry.h"
#include "Engine/EffectChain.h"
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
using Pairs = std::vector<std::pair<int, int>>;

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
        s.layoutStyle = 0;
    });
    c->setVoiceChangerOn (true);
    c->loadPreset ("character-demon-king");
    c->dispatchPendingMessages();
    return c;
}

SlotDef slot (const char* type, bool enabled = true, float wet = 1.0f, const char* file = "")
{
    auto s = *makeDefaultSlot (type);
    s.enabled = enabled;
    s.wet = wet;
    s.file = file;
    return s;
}

/** Every parameter at its minimum (atMax false) or maximum. */
SlotDef extreme (const char* type, bool atMax)
{
    auto s = slot (type);
    auto* info = findEffectInfo (type);
    for (size_t k = 0; k < info->params.size(); ++k) s.params[k] = atMax ? info->params[k].max : info->params[k].min;
    return s;
}

std::string saveUser (AppController& c, const juce::String& name, std::vector<SlotDef> chain)
{
    Preset p;
    p.chain = std::move (chain);
    std::string id;
    juce::String error;
    c.getPresetLibrary().saveNew (p, name, id, error);
    return id;
}

std::vector<float> renderChain (const std::vector<SlotDef>& defs, const std::vector<float>& in)
{
    auto chain = EffectChain::create (defs, kSr, kBlock);
    auto buf = in;
    for (size_t pos = 0; pos + kBlock <= buf.size(); pos += kBlock) chain->process (buf.data() + pos, kBlock);
    return buf;
}

std::vector<float> render (VoiceProcessor& vp, const std::vector<float>& in)
{
    std::vector<float> out (in.size(), 0.0f);
    for (size_t pos = 0; pos + kBlock <= in.size(); pos += kBlock) vp.process (in.data() + pos, out.data() + pos, nullptr, kBlock);
    return out;
}

float maxDiff (const std::vector<float>& a, const std::vector<float>& b)
{
    float d = 0.0f;
    for (size_t i = 0; i < std::min (a.size(), b.size()); ++i) d = std::max (d, std::abs (a[i] - b[i]));
    return d;
}

MorphData makeMorph (const Preset& a, const Preset& b)
{
    MorphData m;
    m.a = a;
    m.b = b;
    m.trimA = a.outputTrimDb;
    m.trimB = b.outputTrimDb;
    m.pairs = morph::pairSlots (a.chain, b.chain);
    return m;
}

/** B's paired slots keep B's order in the blend (else t = 1 cannot equal B: the order is A's). */
bool keepsBOrder (const Pairs& pairs)
{
    int last = -1;
    for (auto [ia, ib] : pairs)
        if (ib >= 0)
        {
            if (ib < last) return false;
            last = ib;
        }
    return true;
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
} // namespace

class MorphTests : public juce::UnitTest
{
public:
    MorphTests() : juce::UnitTest ("Blend two presets (wave8/morph)", "Morph") {}

    void runTest() override
    {
        pairing();
        blendValues();
        identityAtEnds();
        controller();
        refusals();
        saveAndReload();
        names();
        tool();
    }

private:
    // ---------------------------------------------------------------------------------------------
    void pairing()
    {
        beginTest ("pairs: k-th of a type with k-th, A's order, B-only after the pair before it in B, convolution only with the same file");
        const std::vector<SlotDef> a { slot ("eq"), slot ("echo"), slot ("reverb"), slot ("echo") };
        const std::vector<SlotDef> b { slot ("compressor"), slot ("echo"), slot ("echo"), slot ("distortion") };
        expect (morph::pairSlots (a, b) == Pairs { { -1, 0 }, { 0, -1 }, { 1, 1 }, { 2, -1 }, { 3, 2 }, { -1, 3 } });
        expect (morph::pairSlots ({}, { slot ("eq"), slot ("reverb") }) == Pairs { { -1, 0 }, { -1, 1 } });
        expect (morph::pairSlots ({ slot ("eq"), slot ("reverb") }, {}) == Pairs { { 0, -1 }, { 1, -1 } });
        expect (morph::pairSlots ({ slot ("convolution", true, 1.0f, "x.wav") }, { slot ("convolution", true, 1.0f, "y.wav") })
                == Pairs { { -1, 0 }, { 0, -1 } }, "different files stay apart");
        expect (morph::pairSlots ({ slot ("convolution", true, 1.0f, "x.wav") }, { slot ("convolution", true, 1.0f, "x.wav") }) == Pairs { { 0, 0 } });
    }

    // ---------------------------------------------------------------------------------------------
    static Preset craftedA()
    {
        Preset a;
        a.id = "test-a";
        a.hasShifter = true;
        a.pitchSt = -6.0f;
        a.formantSt = -2.0f;
        LayerDef l;
        l.pitchSt = 12.0f;
        l.levelDb = -6.0f;
        a.layers = { l };
        a.chain = { extreme ("echo", false), extreme ("reverb", false), slot ("eq", false) };
        a.outputTrimDb = 2.0f;
        return a;
    }

    static Preset craftedB()
    {
        Preset b;
        b.id = "test-b";
        b.hasShifter = false;
        b.pitchSt = 5.0f; // ignored: no converter
        b.chain = { extreme ("echo", true), slot ("distortion", true, 0.5f), extreme ("reverb", true) };
        b.outputTrimDb = -4.0f;
        return b;
    }

    void blendValues()
    {
        beginTest ("blend: numbers glide, choices switch at 0.5, one-sided slots fade by wet (OFF = 0), pitch / voices / trim");
        const auto a = craftedA(), b = craftedB();
        const auto m = makeMorph (a, b);
        expect (m.pairs == Pairs { { 0, 0 }, { -1, 1 }, { 1, 2 }, { 2, -1 } });
        for (float t : { 0.25f, 0.75f })
        {
            const auto label = "t=" + juce::String (t);
            const auto p = morph::blend (m, t);
            expect (! p.builtin && p.hasShifter, label);
            expectWithinAbsoluteError (p.pitchSt, -6.0f * (1.0f - t), 1.0e-5f, label + " pitch (no converter = 0)");
            expectWithinAbsoluteError (p.formantSt, -2.0f * (1.0f - t), 1.0e-5f, label);
            expectWithinAbsoluteError (p.outputTrimDb, 2.0f * (1.0f - t) - 4.0f * t, 1.0e-5f, label + " trim");
            expect (p.chain.size() == 4 && p.chain[0].type == "echo" && p.chain[1].type == "distortion" && p.chain[2].type == "reverb"
                    && p.chain[3].type == "eq", label + " order");
            if (p.chain.size() != 4) continue;
            for (int s : { 0, 2 })
            {
                auto* info = findEffectInfo (p.chain[size_t (s)].type);
                for (size_t k = 0; k < info->params.size(); ++k)
                {
                    const auto& spec = info->params[k];
                    const float want = spec.isChoice() ? (t < 0.5f ? spec.min : spec.max) : spec.clamp (spec.min * (1.0f - t) + spec.max * t);
                    expectWithinAbsoluteError (p.chain[size_t (s)].params[k], want, 1.0e-4f, label + " " + info->type + "." + spec.id);
                }
                expect (p.chain[size_t (s)].enabled && p.chain[size_t (s)].wet == 1.0f, label + " both ON: full wet");
            }
            expect (p.chain[1].enabled, label + " B-only distortion ON");
            expectWithinAbsoluteError (p.chain[1].wet, 0.5f * t, 1.0e-6f, label + " B-only wet = t x its own wet");
            expect (! p.chain[3].enabled, label + " A's OFF eq stays OFF (wet 0 on both sides)");
            expect (p.layers.size() == 1, label + " A's voice");
        }
        const auto quarter = morph::blend (m, 0.25f);
        expect (quarter.layers[0].enabled, "the A-only voice is still on at t=0.25");
        expectWithinAbsoluteError (quarter.layers[0].levelDb, dsp::gainToDb (dsp::dbToGain (-6.0f) * 0.75f), 1.0e-3f, "fades in linear gain");
        expectWithinAbsoluteError (quarter.layers[0].pitchSt, 12.0f, 1.0e-6f);
        expect (! morph::blend (m, 0.9f).layers[0].enabled, "below the level range (-24 dB) the voice turns OFF");
        expectWithinAbsoluteError (morph::blend (m, 0.0f).layers[0].levelDb, -6.0f, 1.0e-4f, "t=0: A's voice level");

        beginTest ("blend: the crafted pair at t=0 sounds exactly like A's chain, at t=1 exactly like B's");
        const auto in = synthVoice (0.5);
        expectEquals (maxDiff (renderChain (morph::blend (m, 0.0f).chain, in), renderChain (a.chain, in)), 0.0f);
        expectEquals (maxDiff (renderChain (morph::blend (m, 1.0f).chain, in), renderChain (b.chain, in)), 0.0f);
        expect (maxDiff (renderChain (morph::blend (m, 0.5f).chain, in), renderChain (a.chain, in)) > 1.0e-3f, "in between it is neither");
        expect (allFinite (renderChain (morph::blend (m, 0.5f).chain, in)));
    }

    // ---------------------------------------------------------------------------------------------
    void identityAtEnds()
    {
        beginTest ("built-in presets: 魔王 with every built-in (both ways) - t=0 chain output == A's, t=1 == B's (合成音声で代用)");
        freshDataDir();
        auto c = makeController();
        const auto in = synthVoice (0.3);
        const auto* king = c->getPresetLibrary().find ("character-demon-king");
        int compared0 = 0, compared1 = 0, refused = 0, orderSkipped = 0;
        for (auto& other : c->getPresetLibrary().all())
        {
            if (! other.builtin) continue;
            for (int way = 0; way < 2; ++way)
            {
                const auto& a = way == 0 ? *king : other;
                const auto& b = way == 0 ? other : *king;
                const auto m = makeMorph (a, b);
                if (morph::checkBlend (morph::blend (m, 0.0f)).isNotEmpty())
                {
                    ++refused;
                    continue;
                }
                const auto label = juce::String (a.id + " x " + b.id);
                const float d0 = maxDiff (renderChain (morph::blend (m, 0.0f).chain, in), renderChain (a.chain, in));
                expect (d0 == 0.0f, label + " t=0 diff " + juce::String (d0, 9));
                ++compared0;
                if (! keepsBOrder (m.pairs))
                {
                    ++orderSkipped;
                    continue;
                }
                const float d1 = maxDiff (renderChain (morph::blend (m, 1.0f).chain, in), renderChain (b.chain, in));
                expect (d1 == 0.0f, label + " t=1 diff " + juce::String (d1, 9));
                ++compared1;
            }
        }
        logMessage ("t=0 compared " + juce::String (compared0) + ", t=1 compared " + juce::String (compared1) + ", B order differs "
                    + juce::String (orderSkipped) + ", refused " + juce::String (refused));
        expect (compared0 > 150 && compared1 > 100);
        c->shutdown();
    }

    // ---------------------------------------------------------------------------------------------
    void controller()
    {
        beginTest ("beginMorph / setMorphAmount: one rebuild, then live writes into the same chain; name, modified, not built-in");
        freshDataDir();
        auto c = makeController();
        auto& vp = c->getProcessorForTests();
        juce::String why;
        expect (! c->isMorphing());
        expect (c->beginMorph ("character-demon-king", "character-alien", why), why);
        expect (c->isMorphing());
        const auto& cur = c->getCurrentPreset();
        expectEquals (cur.name, juce::String::fromUTF8 ("魔王 × エイリアン"));
        expect (! cur.builtin && c->isCurrentPresetModified());
        expectEquals (c->getMorphAmount(), 0.0f);
        auto* chain = vp.getRequestedChain();
        expect (chain != nullptr && chain->size() == int (cur.chain.size()));
        const auto in = synthVoice (1.0);
        render (vp, in); // the blended chain is in

        c->setMorphAmount (0.5f);
        expect (c->isMorphing() && vp.getRequestedChain() == chain, "no rebuild while blending");
        expectEquals (c->getMorphAmount(), 0.5f);
        for (int i = 0; i < int (cur.chain.size()) && chain != nullptr; ++i)
        {
            const auto& s = cur.chain[size_t (i)];
            expectWithinAbsoluteError (chain->slot (i).wet.load(), s.wet, 1.0e-6f, s.type);
            for (size_t k = 0; k < s.params.size(); ++k) expectEquals (chain->slot (i).params[k].load(), s.params[k], s.type + " param");
        }
        // 魔王 pitch -9 + 1 voice, エイリアン: check the working preset follows the blend
        const auto* king = c->getPresetLibrary().find ("character-demon-king");
        const auto* alien = c->getPresetLibrary().find ("character-alien");
        expectWithinAbsoluteError (cur.pitchSt, 0.5f * (king->hasShifter ? king->pitchSt : 0.0f) + 0.5f * (alien->hasShifter ? alien->pitchSt : 0.0f), 1.0e-4f);

        c->setMorphAmount (0.55f);
        {
            std::vector<float> out (in.size(), 0.0f);
            long long n = 0;
            {
                AllocationCounter allocs;
                for (size_t pos = 0; pos + kBlock <= in.size(); pos += kBlock) vp.process (in.data() + pos, out.data() + pos, nullptr, kBlock);
                n = allocs.count();
            }
            expectEquals (n, 0LL, "no allocation on the audio thread");
            expect (allFinite (out) && rmsDb (out) > -60.0f, "finite, audible");
        }
        c->setMorphAmount (1.0f);
        expect (allFinite (render (vp, in)));

        beginTest ("isMorphing: false after another rebuild (effect added, preset load) or endMorph; setMorphAmount then does nothing");
        expect (c->addEffect ("eq", why), why);
        expect (! c->isMorphing());
        const auto before = c->getCurrentPreset();
        c->setMorphAmount (0.2f);
        expect (c->getCurrentPreset() == before && c->getMorphAmount() == 1.0f);
        expect (c->beginMorph ("space-cave", "character-ghost", why), why);
        c->loadPreset ("natural-asis");
        expect (! c->isMorphing());
        expect (c->beginMorph ("space-cave", "character-ghost", why), why);
        c->endMorph();
        expect (! c->isMorphing());
        c->shutdown();
    }

    // ---------------------------------------------------------------------------------------------
    void refusals()
    {
        beginTest ("refused: over 10 slots, 3 heavy ON, looper recording (E-27), unknown preset; the working preset unchanged");
        freshDataDir();
        auto c = makeController();
        const auto six1 = saveUser (*c, "six1", { slot ("compressor"), slot ("eq"), slot ("distortion"), slot ("bitcrusher"), slot ("tremolo"), slot ("slicer") });
        const auto six2 = saveUser (*c, "six2", { slot ("deesser"), slot ("enhancer"), slot ("peq"), slot ("saturator"), slot ("noise"), slot ("ringmod") });
        const auto heavy1 = saveUser (*c, "heavy1", { slot ("autopitch"), slot ("autopitch"), slot ("autopitch", false) });
        const auto heavy2 = saveUser (*c, "heavy2", { slot ("autopitch", false), slot ("autopitch", false), slot ("autopitch") });
        expect (! six1.empty() && ! six2.empty() && ! heavy1.empty() && ! heavy2.empty(), "test presets saved");
        const auto before = c->getCurrentPreset();
        juce::String why;
        expect (! c->beginMorph (six1, six2, why) && why.contains ("10"), why);
        why = {};
        expect (! c->beginMorph (heavy1, heavy2, why) && why.isNotEmpty(), why);
        why = {};
        expect (! c->beginMorph ("character-demon-king", "no-such-preset", why) && why.isNotEmpty());
        expect (c->getCurrentPreset() == before && ! c->isMorphing());
        expect (c->beginMorph (six1, heavy1, why), "6 + 3 slots, 2 heavy ON is fine: " + why);

        c->loadPreset ("character-demon-king");
        expect (c->addEffect ("looper", why), why);
        auto& vp = c->getProcessorForTests();
        const auto in = synthVoice (0.5);
        render (vp, in);
        c->triggerSlot (int (c->getChain().size()) - 1, EffectTrigger::looperRecordPlay);
        render (vp, in);
        expect (c->hasLooperRecording());
        const auto withLooper = c->getCurrentPreset();
        why = {};
        expect (! c->beginMorph ("space-cave", "character-ghost", why) && why.isNotEmpty(), "E-27");
        expect (c->getCurrentPreset() == withLooper);
        c->shutdown();
    }

    // ---------------------------------------------------------------------------------------------
    void saveAndReload()
    {
        beginTest ("save as new mid-blend: wet is saved; reading it back (and from disk) gives the same preset");
        freshDataDir();
        auto c = makeController();
        juce::String why;
        expect (c->beginMorph ("character-demon-king", "character-alien", why), why);
        c->setMorphAmount (0.3f);
        const auto blended = c->getCurrentPreset();
        bool partWet = false;
        for (auto& s : blended.chain) partWet = partWet || (s.enabled && s.wet < 1.0f);
        expect (partWet, "some slot is partly wet at 0.3");
        expect (serializePreset (blended).contains ("\"wet\""));
        expect (c->saveCurrentAsNew (juce::String::fromUTF8 ("混ぜた声"), why), why);
        const auto savedId = c->getCurrentPreset().id;
        PresetLibrary fromDisk (paths::presetsDir());
        fromDisk.reload();
        for (const auto* back : { c->getPresetLibrary().find (savedId), fromDisk.find (savedId) })
        {
            expect (back != nullptr);
            if (back == nullptr) continue;
            expect (! back->builtin && back->hasShifter == blended.hasShifter);
            expectWithinAbsoluteError (back->pitchSt, blended.pitchSt, 1.0e-3f);
            expectWithinAbsoluteError (back->formantSt, blended.formantSt, 1.0e-3f);
            expectWithinAbsoluteError (back->outputTrimDb, blended.outputTrimDb, 1.0e-3f);
            expect (back->layers.size() == blended.layers.size());
            for (size_t i = 0; i < std::min (back->layers.size(), blended.layers.size()); ++i)
            {
                expect (back->layers[i].enabled == blended.layers[i].enabled);
                expectWithinAbsoluteError (back->layers[i].levelDb, blended.layers[i].levelDb, 1.0e-3f);
            }
            expect (back->chain.size() == blended.chain.size());
            for (size_t i = 0; i < std::min (back->chain.size(), blended.chain.size()); ++i)
            {
                const auto& x = back->chain[i];
                const auto& y = blended.chain[i];
                expect (x.type == y.type && x.enabled == y.enabled, y.type);
                expectWithinAbsoluteError (x.wet, y.wet, 1.0e-4f, y.type + " wet");
                for (size_t k = 0; k < std::min (x.params.size(), y.params.size()); ++k)
                    expectWithinAbsoluteError (x.params[k], y.params[k], 1.0e-3f * std::max (1.0f, std::abs (y.params[k])), y.type + " param");
            }
        }
        c->shutdown();
    }

    // ---------------------------------------------------------------------------------------------
    void names()
    {
        beginTest ("name: \"A × B\", within 32 characters");
        expectEquals (morph::blendName (juce::String::fromUTF8 ("魔王"), juce::String::fromUTF8 ("エイリアン")), juce::String::fromUTF8 ("魔王 × エイリアン"));
        const auto longA = juce::String::repeatedString (juce::String::fromUTF8 ("あ"), 32);
        const auto n = morph::blendName (longA, juce::String::fromUTF8 ("洞窟"));
        expect (n.length() <= kPresetNameMaxChars && n.endsWith (juce::String::fromUTF8 (" × 洞窟")), n);
        const auto both = morph::blendName (longA, longA);
        expect (both.length() <= kPresetNameMaxChars && both.contains (juce::String::fromUTF8 (" × ")), both);
    }

    // ---------------------------------------------------------------------------------------------
    void tool()
    {
        using namespace ui;
        const bool wasOff = mainui::animationsOff();
        mainui::animationsOff() = true;
        beginTest ("混ぜる tool: every control inside the card without overlaps (wide and narrow), pick A / B, start, slider drives the blend");
        freshDataDir();
        auto c = makeController();
        MainComponent mc (*c);
        for (auto size : { juce::Point<int> (Theme::defaultWidth, Theme::defaultHeight), juce::Point<int> (Theme::minWidth, Theme::minHeight) })
        {
            c->loadPreset ("character-demon-king");
            mc.setSize (size.x, size.y);
            mc.showPage (Navigator::Page::tools);
            if (auto* v = dynamic_cast<ToolsView*> (mainui::findById (&mc, "page.tools"))) v->showTool (ToolsView::Tool::morph);
            const auto label = juce::String (size.x) + "x" + juce::String (size.y);
            auto* page = shown (&mc, "tools.morph");
            expect (page != nullptr, label);
            if (page == nullptr) continue;
            std::vector<juce::Component*> parts;
            for (auto* id : { "morph.a", "morph.b", "morph.start", "morph.amount", "morph.save" })
            {
                auto* comp = shown (page, id);
                expect (comp != nullptr, label + " " + id);
                if (comp == nullptr) continue;
                expect (page->getLocalBounds().contains (comp->getBounds()), label + " " + id + " inside");
                expect (comp->getHeight() >= Theme::touchMin, label + " " + id + " tall enough");
                for (auto* other : parts) expect (! other->getBounds().intersects (comp->getBounds()), label + " " + id + " overlaps " + other->getComponentID());
                parts.push_back (comp);
            }
            expect (page->getWidth() >= 400 && page->getHeight() >= 300, label + " card " + page->getLocalBounds().toString());
            if (parts.size() != 5) continue;

            auto* boxA = dynamic_cast<juce::ComboBox*> (parts[0]);
            auto* boxB = dynamic_cast<juce::ComboBox*> (parts[1]);
            auto* slider = dynamic_cast<juce::Slider*> (parts[3]);
            expect (boxA->getText() == juce::String::fromUTF8 ("魔王"), label + ": A starts at the current preset");
            expect (! slider->isEnabled() && ! parts[4]->isEnabled(), label + ": slider and save wait for the start");
            int items = 0;
            for (int k = 0; k < boxB->getNumItems(); ++k)
            {
                ++items;
                if (boxB->getItemText (k) == juce::String::fromUTF8 ("洞窟")) boxB->setSelectedItemIndex (k, juce::sendNotificationSync);
            }
            expect (items >= 81, label + ": every preset in the list");
            if (auto* start = dynamic_cast<juce::Button*> (parts[2]); start != nullptr && start->onClick) start->onClick();
            expect (c->isMorphing(), label + ": started");
            expect (slider->isEnabled() && parts[4]->isEnabled(), label + ": slider and save usable");
            slider->setValue (0.6, juce::sendNotificationSync);
            expectWithinAbsoluteError (c->getMorphAmount(), 0.6f, 1.0e-4f, label);
            c->loadPreset ("natural-asis");
            page->setVisible (false);
            page->setVisible (true); // refreshes like its timer does
            expect (! slider->isEnabled(), label + ": the blend ended with the preset load");
        }

        beginTest ("混ぜる tool rebuilt mid-blend (theme change): A and B come back");
        {
            juce::String why;
            expect (c->beginMorph ("character-demon-king", "space-cave", why), why);
            auto tool = makeMorphTool (*c, mc);
            auto* a = dynamic_cast<juce::ComboBox*> (mainui::findById (tool.get(), "morph.a"));
            auto* b = dynamic_cast<juce::ComboBox*> (mainui::findById (tool.get(), "morph.b"));
            expect (a != nullptr && a->getText() == juce::String::fromUTF8 ("魔王"));
            expect (b != nullptr && b->getText() == juce::String::fromUTF8 ("洞窟"));
        }
        c->shutdown();
        mainui::animationsOff() = wasOff;
    }
};

static MorphTests morphTests;
} // namespace koe

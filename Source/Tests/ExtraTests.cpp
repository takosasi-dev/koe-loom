// 聞き比べ (hold for the voice-changer-OFF sound) and おまかせ生成 (INTERFACES.md §9.4 / §9.5, owner wave7/extra).
// Category "Extra". No window, no audio device: AppController (false), MainComponent offscreen, synthetic speech
// (test::synthVoice; the peak test runs the chain alone through EffectChain::create, §9.5).

#include "App/AppController.h"
#include "Core/Paths.h"
#include "Effects/EffectRegistry.h"
#include "Engine/EffectChain.h"
#include "Tests/TestUtil.h"
#include "UI/MainComponent.h"
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

std::unique_ptr<AppController> makeController (int layoutStyle = 0)
{
    auto c = std::make_unique<AppController> (false);
    c->startup();
    c->updateSettings ([layoutStyle] (Settings& s)
    {
        s.setupDone = true;
        s.tourStep = 7;
        s.layoutStyle = layoutStyle;
    });
    c->setVoiceChangerOn (true);
    c->loadPreset ("character-demon-king"); // 魔王: pitch -9, 4 slots, 1 voice
    c->dispatchPendingMessages();
    return c;
}

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

juce::MouseEvent mouseAt (juce::Component* comp)
{
    const auto p = comp->getLocalBounds().getCentre().toFloat();
    const auto now = juce::Time::getCurrentTime();
    return juce::MouseEvent (juce::Desktop::getInstance().getMainMouseSource(), p, juce::ModifierKeys::leftButtonModifier, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f,
                             comp, comp, now, p, now, 1, false);
}
} // namespace

class ExtraTests : public juce::UnitTest
{
public:
    ExtraTests() : juce::UnitTest ("Compare and random voice (wave7/extra)", "Extra") {}

    void runTest() override
    {
        randomRules();
        randomThousandSeeds();
        randomRefusedWithLooperRecording();
        compareHold();
        buttons();
    }

private:
    // ---------------------------------------------------------------------------------------------
    void randomRules()
    {
        beginTest ("おまかせ: same seed -> same voice; converter ON, pitch / formant / 2..4 allowed distinct effects / 0..1 voice; modified");
        freshDataDir();
        auto c = makeController();
        const bool voiceWasOn = c->isVoiceChangerOn();
        juce::String why;
        expect (c->randomizeCurrent (42, why), why);
        const auto first = c->getCurrentPreset();
        expect (c->isCurrentPresetModified(), "the result is the working preset, marked modified");
        expect (c->randomizeCurrent (43, why), why);
        expect (! (c->getCurrentPreset() == first), "another seed, another voice");
        expect (c->randomizeCurrent (42, why), why);
        expect (c->getCurrentPreset() == first, "same seed, same voice");
        expect (c->isVoiceChangerOn() == voiceWasOn, "the global voice changer switch is left alone (§9.5)");

        int seen2 = 0, seen4 = 0, layers = 0;
        for (uint32_t seed = 0; seed < 200; ++seed)
        {
            expect (c->randomizeCurrent (seed, why), why);
            const auto& p = c->getCurrentPreset();
            expect (p.hasShifter, "converter ON (card 02)");
            expect (p.pitchSt >= -8.0f && p.pitchSt <= 8.0f && p.formantSt >= -4.0f && p.formantSt <= 4.0f);
            expect (p.layers.size() <= 1);
            layers += int (p.layers.size());
            expect (p.chain.size() >= 2 && p.chain.size() <= 4, juce::String (int (p.chain.size())));
            seen2 += p.chain.size() == 2;
            seen4 += p.chain.size() == 4;
            std::set<std::string> types;
            for (auto& s : p.chain)
            {
                auto* info = findEffectInfo (s.type);
                expect (info != nullptr && info->weight != EffectWeight::heavy, s.type);
                expect (s.type != "freeze" && s.type != "looper" && s.type != "convolution", s.type);
                expect (types.insert (s.type).second, "one of each type: " + juce::String (s.type));
                for (size_t k = 0; info != nullptr && k < info->params.size(); ++k)
                {
                    const auto& spec = info->params[k];
                    const float v = s.params[k];
                    expect (spec.clamp (v) == v, juce::String (s.type) + "." + spec.id);
                    if (juce::String (spec.id).containsIgnoreCase ("mix")) expect (v >= spec.min + (spec.max - spec.min) * 0.2f - 1.0e-4f && v <= spec.min + (spec.max - spec.min) * 0.7f + 1.0e-4f);
                    if (juce::String (spec.id).containsIgnoreCase ("feedback")) expect (v <= spec.max * 0.6f + 1.0e-4f);
                }
            }
        }
        expect (seen2 > 0 && seen4 > 0, "both 2 and 4 effects happen");
        expect (layers > 20 && layers < 180, "a voice about half the time: " + juce::String (layers));
        c->shutdown();
    }

    void randomThousandSeeds()
    {
        beginTest ("おまかせ x1000 seeds: reload with 0 corrections; synthetic speech through the chain stays finite and <= +6 dBFS (合成音声で代用)");
        freshDataDir();
        auto c = makeController();
        const auto voice = synthVoice (1.0);
        std::vector<float> buf (voice.size());
        int corrected = 0, notFinite = 0, loud = 0;
        float worst = -100.0f;
        juce::String why;
        for (uint32_t seed = 1000; seed < 2000; ++seed)
        {
            expect (c->randomizeCurrent (seed, why), why);
            const auto& p = c->getCurrentPreset();
            PresetLoadReport report;
            const auto back = parsePreset (serializePreset (p), report);
            if (! back || report.hasNotices())
            {
                ++corrected;
                logMessage ("seed " + juce::String (seed) + ": " + report.toJapanese());
            }
            auto chain = EffectChain::create (p.chain, kSr, kBlock);
            buf = voice;
            for (size_t pos = 0; pos + kBlock <= buf.size(); pos += kBlock) chain->process (buf.data() + pos, kBlock);
            if (! allFinite (buf)) ++notFinite;
            const float peak = peakDb (buf);
            worst = std::max (worst, peak);
            if (peak > 6.0f)
            {
                ++loud;
                logMessage ("seed " + juce::String (seed) + " peak " + juce::String (peak, 1) + " dBFS");
            }
        }
        expectEquals (corrected, 0);
        expectEquals (notFinite, 0);
        expectEquals (loud, 0);
        logMessage ("おまかせ x1000: loudest chain output " + juce::String (worst, 1) + " dBFS (合成音声で代用、変換器と重ねる声は含まない)");
        c->shutdown();
    }

    void randomRefusedWithLooperRecording()
    {
        beginTest ("おまかせ: refused while the looper has a recording (E-27), the working preset unchanged");
        freshDataDir();
        auto c = makeController();
        juce::String why;
        expect (c->addEffect ("looper", why), why);
        auto& vp = c->getProcessorForTests();
        const auto in = synthVoice (0.5);
        render (vp, in); // the new chain is in
        const int looper = int (c->getChain().size()) - 1;
        c->triggerSlot (looper, EffectTrigger::looperRecordPlay);
        render (vp, in);
        expect (c->hasLooperRecording(), "the looper is recording");
        const auto before = c->getCurrentPreset();
        why = {};
        expect (! c->randomizeCurrent (7, why));
        expect (why.isNotEmpty(), "a Japanese reason for the toast");
        expect (c->getCurrentPreset() == before);
        c->shutdown();
    }

    // ---------------------------------------------------------------------------------------------
    void compareHold()
    {
        beginTest ("聞き比べ: while held the output equals voice changer OFF; released it comes back; never saved; a preset load releases it");
        freshDataDir();
        auto held = makeController();
        auto off = makeController();
        const auto in = synthVoice (2.0, 7, kSr, false);
        held->setCompareHold (true);
        off->setVoiceChangerOn (false);
        expect (held->isCompareHeld() && held->isVoiceChangerOn(), "held, and the setting still says ON");
        expect (! held->isCurrentPresetModified(), "holding does not mark the preset modified");
        const auto a = render (held->getProcessorForTests(), in);
        const auto b = render (off->getProcessorForTests(), in);
        expect (allFinite (a) && rmsDb (a) > -40.0f, "the dry voice comes out");
        const float d = maxDiff (a, b, size_t (kSr * 0.2));
        expect (d < 1.0e-5f, "same as voice changer OFF, max diff " + juce::String (d, 8));

        held->setCompareHold (false);
        off->setVoiceChangerOn (true);
        const auto a2 = render (held->getProcessorForTests(), in);
        const auto b2 = render (off->getProcessorForTests(), in);
        expect (maxDiff (a2, b2, size_t (kSr * 0.5)) < 1.0e-5f, "released: same as switching the voice changer back ON");
        expect (maxDiff (a2, b, size_t (kSr * 0.5)) > 0.01f, "and no longer the dry voice");

        held->setCompareHold (true);
        for (int i = 0; i < 60; ++i) held->tickForTests(); // the settings debounce runs
        expect (held->getSettings().voiceChangerOn, "the saved switch stays ON");
        held->loadPreset ("natural-asis");
        expect (! held->isCompareHeld(), "a preset load releases 聞き比べ");
        held->setCompareHold (true);
        held->shutdown();
        off->shutdown();
        AppController again (false);
        again.startup();
        expect (! again.isCompareHeld() && again.isVoiceChangerOn(), "nothing of the hold was saved");
        again.shutdown();
    }

    // ---------------------------------------------------------------------------------------------
    void buttons()
    {
        using namespace ui;
        const bool wasOff = mainui::animationsOff();
        mainui::animationsOff() = true;
        for (int style : { 0, 1, 2 })
        {
            const auto look = juce::String ("layout ") + juce::String (style);
            beginTest ("聞き比べ / おまかせ buttons (" + look + "): mouse and Space hold, focus loss releases, OFF disables, おまかせ changes the voice");
            freshDataDir();
            auto c = makeController (style);
            MainComponent mc (*c);
            for (auto size : { juce::Point<int> (Theme::defaultWidth, Theme::defaultHeight), juce::Point<int> (Theme::minWidth, Theme::minHeight) })
            {
                mc.setSize (size.x, size.y);
                const auto label = look + " " + juce::String (size.x) + "x" + juce::String (size.y);
                auto* compare = shown (&mc, "voice.compare");
                auto* random = shown (&mc, "voice.random");
                expect (compare != nullptr && random != nullptr, label + ": both buttons");
                if (compare == nullptr || random == nullptr) continue;
                expect (compare->getWidth() >= Theme::touchMin && compare->getHeight() >= Theme::touchMin, label + ": compare is big enough to press");
                expect (random->getWidth() >= Theme::touchMin && random->getHeight() >= Theme::touchMin, label + ": random is big enough to press");
                expect (dynamic_cast<juce::SettableTooltipClient*> (compare)->getTooltip().isNotEmpty()
                        && dynamic_cast<juce::SettableTooltipClient*> (random)->getTooltip().isNotEmpty(), label + ": tooltips");

                compare->mouseDown (mouseAt (compare));
                expect (c->isCompareHeld(), label + ": held while the mouse is down");
                compare->mouseUp (mouseAt (compare));
                expect (! c->isCompareHeld(), label + ": released with the mouse");

                expect (compare->keyPressed (juce::KeyPress (juce::KeyPress::spaceKey)), label + ": Space is taken");
                expect (c->isCompareHeld(), label + ": held while Space is down");
                compare->keyStateChanged (false); // Space is not down (no real keyboard in tests)
                expect (! c->isCompareHeld(), label + ": released with Space");

                compare->keyPressed (juce::KeyPress (juce::KeyPress::spaceKey));
                compare->focusLost (juce::Component::focusChangedDirectly); // the window lost focus
                expect (! c->isCompareHeld(), label + ": focus loss releases");
            }

            c->setVoiceChangerOn (false);
            c->dispatchPendingMessages();
            auto* compare = shown (&mc, "voice.compare");
            expect (compare != nullptr && ! compare->isEnabled(), look + ": not pressable while the voice changer is OFF");
            if (compare != nullptr) compare->keyPressed (juce::KeyPress (juce::KeyPress::spaceKey));
            expect (! c->isCompareHeld(), look + ": no hold while OFF");
            c->setVoiceChangerOn (true);
            c->dispatchPendingMessages();
            expect (compare != nullptr && compare->isEnabled(), look + ": pressable again");

            if (compare != nullptr) compare->keyPressed (juce::KeyPress (juce::KeyPress::spaceKey));
            c->loadPreset ("natural-asis");
            c->dispatchPendingMessages();
            expect (! c->isCompareHeld(), look + ": a preset load releases it");
            if (compare != nullptr) compare->keyStateChanged (false); // the button follows without calling back

            const auto before = c->getCurrentPreset();
            if (auto* b = dynamic_cast<juce::Button*> (shown (&mc, "voice.random")); b != nullptr && b->onClick) b->onClick();
            expect (! (c->getCurrentPreset() == before) && c->isCurrentPresetModified(), look + ": おまかせ makes a new, modified voice");
        }
        mainui::animationsOff() = wasOff;
    }
};

static ExtraTests extraTests;
} // namespace koe

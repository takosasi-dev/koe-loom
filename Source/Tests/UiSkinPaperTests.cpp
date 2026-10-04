// 案 B "Paper" S-01 checks (wave5/paper, INTERFACES.md §8.3). Category "UiSkin". No window, no audio:
// AppController (false) and MainComponent stay offscreen; popup menus are never opened.

#include "App/AppController.h"
#include "Core/Paths.h"
#include "UI/MainComponent.h"
#include "UI/Screens.h"
#include "UI/main/GuideTour.h"
#include "UI/main/Panels.h"
#include "UI/main/Shell.h"
#include "UI/skins/paper/PaperChain.h"

#include <set>

namespace koe::ui
{
namespace
{
using namespace mainui;

std::unique_ptr<AppController> makePaperController (int layoutStyle = 1)
{
    auto d = paths::dataDir();
    if (d.getFullPathName().contains ("KoeLoomTests")) d.deleteRecursively();
    d.createDirectory();
    auto c = std::make_unique<AppController> (false);
    c->startup();
    c->updateSettings ([layoutStyle] (Settings& s)
    {
        s.setupDone = true;
        s.tourStep = 7;
        s.layoutStyle = layoutStyle;
        s.favorites = juce::StringArray { "natural-asis", "character-demon-king" };
    });
    c->loadPreset ("character-demon-king"); // 魔王: 4 slots, 1 voice
    c->dispatchPendingMessages();
    return c;
}

juce::Component* shown (juce::Component* root, const juce::String& id)
{
    std::function<juce::Component* (juce::Component*)> find = [&] (juce::Component* comp) -> juce::Component*
    {
        if (comp != root && ! comp->isVisible()) return nullptr;
        if (comp->getComponentID() == id && visibleWithin (comp, root)) return comp;
        for (auto* child : comp->getChildren())
            if (auto* f = find (child)) return f;
        return nullptr;
    };
    return find (root);
}

juce::Rectangle<int> areaIn (juce::Component& root, juce::Component* comp) { return root.getLocalArea (comp, comp->getLocalBounds()); }

/** The part of comp that is on screen: clipped by every parent (the chain scrolls). */
juce::Rectangle<int> clippedIn (juce::Component& root, juce::Component* comp)
{
    auto r = areaIn (root, comp);
    for (auto* p = comp->getParentComponent(); p != nullptr && p != &root; p = p->getParentComponent()) r = r.getIntersection (areaIn (root, p));
    return r;
}

void press (juce::Component* comp)
{
    if (auto* b = dynamic_cast<juce::Button*> (comp); b != nullptr && b->onClick) b->onClick();
}

juce::Component* overlayPanel (MainComponent& mc) { return dynamic_cast<OverlayHost*> (findById (&mc, "main.overlay"))->panel(); }

Notice banner (const char* key, NoticeLevel level)
{
    Notice n;
    n.key = key;
    n.level = level;
    n.text = juce::String::fromUTF8 ("テスト用の警告です");
    return n;
}
} // namespace

class UiSkinPaperTests : public juce::UnitTest
{
public:
    UiSkinPaperTests() : juce::UnitTest ("UI skin: Paper S-01 (wave5/paper)", "UiSkin") {}

    void runTest() override
    {
        const bool wasOff = animationsOff();
        animationsOff() = true;
        layout();
        actions();
        tour();
        switching();
        animationsOff() = wasOff;
    }

private:
    static constexpr const char* kRequired[] = { "main.header", "tour.mute", "tour.voiceToggle", "header.help", "tour.presets", "voice.presetSelector",
                                                 "voice.presetList", "voice.favorites", "voice.presetSave", "voice.presetDuplicate", "tour.pitch",
                                                 "tour.formant", "voice.shifterToggle", "voice.inputDevice", "voice.inputMeter", "tour.outputMeter",
                                                 "voice.outputDeviceLink", "tour.chain", "chain.slot.0", "chain.slot.0.toggle", "chain.add",
                                                 "voice.bottom", "tour.monitor", "voice.monitorToggle", "tour.status", "voice.input", "voice.output",
                                                 "voice.shifter", "voice.layers", "voice.layer.0", "voice.layer.0.toggle", "voice.compare", "voice.random" };

    void checkLayout (MainComponent& mc, const juce::String& label, bool expectPath)
    {
        const auto root = mc.getLocalBounds();
        for (auto* id : kRequired)
        {
            auto* comp = shown (&mc, id);
            expect (comp != nullptr, label + ": missing or hidden " + id);
            if (comp != nullptr)
            {
                const auto b = clippedIn (mc, comp);
                expect (root.contains (b) && ! b.isEmpty(), label + ": outside the window " + id + " " + b.toString());
            }
        }
        expect (! expectPath || shown (&mc, "paper.path") != nullptr, label + ": the signal path strip is shown");

        auto noOverlap = [&] (std::initializer_list<const char*> ids, const char* containerId)
        {
            auto* container = containerId != nullptr ? shown (&mc, containerId) : nullptr;
            std::vector<std::pair<juce::String, juce::Rectangle<int>>> rects;
            for (auto* id : ids)
                if (auto* comp = shown (&mc, id))
                {
                    rects.emplace_back (id, areaIn (mc, comp));
                    if (container != nullptr) expect (areaIn (mc, container).contains (rects.back().second), label + ": " + id + " sticks out of " + containerId);
                }
            for (size_t i = 0; i < rects.size(); ++i)
                for (size_t j = i + 1; j < rects.size(); ++j)
                    expect (! rects[i].second.intersects (rects[j].second), label + ": " + rects[i].first + " overlaps " + rects[j].first);
        };
        noOverlap ({ "main.header", "main.notices", "paper.path", "tour.presets", "voice.input", "voice.shifter", "voice.layers", "voice.output",
                     "tour.chain", "voice.bottom" }, nullptr);
        noOverlap ({ "voice.presetSelector", "voice.presetList", "voice.favorites", "voice.presetSave", "voice.presetDuplicate", "voice.compare", "voice.random" },
                   "tour.presets");
        noOverlap ({ "tour.pitch", "tour.formant", "voice.shifterToggle" }, "voice.shifter");
        noOverlap ({ "voice.layerAdd", "voice.layersResume", "voice.layer.0", "voice.layer.1" }, "voice.layers");
        noOverlap ({ "voice.inputDevice", "voice.inputMeter" }, "voice.input");
        noOverlap ({ "tour.outputMeter", "voice.outputDeviceLink" }, "voice.output");
        noOverlap ({ "tour.monitor", "tour.status" }, "voice.bottom");

        // every control on the page: on screen parts never overlap one another
        auto* page = shown (&mc, "page.voice");
        std::vector<juce::Component*> controls;
        std::function<void (juce::Component*)> collect = [&] (juce::Component* comp)
        {
            if (! comp->isVisible()) return;
            if (dynamic_cast<juce::Button*> (comp) != nullptr || dynamic_cast<juce::Slider*> (comp) != nullptr || dynamic_cast<juce::ComboBox*> (comp) != nullptr)
            {
                if (! clippedIn (mc, comp).isEmpty()) controls.push_back (comp);
                return;
            }
            for (auto* child : comp->getChildren()) collect (child);
        };
        if (page != nullptr) collect (page);
        expect (controls.size() > 25, label + ": controls found " + juce::String (int (controls.size())));
        for (size_t i = 0; i < controls.size(); ++i)
            for (size_t j = i + 1; j < controls.size(); ++j)
                expect (! clippedIn (mc, controls[i]).intersects (clippedIn (mc, controls[j])),
                        label + ": " + controls[i]->getTitle() + controls[i]->getComponentID() + " overlaps " + controls[j]->getTitle() + controls[j]->getComponentID());

        // the slots keep a parameter bar and their ON/OFF row, and their parts stay inside
        if (auto* slot = shown (&mc, "chain.slot.0"))
        {
            const bool cp = mc.getWidth() < Theme::narrowWidth;
            expect (slot->getHeight() >= paper::PaperSlotCard::height (cp, 1), label + ": slot too short " + juce::String (slot->getHeight()));
            for (auto* child : slot->getChildren())
                if (child->isVisible()) expect (areaIn (mc, slot).contains (areaIn (mc, child)), label + ": slot part outside the slot");
        }
    }

    void layout()
    {
        beginTest ("Paper S-01: every part present, inside and not overlapping, wide and 800x560, with banners and 2 voices (§8.3, AC-59)");
        auto c = makePaperController();
        MainComponent mc (*c);
        expect (shown (&mc, "paper.path") != nullptr, "the Paper page is built for layoutStyle 1");
        const juce::Point<int> sizes[] = { { Theme::defaultWidth, Theme::defaultHeight }, { Theme::minWidth, Theme::minHeight },
                                           { Theme::narrowWidth - 1, Theme::minHeight }, { Theme::narrowWidth, Theme::minHeight + 80 },
                                           { 1600, 1000 } };
        for (auto size : sizes)
        {
            mc.setSize (size.x, size.y);
            const auto label = juce::String (size.x) + "x" + juce::String (size.y);
            checkLayout (mc, label, true);

            std::unique_ptr<juce::ComponentTraverser> traverser (mc.createKeyboardFocusTraverser());
            std::set<juce::Component*> seen;
            for (auto* comp = traverser->getDefaultComponent (&mc); comp != nullptr && seen.insert (comp).second; comp = traverser->getNextComponent (comp)) {}
            for (auto* id : { "voice.presetSelector", "voice.presetList", "voice.favorites", "voice.presetSave", "voice.presetDuplicate", "voice.shifterToggle",
                              "voice.layer.0", "voice.layer.0.toggle", "voice.layerAdd", "voice.inputDevice", "voice.outputDeviceLink", "chain.slot.0",
                              "chain.slot.0.toggle", "chain.add", "voice.monitorToggle" })
                expect (seen.count (shown (&mc, id)) == 1, label + ": Tab does not reach " + id);
            auto* pitch = shown (&mc, "tour.pitch");
            expect (pitch != nullptr && seen.count (pitch->getChildComponent (0)) == 1, label + ": Tab does not reach the pitch slider");
        }

        juce::String why;
        c->addLayer (why);
        c->dispatchPendingMessages();
        auto* bar = dynamic_cast<NoticeBar*> (findById (&mc, "main.notices"));
        for (int banners : { 0, 2 })
        {
            bar->setNotices (banners == 0 ? std::vector<Notice>() : std::vector<Notice> { banner ("a", NoticeLevel::danger), banner ("b", NoticeLevel::warning) });
            for (auto size : { juce::Point<int> (Theme::defaultWidth, Theme::defaultHeight), juce::Point<int> (Theme::minWidth, Theme::minHeight) })
            {
                mc.setSize (size.x, size.y);
                const auto label = juce::String (size.x) + "x" + juce::String (size.y) + " 2 voices " + juce::String (banners) + " banners";
                checkLayout (mc, label, banners == 0);
                expect (shown (&mc, "voice.layer.1") != nullptr && shown (&mc, "voice.layerAdd") == nullptr, label + ": both voices, no add link at 2");
            }
        }
        bar->setNotices ({});

        beginTest ("Paper S-01: an empty chain and no voices still lay out");
        c->loadPreset ("natural-asis");
        c->dispatchPendingMessages();
        for (auto size : { juce::Point<int> (Theme::defaultWidth, Theme::defaultHeight), juce::Point<int> (Theme::minWidth, Theme::minHeight) })
        {
            mc.setSize (size.x, size.y);
            auto* chain = dynamic_cast<paper::PaperChain*> (shown (&mc, "tour.chain"));
            expect (chain != nullptr && chain->numCards() == 0);
            expect (shown (&mc, "chain.add") != nullptr && shown (&mc, "voice.layerAdd") != nullptr && shown (&mc, "voice.layer.0") == nullptr);
        }
    }

    void actions()
    {
        beginTest ("Paper S-01: the controls reach AppController (converter, voices, pitch, formant, slots, presets, output link)");
        auto c = makePaperController();
        MainComponent mc (*c);
        for (auto size : { juce::Point<int> (Theme::defaultWidth, Theme::defaultHeight), juce::Point<int> (Theme::minWidth, Theme::minHeight) })
        {
            mc.setSize (size.x, size.y);
            auto* shifter = dynamic_cast<juce::Button*> (shown (&mc, "voice.shifterToggle"));
            expect (shifter != nullptr && shifter->getToggleState());
            shifter->setToggleState (false, juce::sendNotificationSync);
            expect (! c->hasShifter(), "変換 OFF reaches setShifterEnabled");
            shifter->setToggleState (true, juce::sendNotificationSync);
            expect (c->hasShifter());

            auto* layer = dynamic_cast<juce::Button*> (shown (&mc, "voice.layer.0.toggle"));
            expect (layer != nullptr && layer->getToggleState());
            layer->setToggleState (false, juce::sendNotificationSync);
            expect (! c->getLayer (0).enabled, "声 1 OFF reaches setLayerEnabled");
            layer->setToggleState (true, juce::sendNotificationSync);
            c->dispatchPendingMessages();
        }

        auto* pitch = dynamic_cast<juce::Slider*> (shown (&mc, "tour.pitch")->getChildComponent (0));
        auto* formant = dynamic_cast<juce::Slider*> (shown (&mc, "tour.formant")->getChildComponent (0));
        expect (pitch != nullptr && formant != nullptr);
        expect (std::abs (pitch->getValue() - double (c->getPitch())) < 1.0e-3, "the slider shows the preset's pitch " + juce::String (pitch->getValue()));
        pitch->setValue (3.5, juce::sendNotificationSync);
        formant->setValue (-2.0, juce::sendNotificationSync);
        expect (std::abs (c->getPitch() - 3.5f) < 1.0e-3f && std::abs (c->getFormant() + 2.0f) < 1.0e-3f, "pitch / formant reach the controller");
        c->dispatchPendingMessages();
        expect (pitch->getMinimum() == double (kPitchSt.min) && pitch->getMaximum() == double (kPitchSt.max), "pitch range -12..12");

        beginTest ("Paper S-01: slots: ON/OFF, a bar slider, move, remove, add opens S-07, a click opens S-09 (F-04)");
        const auto first = c->getChain()[0].type, second = c->getChain()[1].type;
        auto* slotToggle = dynamic_cast<juce::Button*> (shown (&mc, "chain.slot.1.toggle"));
        slotToggle->setToggleState (false, juce::sendNotificationSync);
        expect (! c->getChain()[1].enabled, "slot ON/OFF reaches setSlotEnabled");
        auto* card = dynamic_cast<paper::PaperSlotCard*> (shown (&mc, "chain.slot.0"));
        expect (card != nullptr && card->bars.size() > 0);
        if (card != nullptr && card->bars.size() > 0)
        {
            const auto before = c->getChain()[0].params;
            auto* b = card->bars[0];
            b->setValue (b->getValue() == b->getMaximum() ? b->getMinimum() : b->getMaximum(), juce::sendNotificationSync);
            expect (c->getChain()[0].params != before, "the bar reaches setSlotParam");
        }
        for (auto* child : shown (&mc, "chain.slot.0")->getChildren())
            if (child->getTitle() == juce::String::fromUTF8 ("右へ")) press (child);
        c->dispatchPendingMessages();
        expect (c->getChain()[0].type == second && c->getChain()[1].type == first, "▶ moves the slot to the right");
        const auto size = c->getChain().size();
        for (auto* child : shown (&mc, "chain.slot.0")->getChildren())
            if (child->getTitle() == juce::String::fromUTF8 ("削除")) press (child);
        c->dispatchPendingMessages();
        expectEquals (int (c->getChain().size()), int (size) - 1, "× removes the slot");
        press (shown (&mc, "chain.add"));
        expect (dynamic_cast<EffectPicker*> (overlayPanel (mc)) != nullptr, "+ エフェクトを追加 opens S-07");
        mc.closeOverlay();
        auto* slot = shown (&mc, "chain.slot.0");
        slot->keyPressed (juce::KeyPress (juce::KeyPress::returnKey));
        expect (dynamic_cast<SlotDetailPanel*> (overlayPanel (mc)) != nullptr, "Enter on a slot opens S-09");
        mc.closeOverlay();

        beginTest ("Paper S-01: voices: add, the row opens its editor, the editor removes it (F-02-7)");
        press (shown (&mc, "voice.layerAdd"));
        expectEquals (c->getNumLayers(), 2);
        c->dispatchPendingMessages();
        expect (shown (&mc, "voice.layer.1") != nullptr && shown (&mc, "voice.layerAdd") == nullptr);
        shown (&mc, "voice.layer.1")->keyPressed (juce::KeyPress (juce::KeyPress::returnKey));
        auto* panel = overlayPanel (mc);
        expect (panel != nullptr && dynamic_cast<PanelBase*> (panel) != nullptr, "the row opens the voice editor");
        press (findById (panel, "voice.layerPanel.remove"));
        c->dispatchPendingMessages();
        expectEquals (c->getNumLayers(), 1, "the editor's × removes the voice");
        mc.closeOverlay();

        beginTest ("Paper S-01: preset links: 一覧 opens S-06, 保存 and 複製 ask for a name, the output link opens S-03 devices (F-01-11)");
        press (shown (&mc, "voice.presetList"));
        expect (dynamic_cast<PresetBrowser*> (overlayPanel (mc)) != nullptr);
        mc.closeOverlay();
        press (shown (&mc, "voice.presetSave"));
        expect (dynamic_cast<ConfirmPanel*> (overlayPanel (mc)) != nullptr, "a built-in preset is saved under a new name");
        mc.closeOverlay();
        press (shown (&mc, "voice.presetDuplicate"));
        expect (dynamic_cast<ConfirmPanel*> (overlayPanel (mc)) != nullptr);
        mc.closeOverlay();
        c->loadPreset ("natural-asis");
        c->dispatchPendingMessages();
        expect (shown (&mc, "chain.slot.0") == nullptr, "a preset change rebuilds the chain");
        press (shown (&mc, "voice.outputDeviceLink"));
        expect (shown (&mc, "page.settings") != nullptr && shown (&mc, "page.voice") == nullptr);
    }

    void tour()
    {
        beginTest ("Paper S-01: the tour finds every part on the page and runs to the end (§8.3, AC-52)");
        auto c = makePaperController();
        MainComponent mc (*c);
        for (auto size : { juce::Point<int> (Theme::minWidth, Theme::minHeight), juce::Point<int> (Theme::defaultWidth, Theme::defaultHeight) })
        {
            mc.setSize (size.x, size.y);
            mc.startTour (false);
            auto* t = dynamic_cast<GuideTour*> (findById (&mc, "main.tour"));
            expect (t != nullptr && t->isVisible() && t->currentStep() == 0);
            std::set<int> visited { 0 };
            int guard = 0;
            while (t->isVisible() && ++guard < 20)
            {
                t->next();
                if (t->isVisible()) visited.insert (t->currentStep());
            }
            expect (! t->isVisible());
            // the steps on the voice page find their parts here (a settings step may skip on its own, E-32)
            for (int i = 0; i < int (GuideTour::steps().size()); ++i)
                if (GuideTour::steps()[size_t (i)].page == Navigator::Page::voice)
                    expect (visited.count (i) == 1, "voice page step " + juce::String (i + 1) + " was skipped (part not found)");
            expectEquals (c->getSettings().tourStep, 7);
            expect (shown (&mc, "page.voice") != nullptr && shown (&mc, "paper.path") != nullptr, "back on the Paper voice page");
        }
    }

    void switching()
    {
        beginTest ("Paper S-01: S-03 「画面の配置」 switches the page; light and dark both build");
        auto c = makePaperController (0);
        MainComponent mc (*c);
        mc.setSize (Theme::defaultWidth, Theme::defaultHeight);
        expect (shown (&mc, "paper.path") == nullptr, "Studio by default");
        c->updateSettings ([] (Settings& s) { s.layoutStyle = 1; });
        c->dispatchPendingMessages();
        expect (shown (&mc, "paper.path") != nullptr, "Paper after the switch");
        for (const bool dark : { false, true })
        {
            c->updateSettings ([dark] (Settings& s) { s.darkTheme = dark; });
            c->dispatchPendingMessages();
            checkLayout (mc, dark ? "dark" : "light", true);
            expect (mc.createComponentSnapshot (mc.getLocalBounds()).isValid());
        }
        c->updateSettings ([] (Settings& s) { s.layoutStyle = 0; });
        c->dispatchPendingMessages();
        expect (shown (&mc, "paper.path") == nullptr && shown (&mc, "tour.pitch") != nullptr, "back to Studio");
    }
};

static UiSkinPaperTests uiSkinPaperTests;
} // namespace koe::ui

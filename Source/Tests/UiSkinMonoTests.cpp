// 案 C Mono S-01 checks (wave5/mono, INTERFACES.md §8.3). Category "UiMain". No window, no audio: AppController (false)
// and MainComponent stay offscreen, Settings::layoutStyle = 2.

#include "App/AppController.h"
#include "Core/Paths.h"
#include "UI/MainComponent.h"
#include "UI/main/GuideTour.h"
#include "UI/main/Panels.h"
#include "UI/main/Shell.h"
#include "UI/main/VoicePage.h"
#include "UI/skins/mono/MonoParts.h"

#include <algorithm>
#include <set>

namespace koe::ui
{
namespace
{
using namespace mainui;

const juce::StringArray kMonoFavourites { "natural-asis", "character-demon-king", "character-robot" };

std::unique_ptr<AppController> makeMonoController (int layoutStyle = 2)
{
    if (auto d = paths::dataDir(); d.getFullPathName().contains ("KoeLoomTests")) d.deleteRecursively();
    paths::dataDir().createDirectory();
    auto c = std::make_unique<AppController> (false);
    c->startup();
    c->updateSettings ([layoutStyle] (Settings& s)
    {
        s.setupDone = true;
        s.tourStep = 7;
        s.favorites = kMonoFavourites;
        s.layoutStyle = layoutStyle;
    });
    c->loadPreset ("character-demon-king"); // 4 slots, 1 voice
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

void press (juce::Component* comp)
{
    if (auto* b = dynamic_cast<juce::Button*> (comp); b != nullptr && b->onClick) b->onClick();
}

Notice testNotice (const char* key, NoticeLevel level)
{
    Notice n;
    n.key = key;
    n.level = level;
    n.text = juce::String::fromUTF8 ("テスト用の警告です");
    return n;
}
} // namespace

class UiSkinMonoTests : public juce::UnitTest
{
public:
    UiSkinMonoTests() : juce::UnitTest ("UI layout C Mono S-01 (wave5/mono)", "UiMain") {}

    void runTest() override
    {
        const bool wasOff = animationsOff();
        animationsOff() = true;
        idsAndHeader();
        layout();
        selectedIsNotOn();
        actions();
        presetsList();
        tour();
        animationsOff() = wasOff;
    }

private:
    static constexpr const char* kContractIds[] = { "tour.presets", "voice.presetSelector", "tour.pitch", "tour.formant", "voice.shifterToggle",
                                                    "voice.inputMeter", "tour.outputMeter", "voice.outputDeviceLink", "tour.chain", "chain.slot.0",
                                                    "chain.slot.0.toggle", "voice.bottom", "tour.monitor", "voice.monitorToggle", "tour.status",
                                                    "voice.input", "voice.output", "voice.shifter", "voice.layers",
                                                    // the page's own header (§8.3: 案 C)
                                                    "tour.mute", "tour.voiceToggle", "mono.settings", "mono.help", "mono.soundboard" };

    static std::vector<juce::Point<int>> sizes()
    {
        return { { Theme::defaultWidth, Theme::defaultHeight }, { Theme::minWidth, Theme::minHeight }, { Theme::narrowWidth - 1, Theme::minHeight },
                 { Theme::narrowWidth, Theme::minHeight + 80 } };
    }

    // ---------------------------------------------------------------------------------------------
    void idsAndHeader()
    {
        beginTest ("Mono: the page is layout C, owns the header; every tour ID is there and visible at each size");
        auto c = makeMonoController();
        MainComponent mc (*c);
        auto* page = dynamic_cast<VoicePage*> (findById (&mc, "page.voice"));
        expect (page != nullptr && page->ownsHeader(), "layoutStyle 2 builds the Mono page");
        expect (findById (&mc, "mono.presets") != nullptr || findById (&mc, "tour.presets") != nullptr);
        for (auto size : sizes())
        {
            mc.setSize (size.x, size.y);
            const auto label = juce::String (size.x) + "x" + juce::String (size.y);
            expect (page->isCompact() == (size.x < Theme::narrowWidth), label + ": compact below 1000 px");
            expect (shown (&mc, "main.header") == nullptr, label + ": header hidden on the voice page");
            for (auto* id : kContractIds)
            {
                auto* comp = shown (&mc, id);
                expect (comp != nullptr, label + ": missing or hidden " + id);
                if (comp != nullptr)
                {
                    const auto b = areaIn (mc, comp);
                    expect (mc.getLocalBounds().contains (b) && ! b.isEmpty(), label + ": outside the window " + id + " " + b.toString());
                }
            }
            expect (dynamic_cast<juce::Component*> (page)->getBounds() == mc.getLocalBounds(), label + ": edge to edge without banners");
        }

        beginTest ("Mono: the header comes back on the other pages and hides again on the voice page");
        mc.setSize (Theme::defaultWidth, Theme::defaultHeight);
        press (shown (&mc, "mono.settings"));
        expect (shown (&mc, "page.settings") != nullptr && shown (&mc, "main.header") != nullptr, "設定 opens S-03 with the header");
        expect (shown (&mc, "page.voice") == nullptr);
        press (shown (&mc, "header.tab.voice"));
        expect (shown (&mc, "page.voice") != nullptr && shown (&mc, "main.header") == nullptr, "back: header hidden");
        press (shown (&mc, "mono.soundboard"));
        expect (shown (&mc, "page.soundboard") != nullptr && shown (&mc, "main.header") != nullptr, "音源 opens the soundboard with the header");
        mc.showPage (Navigator::Page::voice);
        mc.setSize (Theme::minWidth, Theme::minHeight);
        press (shown (&mc, "mono.settings"));
        expect (shown (&mc, "page.settings") != nullptr && shown (&mc, "main.header") != nullptr, "800x560 too");
        mc.showPage (Navigator::Page::voice);
        expect (shown (&mc, "main.header") == nullptr);
    }

    // ---------------------------------------------------------------------------------------------
    void checkNoOverlap (MainComponent& mc, const juce::String& label)
    {
        auto group = [&] (std::initializer_list<const char*> ids, juce::Component* container)
        {
            std::vector<std::pair<juce::String, juce::Rectangle<int>>> rects;
            for (auto* id : ids)
                if (auto* comp = shown (&mc, id))
                {
                    rects.emplace_back (id, areaIn (mc, comp));
                    if (container != nullptr)
                        expect (areaIn (mc, container).contains (rects.back().second), label + ": " + id + " sticks out of its group");
                }
            for (size_t i = 0; i < rects.size(); ++i)
                for (size_t j = i + 1; j < rects.size(); ++j)
                    expect (! rects[i].second.intersects (rects[j].second), label + ": " + rects[i].first + " overlaps " + rects[j].first);
        };
        group ({ "main.notices", "tour.presets", "tour.voiceToggle", "tour.mute", "voice.input", "voice.output", "voice.shifter", "voice.layers",
                 "tour.chain", "voice.bottom", "mono.settings", "mono.help", "mono.soundboard" }, nullptr);
        group ({ "tour.pitch", "tour.formant" }, shown (&mc, "voice.shifter"));
        group ({ "tour.monitor", "tour.status" }, shown (&mc, "voice.bottom"));
        group ({ "voice.inputMeter", "voice.inputDevice" }, shown (&mc, "voice.input"));
        group ({ "tour.outputMeter", "voice.outputDeviceLink" }, shown (&mc, "voice.output"));
        group ({ "mono.pitch.minus", "mono.pitch.slider", "mono.pitch.plus", "voice.shifterToggle" }, shown (&mc, "tour.pitch"));
        group ({ "mono.formant.minus", "mono.formant.slider", "mono.formant.plus" }, shown (&mc, "tour.formant"));
        group ({ "voice.layer.0", "voice.layerAdd" }, shown (&mc, "voice.layers"));
        group ({ "chain.slot.0.toggle", "chain.slot.0.up", "chain.slot.0.down", "chain.slot.0.remove" }, shown (&mc, "chain.slot.0"));
        group ({ "chain.slot.0", "chain.slot.1", "chain.slot.2", "chain.slot.3", "chain.add" }, nullptr); // rows scroll inside the rack
        if (auto* slot = shown (&mc, "chain.slot.0"))
        {
            const auto sb = areaIn (mc, slot);
            std::vector<juce::Rectangle<int>> parts;
            for (auto* child : slot->getChildren())
                if (child->isVisible())
                {
                    const auto b = areaIn (mc, child);
                    expect (sb.contains (b), label + ": slot part outside the row");
                    for (auto& o : parts) expect (! o.intersects (b), label + ": slot parts overlap");
                    parts.push_back (b);
                }
        }
        if (auto* side = shown (&mc, "mono.presets"))
            group ({ "mono.presetSearch", "voice.presetSelector", "mono.presetSave", "mono.presetDuplicate", "mono.presetBrowser" }, side);
    }

    void layout()
    {
        beginTest ("Mono: no overlaps (wide, 800x560, around 1000 px, with banners); Tab reaches the controls; 2 voices fit");
        auto c = makeMonoController();
        MainComponent mc (*c);
        for (auto size : sizes())
        {
            mc.setSize (size.x, size.y);
            const auto label = juce::String (size.x) + "x" + juce::String (size.y);
            checkNoOverlap (mc, label);

            std::unique_ptr<juce::ComponentTraverser> traverser (mc.createKeyboardFocusTraverser());
            std::set<juce::Component*> seen;
            for (auto* comp = traverser->getDefaultComponent (&mc); comp != nullptr && seen.insert (comp).second; comp = traverser->getNextComponent (comp)) {}
            for (auto* id : { "tour.voiceToggle", "tour.mute", "mono.settings", "mono.help", "mono.soundboard", "mono.pitch.plus", "mono.pitch.slider",
                              "voice.shifterToggle", "voice.layer.0", "voice.layer.0.toggle", "voice.inputDevice", "voice.outputDeviceLink", "chain.slot.0",
                              "chain.slot.0.toggle", "chain.slot.0.down", "chain.add", "voice.monitorToggle" })
                expect (seen.count (shown (&mc, id)) == 1, label + ": Tab does not reach " + id);
        }

        auto* bar = dynamic_cast<NoticeBar*> (findById (&mc, "main.notices"));
        bar->setNotices ({ testNotice ("a", NoticeLevel::danger), testNotice ("b", NoticeLevel::warning) });
        for (auto size : sizes())
        {
            mc.setSize (size.x, size.y);
            const auto label = juce::String (size.x) + "x" + juce::String (size.y) + " banners";
            checkNoOverlap (mc, label);
            for (auto* id : kContractIds) expect (shown (&mc, id) != nullptr, label + ": hidden " + id);
            expect (shown (&mc, "chain.slot.0")->getHeight() >= mono::ChainRack::rowHeight (size.x < Theme::narrowWidth), label + ": a whole rack row");
        }
        bar->setNotices ({});

        juce::String why;
        expect (c->addLayer (why));
        c->dispatchPendingMessages();
        for (auto size : sizes())
        {
            mc.setSize (size.x, size.y);
            const auto label = juce::String (size.x) + "x" + juce::String (size.y) + " 2 voices";
            checkNoOverlap (mc, label);
            expect (shown (&mc, "voice.layer.1.toggle") != nullptr, label);
            expect (shown (&mc, "voice.layerAdd") == nullptr, label + ": no add button at 2 voices");
            expect (areaIn (mc, shown (&mc, "voice.layers")).contains (areaIn (mc, shown (&mc, "voice.layer.1"))), label + ": voice 2 inside its card");
        }

        beginTest ("Mono: an empty chain still lays out with the add button");
        c->loadPreset ("natural-asis");
        c->dispatchPendingMessages();
        auto* rack = dynamic_cast<mono::ChainRack*> (shown (&mc, "tour.chain"));
        expect (rack != nullptr && rack->numRows() == 0);
        expect (shown (&mc, "chain.add") != nullptr);
    }

    // ---------------------------------------------------------------------------------------------
    void selectedIsNotOn()
    {
        beginTest ("D-23: the selected preset (check + outline) does not look like voice changer ON (filled)");
        auto c = makeMonoController();
        MainComponent mc (*c);
        mc.setSize (Theme::defaultWidth, Theme::defaultHeight);
        const auto image = mc.createComponentSnapshot (mc.getLocalBounds(), true, 1.0f);
        auto* on = shown (&mc, "tour.voiceToggle");
        auto* row = shown (&mc, "mono.preset.character-demon-king");
        expect (on != nullptr && row != nullptr, "the current preset is in the list");
        if (on == nullptr || row == nullptr) return;
        expect (dynamic_cast<juce::Button*> (row)->getToggleState());
        const auto onArea = areaIn (mc, on), rowArea = areaIn (mc, row);
        const auto onPixel = image.getPixelAt (onArea.getRight() - Theme::space3, onArea.getBottom() - Theme::space2);
        const auto rowPixel = image.getPixelAt (rowArea.getX() + rowArea.getWidth() * 2 / 3, rowArea.getCentreY());
        const auto& p = Theme::colours();
        expect (onPixel == p.accent, "ON is the accent fill");
        expect (rowPixel == p.raised, "the selected row is the raised colour, not a fill");
        expect (image.getPixelAt (rowArea.getX() + 1, rowArea.getCentreY()) == p.accent, "with the accent outline");
    }

    // ---------------------------------------------------------------------------------------------
    void actions()
    {
        beginTest ("Mono: ON, mute, pitch / formant -/+ and slider, converter, voices reach AppController (wide and 800x560)");
        auto c = makeMonoController();
        MainComponent mc (*c);
        for (auto size : { juce::Point<int> (Theme::defaultWidth, Theme::defaultHeight), juce::Point<int> (Theme::minWidth, Theme::minHeight) })
        {
            mc.setSize (size.x, size.y);
            const auto label = juce::String (size.x) + ": ";
            auto* voice = dynamic_cast<juce::Button*> (shown (&mc, "tour.voiceToggle"));
            press (voice);
            expect (! c->isVoiceChangerOn(), label + "ボイチェン OFF");
            c->dispatchPendingMessages();
            expectEquals (voice->getButtonText(), juce::String::fromUTF8 ("ボイチェン OFF"));
            press (voice);
            expect (c->isVoiceChangerOn());

            auto* mute = dynamic_cast<juce::Button*> (shown (&mc, "tour.mute"));
            press (mute);
            expect (c->isMicMuted(), label + "mute");
            c->dispatchPendingMessages();
            expectEquals (mute->getButtonText(), juce::String ("MUTE"), "MUTE in text, not colour alone (F-08-6)");
            press (mute);
            expect (! c->isMicMuted());
            c->dispatchPendingMessages();

            const float pitch = c->getPitch();
            press (shown (&mc, "mono.pitch.plus"));
            expectWithinAbsoluteError (c->getPitch(), pitch + 1.0f, 0.01f, label + "pitch +1 st");
            press (shown (&mc, "mono.pitch.minus"));
            press (shown (&mc, "mono.pitch.minus"));
            expectWithinAbsoluteError (c->getPitch(), pitch - 1.0f, 0.01f, label + "pitch -1 st");
            const float formant = c->getFormant();
            press (shown (&mc, "mono.formant.minus"));
            expectWithinAbsoluteError (c->getFormant(), formant - 0.5f, 0.01f, label + "formant -0.5 st");
            dynamic_cast<juce::Slider*> (shown (&mc, "mono.pitch.slider"))->setValue (3.0, juce::sendNotificationSync);
            expectWithinAbsoluteError (c->getPitch(), 3.0f, 0.01f, label + "pitch slider");
            c->dispatchPendingMessages();

            auto* shifter = dynamic_cast<juce::Button*> (shown (&mc, "voice.shifterToggle"));
            shifter->setToggleState (false, juce::sendNotificationSync);
            expect (! c->hasShifter(), label + "変換 OFF");
            shifter->setToggleState (true, juce::sendNotificationSync);
            expect (c->hasShifter());
            c->dispatchPendingMessages();

            auto* layer = dynamic_cast<juce::Button*> (shown (&mc, "voice.layer.0.toggle"));
            layer->setToggleState (false, juce::sendNotificationSync);
            expect (! c->getLayer (0).enabled, label + "声 1 OFF");
            layer->setToggleState (true, juce::sendNotificationSync);
            c->dispatchPendingMessages();
        }

        beginTest ("Mono: a voice row opens its editor; the add button adds the second voice");
        mc.setSize (Theme::defaultWidth, Theme::defaultHeight);
        auto* row = shown (&mc, "voice.layer.0");
        expect (row != nullptr && row->keyPressed (juce::KeyPress (juce::KeyPress::returnKey)));
        expect (shown (&mc, "mono.layerPanel") != nullptr, "Enter opens the voice editor");
        mc.closeOverlay();
        press (shown (&mc, "voice.layerAdd"));
        expectEquals (c->getNumLayers(), 2);
        c->dispatchPendingMessages();

        beginTest ("Mono: rack rows reach the chain: ON/OFF, down, up, remove, add (S-07), the 11th is refused, Enter = S-09");
        const auto type0 = c->getChain()[0].type, type1 = c->getChain()[1].type;
        auto* slotToggle = dynamic_cast<juce::Button*> (shown (&mc, "chain.slot.1.toggle"));
        slotToggle->setToggleState (false, juce::sendNotificationSync);
        expect (! c->getChain()[1].enabled, "slot 2 OFF");
        c->dispatchPendingMessages();
        press (shown (&mc, "chain.slot.0.down"));
        expect (c->getChain()[0].type == type1 && c->getChain()[1].type == type0, "slot 1 moved down");
        c->dispatchPendingMessages();
        press (shown (&mc, "chain.slot.1.up"));
        expect (c->getChain()[0].type == type0, "and back up");
        c->dispatchPendingMessages();
        const auto before = c->getChain().size();
        press (shown (&mc, "chain.slot.0.remove"));
        expectEquals (int (c->getChain().size()), int (before) - 1, "removed");
        c->dispatchPendingMessages();
        expectEquals (dynamic_cast<mono::ChainRack*> (shown (&mc, "tour.chain"))->numRows(), int (before) - 1, "the rack follows");
        press (shown (&mc, "chain.add"));
        bool picker = false;
        std::function<void (juce::Component*)> scan = [&] (juce::Component* comp)
        {
            if (dynamic_cast<EffectPicker*> (comp) != nullptr && comp->isVisible()) picker = true;
            for (auto* ch : comp->getChildren()) scan (ch);
        };
        scan (&mc);
        expect (picker, "エフェクトを追加 opens S-07");
        mc.closeOverlay();
        expect (shown (&mc, "chain.slot.0")->keyPressed (juce::KeyPress (juce::KeyPress::returnKey)));
        bool detail = false;
        std::function<void (juce::Component*)> scanDetail = [&] (juce::Component* comp)
        {
            if (dynamic_cast<SlotDetailPanel*> (comp) != nullptr && comp->isVisible()) detail = true;
            for (auto* ch : comp->getChildren()) scanDetail (ch);
        };
        scanDetail (&mc);
        expect (detail, "Enter on a row opens S-09");
        mc.closeOverlay();
        while (int (c->getChain().size()) < kMaxSlots)
        {
            juce::String why;
            c->addEffect ("eq", why);
        }
        c->dispatchPendingMessages();
        expectEquals (dynamic_cast<mono::ChainRack*> (shown (&mc, "tour.chain"))->numRows(), kMaxSlots, "10 rows (the rack scrolls)");
        press (findById (&mc, "chain.add"));
        auto* toast = dynamic_cast<ToastView*> (findById (&mc, "main.toast"));
        expect (toast != nullptr && toast->isVisible() && toast->currentText().contains ("10"), "the 11th slot is refused with a toast (F-04-1)");
        mc.setSize (Theme::minWidth, Theme::minHeight);
        checkNoOverlap (mc, "10 slots, 800x560");
    }

    // ---------------------------------------------------------------------------------------------
    void presetsList()
    {
        beginTest ("Mono presets: choose from the list, the star toggles a favourite, filters and search (wide)");
        auto c = makeMonoController();
        MainComponent mc (*c);
        mc.setSize (Theme::defaultWidth, Theme::defaultHeight);
        auto* side = dynamic_cast<mono::PresetSidebar*> (shown (&mc, "tour.presets"));
        expect (side != nullptr, "wide: the column is the tour's preset part");
        if (side == nullptr) return;
        expectEquals (side->getFilter(), juce::String ("character"), "opens on the category of the preset in use");
        expect (side->listedIds().contains ("character-helium"));
        press (shown (&mc, "mono.preset.character-helium"));
        expect (c->getCurrentPreset().id == "character-helium", "a row loads its preset");
        c->dispatchPendingMessages();
        expect (dynamic_cast<juce::Button*> (shown (&mc, "mono.preset.character-helium"))->getToggleState(), "and becomes the checked row");
        expect (! dynamic_cast<juce::Button*> (shown (&mc, "mono.preset.character-demon-king"))->getToggleState());

        expect (! c->isFavorite ("character-helium"));
        press (shown (&mc, "mono.preset.character-helium.star"));
        expect (c->isFavorite ("character-helium"), "★ adds a favourite");
        c->dispatchPendingMessages();
        press (shown (&mc, "mono.filter.fav"));
        expect (side->listedIds() == juce::StringArray { "natural-asis", "character-demon-king", "character-robot", "character-helium" },
                      "★ lists the favourites in their order");
        press (shown (&mc, "mono.preset.character-helium.star"));
        c->dispatchPendingMessages();
        expect (! c->isFavorite ("character-helium") && ! side->listedIds().contains ("character-helium"), "★ again removes it");

        press (shown (&mc, "mono.filter.all"));
        expectEquals (side->listedIds().size(), int (c->getPresetLibrary().all().size()), "全部");
        side->setSearchText (juce::String::fromUTF8 ("ロボ"));
        expect (side->listedIds().contains ("character-robot") && side->listedIds().size() < 10, "search narrows the list");
        side->setSearchText (juce::String::fromUTF8 ("該当しない名前"));
        expect (side->listedIds().isEmpty() && shown (&mc, "mono.presetEmpty") != nullptr, "nothing found: a message");
        side->setSearchText ({});
        expect (shown (&mc, "mono.filter.user") == nullptr, "no user presets: no ユーザー chip");
        juce::String error;
        expect (c->saveCurrentAsNew (juce::String::fromUTF8 ("自作"), error));
        c->dispatchPendingMessages();
        expect (shown (&mc, "mono.filter.user") != nullptr, "the chip appears with the first user preset");
        press (shown (&mc, "mono.presetSave"));
        auto* toast = dynamic_cast<ToastView*> (findById (&mc, "main.toast"));
        expect (toast != nullptr && toast->currentText().isNotEmpty(), "保存 on a user preset overwrites it (toast)");

        beginTest ("Mono presets (800x560): the column folds into a button; its panel lists and loads presets");
        mc.setSize (Theme::minWidth, Theme::minHeight);
        expect (shown (&mc, "mono.presets") == nullptr, "no column in the small window");
        press (shown (&mc, "voice.presetSelector"));
        auto* panel = dynamic_cast<mono::PresetPanel*> (findById (&mc, "mono.presetPanel"));
        expect (panel != nullptr && panel->isVisible(), "the preset button opens the list panel");
        if (panel == nullptr) return;
        panel->sidebar().setFilter ("natural");
        auto* row = findById (panel, "mono.preset.natural-ikebo");
        expect (row != nullptr && visibleWithin (row, panel));
        press (row);
        expect (c->getCurrentPreset().id == "natural-ikebo", "a row in the panel loads its preset");
        c->dispatchPendingMessages();
        expect (dynamic_cast<juce::Button*> (findById (panel, "mono.preset.natural-ikebo"))->getToggleState(), "the panel follows the change");
        mc.closeOverlay();
    }

    // ---------------------------------------------------------------------------------------------
    /** Steps the tour shows, first to last (parts it cannot show are skipped, E-32). */
    std::vector<int> runTour (int layoutStyle, juce::Point<int> size, bool& endedOnVoicePage)
    {
        auto c = makeMonoController (layoutStyle);
        MainComponent mc (*c);
        mc.setSize (size.x, size.y);
        mc.startTour (false);
        std::vector<int> steps;
        auto* t = dynamic_cast<GuideTour*> (findById (&mc, "main.tour"));
        if (t == nullptr) return steps;
        if (layoutStyle == 2)
        {
            auto* bubble = findById (t, "tour.bubble");
            expect (! areaIn (mc, bubble).intersects (areaIn (mc, shown (&mc, "tour.voiceToggle"))), "step 1: the bubble leaves the ON block visible");
        }
        for (int guard = 0; t->isVisible() && guard < 20; ++guard)
        {
            steps.push_back (t->currentStep());
            t->next();
        }
        expect (! t->isVisible());
        expectEquals (c->getSettings().tourStep, 7);
        endedOnVoicePage = shown (&mc, "page.voice") != nullptr && (shown (&mc, "main.header") == nullptr) == (layoutStyle == 2);
        return steps;
    }

    void tour()
    {
        for (auto size : { juce::Point<int> (Theme::defaultWidth, Theme::defaultHeight), juce::Point<int> (Theme::minWidth, Theme::minHeight) })
        {
            const auto label = juce::String (size.x) + "x" + juce::String (size.y);
            beginTest ("Mono: the guide tour shows every step the Studio page shows and ends on the voice page, " + label);
            bool studioEnd = false, monoEnd = false;
            const auto studio = runTour (0, size, studioEnd);
            const auto mono = runTour (2, size, monoEnd);
            auto text = [] (const std::vector<int>& v) { juce::StringArray a; for (int i : v) a.add (juce::String (i + 1)); return a.joinIntoString (" "); };
            expectEquals (text (mono), text (studio), label);
            for (int i : { 0, 1, 2, 3, 4, 6 }) // step 6 (the long hotkeys card) is not shown on either page: pre-existing, see the report
                expect (std::find (mono.begin(), mono.end(), i) != mono.end(), label + ": step " + juce::String (i + 1) + " shown");
            expect (monoEnd, "back on the Mono page, header hidden again");
        }
    }
};

static UiSkinMonoTests uiSkinMonoTests;
} // namespace koe::ui

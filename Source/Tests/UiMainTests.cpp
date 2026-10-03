// Main window checks (ui-main). Category "UiMain". No window, no audio: AppController (false) and
// MainComponent stay offscreen. Covers the measurable parts of AC-59 / AC-60 / AC-62 / AC-67 / AC-52.

#include "App/AppController.h"
#include "Core/Paths.h"
#include "Effects/EffectRegistry.h"
#include "UI/MainComponent.h"
#include "UI/Screens.h"
#include "UI/main/ChainStrip.h"
#include "UI/main/GuideTour.h"
#include "UI/main/Panels.h"
#include "UI/main/Shell.h"

#include <algorithm>
#include <set>

namespace koe::ui
{
namespace
{
using namespace mainui;

const juce::StringArray kSixFavourites { "natural-asis", "character-demon-king", "natural-ikebo", "character-helium", "character-robot", "space-cave" };

void freshDataDir()
{
    auto d = paths::dataDir();
    if (d.getFullPathName().contains ("KoeLoomTests")) d.deleteRecursively();
    d.createDirectory();
}

/** Setup done and tour finished, so MainComponent opens on S-01 without an overlay. */
std::unique_ptr<AppController> makeController (const juce::StringArray& favourites, const char* preset = "character-demon-king")
{
    freshDataDir();
    auto c = std::make_unique<AppController> (false);
    c->startup();
    c->updateSettings ([&] (Settings& s)
    {
        s.setupDone = true;
        s.tourStep = 7;
        s.favorites = favourites;
    });
    c->loadPreset (preset);
    c->dispatchPendingMessages();
    return c;
}

juce::Component* visibleById (juce::Component* root, const juce::String& id)
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

juce::Rectangle<int> boundsIn (juce::Component& root, juce::Component* comp) { return root.getLocalArea (comp, comp->getLocalBounds()); }

Notice notice (const char* key, NoticeLevel level)
{
    Notice n;
    n.key = key;
    n.level = level;
    n.text = juce::String::fromUTF8 ("テスト用の警告です");
    return n;
}

void click (juce::Component* comp)
{
    if (auto* b = dynamic_cast<juce::Button*> (comp); b != nullptr && b->onClick) b->onClick();
}
} // namespace

class UiMainTests : public juce::UnitTest
{
public:
    UiMainTests() : juce::UnitTest ("UI main window (ui-main)", "UiMain") {}

    void runTest() override
    {
        const bool wasOff = animationsOff();
        animationsOff() = true;
        formatting();
        widgets();
        layoutAndKeyboard();
        favourites();
        banners();
        voiceControls();
        effectPickerAndSlotDetail();
        navigationAndOverlays();
        tour();
        theme();
        screenSettings();
        animationsOff() = wasOff;
    }

private:
    // ---------------------------------------------------------------------------------------------
    void formatting()
    {
        beginTest ("parameter value text: dB 0.1, Hz -> kHz, 0..1 -> %, st, :1, ms, choices");
        const ParamSpec db { "g", "g", -24.0f, 24.0f, 0.0f, "dB" };
        const ParamSpec hz { "f", "f", 20.0f, 20000.0f, 1000.0f, "Hz" };
        const ParamSpec mix { "m", "m", 0.0f, 1.0f, 0.5f, "" };
        const ParamSpec st { "p", "p", -12.0f, 12.0f, 0.0f, "st" };
        const ParamSpec ratio { "r", "r", 1.0f, 20.0f, 3.0f, ":1" };
        const ParamSpec ms { "t", "t", 1.0f, 4000.0f, 300.0f, "ms" };
        const ParamSpec choice { "c", "c", 0.0f, 1.0f, 0.0f, "", { { "a", "A" }, { "b", "B" } } };
        expectEquals (formatParam (db, -3.27f), juce::String ("-3.3 dB"));
        expectEquals (formatParam (db, -0.01f), juce::String ("0 dB"));
        expectEquals (formatParam (db, 10.0f), juce::String ("10 dB")); // the mock's format: no trailing .0
        expectEquals (formatParam (hz, 4500.0f), juce::String ("4.5 kHz"));
        expectEquals (formatParam (hz, 440.0f), juce::String ("440 Hz"));
        expectEquals (formatParam (mix, 0.15f), juce::String ("15 %"));
        expectEquals (formatParam (st, -9.0f), juce::String ("-9 st"));
        expectEquals (formatParam (st, 2.5f), juce::String ("+2.5 st"));
        expectEquals (formatParam (ratio, 3.0f), juce::String ("3:1"));
        expectEquals (formatParam (ms, 250.0f), juce::String ("250 ms"));
        expectEquals (formatParam (ms, 5.0f), juce::String ("5 ms"));
        expectEquals (formatParam (ms, 2.5f), juce::String ("2.5 ms"));
        expectEquals (formatParam (choice, 1.0f), juce::String ("B"));
        expect (paramLabel (db).contains (juce::String::fromUTF8 ("初期値")), "tooltip shows the default (F-13-6)");
    }

    // ---------------------------------------------------------------------------------------------
    void widgets()
    {
        beginTest ("Knob: tooltip has name, value, range and default (F-13-6); any diameter");
        {
            Knob k (Knob::Size::big);
            k.setup (-12.0, 12.0, 0.0, 0.1, formatSemitones, "st");
            k.setLabel (juce::String::fromUTF8 ("ピッチ"));
            k.setValue (2.0, juce::sendNotificationSync);
            expectEquals (k.getTooltip(), juce::String::fromUTF8 ("ピッチ：+2 st（-12 st〜+12 st、初期値 0 st）"));
            expectEquals (k.getDiameter(), int (Theme::knobBig));
            k.setDiameter (84);
            k.setSize (84, 106);
            expectEquals (k.getDiameter(), 84);
            expect (k.createComponentSnapshot (k.getLocalBounds()).isValid());
        }

        beginTest ("LevelMeter: tick labels stay inside and never overlap; short meters drop the middle ones first");
        for (int h : { 300, 160, 120, 90, 60, 30 })
        {
            LevelMeter m (true);
            m.setSize (38, h);
            const auto labels = m.scaleLabels();
            expect (! labels.empty() && labels.front().first == 0, "0 is always there");
            for (size_t i = 0; i < labels.size(); ++i)
            {
                expect (m.getLocalBounds().contains (labels[i].second), "inside at h = " + juce::String (h));
                for (size_t j = i + 1; j < labels.size(); ++j)
                    expect (! labels[i].second.intersects (labels[j].second), "no overlap at h = " + juce::String (h));
            }
            if (h >= 300) expectEquals (int (labels.size()), 5);
            if (h >= 60) expect (std::any_of (labels.begin(), labels.end(), [] (auto& l) { return l.first == -48; }), "-48 kept");
        }

        beginTest ("S-03 外観: defaults are the mock's palettes; every accent x tone x theme keeps AC-53 contrast");
        {
            auto same = [] (const Palette& a, const Palette& b)
            {
                const juce::Colour* x = &a.bg;
                const juce::Colour* y = &b.bg;
                for (size_t i = 0; i < sizeof (Palette) / sizeof (juce::Colour); ++i)
                    if (x[i] != y[i]) return false;
                return true;
            };
            expect (same (Theme::make (true, 0, 0), Theme::dark()), "dark default = mock");
            expect (same (Theme::make (false, 0, 0), Theme::light()), "light default = mock");
            for (const bool dark : { true, false })
                for (int tone = 0; tone < Theme::numTones; ++tone)
                    for (int accent = 0; accent < Theme::numAccents; ++accent)
                    {
                        const auto p = Theme::make (dark, accent, tone);
                        const auto combo = juce::String (dark ? "dark " : "light ") + Theme::toneName (tone, dark) + " " + Theme::accentName (accent);
                        double worst = 100.0, worstRatio = 0.0;
                        juce::String worstWhat;
                        auto atLeast = [&] (juce::Colour fg, juce::Colour bgc, double min, const char* what)
                        {
                            const double r = contrastRatio (fg, bgc);
                            if (r / min < worst) { worst = r / min; worstRatio = r; worstWhat = what; }
                            expect (r >= min, combo + ": " + what + " " + juce::String (r, 2));
                        };
                        for (auto bgc : { p.bg, p.surface, p.raised })
                        {
                            atLeast (p.text, bgc, 4.5, "text");
                            atLeast (p.textSub, bgc, 4.5, "sub text");
                            atLeast (p.accent, bgc, 4.5, "accent text (links, 追加)");
                            atLeast (p.ok, bgc, 4.5, "ok");
                            atLeast (p.warn, bgc, 4.5, "warn");
                            atLeast (p.danger, bgc, 4.5, "danger");
                            atLeast (p.border, bgc, 3.0, "border / knob track");
                        }
                        atLeast (p.onAccent, p.accent, 4.5, "text on accent");
                        atLeast (p.onDanger, p.danger, 4.5, "text on danger");
                        logMessage ("contrast " + combo + ": worst " + worstWhat + " " + juce::String (worstRatio, 2));
                    }
        }

        beginTest ("S-03 外観 settings: saved and loaded; old files load with the defaults; out of range is clamped");
        {
            freshDataDir();
            const auto file = paths::dataDir().getChildFile ("appearance-test.json");
            Settings s;
            s.accentColour = 4;
            s.backgroundTone = 2;
            expect (saveSettings (s, file));
            SettingsLoadResult r;
            const auto back = loadSettings (file, r);
            expectEquals (back.accentColour, 4);
            expectEquals (back.backgroundTone, 2);
            expect (file.replaceWithText ("{ \"darkTheme\": false }"));
            const auto old = loadSettings (file, r);
            expect (! old.darkTheme && old.accentColour == 0 && old.backgroundTone == 0, "settings from before the choice existed");
            expect (file.replaceWithText ("{ \"accentColour\": 9, \"backgroundTone\": -1 }"));
            SettingsLoadResult r2;
            const auto bad = loadSettings (file, r2);
            expect (bad.accentColour == 0 && bad.backgroundTone == 0 && r2.clampedKeys.contains ("accentColour"), "clamped (E-29)");
            file.deleteFile();
        }

        beginTest ("type scale is the mock's CSS size; Japanese in mono text uses the UI face");
        for (float size : { Theme::fontXS, Theme::fontS, Theme::fontL })
            expectWithinAbsoluteError (Theme::ui (size).getHeightInPoints(), size, 0.01f);
        const auto kana = juce::String::fromUTF8 ("未割り当て");
        expectWithinAbsoluteError (textRunWidth (kana, Theme::fontS, true, true), juce::GlyphArrangement::getStringWidth (Theme::ui (Theme::fontS, true), kana), 0.01f);
        const auto mixed = "4 / 10" + juce::String::fromUTF8 (" スロット");
        expectWithinAbsoluteError (textRunWidth (mixed, Theme::fontS, false, true),
                                   juce::GlyphArrangement::getStringWidth (Theme::mono (Theme::fontS), "4 / 10 ")
                                       + juce::GlyphArrangement::getStringWidth (Theme::ui (Theme::fontS), juce::String::fromUTF8 ("スロット")), 0.01f);
        expectWithinAbsoluteError (textRunWidth ("12 dB", Theme::fontS, false, true), juce::GlyphArrangement::getStringWidth (Theme::mono (Theme::fontS), "12 dB"), 0.01f);

        beginTest ("slot slide mapping: a move, a removal, an insertion, duplicates (F-14-6)");
        const juce::StringArray abc { "a", "b", "c" };
        expect (matchSlots (abc, { "b", "a", "c" }) == std::vector<int> { 1, 0, 2 });
        expect (matchSlots (abc, { "a", "c" }) == std::vector<int> { 0, 2 });
        expect (matchSlots (abc, { "a", "x", "b", "c" }) == std::vector<int> { 0, -1, 1, 2 });
        expect (matchSlots ({ "a", "a", "b" }, { "b", "a", "a" }) == std::vector<int> { 2, 0, 1 });

        beginTest ("looper confirmation: an action whose card was rebuilt meanwhile does nothing (no use after free)");
        {
            int runs = 0;
            auto owner = std::make_unique<juce::Component>();
            auto act = guardedBy (*owner, [&runs] { ++runs; });
            act();
            expectEquals (runs, 1);
            owner.reset(); // the card is rebuilt while the confirm panel is open
            act();         // 続ける
            expectEquals (runs, 1);
        }
    }

    // ---------------------------------------------------------------------------------------------
    void checkLayout (MainComponent& mc, const juce::String& label)
    {
        const auto root = mc.getLocalBounds();
        const char* required[] = { "main.header", "header.tab.voice", "header.tab.soundboard", "header.tab.settings", "tour.mute", "tour.voiceToggle",
                                   "header.help", "tour.presets", "voice.presetSelector", "tour.pitch", "tour.formant", "voice.shifterToggle",
                                   "voice.inputMeter", "tour.outputMeter", "voice.outputDeviceLink", "tour.chain", "chain.slot.0",
                                   "chain.slot.0.toggle", "voice.bottom", "tour.monitor", "voice.monitorToggle", "tour.status",
                                   "voice.input", "voice.output", "voice.shifter", "voice.layers" };
        for (auto* id : required)
        {
            auto* comp = visibleById (&mc, id);
            expect (comp != nullptr, label + ": missing or hidden " + id);
            if (comp != nullptr)
            {
                const auto b = boundsIn (mc, comp);
                expect (root.contains (b) && ! b.isEmpty(), label + ": outside the window " + id + " " + b.toString());
            }
        }

        auto noOverlap = [&] (std::initializer_list<const char*> ids, juce::Component* container)
        {
            std::vector<std::pair<juce::String, juce::Rectangle<int>>> rects;
            for (auto* id : ids)
                if (auto* comp = visibleById (&mc, id))
                {
                    rects.emplace_back (id, boundsIn (mc, comp));
                    if (container != nullptr)
                        expect (boundsIn (mc, container).contains (rects.back().second), label + ": " + id + " sticks out of its group");
                }
            for (size_t i = 0; i < rects.size(); ++i)
                for (size_t j = i + 1; j < rects.size(); ++j)
                    expect (! rects[i].second.intersects (rects[j].second), label + ": " + rects[i].first + " overlaps " + rects[j].first);
        };
        noOverlap ({ "main.header", "main.notices", "tour.presets", "voice.input", "voice.shifter", "voice.layers", "voice.output", "tour.chain", "voice.bottom" }, nullptr);
        noOverlap ({ "header.tab.voice", "header.tab.soundboard", "header.tab.settings", "tour.mute", "tour.voiceToggle", "header.help" }, visibleById (&mc, "main.header"));
        noOverlap ({ "tour.pitch", "tour.formant", "voice.shifterToggle" }, visibleById (&mc, "voice.shifter"));
        noOverlap ({ "tour.monitor", "tour.status" }, visibleById (&mc, "voice.bottom"));
        noOverlap ({ "voice.presetSelector", "voice.fav.0", "voice.fav.1", "voice.fav.2", "voice.fav.3", "voice.fav.more" }, visibleById (&mc, "tour.presets"));

        // §8.2.1: the slots keep their knobs and ON/OFF even when banners shrink the chain
        if (auto* slot = visibleById (&mc, "chain.slot.0"))
        {
            expect (slot->getHeight() >= SlotCard::minHeight, label + ": slot too short " + juce::String (slot->getHeight()));
            const auto sb = boundsIn (mc, slot);
            for (auto* child : slot->getChildren())
                if (child->isVisible()) expect (sb.contains (boundsIn (mc, child)), label + ": slot part outside the slot");
        }
    }

    void layoutAndKeyboard()
    {
        beginTest ("S-01 parts fit and do not overlap at 1120x720 and 800x560 (AC-59), Tab reaches them (F-14-4)");
        auto c = makeController (kSixFavourites);
        MainComponent mc (*c);
        for (auto size : { juce::Point<int> (Theme::defaultWidth, Theme::defaultHeight), juce::Point<int> (Theme::minWidth, Theme::minHeight),
                           juce::Point<int> (Theme::narrowWidth - 1, Theme::minHeight), juce::Point<int> (Theme::narrowWidth, Theme::minHeight + 80) })
        {
            mc.setSize (size.x, size.y);
            const auto label = juce::String (size.x) + "x" + juce::String (size.y);
            checkLayout (mc, label);

            std::unique_ptr<juce::ComponentTraverser> traverser (mc.createKeyboardFocusTraverser());
            std::set<juce::Component*> seen;
            for (auto* comp = traverser->getDefaultComponent (&mc); comp != nullptr && seen.insert (comp).second; comp = traverser->getNextComponent (comp)) {}
            for (auto* id : { "tour.voiceToggle", "tour.mute", "header.help", "voice.presetSelector", "voice.fav.0", "voice.fav.more", "tour.pitch",
                              "tour.formant", "voice.shifterToggle", "voice.layer.0.toggle", "voice.inputDevice", "voice.outputDeviceLink",
                              "chain.slot.0", "chain.slot.0.toggle", "chain.slot.3", "chain.add", "voice.monitorToggle" })
                expect (seen.count (visibleById (&mc, id)) == 1, label + ": Tab does not reach " + id);
        }

        beginTest ("an empty chain shows its hint and still lays out");
        c->loadPreset ("natural-asis");
        c->dispatchPendingMessages();
        auto* strip = dynamic_cast<ChainStrip*> (visibleById (&mc, "tour.chain"));
        expect (strip != nullptr && strip->numCards() == 0);
        expect (visibleById (&mc, "chain.add") != nullptr);
    }

    // ---------------------------------------------------------------------------------------------
    void favourites()
    {
        beginTest ("favourites: 4 + \"+2\" wide, 2 + \"+4\" narrow, current one checked, empty message (F-05-12, AC-60)");
        auto c = makeController (kSixFavourites);
        MainComponent mc (*c);
        mc.setSize (Theme::defaultWidth, Theme::defaultHeight);
        for (int i = 0; i < 4; ++i) expect (visibleById (&mc, "voice.fav." + juce::String (i)) != nullptr);
        expect (visibleById (&mc, "voice.fav.4") == nullptr);
        auto* more = dynamic_cast<juce::Button*> (visibleById (&mc, "voice.fav.more"));
        expect (more != nullptr && more->getButtonText() == "+2");
        auto* demon = dynamic_cast<juce::Button*> (visibleById (&mc, "voice.fav.1"));
        expect (demon != nullptr && demon->getToggleState(), "the current preset is checked");
        expect (! dynamic_cast<juce::Button*> (visibleById (&mc, "voice.fav.0"))->getToggleState());

        mc.setSize (Theme::minWidth, Theme::minHeight);
        expect (visibleById (&mc, "voice.fav.1") != nullptr && visibleById (&mc, "voice.fav.2") == nullptr);
        more = dynamic_cast<juce::Button*> (visibleById (&mc, "voice.fav.more"));
        expect (more != nullptr && more->getButtonText() == "+4");

        click (visibleById (&mc, "voice.fav.0")); // そのまま
        expectEquals (juce::String (c->getCurrentPreset().id), juce::String ("natural-asis"));
        c->dispatchPendingMessages();
        expect (dynamic_cast<juce::Button*> (visibleById (&mc, "voice.fav.0"))->getToggleState());

        click (visibleById (&mc, "voice.fav.more"));
        expect (dynamic_cast<PresetBrowser*> (dynamic_cast<OverlayHost*> (findById (&mc, "main.overlay"))->panel()) != nullptr, "+n opens S-06");

        auto none = makeController ({});
        MainComponent empty (*none);
        empty.setSize (Theme::defaultWidth, Theme::defaultHeight);
        expect (visibleById (&empty, "voice.fav.empty") != nullptr && visibleById (&empty, "voice.fav.0") == nullptr
                && visibleById (&empty, "voice.fav.more") == nullptr);
    }

    // ---------------------------------------------------------------------------------------------
    void banners()
    {
        beginTest ("banners: at most 2 rows of 36 px, the rest as \"+n\" (spec 8.3); the chain keeps its knobs (spec 8.2.1)");
        auto c = makeController (kSixFavourites);
        MainComponent mc (*c);
        mc.setSize (Theme::minWidth, Theme::minHeight);
        auto* bar = dynamic_cast<NoticeBar*> (findById (&mc, "main.notices"));
        expect (bar != nullptr);
        bar->setNotices ({ notice ("a", NoticeLevel::danger), notice ("b", NoticeLevel::warning), notice ("c", NoticeLevel::info) });
        expect (visibleById (&mc, "notice.0") != nullptr && visibleById (&mc, "notice.1") != nullptr && visibleById (&mc, "notice.2") == nullptr);
        expectEquals (visibleById (&mc, "notice.0")->getHeight(), Theme::bannerH);
        auto* more = dynamic_cast<juce::Button*> (visibleById (&mc, "notice.more"));
        expect (more != nullptr && more->getButtonText() == "+1");
        checkLayout (mc, "800x560 + 2 banners");
        mc.setSize (Theme::defaultWidth, Theme::defaultHeight);
        checkLayout (mc, "1120x720 + 2 banners");

        bar->setNotices ({ notice ("a", NoticeLevel::warning) });
        expect (visibleById (&mc, "notice.0") != nullptr && visibleById (&mc, "notice.1") == nullptr && visibleById (&mc, "notice.more") == nullptr);

        beginTest ("a real banner from the controller (output gain over +6 dB, F-12-4) reaches the bar");
        c->setOutputGainDb (8.0f);
        c->dispatchPendingMessages();
        expect (bar->preferredHeight() == Theme::bannerH);
    }

    // ---------------------------------------------------------------------------------------------
    void voiceControls()
    {
        beginTest ("voice controls reach AppController: converter OFF, voice OFF, mute, voice changer, slot ON/OFF");
        auto c = makeController (kSixFavourites);
        MainComponent mc (*c);
        for (auto size : { juce::Point<int> (Theme::defaultWidth, Theme::defaultHeight), juce::Point<int> (Theme::minWidth, Theme::minHeight) })
        {
            mc.setSize (size.x, size.y);
            auto* shifter = dynamic_cast<juce::Button*> (visibleById (&mc, "voice.shifterToggle"));
            expect (shifter != nullptr && shifter->getToggleState());
            shifter->setToggleState (false, juce::sendNotificationSync);
            expect (! c->hasShifter(), "変換 OFF reaches setShifterEnabled");
            shifter->setToggleState (true, juce::sendNotificationSync);
            expect (c->hasShifter());

            auto* layer = dynamic_cast<juce::Button*> (visibleById (&mc, "voice.layer.0.toggle"));
            expect (layer != nullptr && layer->getToggleState());
            layer->setToggleState (false, juce::sendNotificationSync);
            expect (! c->getLayer (0).enabled, "声 1 OFF reaches setLayerEnabled");
            layer->setToggleState (true, juce::sendNotificationSync);
            expect (c->getLayer (0).enabled);
            c->dispatchPendingMessages();
        }

        auto* mute = dynamic_cast<juce::Button*> (visibleById (&mc, "tour.mute"));
        mute->setToggleState (true, juce::sendNotificationSync);
        expect (c->isMicMuted());
        c->dispatchPendingMessages();
        expectEquals (mute->getButtonText(), juce::String ("MUTE"), "mute shows MUTE (F-08-6)");

        auto* voice = dynamic_cast<juce::Button*> (visibleById (&mc, "tour.voiceToggle"));
        voice->setToggleState (false, juce::sendNotificationSync);
        expect (! c->isVoiceChangerOn());
        c->dispatchPendingMessages();
        expectEquals (voice->getButtonText(), juce::String::fromUTF8 ("ボイチェン OFF"));

        auto* slot = dynamic_cast<juce::Button*> (visibleById (&mc, "chain.slot.1.toggle"));
        slot->setToggleState (false, juce::sendNotificationSync);
        expect (! c->getChain()[1].enabled);

        beginTest ("adding a voice from card 03, then the third is refused with a toast");
        mc.setSize (Theme::defaultWidth, Theme::defaultHeight);
        auto layerH = [&mc] { auto* l = visibleById (&mc, "voice.layer.0"); return l != nullptr ? l->getHeight() : 0; };
        expectEquals (layerH(), 152, "one voice: label and value over each slider, as in the mock");
        click (visibleById (&mc, "voice.layerAdd"));
        expectEquals (c->getNumLayers(), 2);
        c->dispatchPendingMessages();
        expectEquals (layerH(), 92, "two voices: one line per value so both fit");
        expect (visibleById (&mc, "voice.layerAdd") == nullptr, "no add button at 2 voices");

        beginTest ("the 11th slot is refused with a toast (F-04-1)");
        while (int (c->getChain().size()) < kMaxSlots)
        {
            juce::String why;
            c->addEffect ("eq", why);
        }
        c->dispatchPendingMessages();
        click (findById (&mc, "chain.add"));
        auto* toast = dynamic_cast<ToastView*> (findById (&mc, "main.toast"));
        expect (toast != nullptr && toast->isVisible() && toast->currentText().contains ("10"));
    }

    // ---------------------------------------------------------------------------------------------
    void effectPickerAndSlotDetail()
    {
        beginTest ("S-07: category filter, partial name search, unavailable rows, add at the end and at a position (F-04-15)");
        auto c = makeController (kSixFavourites);
        MainComponent mc (*c);
        mc.setSize (Theme::minWidth, Theme::minHeight);
        mc.showEffectPicker (-1);
        auto* picker = dynamic_cast<EffectPicker*> (findById (&mc, "panel.effectPicker"));
        expect (picker != nullptr);
        expectEquals (int (picker->listedTypes().size()), int (allEffectInfos().size()));
        picker->setSearchText (juce::String::fromUTF8 ("エコ"));
        expect (picker->listedTypes() == std::vector<std::string> { "echo" });
        picker->setSearchText ({});
        picker->setCategory (int (EffectCategory::timeSpace));
        expect (picker->listedTypes() == std::vector<std::string> { "echo", "reverb" });
        picker->setCategory (-1);
        for (auto& info : allEffectInfos())
            if (! hasEffectFactory (info.type))
            {
                auto* b = dynamic_cast<juce::Button*> (findById (picker, "picker." + juce::String (info.type) + ".add"));
                expect (b != nullptr && ! b->isEnabled() && b->getButtonText() == juce::String::fromUTF8 ("準備中"), juce::String (info.type));
            }
        expect (boundsIn (mc, picker).getBottom() <= mc.getHeight() && boundsIn (mc, picker).getRight() <= mc.getWidth(), "S-07 fits 800x560");
        click (findById (picker, "picker.reverb.add"));
        expectEquals (int (c->getChain().size()), 5);
        expectEquals (juce::String (c->getChain().back().type), juce::String ("reverb"));
        expect (! findById (&mc, "main.overlay")->isVisible(), "the picker closes after adding");

        mc.showEffectPicker (0);
        click (findById (findById (&mc, "panel.effectPicker"), "picker.compressor.add"));
        expectEquals (juce::String (c->getChain().front().type), juce::String ("compressor"));

        beginTest ("S-09: every parameter editable (knobs, choices as ComboBox), one panel at a time, Esc closes (AC-67)");
        c->dispatchPendingMessages();
        mc.showSlotDetail (0);
        auto* panel = dynamic_cast<SlotDetailPanel*> (findById (&mc, "panel.slotDetail"));
        expect (panel != nullptr);
        const auto* info = findEffectInfo (c->getChain()[0].type);
        expectEquals (panel->numKnobs() + panel->numChoices(), int (info->params.size()));
        expect (boundsIn (mc, panel).getBottom() <= mc.getHeight(), "S-09 fits 800x560");
        auto* k = dynamic_cast<Knob*> (panel->controlFor (0));
        expect (k != nullptr);
        k->setValue (-30.0, juce::sendNotificationSync);
        expectWithinAbsoluteError (c->getChain()[0].params[0], -30.0f, 0.01f);

        mc.showSlotDetail (1); // distortion: has a choice
        panel = dynamic_cast<SlotDetailPanel*> (findById (&mc, "panel.slotDetail"));
        int panels = 0;
        for (auto* child : findById (&mc, "main.overlay")->getChildren()) panels += dynamic_cast<SlotDetailPanel*> (child) != nullptr;
        expectEquals (panels, 1);
        const auto* dist = findEffectInfo (c->getChain()[1].type);
        for (int p = 0; p < int (dist->params.size()); ++p)
            if (dist->params[size_t (p)].isChoice())
            {
                auto* box = dynamic_cast<juce::ComboBox*> (panel->controlFor (p));
                expect (box != nullptr);
                box->setSelectedId (2, juce::sendNotificationSync);
                expectWithinAbsoluteError (c->getChain()[1].params[size_t (p)], 1.0f, 0.001f);
            }
        panel->keyPressed (juce::KeyPress (juce::KeyPress::escapeKey));
        expect (! findById (&mc, "main.overlay")->isVisible(), "Esc closes S-09");
    }

    // ---------------------------------------------------------------------------------------------
    void navigationAndOverlays()
    {
        beginTest ("output device link opens S-03 devices (F-01-11, AC-62); help menu panels; Esc closes overlays");
        auto c = makeController (kSixFavourites);
        MainComponent mc (*c);
        mc.setSize (Theme::defaultWidth, Theme::defaultHeight);
        click (visibleById (&mc, "voice.outputDeviceLink"));
        expect (visibleById (&mc, "page.settings") != nullptr && visibleById (&mc, "page.voice") == nullptr);
        mc.showPage (Navigator::Page::voice);
        expect (visibleById (&mc, "page.voice") != nullptr);

        mc.showHelp (Navigator::HelpTopic::licenses);
        expect (findById (&mc, "main.overlay")->isVisible());
        auto* host = dynamic_cast<OverlayHost*> (findById (&mc, "main.overlay"));
        host->keyPressed (juce::KeyPress (juce::KeyPress::escapeKey));
        expect (! host->isVisible());

        beginTest ("first run shows S-04; an interrupted tour asks to resume (F-13-4)");
        c->updateSettings ([] (Settings& s) { s.setupDone = false; });
        {
            MainComponent first (*c);
            auto* overlay = dynamic_cast<OverlayHost*> (findById (&first, "main.overlay"));
            expect (overlay->isVisible() && dynamic_cast<SetupWizard*> (overlay->panel()) != nullptr);
        }
        beginTest ("S-04: 完了して始める starts the tour the first time only, あとで設定する does not (F-13-1)");
        auto runSetup = [&] (int tourStep, const char* button, int clicks)
        {
            c->updateSettings ([tourStep] (Settings& s) { s.setupDone = false; s.tourStep = tourStep; });
            MainComponent m (*c);
            for (int i = 0; i < clicks; ++i) click (findById (&m, button));
            expect (c->getSettings().setupDone);
            expect (! findById (&m, "main.overlay")->isVisible(), "the wizard is closed");
            auto* t = findById (&m, "main.tour");
            return t != nullptr && t->isVisible();
        };
        expect (! runSetup (-1, "setup.later", 1), "あとで設定する: no tour");
        expect (runSetup (-1, "setup.next", 3), "完了して始める on the first run: tour");
        expect (! runSetup (7, "setup.next", 3), "S-04 again after the tour: no replay");

        c->updateSettings ([] (Settings& s) { s.setupDone = true; s.tourStep = 3; });
        {
            MainComponent resume (*c);
            auto* overlay = dynamic_cast<OverlayHost*> (findById (&resume, "main.overlay"));
            expect (overlay->isVisible() && dynamic_cast<ConfirmPanel*> (overlay->panel()) != nullptr);
        }
    }

    // ---------------------------------------------------------------------------------------------
    void tour()
    {
        beginTest ("tour: 7 steps, texts within 60 characters, n / 7, progress saved, skip ends it (spec 8.5, F-13-3, AC-52)");
        expectEquals (int (GuideTour::steps().size()), 7);
        for (auto& s : GuideTour::steps())
            expect (juce::String::fromUTF8 (s.text).length() <= 60, juce::String::fromUTF8 (s.title));

        auto c = makeController (kSixFavourites);
        MainComponent mc (*c);
        mc.setSize (Theme::minWidth, Theme::minHeight);
        mc.startTour (false);
        auto* t = dynamic_cast<GuideTour*> (findById (&mc, "main.tour"));
        expect (t != nullptr && t->isVisible());
        expectEquals (t->currentStep(), 0);
        expectEquals (c->getSettings().tourStep, 0);
        // banners appear during the tour (E-31): the tour goes on and its bubble keeps clear of them
        dynamic_cast<NoticeBar*> (findById (&mc, "main.notices"))->setNotices ({ notice ("a", NoticeLevel::danger), notice ("b", NoticeLevel::warning) });
        expect (t->isVisible() && t->currentStep() == 0);
        auto* bubble = findById (t, "tour.bubble");
        expect (! boundsIn (mc, bubble).intersects (boundsIn (mc, visibleById (&mc, "tour.voiceToggle"))), "the bubble does not cover the part");
        expect (! boundsIn (mc, bubble).intersects (boundsIn (mc, findById (&mc, "main.notices"))), "nor the banners (E-31)");
        t->next();
        expectEquals (t->currentStep(), 1);
        expectEquals (c->getSettings().tourStep, 1);
        t->back();
        expectEquals (t->currentStep(), 0);

        int guard = 0;
        int last = t->currentStep();
        while (t->isVisible() && ++guard < 20)
        {
            t->next();
            if (t->isVisible())
            {
                expect (t->currentStep() > last, "steps move forward (missing parts are skipped, E-32)");
                last = t->currentStep();
                expect (! boundsIn (mc, bubble).intersects (boundsIn (mc, findById (&mc, "main.notices"))), "bubble clear of the banners");
            }
        }
        expect (! t->isVisible());
        expectEquals (c->getSettings().tourStep, 7);
        expect (visibleById (&mc, "page.voice") != nullptr, "back on the voice page");

        c->updateSettings ([] (Settings& s) { s.tourStep = 4; });
        mc.startTour (true);
        t = dynamic_cast<GuideTour*> (findById (&mc, "main.tour"));
        expect (t->currentStep() >= 4, "resume starts at the saved step");
        t->skip();
        expectEquals (c->getSettings().tourStep, 7);

        // without banners, at the default size, no step is skipped (the hotkey card is taller than the window:
        // the tour points at its hint line, E-32)
        dynamic_cast<NoticeBar*> (findById (&mc, "main.notices"))->setNotices ({});
        c->updateSettings ([] (Settings& s) { s.tourStep = -1; });
        mc.startTour (false);
        t = dynamic_cast<GuideTour*> (findById (&mc, "main.tour"));
        int visited = 0;
        for (int g = 0; t->isVisible() && g < 20; ++g, t->next())
        {
            expectEquals (t->currentStep(), visited, "every step in order");
            ++visited;
        }
        expectEquals (visited, 7);
    }

    // ---------------------------------------------------------------------------------------------
    void theme()
    {
        beginTest ("light theme switch rebuilds the pages (F-14-2)");
        auto c = makeController (kSixFavourites);
        MainComponent mc (*c);
        mc.setSize (Theme::defaultWidth, Theme::defaultHeight);
        expect (Theme::isDark());
        c->updateSettings ([] (Settings& s) { s.darkTheme = false; });
        c->dispatchPendingMessages();
        expect (! Theme::isDark());
        expect (visibleById (&mc, "page.voice") != nullptr && visibleById (&mc, "tour.pitch") != nullptr);
        checkLayout (mc, "light");
        c->updateSettings ([] (Settings& s) { s.darkTheme = true; });
        c->dispatchPendingMessages();
        expect (Theme::isDark());
    }

    // ---------------------------------------------------------------------------------------------
    void screenSettings()
    {
        beginTest ("S-03 外観 detailed settings reach the screen: 拡大率 (min window size), animations, meter, knobs (INTERFACES.md 7.3)");
        auto c = makeController (kSixFavourites);
        {
            MainComponent mc (*c);
            const Theme::Prefs defaults;
            expectEquals (juce::Desktop::getInstance().getGlobalScaleFactor(), 1.0f, "default 100 %");
            expect (minimumWindowSize (c->getSettings()) == juce::Point<int> (Theme::minWidth, Theme::minHeight));
            expectEquals (Theme::prefs().meterFps, 30);
            expectEquals (Theme::prefs().peakHoldMs, 1500.0f, "the old 45 frames at 30 fps");
            expectEquals (Theme::prefs().animations, defaults.animations);

            c->updateSettings ([] (Settings& s)
            {
                s.uiScalePercent = 125;
                s.animations = 2;
                s.meterFps = 60;
                s.knobSensitivity = 2;
                s.knobWheel = false;
            });
            c->dispatchPendingMessages();
            expectEquals (juce::Desktop::getInstance().getGlobalScaleFactor(), 1.25f, "拡大率 125 %");
            expect (minimumWindowSize (c->getSettings()) == juce::Point<int> (1000, 700), "the smallest window grows with the scale");
            expect (! Theme::animationsEnabled(), "画面の動き オフ");
            expectEquals (Theme::prefs().meterFps, 60);
            expectEquals (Theme::prefs().knobSensitivity, 2);
            expect (! Theme::prefs().knobWheel);
            c->updateSettings ([] (Settings& s) { s.animations = 1; });
            c->dispatchPendingMessages();
            expect (Theme::animationsEnabled(), "画面の動き オン");

            c->updateSettings ([] (Settings& s)
            {
                const Settings d;
                s.uiScalePercent = d.uiScalePercent;
                s.animations = d.animations;
                s.meterFps = d.meterFps;
                s.knobSensitivity = d.knobSensitivity;
                s.knobWheel = d.knobWheel;
            });
            c->dispatchPendingMessages();
            expectEquals (juce::Desktop::getInstance().getGlobalScaleFactor(), 1.0f);
            expectEquals (Theme::prefs().meterFps, 30);
        }
        juce::Desktop::getInstance().setGlobalScaleFactor (1.0f);
    }
};

static UiMainTests uiMainTests;
} // namespace koe::ui

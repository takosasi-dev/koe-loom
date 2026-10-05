// つまみの形（細い棒）とスロットの右クリックメニュー (INTERFACES.md §12.3, owner wave10/ui). Category "UiTweak".
// No window, no audio device, no menu on screen: AppController (false), components laid out offscreen, mouse events
// handed to the Knob directly, and the slot menu checked through slotMenuItems / runSlotAction (never showSlotMenu).

#include "App/AppController.h"
#include "Core/Paths.h"
#include "Effects/EffectRegistry.h"
#include "Platform/Hotkeys.h"
#include "Tests/TestUtil.h"
#include "UI/MainComponent.h"
#include "UI/Screens.h"
#include "UI/main/ChainStrip.h"
#include "UI/main/Panels.h"
#include "UI/main/Shell.h"
#include "UI/screens/Common.h"

#include <cmath>

namespace koe
{
namespace
{
using namespace ui;
using namespace ui::mainui;

void freshDataDir()
{
    auto d = paths::dataDir();
    if (d.getFullPathName().contains ("KoeLoomTests")) d.deleteRecursively();
    d.createDirectory();
}

std::unique_ptr<AppController> makeController (const char* preset = "character-demon-king", int knobStyle = 1)
{
    auto c = std::make_unique<AppController> (false);
    c->startup();
    c->updateSettings ([knobStyle] (Settings& s)
    {
        s.setupDone = true;
        s.tourStep = 7;
        s.layoutStyle = 0;
        s.knobStyle = knobStyle;
    });
    c->setVoiceChangerOn (true); // the chain runs (the looper records)
    c->loadPreset (preset); // 魔王: distortion, ringmod, echo, reverb
    c->dispatchPendingMessages();
    return c;
}

/** Theme::prefs() (knobStyle, sensitivity, wheel) back as they were when the test ends: other categories make Knobs too. */
struct PrefsScope
{
    Theme::Prefs saved = Theme::prefs();
    bool wasOff = animationsOff();
    PrefsScope() { animationsOff() = true; }
    ~PrefsScope()
    {
        Theme::prefs() = saved;
        animationsOff() = wasOff;
    }
};

juce::MouseEvent mouse (juce::Component& comp, juce::Point<float> at, juce::Point<float> down, juce::ModifierKeys mods)
{
    const auto now = juce::Time::getCurrentTime();
    return juce::MouseEvent (juce::Desktop::getInstance().getMainMouseSource(), at, mods, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, &comp, &comp, now, down, now, 1,
                             at != down);
}

juce::String nameOf (const juce::Component* c)
{
    if (c->getComponentID().isNotEmpty()) return c->getComponentID();
    if (c->getTitle().isNotEmpty()) return c->getTitle();
    return typeid (*c).name();
}

/** "" when the visible children of parent lie inside it and apart from each other; otherwise what is wrong. */
juce::String layoutProblem (const juce::Component& parent)
{
    juce::Array<juce::Component*> kids;
    for (auto* ch : parent.getChildren())
        if (ch->isVisible() && ! ch->getBounds().isEmpty()) kids.add (ch);
    for (int i = 0; i < kids.size(); ++i)
    {
        if (! parent.getLocalBounds().contains (kids[i]->getBounds()))
            return nameOf (kids[i]) + " " + kids[i]->getBounds().toString() + " outside " + parent.getLocalBounds().toString();
        for (int j = i + 1; j < kids.size(); ++j)
            if (kids[i]->getBounds().intersects (kids[j]->getBounds()))
                return nameOf (kids[i]) + " " + kids[i]->getBounds().toString() + " overlaps " + nameOf (kids[j]) + " " + kids[j]->getBounds().toString();
    }
    return {};
}

juce::TextEditor* editorOf (juce::Component& c)
{
    for (auto* ch : c.getChildren())
        if (auto* e = dynamic_cast<juce::TextEditor*> (ch)) return e;
    return nullptr;
}

bool sameColour (juce::Colour a, juce::Colour b)
{
    return std::abs (a.getRed() - b.getRed()) <= 2 && std::abs (a.getGreen() - b.getGreen()) <= 2 && std::abs (a.getBlue() - b.getBlue()) <= 2;
}

/** Navigator that records instead of showing; the E-27 question is kept so the test can answer it. */
struct TestNav : Navigator
{
    void showPage (Page) override {}
    void showSettings (SettingsSection) override {}
    void showPresetBrowser() override {}
    void showEffectPicker (int) override {}
    void showSlotDetail (int s) override { detail = s; }
    void showSetupWizard() override {}
    void showHelp (HelpTopic) override {}
    void startTour (bool) override {}
    void showOverlay (std::unique_ptr<juce::Component> panel) override { overlay = std::move (panel); }
    void closeOverlay() override {} // the panel stays until the next one (ConfirmPanel runs its action after closing)
    void showToast (const juce::String& text) override { toasts.add (text); }

    /** Presses 続ける on the question; false when there is none. */
    bool confirm()
    {
        if (overlay == nullptr) return false;
        for (auto* ch : overlay->getChildren())
            if (auto* b = dynamic_cast<juce::Button*> (ch); b != nullptr && b->getButtonText() == ja ("続ける") && b->onClick)
            {
                b->onClick();
                return true;
            }
        return false;
    }

    int detail = -1;
    std::unique_ptr<juce::Component> overlay;
    juce::StringArray toasts;
};

juce::StringArray chainTypes (const AppController& c)
{
    juce::StringArray t;
    for (auto& s : c.getChain()) t.add (juce::String (s.type));
    return t;
}

void fillTo (AppController& c, int slots, const char* type)
{
    juce::String why;
    while (int (c.getChain().size()) < slots && c.addEffect (type, why)) {}
}
} // namespace

class UiTweakTests : public juce::UnitTest
{
public:
    UiTweakTests() : juce::UnitTest ("Knob style and slot menu (wave10/ui)", "UiTweak") {}

    void runTest() override
    {
        barKnobInput();
        barKnobDrawing();
        slotCards();
        slotDetail();
        shifter();
        settingSwitch();
        menuItems();
        menuActions();
        menuKeys();
    }

private:
    // ---------------------------------------------------------------------------------------------
    void barKnobInput()
    {
        beginTest ("棒: sideways drag by the sensitivity (Shift = fine, ゆっくり), double-click, arrows, Home, typed value, right-click, wheel; range and skew");
        PrefsScope prefs;
        Theme::prefs().knobStyle = 1;
        Theme::prefs().knobSensitivity = 1;
        const ParamSpec hz { "cutoffHz", "カットオフ（ローパス）", 20.0f, 20000.0f, 1000.0f, "Hz" };
        Knob k;
        expect (k.isBar(), "made while 棒");
        expect (k.getSliderStyle() == juce::Slider::RotaryHorizontalDrag, "drags sideways only");
        k.setup (hz.min, hz.max, hz.def, 0.0, [hz] (double v) { return formatParam (hz, float (v)); });
        applyParamRange (k, hz, hz.def);
        k.setLabel (juce::String::fromUTF8 (hz.nameJa));
        expectEquals (k.barHeight(), 32);
        k.setSize (160, k.barHeight());
        expectWithinAbsoluteError (k.proportionOfLengthToValue (0.5), std::sqrt (20.0 * 20000.0), 0.01); // the skew of applyParamRange
        expect (k.getTooltip().contains (juce::String::fromUTF8 ("初期値 1 kHz")), "tooltip as before: " + k.getTooltip());

        const auto left = juce::ModifierKeys (juce::ModifierKeys::leftButtonModifier);
        auto drag = [&k] (float dx, float dy, juce::ModifierKeys mods)
        {
            const juce::Point<float> from (80.0f, 16.0f), to (80.0f + dx, 16.0f + dy);
            k.mouseDown (mouse (k, from, from, mods));
            k.mouseDrag (mouse (k, to, from, mods));
            k.mouseUp (mouse (k, to, from, mods));
        };
        auto prop = [&k] { return k.valueToProportionOfLength (k.getValue()); };
        int changes = 0;
        k.onValueChange = [&changes] { ++changes; };

        double p0 = prop();
        drag (50.0f, 0.0f, left);
        expectWithinAbsoluteError (prop(), p0 + 50.0 / 200.0, 1.0e-3);
        expect (changes > 0, "onValueChange runs while dragging");
        p0 = prop();
        drag (0.0f, -80.0f, left);
        expectWithinAbsoluteError (prop(), p0, 1.0e-9);
        p0 = prop();
        drag (60.0f, 0.0f, left.withFlags (juce::ModifierKeys::shiftModifier));
        expectWithinAbsoluteError (prop(), p0 + 60.0 / 1200.0, 1.0e-3);
        Theme::prefs().knobSensitivity = 0; // ゆっくり: 1.6 x the distance
        p0 = prop();
        drag (-64.0f, 0.0f, left);
        expectWithinAbsoluteError (prop(), p0 - 64.0 / 320.0, 1.0e-3);
        Theme::prefs().knobSensitivity = 1;
        drag (2000.0f, 0.0f, left);
        expectWithinAbsoluteError (k.getValue(), 20000.0, 1.0e-6);
        drag (-4000.0f, 0.0f, left);
        expectWithinAbsoluteError (k.getValue(), 20.0, 1.0e-6);

        k.mouseDoubleClick (mouse (k, { 80.0f, 16.0f }, { 80.0f, 16.0f }, left));
        expectWithinAbsoluteError (k.getValue(), 1000.0, 1.0e-6);
        expect (k.keyPressed (juce::KeyPress (juce::KeyPress::rightKey)));
        expectWithinAbsoluteError (k.getValue(), 1000.0 + 5 * 199.8, 1.0e-6);
        expect (k.keyPressed (juce::KeyPress (juce::KeyPress::leftKey, juce::ModifierKeys::shiftModifier, 0)));
        expectWithinAbsoluteError (k.getValue(), 1000.0 + 4 * 199.8, 1.0e-6);
        expect (k.keyPressed (juce::KeyPress (juce::KeyPress::homeKey)));
        expectWithinAbsoluteError (k.getValue(), 1000.0, 1.0e-6);

        expect (k.keyPressed (juce::KeyPress (juce::KeyPress::returnKey)), "Enter types a value");
        auto* ed = editorOf (k);
        expect (ed != nullptr, "the value editor");
        if (ed != nullptr)
        {
            ed->setText ("2500", false);
            ed->onReturnKey();
            expectWithinAbsoluteError (k.getValue(), 2500.0, 1.0e-6);
        }
        k.editValue();
        if ((ed = editorOf (k)) != nullptr)
        {
            ed->setText ("99999", false);
            ed->onReturnKey();
            expectWithinAbsoluteError (k.getValue(), 20000.0, 1.0e-6);
        }
        k.removeChildComponent (editorOf (k));
        const auto right = juce::ModifierKeys (juce::ModifierKeys::rightButtonModifier);
        const double before = k.getValue();
        k.mouseDown (mouse (k, { 80.0f, 16.0f }, { 80.0f, 16.0f }, right));
        expect (editorOf (k) != nullptr, "right-click types a value");
        expectEquals (k.getValue(), before);

        k.setValue (1000.0, juce::dontSendNotification);
        Theme::prefs().knobWheel = false;
        k.mouseWheelMove (mouse (k, { 80.0f, 16.0f }, { 80.0f, 16.0f }, {}), { 0.0f, 0.5f, false, false, false });
        expectEquals (k.getValue(), 1000.0, "wheel OFF: no change");
        Theme::prefs().knobWheel = true;
        juce::Thread::sleep (2); // a new event time (the slider drops repeats)
        k.mouseWheelMove (mouse (k, { 80.0f, 16.0f }, { 80.0f, 16.0f }, {}), { 0.0f, 0.5f, false, false, false });
        expect (k.getValue() > 1000.0, "wheel ON turns it");

        beginTest ("棒: the arc knob stays an arc while the setting is 円 (style and drag both ways)");
        Theme::prefs().knobStyle = 0;
        Knob arc (Knob::Size::big);
        expect (! arc.isBar());
        expect (arc.getSliderStyle() == juce::Slider::RotaryHorizontalVerticalDrag);
    }

    void barKnobDrawing()
    {
        beginTest ("棒: filled from 0 across 0, from the start otherwise; the text row only while there is room");
        PrefsScope prefs;
        Theme::prefs().knobStyle = 1;
        const auto& p = Theme::colours();
        Knob k;
        k.setup (-12.0, 12.0, 0.0, 0.1, formatSemitones, "st");
        k.setLabel (ja ("ピッチ"));
        k.setSize (214, 32); // track x 7..207, centre y 24
        k.setValue (-6.0, juce::dontSendNotification);
        auto img = k.createComponentSnapshot (k.getLocalBounds(), true, 1.0f);
        auto at = [&img] (double prop) { return img.getPixelAt (7 + juce::roundToInt (200.0 * prop), 24); };
        expect (sameColour (at (0.40), p.accent), "between the value and 0: accent");
        expect (sameColour (at (0.10), p.border), "below the value: the track");
        expect (sameColour (at (0.65), p.border), "above 0: the track");
        k.setRange (0.0, 1.0, 0.0);
        k.setValue (0.5, juce::dontSendNotification);
        img = k.createComponentSnapshot (k.getLocalBounds(), true, 1.0f);
        expect (sameColour (at (0.10), p.accent), "0..1: filled from the start");
        expect (sameColour (at (0.80), p.border));

        auto inkRows = [] (const juce::Image& im, int y0, int y1)
        {
            int n = 0;
            for (int y = y0; y < y1; ++y)
                for (int x = 0; x < im.getWidth(); ++x) n += im.getPixelAt (x, y).getAlpha() > 0;
            return n;
        };
        expect (inkRows (img, 0, 14) > 0, "32 px: name and value over the bar");
        k.setSize (214, 20);
        expectEquals (inkRows (k.createComponentSnapshot (k.getLocalBounds(), true, 1.0f), 0, 2), 0, "20 px: the bar alone (thumb from y 2)");

        Knob big (Knob::Size::big);
        big.setup (-12.0, 12.0, 0.0, 0.1, formatSemitones, "st");
        expectEquals (big.barHeight(), 28 + 4 + 20);
        big.setDiameter (84);
        expectEquals (big.barHeight(), 20 + 4 + 20, "narrow: the smaller value");
        big.setSize (180, big.barHeight());
        expect (big.createComponentSnapshot (big.getLocalBounds()).isValid());
    }

    // ---------------------------------------------------------------------------------------------
    void slotCards()
    {
        beginTest ("棒: Studio slot cards (wide, narrow, every height down to the minimum) keep the bars apart from the header, ON/OFF and buttons");
        PrefsScope prefs;
        freshDataDir();
        auto c = makeController();
        MainComponent mc (*c);
        for (auto [w, h] : { std::pair (Theme::defaultWidth, Theme::defaultHeight), std::pair (Theme::minWidth, Theme::minHeight) })
        {
            mc.setSize (w, h);
            auto* strip = dynamic_cast<ChainStrip*> (findById (&mc, "tour.chain"));
            expect (strip != nullptr && strip->numCards() == 4, "the chain");
            if (strip == nullptr) return;
            for (int i = 0; i < strip->numCards(); ++i)
            {
                auto* card = strip->card (i);
                expect (layoutProblem (*card).isEmpty(), juce::String (w) + ": " + layoutProblem (*card));
                for (auto* k : card->knobs)
                {
                    expect (k->isBar(), "bars");
                    expect (k->getY() >= Theme::space1 + 20 && k->getBottom() <= card->toggle.getY(), "between the header and the ON/OFF row");
                    expect (k->getHeight() >= Theme::space5, "the full bar with its name and value: " + k->getBounds().toString());
                    expect (k->getWidth() >= card->getWidth() - Theme::space3 - Theme::space2, "full width");
                }
            }
        }
        auto* card = dynamic_cast<ChainStrip*> (findById (&mc, "tour.chain"))->card (1);
        for (const bool compact : { false, true })
            for (int h : { SlotCard::minHeight, 111, 112, 127, 128, 143, 144, 200 })
            {
                card->setCompact (compact);
                card->setSize (compact ? Theme::slotWNarrow : Theme::slotW, h);
                const auto where = juce::String (compact ? "narrow " : "wide ") + juce::String (h) + ": ";
                expect (layoutProblem (*card).isEmpty(), where + layoutProblem (*card));
                for (auto* k : card->knobs)
                {
                    expect (k->getY() >= Theme::space1 + 20 && k->getBottom() <= card->toggle.getY(), where + "inside the slot " + k->getBounds().toString());
                    expect (k->getHeight() >= 16, where + "a bar to grab");
                    if (h >= 128) expect (k->getHeight() >= 26, where + "tall enough for the name and value"); // the values tier of the arc knobs
                }
            }
    }

    void slotDetail()
    {
        beginTest ("棒: S-09 of every effect in 800x560 fits, its cells (bars, choices) and rows overlap nothing; the arc layout too");
        PrefsScope prefs;
        freshDataDir();
        for (const int style : { 1, 0 })
        {
            auto c = makeController ("natural-asis", style);
            int checked = 0;
            for (auto& info : allEffectInfos())
            {
                if (! hasEffectFactory (info.type)) continue;
                c->loadPreset ("natural-asis");
                while (! c->getChain().empty()) c->removeSlot (0);
                juce::String why;
                if (! c->addEffect (info.type, why)) continue;
                MainComponent mc (*c);
                mc.setSize (Theme::minWidth, Theme::minHeight);
                mc.showSlotDetail (0);
                auto* panel = dynamic_cast<SlotDetailPanel*> (findById (&mc, "panel.slotDetail"));
                if (panel == nullptr) { expect (false, juce::String (info.type) + ": panel"); continue; }
                const auto where = juce::String (style == 1 ? "bar " : "arc ") + info.type + ": ";
                expect (mc.getLocalBounds().contains (mc.getLocalArea (panel, panel->getLocalBounds())), where + "fits the window");
                expect (layoutProblem (*panel).isEmpty(), where + layoutProblem (*panel));
                for (int pi = 0; pi < int (info.params.size()); ++pi)
                    if (auto* k = dynamic_cast<Knob*> (panel->controlFor (pi)))
                    {
                        expect (k->isBar() == (style == 1), where + "the shape");
                        if (style == 1) expect (k->getHeight() >= Theme::space5 && k->getWidth() >= Theme::space5 * 4, where + "a bar with its text " + k->getBounds().toString());
                    }
                ++checked;
            }
            expect (checked >= 31, "every effect: " + juce::String (checked)); // 30 + convolution (wave 7 added types, not effects)
        }

        beginTest ("棒: S-09 bars are lower than the arc cells, and set the parameter");
        auto c = makeController();
        auto height = [&c] (int style)
        {
            c->updateSettings ([style] (Settings& s) { s.knobStyle = style; });
            MainComponent mc (*c);
            mc.setSize (Theme::minWidth, Theme::minHeight);
            mc.showSlotDetail (0);
            auto* panel = findById (&mc, "panel.slotDetail");
            return panel != nullptr ? panel->getHeight() : 0;
        };
        const int arcH = height (0), barH = height (1); // leaves 棒 set
        expect (barH < arcH, "the 魔王 distortion panel is lower with bars: " + juce::String (barH) + " / " + juce::String (arcH));
        MainComponent mc (*c);
        mc.setSize (Theme::minWidth, Theme::minHeight);
        mc.showSlotDetail (0);
        if (auto* panel = dynamic_cast<SlotDetailPanel*> (findById (&mc, "panel.slotDetail")))
            for (int pi = 0; pi < panel->numKnobs() + panel->numChoices(); ++pi)
                if (auto* k = dynamic_cast<Knob*> (panel->controlFor (pi)))
                {
                    expectEquals (k->getComponentID(), "slotDetail.param." + juce::String (pi), "IDs as before");
                    k->setValue (k->getMinimum(), juce::sendNotificationSync);
                    expectWithinAbsoluteError (c->getChain()[0].params[size_t (pi)], float (k->getMinimum()), 1.0e-4f);
                    break;
                }
    }

    void shifter()
    {
        beginTest ("棒: 声の変換 (wide, narrow, narrow with 3 banners): ピッチ / フォルマント / 変換 apart, the large value shown, setPitch");
        PrefsScope prefs;
        freshDataDir();
        auto c = makeController();
        for (const int banners : { 0, 3 })
            for (auto [w, h] : { std::pair (Theme::defaultWidth, Theme::defaultHeight), std::pair (Theme::minWidth, Theme::minHeight) })
            {
                MainComponent mc (*c);
                mc.setSize (w, h);
                if (banners > 0)
                    if (auto* bar = dynamic_cast<NoticeBar*> (findById (&mc, "main.notices")))
                        bar->setNotices ({ { "t1", NoticeLevel::danger, ja ("（試験）1 件目"), false },
                                           { "t2", NoticeLevel::warning, ja ("（試験）2 件目"), true },
                                           { "t3", NoticeLevel::info, ja ("（試験）3 件目") } });
                auto* box = findById (&mc, "voice.shifter");
                auto* pitch = dynamic_cast<Knob*> (findById (&mc, "tour.pitch"));
                auto* formant = dynamic_cast<Knob*> (findById (&mc, "tour.formant"));
                const auto where = juce::String (w) + "x" + juce::String (h) + " banners " + juce::String (banners) + ": ";
                expect (box != nullptr && pitch != nullptr && formant != nullptr, where + "parts");
                if (box == nullptr || pitch == nullptr || formant == nullptr) continue;
                expect (pitch->isBar() && formant->isBar());
                expect (layoutProblem (*box).isEmpty(), where + layoutProblem (*box));
                expect (pitch->getY() >= Theme::space4, where + "under the header row");
                expect (pitch->getHeight() >= int (Theme::fontS) + 14 && formant->getHeight() == pitch->getHeight(), where + "value row shown " + pitch->getBounds().toString());
                expect (pitch->getWidth() == box->getWidth(), where + "full width");
            }
        MainComponent mc (*c);
        mc.setSize (Theme::defaultWidth, Theme::defaultHeight);
        if (auto* pitch = dynamic_cast<Knob*> (findById (&mc, "tour.pitch")))
        {
            pitch->setValue (-7.0, juce::sendNotificationSync);
            expectWithinAbsoluteError (c->getPitch(), -7.0f, 1.0e-4f);
            expectEquals (pitch->getValueText(), juce::String ("-7"));
        }
    }

    // ---------------------------------------------------------------------------------------------
    void settingSwitch()
    {
        beginTest ("つまみの形: S-03 外観 rebuilds the pages with bars and back; the row is found by つまみ / ノブ / 棒 / スライダー / バー");
        PrefsScope prefs;
        freshDataDir();
        auto c = makeController ("character-demon-king", 0);
        MainComponent mc (*c);
        mc.setSize (Theme::defaultWidth, Theme::defaultHeight);
        auto shape = [&mc] (const char* id)
        {
            auto* k = dynamic_cast<Knob*> (findById (&mc, id));
            return k == nullptr ? -1 : k->isBar() ? 1 : 0;
        };
        auto slotShape = [&mc]
        {
            auto* strip = dynamic_cast<ChainStrip*> (findById (&mc, "tour.chain"));
            return strip == nullptr || strip->numCards() == 0 || strip->card (0)->knobs.isEmpty() ? -1 : strip->card (0)->knobs[0]->isBar() ? 1 : 0;
        };
        expectEquals (shape ("tour.pitch"), 0, "円 to start with");
        expectEquals (slotShape(), 0);
        mc.showSettings (Navigator::SettingsSection::appearance);
        auto* seg = dynamic_cast<screens::Segmented*> (findById (&mc, "settings.knobStyle"));
        expect (seg != nullptr && visibleWithin (seg, &mc), "the row in 外観");
        if (seg == nullptr) return;
        expectEquals (seg->getSelected(), 0);
        expectEquals (seg->getButton (0)->getButtonText(), ja ("円（つまみ）"));
        expectEquals (seg->getButton (1)->getButtonText(), ja ("棒"));
        seg->setSelected (1, true);
        c->dispatchPendingMessages(); // the change message: MainComponent rebuilds the pages
        expectEquals (c->getSettings().knobStyle, 1);
        expectEquals (shape ("tour.pitch"), 1, "the voice page was rebuilt with bars");
        expectEquals (shape ("tour.formant"), 1);
        expectEquals (slotShape(), 1);
        seg = dynamic_cast<screens::Segmented*> (findById (&mc, "settings.knobStyle"));
        expect (seg != nullptr && visibleWithin (seg, &mc) && seg->getSelected() == 1, "still on 外観, 棒 selected");

        auto* view = dynamic_cast<SettingsView*> (findById (&mc, "page.settings"));
        expect (view != nullptr);
        if (view != nullptr)
        {
            for (const char* q : { "つまみの形", "つまみ", "ノブ", "棒", "スライダー", "バー", "すらいだー" })
            {
                view->setSearchText (juce::String::fromUTF8 (q));
                expect (visibleWithin (findById (&mc, "settings.knobStyle"), &mc), juce::String::fromUTF8 (q) + ": found");
            }
            view->setSearchText (ja ("ピークの表示時間"));
            expect (! visibleWithin (findById (&mc, "settings.knobStyle"), &mc), "not on an unrelated search");
            view->setSearchText ({});
        }
        if ((seg = dynamic_cast<screens::Segmented*> (findById (&mc, "settings.knobStyle"))) != nullptr) seg->setSelected (0, true);
        c->dispatchPendingMessages();
        expectEquals (shape ("tour.pitch"), 0, "back to the arc");
        expectEquals (slotShape(), 0);
        expectEquals (Theme::prefs().knobStyle, 0);
    }

    // ---------------------------------------------------------------------------------------------
    void menuItems()
    {
        beginTest ("スロットのメニュー: the entries, ON/OFF wording, the ends, 10 slots, once-per-chain types, Mono's 上へ / 下へ");
        PrefsScope prefs;
        freshDataDir();
        auto c = makeController();
        auto items = slotMenuItems (*c, 0, false);
        expectEquals (int (items.size()), 7);
        if (items.size() != 7) return;
        const char* texts[] = { "OFF にする", "詳細を開く", "複製", "初期値に戻す", "左へ移動", "右へ移動", "削除" };
        const SlotAction actions[] = { SlotAction::toggle, SlotAction::detail, SlotAction::duplicate, SlotAction::reset,
                                       SlotAction::moveBack, SlotAction::moveForward, SlotAction::remove };
        for (int i = 0; i < 7; ++i)
        {
            expectEquals (items[size_t (i)].text, juce::String::fromUTF8 (texts[i]));
            expect (items[size_t (i)].action == actions[i]);
            expect (items[size_t (i)].enabled == (actions[i] != SlotAction::moveBack), juce::String::fromUTF8 (texts[i]) + " on the first slot");
        }
        items = slotMenuItems (*c, 3, false);
        expect (items[4].enabled && ! items[5].enabled, "the last slot: left yes, right no");
        items = slotMenuItems (*c, 1, true);
        expectEquals (items[4].text, ja ("上へ移動"));
        expectEquals (items[5].text, ja ("下へ移動"));
        expect (items[4].enabled && items[5].enabled, "a middle slot moves both ways");
        juce::String why;
        expect (c->setSlotEnabled (1, false, why));
        expectEquals (slotMenuItems (*c, 1, false)[0].text, ja ("ON にする"));
        expect (slotMenuItems (*c, -1, false).empty() && slotMenuItems (*c, 4, false).empty(), "no slot, no entries");

        fillTo (*c, kMaxSlots, "compressor");
        expectEquals (int (c->getChain().size()), kMaxSlots);
        for (int s = 0; s < kMaxSlots; ++s)
        {
            items = slotMenuItems (*c, s, false);
            expect (! items[2].enabled, "10 slots: no 複製");
            expect (items[0].enabled && items[1].enabled && items[3].enabled && items[6].enabled);
        }
        c->loadPreset ("natural-asis");
        while (! c->getChain().empty()) c->removeSlot (0);
        for (const char* type : { "freeze", "looper", "compressor" })
            expect (c->addEffect (type, why), why);
        expect (! slotMenuItems (*c, 0, false)[2].enabled && ! slotMenuItems (*c, 1, false)[2].enabled, "freeze / looper: once per chain");
        expect (slotMenuItems (*c, 2, false)[2].enabled, "compressor can be copied");
    }

    void menuActions()
    {
        beginTest ("スロットのメニュー: ON/OFF, 詳細, 複製, 初期値に戻す, 移動, 削除 do what they say; refusals come as toasts");
        PrefsScope prefs;
        freshDataDir();
        auto c = makeController();
        TestNav nav;
        juce::Component owner;
        runSlotAction (*c, nav, owner, 1, SlotAction::toggle);
        expect (! c->getChain()[1].enabled, "OFF");
        runSlotAction (*c, nav, owner, 1, SlotAction::toggle);
        expect (c->getChain()[1].enabled, "ON again");
        runSlotAction (*c, nav, owner, 2, SlotAction::detail);
        expectEquals (nav.detail, 2);

        const auto first = c->getChain()[0];
        runSlotAction (*c, nav, owner, 0, SlotAction::duplicate);
        expectEquals (int (c->getChain().size()), 5);
        expect (c->getChain()[0] == first && c->getChain()[1] == first, "a copy right after it");
        expect (c->isCurrentPresetModified());

        const auto* info = findEffectInfo (c->getChain()[1].type);
        c->setSlotParam (1, 0, info->params[0].max);
        c->setSlotMod (1, "wet", 0.5f);
        juce::String why;
        c->setSlotEnabled (1, false, why);
        runSlotAction (*c, nav, owner, 1, SlotAction::reset);
        for (size_t pi = 0; pi < info->params.size(); ++pi)
            expectWithinAbsoluteError (c->getChain()[1].params[pi], info->params[pi].def, 1.0e-5f);
        expect (c->getChain()[1].modTarget.empty() && ! c->getChain()[1].enabled, "mod off, ON/OFF kept");

        const auto types = chainTypes (*c);
        runSlotAction (*c, nav, owner, 2, SlotAction::moveForward);
        auto moved = types;
        moved.move (2, 3);
        expect (chainTypes (*c) == moved, "right: " + chainTypes (*c).joinIntoString (","));
        runSlotAction (*c, nav, owner, 3, SlotAction::moveBack);
        expect (chainTypes (*c) == types, "left again");
        runSlotAction (*c, nav, owner, 0, SlotAction::moveBack); // the first slot: nothing
        expect (chainTypes (*c) == types);
        runSlotAction (*c, nav, owner, 0, SlotAction::remove);
        expectEquals (int (c->getChain().size()), 4);
        expect (nav.toasts.isEmpty(), "no refusal so far: " + nav.toasts.joinIntoString (" / "));

        fillTo (*c, kMaxSlots, "compressor");
        runSlotAction (*c, nav, owner, 0, SlotAction::duplicate);
        expectEquals (int (c->getChain().size()), kMaxSlots);
        expect (nav.toasts.size() == 1 && nav.toasts[0].contains ("10"), "10 slots: the reason as a toast");

        c->loadPreset ("natural-asis");
        while (! c->getChain().empty()) c->removeSlot (0);
        for (const char* type : { "freeze", "autopitch", "autopitch", "autopitch" })
            c->addEffect (type, why);
        expect (! c->getChain()[3].enabled, "the 3rd heavy one comes OFF");
        nav.toasts.clear();
        runSlotAction (*c, nav, owner, 0, SlotAction::duplicate);
        runSlotAction (*c, nav, owner, 3, SlotAction::toggle);
        expectEquals (nav.toasts.size(), 2);
        expect (nav.toasts[0].contains (ja ("1 つまで")) && nav.toasts[1].contains (ja ("2 つまで")), nav.toasts.joinIntoString (" / "));
        expectEquals (int (c->getChain().size()), 4);
        expect (! c->getChain()[3].enabled);

        beginTest ("スロットのメニュー: with a looper recording, 複製 / 移動 / 削除 ask first (E-27); a gone slot card does nothing");
        c->loadPreset ("natural-asis");
        while (! c->getChain().empty()) c->removeSlot (0);
        for (const char* type : { "compressor", "echo", "looper" })
            c->addEffect (type, why);
        auto& vp = c->getProcessorForTests();
        std::vector<float> in (480, 0.0f), out (480);
        auto run = [&] (int blocks)
        {
            for (int b = 0; b < blocks; ++b)
            {
                for (size_t i = 0; i < in.size(); ++i) in[i] = 0.2f * float (std::sin (0.05 * double (b * 480 + int (i))));
                vp.process (in.data(), out.data(), nullptr, int (in.size()));
            }
        };
        run (20);
        c->triggerSlot (2, EffectTrigger::looperRecordPlay);
        run (20);
        expect (c->hasLooperRecording(), "the looper holds a recording");
        const auto held = chainTypes (*c);
        for (auto action : { SlotAction::duplicate, SlotAction::moveForward, SlotAction::remove })
        {
            nav.overlay.reset();
            runSlotAction (*c, nav, owner, 0, action);
            expect (nav.overlay != nullptr && chainTypes (*c) == held, "asked first, nothing changed yet");
        }
        expect (nav.confirm(), "続ける");
        expect (chainTypes (*c) == juce::StringArray { "echo", "looper" }, "removed after 続ける: " + chainTypes (*c).joinIntoString (","));
        run (20);
        c->triggerSlot (1, EffectTrigger::looperRecordPlay);
        run (20);
        {
            auto gone = std::make_unique<juce::Component>();
            nav.overlay.reset();
            runSlotAction (*c, nav, *gone, 0, SlotAction::remove);
            gone.reset();
            nav.confirm();
            expectEquals (int (c->getChain().size()), 2, "the card was rebuilt meanwhile: nothing runs");
        }
        nav.overlay.reset();
        runSlotAction (*c, nav, owner, 0, SlotAction::reset); // no rebuild: no question
        expect (nav.overlay == nullptr);
    }

    void menuKeys()
    {
        beginTest ("スロットのメニュー: Shift+F10 and the menu key (read from the key state; JUCE never sends it to keyPressed)");
        expect (isSlotMenuKey (juce::KeyPress (juce::KeyPress::F10Key, juce::ModifierKeys::shiftModifier, 0)));
        expect (! isSlotMenuKey (juce::KeyPress (juce::KeyPress::F10Key)));
        expect (! isSlotMenuKey (juce::KeyPress (juce::KeyPress::F10Key, juce::ModifierKeys::shiftModifier | juce::ModifierKeys::ctrlModifier, 0)));
        Hotkeys::keyStateForTests = [] (int vk) { return vk == 0x5D; };
        expect (isSlotMenuKeyDown(), "VK_APPS down");
        Hotkeys::keyStateForTests = [] (int) { return false; };
        expect (! isSlotMenuKeyDown());
        Hotkeys::keyStateForTests = nullptr;
    }
};

static UiTweakTests uiTweakTests;
} // namespace koe

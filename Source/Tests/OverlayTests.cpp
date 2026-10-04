// 画面の端に今の声を表示 (INTERFACES.md §11.3 / §11.4, owner wave9/overlay). Category "Overlay".
// No window: the decision (OverlayTrigger), the place (overlayBounds) and the content (VoiceOverlayView) are tested alone;
// makeVoiceOverlay is never called. AppController (false), MainComponent offscreen.

#include "App/AppController.h"
#include "Core/Paths.h"
#include "Tests/TestUtil.h"
#include "UI/MainComponent.h"
#include "UI/Overlay.h"
#include "UI/Screens.h"
#include "UI/Theme.h"
#include "UI/main/Common.h"
#include "UI/screens/Common.h"

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
        s.layoutStyle = 0;
    });
    c->setVoiceChangerOn (true);
    c->loadPreset ("character-demon-king");
    c->dispatchPendingMessages();
    return c;
}
} // namespace

class OverlayTests : public juce::UnitTest
{
public:
    OverlayTests() : juce::UnitTest ("On-screen voice display (wave9/overlay)", "Overlay") {}

    void runTest() override
    {
        trigger();
        withController();
        placement();
        content();
        settingsCard();
    }

private:
    void trigger()
    {
        using ui::OverlayState;
        beginTest ("trigger: only ON, only on a change of name / ON-OFF / mute, never the first look, while starting up or with KoeLoom in front");
        const OverlayState demon { "A", true, false }, other { "B", true, false };
        {
            ui::OverlayTrigger t;
            expect (! t.update (demon, true, false, false), "the first look is the baseline (launch)");
            expect (! t.update (demon, true, false, false), "no change");
            expect (t.update (other, true, false, false), "name changed");
            expect (t.update ({ "B", false, false }, true, false, false), "ボイチェン OFF");
            expect (t.update ({ "B", false, true }, true, false, false), "muted");
            expect (t.update ({ "B", true, false }, true, false, false), "unmuted and ON again");
            expect (! t.update ({ "B", true, false }, true, false, false), "same again");
        }
        {
            ui::OverlayTrigger t;
            t.update (demon, true, false, false);
            expect (! t.update (other, false, false, false), "overlayOn OFF");
            expect (! t.update (other, true, false, false), "switching it ON later does not show the old change");
            expect (! t.update (demon, true, false, true), "KoeLoom in front");
            expect (! t.update (demon, true, false, false), "... and not later either");
            expect (! t.update (other, true, true, false), "starting up");
            expect (! t.update (other, true, false, false), "... and not after it");
            expect (t.update (demon, true, false, false), "a real change afterwards");
        }
    }

    void withController()
    {
        beginTest ("overlayStateOf follows the controller (preset load, ボイチェン, mute); status text");
        freshDataDir();
        auto c = makeController();
        ui::OverlayTrigger t;
        auto look = [&] { return t.update (ui::overlayStateOf (*c), true, false, false); };
        expect (! look(), "baseline");
        expect (ui::overlayStateOf (*c).name == juce::String::fromUTF8 ("魔王"));
        c->loadPreset ("natural-asis");
        expect (look(), "preset loaded");
        expect (ui::overlayStateOf (*c).name == c->getCurrentPreset().name);
        c->setVoiceChangerOn (false);
        expect (! ui::overlayStateOf (*c).voiceOn && look(), "OFF");
        expect (ui::overlayStatusText (ui::overlayStateOf (*c)) == juce::String::fromUTF8 ("ボイチェン OFF"));
        c->setMicMuted (true);
        expect (ui::overlayStateOf (*c).muted && look(), "muted");
        expect (ui::overlayStatusText (ui::overlayStateOf (*c)) == juce::String::fromUTF8 ("ミュート中"), "mute wins");
        c->setMicMuted (false);
        c->setVoiceChangerOn (true);
        expect (look());
        expect (ui::overlayStatusText (ui::overlayStateOf (*c)) == juce::String::fromUTF8 ("ボイチェン ON"));
        c->setOutputGainDb (3.0f);
        expect (! look(), "other changes do not show it");
        c->setOutputGainDb (0.0f);
        c->shutdown();
    }

    void placement()
    {
        beginTest ("overlayBounds: the four corners of the work area, 24 px in; unknown corner = top right");
        const juce::Rectangle<int> area (100, 50, 1920, 1040);
        const int m = ui::Theme::space4, w = 200, h = 70;
        expect (ui::overlayBounds (area, w, h, 0) == juce::Rectangle<int> (area.getRight() - m - w, area.getY() + m, w, h), "top right");
        expect (ui::overlayBounds (area, w, h, 1) == juce::Rectangle<int> (area.getRight() - m - w, area.getBottom() - m - h, w, h), "bottom right");
        expect (ui::overlayBounds (area, w, h, 2) == juce::Rectangle<int> (area.getX() + m, area.getBottom() - m - h, w, h), "bottom left");
        expect (ui::overlayBounds (area, w, h, 3) == juce::Rectangle<int> (area.getX() + m, area.getY() + m, w, h), "top left");
        expect (ui::overlayBounds (area, w, h, 7) == ui::overlayBounds (area, w, h, 0));
        for (int corner = 0; corner < 4; ++corner) expect (area.contains (ui::overlayBounds (area, w, h, corner)), "inside");
    }

    void content()
    {
        using ui::Theme;
        beginTest ("content: width grows with the name up to the limit (long names elided), see-through corners, drawn in both themes");
        const bool wasDark = Theme::isDark();
        for (const bool dark : { true, false })
        {
            Theme::setDark (dark);
            const juce::String label = dark ? "dark" : "light";
            ui::VoiceOverlayView v;
            v.setState ({ juce::String::fromUTF8 ("魔王"), true, false });
            const int shortW = v.preferredWidth();
            expect (shortW >= ui::VoiceOverlayView::minWidth && shortW < ui::VoiceOverlayView::maxWidth, label + ": short name");
            v.setState ({ juce::String::repeatedString (juce::String::fromUTF8 ("とても長い名前"), 8), true, true });
            expectEquals (v.preferredWidth(), ui::VoiceOverlayView::maxWidth, label + ": long name stops at the limit");
            expect (! v.isOpaque(), label + ": not opaque (per-pixel alpha)");
            v.setSize (v.preferredWidth(), ui::VoiceOverlayView::preferredHeight());
            const auto img = v.createComponentSnapshot (v.getLocalBounds(), true, 1.0f);
            expect (img.getPixelAt (0, 0).getAlpha() == 0, label + ": rounded corner is see-through");
            const auto mid = img.getPixelAt (img.getWidth() - 4, img.getHeight() / 2);
            expect (mid.getAlpha() > 200 && mid.getAlpha() < 255, label + ": half see-through surface");
            int textPixels = 0;
            const auto surface = Theme::colours().surface;
            for (int y = 0; y < img.getHeight(); ++y)
                for (int x = img.getWidth() / 4; x < img.getWidth() / 2; ++x)
                    if (std::abs (img.getPixelAt (x, y).getPerceivedBrightness() - surface.getPerceivedBrightness()) > 0.3f) ++textPixels;
            expect (textPixels > 50, label + ": the name is drawn (" + juce::String (textPixels) + ")");
        }
        Theme::setDark (wasDark);
    }

    void settingsCard()
    {
        using namespace ui;
        beginTest ("S-03 外観: 画面の端に今の声を表示 card: ON/OFF, corner, seconds (enabled only while ON), 試しに表示, search");
        const bool wasOff = mainui::animationsOff();
        mainui::animationsOff() = true;
        freshDataDir();
        auto c = makeController();
        {
            MainComponent mc (*c);
            mc.setSize (Theme::defaultWidth, Theme::defaultHeight);
            mc.showSettings (Navigator::SettingsSection::appearance);
            auto* on = dynamic_cast<juce::Button*> (mainui::findById (&mc, "settings.overlayOn"));
            auto* corner = dynamic_cast<screens::Segmented*> (mainui::findById (&mc, "settings.overlayCorner"));
            auto* secs = dynamic_cast<juce::Slider*> (mainui::findById (&mc, "settings.overlaySeconds"));
            auto* preview = mainui::findById (&mc, "settings.overlayPreview");
            expect (on != nullptr && corner != nullptr && secs != nullptr && preview != nullptr, "rows"); // 試しに表示 is not pressed: it makes a window
            if (on != nullptr && corner != nullptr && secs != nullptr)
            {
                expect (! on->getToggleState() && ! corner->isEnabled() && ! secs->isEnabled(), "OFF by default, the rest waits");
                on->setToggleState (true, juce::dontSendNotification);
                on->onClick();
                c->dispatchPendingMessages();
                expect (c->getSettings().overlayOn && corner->isEnabled() && secs->isEnabled(), "ON");
                corner->onChange (2);
                c->dispatchPendingMessages();
                expectEquals (c->getSettings().overlayCorner, 2);
                expectEquals (corner->getSelected(), 2);
                secs->setValue (3.5, juce::sendNotificationSync);
                c->dispatchPendingMessages();
                expectWithinAbsoluteError (c->getSettings().overlaySeconds, 3.5f, 1.0e-4f);
            }
            if (auto* v = dynamic_cast<SettingsView*> (mainui::findById (&mc, "page.settings")))
                for (const char* q : { "画面の端", "オーバーレイ", "今の声" })
                {
                    v->setSearchText (juce::String::fromUTF8 (q));
                    auto* card = mainui::findById (&mc, "settings.overlay");
                    expect (card != nullptr && mainui::visibleWithin (card, &mc), juce::String::fromUTF8 (q) + ": card found by the search");
                }
            else
                expect (false, "settings view");
        }
        c->shutdown();
        mainui::animationsOff() = wasOff;
    }
};

static OverlayTests overlayTests;
} // namespace koe

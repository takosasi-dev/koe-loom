// ui-screens checks (category "UiScreens"): layout at 1120x720 and 800x560, settings reaching the
// AppController, hotkey capture, preset list counts / search / CRUD, setup steps, soundboard options,
// help pages. "UiScreens snapshots" (category "Diag", not in the normal run) writes PNGs of every
// screen in both themes to %KOELOOM_SNAPSHOT_DIR% (or %TEMP%\KoeLoomUiSnapshots) for a visual check.
// Nothing here opens a window, a device, a file chooser or the browser.

#include "App/AppController.h"
#include "Core/Constants.h"
#include "Core/Paths.h"
#include "Platform/Hotkeys.h"
#include "Tests/TestUtil.h"
#include "UI/MainComponent.h"
#include "UI/Screens.h"
#include "UI/ThemeLibrary.h"
#include "UI/Widgets.h"
#include "UI/main/VoicePage.h"
#include "UI/screens/Common.h"
#include "UI/screens/ThemeEditor.h"

#include <juce_gui_extra/juce_gui_extra.h>

#include <typeinfo>

namespace koe
{
namespace
{
using namespace ui;
using namespace ui::screens;

void freshUiDataDir()
{
    auto d = paths::dataDir();
    if (d.getFullPathName().contains ("KoeLoomTests") || d.getFullPathName().contains ("KoeLoomUiSnapshots")) d.deleteRecursively();
    d.createDirectory();
}

/** Records what the screens ask for. Closed overlays are kept alive until the test ends (the real
    MainComponent deletes them; here they may still be inside their own callback). */
struct FakeNavigator final : ui::Navigator
{
    juce::StringArray calls, toasts;
    std::unique_ptr<juce::Component> overlay;
    std::vector<std::unique_ptr<juce::Component>> closed;

    void showPage (Page p) override { calls.add ("page:" + juce::String (int (p))); }
    void showSettings (SettingsSection s) override { calls.add ("settings:" + juce::String (int (s))); }
    void showPresetBrowser() override { calls.add ("presets"); }
    void showEffectPicker (int i) override { calls.add ("picker:" + juce::String (i)); }
    void showSlotDetail (int s) override { calls.add ("slot:" + juce::String (s)); }
    void showSetupWizard() override { calls.add ("setup"); }
    void showHelp (HelpTopic t) override { calls.add ("help:" + juce::String (int (t))); }
    void startTour (bool r) override { calls.add (r ? "tour:resume" : "tour"); }
    void showOverlay (std::unique_ptr<juce::Component> p) override
    {
        calls.add ("overlay:" + p->getComponentID());
        if (overlay != nullptr) closed.push_back (std::move (overlay));
        overlay = std::move (p);
    }
    void closeOverlay() override
    {
        calls.add ("closeOverlay");
        if (overlay != nullptr) closed.push_back (std::move (overlay));
    }
    void showToast (const juce::String& t) override { toasts.add (t); }
};

juce::String describe (const juce::Component& c)
{
    if (c.getComponentID().isNotEmpty()) return c.getComponentID();
    if (auto* l = dynamic_cast<const TextLabel*> (&c)) return "TextLabel \"" + l->getText().substring (0, 16) + "\"";
    if (auto* b = dynamic_cast<const juce::Button*> (&c)) return "Button \"" + b->getButtonText() + "\"";
    return typeid (c).name();
}

/** Every visible child lies inside its parent, and visible siblings do not overlap. The insides of
    stock JUCE widgets are skipped; a Viewport's viewed component may be taller than the view. */
int checkLayout (juce::UnitTest& t, juce::Component& root, const juce::String& where)
{
    int problems = 0;
    std::function<void (juce::Component&)> walk = [&] (juce::Component& parent)
    {
        if (dynamic_cast<juce::TextEditor*> (&parent) != nullptr || dynamic_cast<juce::ComboBox*> (&parent) != nullptr
            || dynamic_cast<juce::Slider*> (&parent) != nullptr || dynamic_cast<juce::ScrollBar*> (&parent) != nullptr
            || dynamic_cast<juce::ColourSelector*> (&parent) != nullptr)
            return;
        juce::Array<juce::Component*> vis;
        for (auto* ch : parent.getChildren())
            if (ch->isVisible() && ! ch->getBounds().isEmpty() && dynamic_cast<InlinePrompt*> (ch) == nullptr) vis.add (ch);
        const bool viewed = dynamic_cast<juce::Viewport*> (parent.getParentComponent()) != nullptr;
        for (auto* ch : vis)
        {
            if (! viewed && ! parent.getLocalBounds().contains (ch->getBounds()))
            {
                ++problems;
                t.expect (false, where + ": " + describe (*ch) + " " + ch->getBounds().toString() + " sticks out of " + describe (parent) + " "
                                     + parent.getLocalBounds().toString());
            }
            walk (*ch);
        }
        for (int i = 0; i < vis.size(); ++i)
            for (int j = i + 1; j < vis.size(); ++j)
                if (vis[i]->getBounds().intersects (vis[j]->getBounds()))
                {
                    ++problems;
                    t.expect (false, where + ": " + describe (*vis[i]) + " " + vis[i]->getBounds().toString() + " overlaps " + describe (*vis[j]) + " "
                                         + vis[j]->getBounds().toString());
                }
    };
    walk (root);
    return problems;
}

/** Component::keyPressed is public; Button hides it as protected. */
bool press (juce::Component* c, const juce::KeyPress& k) { return c->keyPressed (k); }

template <typename T>
T* find (juce::Component& root, const juce::String& id)
{
    return dynamic_cast<T*> (findById (root, id));
}

void collect (juce::Component& root, const std::function<void (juce::Component&)>& f)
{
    f (root);
    for (auto* c : root.getChildren()) collect (*c, f);
}

bool containsText (juce::Component& root, const juce::String& needle)
{
    bool found = false;
    collect (root, [&] (juce::Component& c)
    {
        if (auto* l = dynamic_cast<TextLabel*> (&c); l != nullptr && l->isVisible() && l->getText().contains (needle)) found = true;
    });
    return found;
}

juce::StringArray visibleRows (juce::Component& browser)
{
    juce::StringArray ids;
    collect (browser, [&] (juce::Component& c)
    {
        if (c.getComponentID().startsWith ("presets.row.") && c.isVisible()) ids.add (c.getComponentID().fromFirstOccurrenceOf ("presets.row.", false, false));
    });
    return ids;
}

// window-sized areas used by MainComponent (mock: 16 px padding, 52 px header, 14 px gap, 36 px bottom bar)
struct WinSize { int w, h; const char* name; };
constexpr WinSize kWindows[] = { { Theme::defaultWidth, Theme::defaultHeight, "1120x720" }, { Theme::minWidth, Theme::minHeight, "800x560" } };
juce::Rectangle<int> pageArea (WinSize win, bool withBottomBar)
{
    auto r = juce::Rectangle<int> (win.w, win.h).reduced (Theme::space3);
    r.removeFromTop (Theme::headerH + Theme::space3);
    if (withBottomBar) r.removeFromBottom (Theme::bannerH + Theme::space3);
    return r;
}
juce::Rectangle<int> overlayArea (WinSize win, juce::Component& c)
{
    return juce::Rectangle<int> (win.w, win.h).withSizeKeepingCentre (juce::jmin (c.getWidth(), win.w - Theme::space5 * 2),
                                                                      juce::jmin (c.getHeight(), win.h - Theme::space5 * 2));
}

const Navigator::SettingsSection kSections[] = { Navigator::SettingsSection::devices,    Navigator::SettingsSection::environment,
                                                 Navigator::SettingsSection::hotkeys,    Navigator::SettingsSection::startup,
                                                 Navigator::SettingsSection::appearance, Navigator::SettingsSection::advanced,
                                                 Navigator::SettingsSection::diagnostics };
const char* const kSectionNames[] = { "devices", "environment", "hotkeys", "startup", "appearance", "advanced", "diagnostics" };

/** Sample soundboard content (files do not exist; only the names are shown). */
void fillSoundboard (AppController& c)
{
    const char* names[] = { "clap.wav", "drumroll.wav", "buzzer.wav", "chime.flac", "laugh.ogg", "sigh.mp3", "wow.wav", "sfx8.wav" };
    for (int i = 0; i < 8; ++i)
    {
        SoundboardSlotDef d;
        d.file = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("KoeLoomUiSounds").getChildFile (names[i]).getFullPathName();
        d.loop = i == 3;
        c.getSoundboard().setSlotDef (i, d);
    }
    juce::String why;
    c.setHotkey ("sound.1", 2, '1', why);
    c.setHotkey ("sound.2", 2, '2', why);
}
} // namespace

// =============================================================================================== tests
class UiScreensTests : public juce::UnitTest
{
public:
    UiScreensTests() : juce::UnitTest ("UiScreens", "UiScreens") {}

    void runTest() override
    {
        beginTest ("S-03 layout: every section fits 1120x720 and 800x560 without overlaps");
        {
            freshUiDataDir();
            AppController c (false);
            c.startup();
            FakeNavigator nav;
            SettingsView v (c, nav);
            for (auto win : kWindows)
                for (int s = 0; s < 7; ++s)
                {
                    v.setBounds (pageArea (win, false).withPosition (0, 0));
                    v.showSection (kSections[s]);
                    checkLayout (*this, v, juce::String ("S-03 ") + kSectionNames[s] + " " + win.name);
                }
        }

        beginTest ("Segmented buttons get at least their preferred width");
        {
            Segmented seg ({ ja ("ダーク"), ja ("ライト") });
            seg.setSelected (0);
            seg.setBounds (0, 0, seg.preferredWidth() + Theme::space4, Theme::buttonH);
            for (int i = 0; i < 2; ++i)
            {
                auto* b = dynamic_cast<PillButton*> (seg.getButton (i));
                expectGreaterOrEqual (b->getWidth(), b->preferredWidth() - Theme::space1);
            }
        }

        beginTest ("S-03: output gain reaches the AppController, warning above +6 dB (F-12-4, AC-38), tour ids");
        {
            freshUiDataDir();
            AppController c (false);
            c.startup();
            FakeNavigator nav;
            SettingsView v (c, nav);
            v.setSize (Theme::defaultWidth - Theme::space5, Theme::defaultHeight - Theme::space5 * 3);
            v.showSection (Navigator::SettingsSection::environment);
            expect (findById (v, "settings.outputGain") != nullptr, "settings.outputGain (tour step 5)");
            auto* slider = find<juce::Slider> (v, "settings.outputGain.slider");
            auto* warn = find<TextLabel> (v, "settings.outputGainWarn");
            expect (slider != nullptr && warn != nullptr);
            if (slider != nullptr && warn != nullptr)
            {
                slider->setValue (6.5, juce::sendNotificationSync);
                expectEquals (c.getSettings().outputGainDb, 6.5f);
                expect (warn->isVisible(), "warning shown at +6.5 dB");
                expectEquals (warn->getText(), ja ("リミッターが働きやすくなり、音が歪むことがあります"));
                slider->setValue (6.0, juce::sendNotificationSync);
                expectEquals (c.getSettings().outputGainDb, 6.0f);
                expect (! warn->isVisible(), "no warning at +6 dB");
                slider->setValue (-3.5, juce::sendNotificationSync);
                expectEquals (c.getSettings().outputGainDb, -3.5f);
            }
            v.showSection (Navigator::SettingsSection::hotkeys);
            expect (findById (v, "settings.hotkeys") != nullptr, "settings.hotkeys (tour step 6)");
            v.showSection (Navigator::SettingsSection::appearance);
            if (auto* theme = find<Segmented> (v, "settings.theme"))
            {
                theme->getButton (1)->onClick();
                expect (! c.getSettings().darkTheme, "light theme saved");
                theme->getButton (0)->onClick();
                expect (c.getSettings().darkTheme);
            }
            else expect (false, "settings.theme");
            if (auto* sw = find<juce::Button> (v, "settings.accent.3"))
            {
                expectEquals (sw->getTitle(), ja ("グリーン"), "swatch named, not colour alone");
                expect (sw->getWantsKeyboardFocus());
                sw->onClick();
                expectEquals (c.getSettings().accentColour, 3);
                c.dispatchPendingMessages();
                expect (sw->getToggleState(), "the selected swatch is marked");
            }
            else expect (false, "settings.accent.3");
            if (auto* tone = find<Segmented> (v, "settings.tone"))
            {
                tone->getButton (2)->onClick();
                expectEquals (c.getSettings().backgroundTone, 2);
            }
            else expect (false, "settings.tone");
            c.updateSettings ([] (Settings& s) { s.accentColour = 0; s.backgroundTone = 0; });
            v.showSection (Navigator::SettingsSection::devices);
            expect (containsText (v, ja ("仮想ケーブル（VB-CABLE）が見つかりません")), "E-02 text without a cable");
        }

        beginTest ("S-03 hotkeys: capture Ctrl+K, Esc cancels, letters need Ctrl/Alt, duplicates refused (F-07-4)");
        {
            int mods = 0, vk = 0;
            expect (keyPressToHotkey (juce::KeyPress ('a', juce::ModifierKeys::ctrlModifier, 0), mods, vk));
            expectEquals (mods, 2);
            expectEquals (vk, int ('A'));
            expect (keyPressToHotkey (juce::KeyPress (juce::KeyPress::F5Key, juce::ModifierKeys::altModifier | juce::ModifierKeys::shiftModifier, 0), mods, vk));
            expectEquals (mods, 1 | 4);
            expectEquals (vk, 0x74);
            expect (keyPressToHotkey (juce::KeyPress ('7', juce::ModifierKeys::ctrlModifier | juce::ModifierKeys::altModifier, 0), mods, vk));
            expectEquals (mods, 3);
            expectEquals (vk, int ('7'));
            expect (hotkeyNeedsModifier ('A') && hotkeyNeedsModifier (0x20) && ! hotkeyNeedsModifier (0x74));

            freshUiDataDir();
            AppController c (false);
            c.startup();
            FakeNavigator nav;
            SettingsView v (c, nav);
            v.setSize (Theme::defaultWidth, Theme::defaultHeight);
            v.showSection (Navigator::SettingsSection::hotkeys);
            const auto actions = Hotkeys::allActions();
            auto* assign = find<juce::Button> (v, "hotkey.assign." + actions[0]);
            auto* assign2 = find<juce::Button> (v, "hotkey.assign." + actions[1]);
            auto* error2 = find<TextLabel> (v, "hotkey.error." + actions[1]);
            expect (assign != nullptr && assign2 != nullptr && error2 != nullptr);
            if (assign != nullptr && assign2 != nullptr && error2 != nullptr)
            {
                assign->onClick();
                expectEquals (assign->getButtonText(), ja ("キーを押してください"));
                press (assign, juce::KeyPress ('k', juce::ModifierKeys::ctrlModifier, 0));
                expectEquals (c.getHotkeyText (actions[0]), Hotkeys::describe (2, 'K'));
                expectEquals (assign->getButtonText(), ja ("割り当て"));

                assign->onClick();
                press (assign, juce::KeyPress (juce::KeyPress::escapeKey));
                expectEquals (assign->getButtonText(), ja ("割り当て"), "Esc ends the capture");
                expectEquals (c.getHotkeyText (actions[0]), Hotkeys::describe (2, 'K'), "Esc keeps the old key");

                assign2->onClick();
                press (assign2, juce::KeyPress ('j'));
                expect (error2->isVisible(), "a plain letter is refused");
                expect (c.getHotkeyText (actions[1]).isEmpty());

                assign2->onClick();
                press (assign2, juce::KeyPress ('k', juce::ModifierKeys::ctrlModifier, 0));
                expect (error2->isVisible() && error2->getText().contains (ja ("割り当て済み")), "the same key twice is refused");
                expect (c.getHotkeyText (actions[1]).isEmpty());

                assign->onClick();
                press (assign, juce::KeyPress (juce::KeyPress::deleteKey));
                expect (c.getHotkeyText (actions[0]).isEmpty(), "Delete clears");
            }
        }

        detailedSettings();

        beginTest ("S-02 layout, ducking, one-time hint (F-13-7) and the slot options (F-06-4/5)");
        {
            freshUiDataDir();
            AppController c (false);
            c.startup();
            fillSoundboard (c);
            FakeNavigator nav;
            {
                SoundboardView v (c, nav);
                for (auto win : kWindows)
                {
                    v.setBounds (pageArea (win, true).withPosition (0, 0));
                    v.setVisible (true);
                    checkLayout (*this, v, juce::String ("S-02 ") + win.name + (findById (v, "soundboard.hint")->isVisible() ? " with hint" : ""));
                }
                expect (findById (v, "soundboard.hint")->isVisible(), "hint on the first open");
                if (auto* count = find<TextLabel> (v, "soundboard.count")) expectEquals (count->getText(), ja ("8 / 12 割り当て済み"));
                if (auto* duck = find<juce::Slider> (v, "soundboard.ducking"))
                {
                    duck->setValue (-12.0, juce::sendNotificationSync);
                    expectEquals (c.getSettings().duckingDb, -12.0f);
                }
                else expect (false, "soundboard.ducking");

                auto* card = find<juce::Button> (v, "soundboard.slot.1");
                expect (card != nullptr);
                if (card != nullptr) card->onClick(); // assigned slot -> its settings overlay
                expect (nav.overlay != nullptr && nav.overlay->getComponentID() == "soundboard.settings", "slot settings overlay");
                if (nav.overlay != nullptr)
                {
                    auto& panel = *nav.overlay;
                    for (auto win : kWindows)
                    {
                        panel.setBounds (overlayArea (win, panel));
                        checkLayout (*this, panel, juce::String ("slot settings ") + win.name);
                    }
                    if (auto* vol = find<juce::Slider> (panel, "soundboard.settings.volume"))
                    {
                        vol->setValue (6.0, juce::sendNotificationSync);
                        expectEquals (c.getSoundboard().getSlotDef (0).volumeDb, 6.0f);
                        vol->setValue (-24.0, juce::sendNotificationSync);
                        expectEquals (c.getSoundboard().getSlotDef (0).volumeDb, -24.0f);
                    }
                    else expect (false, "volume");
                    if (auto* mode = find<Segmented> (panel, "soundboard.settings.mode")) mode->getButton (1)->onClick();
                    expect (c.getSoundboard().getSlotDef (0).loop, "loop");
                    if (auto* rt = find<Segmented> (panel, "soundboard.settings.retrigger")) rt->getButton (2)->onClick();
                    expectEquals (c.getSoundboard().getSlotDef (0).retrigger, 2);
                    if (auto* mon = find<juce::Button> (panel, "soundboard.settings.monitor"))
                    {
                        mon->setToggleState (false, juce::dontSendNotification);
                        mon->onClick();
                    }
                    expect (! c.getSoundboard().getSlotDef (0).toMonitor, "monitor off");
                    expect (c.getSoundboard().getSlotDef (0).file.endsWith ("clap.wav"), "file kept");
                    if (auto* hk = find<juce::Button> (panel, "soundboard.settings.hotkey")) hk->onClick();
                    expect (nav.calls.contains ("settings:" + juce::String (int (Navigator::SettingsSection::hotkeys))), "hotkey -> settings");
                }
            }
            c.updateSettings ([] (Settings& s) { s.soundboardHintShown = true; });
            SoundboardView again (c, nav);
            again.setSize (Theme::defaultWidth, Theme::defaultHeight / 2);
            again.setVisible (true);
            expect (! findById (again, "soundboard.hint")->isVisible(), "hint only once");
        }

        beginTest ("S-02: a playing slot's round button shows stop (「停止」) and pressing it stops that slot (owner 2026-10-03)");
        {
            freshUiDataDir();
            AppController c (false);
            c.startup();
            auto& sb = c.getSoundboard();
            const auto wav = paths::dataDir().getChildFile ("loop.wav");
            expect (test::writeWav (wav, std::vector<float> (48000, 0.25f)));
            for (int s : { 0, 1 })
            {
                sb.assignFile (s, wav);
                auto d = sb.getSlotDef (s);
                d.loop = true;
                sb.setSlotDef (s, d);
            }
            const auto until = juce::Time::getMillisecondCounter() + 5000;
            while ((sb.getSlotState (0).status == SoundSlotState::Status::loading || sb.getSlotState (1).status == SoundSlotState::Status::loading)
                   && juce::Time::getMillisecondCounter() < until)
                juce::Thread::sleep (5);
            std::vector<float> out (480), mon (480);
            auto renderAndNotify = [&]
            {
                for (int b = 0; b < 4; ++b) sb.render (out.data(), mon.data(), 480);
                c.sendChangeMessage(); // what the soundboard's state timer does
                c.dispatchPendingMessages();
            };
            FakeNavigator nav;
            SoundboardView v (c, nav);
            v.setSize (Theme::defaultWidth, Theme::defaultHeight);
            auto* play = find<juce::Button> (v, "soundboard.play.1");
            expect (play != nullptr);
            if (play != nullptr)
            {
                expect (play->isEnabled());
                expectEquals (play->getButtonText(), ja ("再生"));
                expectEquals (play->getTitle(), ja ("スロット 01 を再生"));
                expectEquals (play->getTooltip(), play->getTitle());

                play->onClick(); // not playing: triggers
                sb.trigger (1);  // another slot keeps playing through the stop
                renderAndNotify();
                expect (sb.getSlotState (0).playing && sb.getSlotState (1).playing);
                expectEquals (play->getButtonText(), ja ("停止"));
                expectEquals (play->getTitle(), ja ("スロット 01 を停止"));
                expectEquals (play->getTooltip(), play->getTitle());
                expectEquals (find<juce::Button> (v, "soundboard.play.2")->getButtonText(), ja ("停止"));

                play->onClick(); // playing: stops that slot only
                renderAndNotify();
                expect (! sb.getSlotState (0).playing, "slot 1 stopped");
                expect (sb.getSlotState (1).playing, "slot 2 keeps playing");
                expectEquals (play->getButtonText(), ja ("再生"));
                expectEquals (play->getTitle(), ja ("スロット 01 を再生"));
            }
            sb.stopAll();
            sb.render (out.data(), mon.data(), 480);
        }

        beginTest ("hotkey soundStop.N stops that slot only; sound.N still triggers (owner 2026-10-03)");
        {
            freshUiDataDir();
            AppController c (false);
            c.startup();
            auto& sb = c.getSoundboard();
            const auto wav = paths::dataDir().getChildFile ("loop.wav");
            expect (test::writeWav (wav, std::vector<float> (48000, 0.25f)));
            for (int s : { 2, 3 })
            {
                sb.assignFile (s, wav);
                auto d = sb.getSlotDef (s);
                d.loop = true;
                sb.setSlotDef (s, d);
            }
            const auto until = juce::Time::getMillisecondCounter() + 5000;
            while ((sb.getSlotState (2).status == SoundSlotState::Status::loading || sb.getSlotState (3).status == SoundSlotState::Status::loading)
                   && juce::Time::getMillisecondCounter() < until)
                juce::Thread::sleep (5);
            std::vector<float> out (480), mon (480);
            auto render = [&] { for (int b = 0; b < 4; ++b) sb.render (out.data(), mon.data(), 480); };
            c.performAction ("sound.3");
            c.performAction ("sound.4");
            render();
            expect (sb.getSlotState (2).playing && sb.getSlotState (3).playing);
            c.performAction ("soundStop.3");
            render();
            expect (! sb.getSlotState (2).playing, "soundStop.3 stops slot 3");
            expect (sb.getSlotState (3).playing, "slot 4 keeps playing");
            c.performAction ("soundStop.4");
            render();
            expect (! sb.getSlotState (3).playing);
        }

        beginTest ("S-06: category counts (AC-44), search, favourites, built-ins only duplicate (F-05-2, AC-66)");
        {
            freshUiDataDir();
            AppController c (false);
            c.startup();
            FakeNavigator nav;
            PresetBrowser b (c, nav);
            for (auto win : kWindows)
            {
                b.setBounds (overlayArea (win, b));
                checkLayout (*this, b, juce::String ("S-06 ") + win.name);
            }
            auto count = [&] (const char* id)
            {
                auto* nb = find<NavButton> (b, juce::String ("presets.cat.") + id);
                return nb != nullptr ? nb->getCount() : -1;
            };
            expectEquals (count ("all"), 81);
            expectEquals (count ("natural"), 11);
            expectEquals (count ("character"), 22);
            expectEquals (count ("device"), 23);
            expectEquals (count ("space"), 17);
            expectEquals (count ("layered"), 8);
            expectEquals (count ("user"), 0);
            expectEquals (count ("favorites"), 0);

            if (auto* all = find<juce::Button> (b, "presets.cat.all")) all->onClick();
            expectEquals (visibleRows (b).size(), 81);
            if (auto* chara = find<juce::Button> (b, "presets.cat.character")) chara->onClick();
            expectEquals (visibleRows (b).size(), 22);
            if (auto* all = find<juce::Button> (b, "presets.cat.all")) all->onClick();
            auto* search = find<juce::TextEditor> (b, "presets.search");
            expect (search != nullptr);
            if (search != nullptr)
            {
                search->setText (ja ("ロボ"), false);
                search->onTextChange();
                const auto rows = visibleRows (b);
                expect (rows.size() >= 1, "search finds ロボ");
                for (auto& id : rows)
                    if (auto* p = c.getPresetLibrary().find (id.toStdString())) expect (p->name.contains (ja ("ロボ")), p->name);
                if (auto* lc = find<TextLabel> (b, "presets.listCount")) expect (lc->getText().contains (juce::String (rows.size()) + ja (" 件")));
                search->setText (ja ("該当なしの名前"), false);
                search->onTextChange();
                expectEquals (visibleRows (b).size(), 0);
                search->setText ({}, false);
                search->onTextChange();
            }

            // favourites (F-05-8): the star toggles, the count follows
            if (auto* star = find<juce::Button> (b, "presets.star.character-demon-king")) star->onClick();
            expect (c.isFavorite ("character-demon-king"));
            expectEquals (count ("favorites"), 1);

            // built-in: only duplicate (F-05-2)
            if (auto* row = findById (b, "presets.row.natural-asis")) row->keyPressed (juce::KeyPress (juce::KeyPress::spaceKey));
            auto enabled = [&] (const char* id) { auto* btn = find<juce::Button> (b, id); return btn != nullptr && btn->isEnabled(); };
            expect (enabled ("presets.duplicate") && enabled ("presets.use") && enabled ("presets.export"));
            expect (! enabled ("presets.rename") && ! enabled ("presets.delete") && ! enabled ("presets.overwrite"), "built-in is read-only");

            if (auto* dup = find<juce::Button> (b, "presets.duplicate")) dup->onClick();
            expectEquals (count ("user"), 1);
            std::string userId;
            for (auto& p : c.getPresetLibrary().all())
                if (! p.builtin) userId = p.id;
            expect (! userId.empty());
            expect (enabled ("presets.rename") && enabled ("presets.delete"), "user preset can be renamed / deleted");
            expect (! enabled ("presets.overwrite"), "overwrite only for the preset in use");

            if (auto* rn = find<juce::Button> (b, "presets.rename")) rn->onClick();
            if (auto* prompt = find<InlinePrompt> (b, "presets.prompt"))
            {
                prompt->getEditor()->setText (ja ("テストの声"), false);
                prompt->ok();
            }
            else expect (false, "rename prompt");
            if (auto* p = c.getPresetLibrary().find (userId)) expectEquals (p->name, ja ("テストの声"));

            // use: loads and closes the overlay
            if (auto* use = find<juce::Button> (b, "presets.use")) use->onClick();
            expectEquals (juce::String (c.getCurrentPreset().id), juce::String (userId));
            expect (nav.calls.contains ("closeOverlay"));
            expect (enabled ("presets.overwrite"), "overwrite for the user preset in use");

            if (auto* del = find<juce::Button> (b, "presets.delete")) del->onClick();
            if (auto* prompt = find<InlinePrompt> (b, "presets.prompt"))
            {
                expect (prompt->isVisible());
                prompt->ok();
            }
            else expect (false, "delete prompt");
            expect (c.getPresetLibrary().find (userId) == nullptr, "deleted");
            expectEquals (count ("user"), 0);

            // favourites limit (AC-21): the 10th is refused with a message
            int added = c.getFavorites().size();
            for (auto& p : c.getPresetLibrary().all())
            {
                if (added >= kMaxFavorites) break;
                if (! c.isFavorite (p.id))
                {
                    juce::String why;
                    c.toggleFavorite (p.id, why);
                    ++added;
                }
            }
            for (auto& p : c.getPresetLibrary().all())
                if (! c.isFavorite (p.id))
                {
                    if (auto* star = find<juce::Button> (b, "presets.star." + juce::String (p.id))) star->onClick();
                    break;
                }
            expectEquals (c.getFavorites().size(), kMaxFavorites);
            if (auto* msg = find<TextLabel> (b, "presets.message")) expect (msg->isVisible() && msg->getText().contains ("9"), "refusal shown");

            if (auto* close = find<juce::Button> (b, "presets.close")) close->onClick();
            expect (nav.calls.size() >= 2 && nav.calls[nav.calls.size() - 1] == "closeOverlay");
        }

        beginTest ("S-04: steps, back / next, setupDone, cable missing texts (F-10, AC-30), layout");
        {
            freshUiDataDir();
            AppController c (false);
            c.startup();
            FakeNavigator nav;
            int finished = 0;
            bool completed = false;
            SetupWizard w (c, nav, [&] (bool done) { ++finished; completed = done; });
            auto stepText = [&] { auto* l = find<TextLabel> (w, "setup.step"); return l != nullptr ? l->getText() : juce::String(); };
            auto next = find<PillButton> (w, "setup.next");
            auto back = find<juce::Button> (w, "setup.back");
            expect (next != nullptr && back != nullptr);
            if (next == nullptr || back == nullptr) return;
            for (int s = 0; s < 3; ++s)
            {
                for (auto win : kWindows)
                {
                    w.setSize (win.w, win.h);
                    checkLayout (*this, w, "S-04 step " + juce::String (s + 1) + " " + win.name);
                }
                if (s < 2) next->onClick();
            }
            back->onClick();
            back->onClick();
            expectEquals (stepText(), juce::String ("1 / 3"));
            expect (! back->isVisible());
            expect (findById (w, "setup.cableMissing")->isVisible() && findById (w, "setup.openCable")->isVisible(), "F-10-2 without a cable");
            expect (! findById (w, "setup.cableFound")->isVisible());
            expect (containsText (w, ja ("管理者権限")), "admin rights note");
            next->onClick();
            expectEquals (stepText(), juce::String ("2 / 3"));
            expect (back->isVisible());
            expect (findById (w, "setup.input") != nullptr && findById (w, "setup.output") != nullptr);
            next->onClick();
            expectEquals (stepText(), juce::String ("3 / 3"));
            expectEquals (next->getButtonText(), ja ("完了して始める"));
            expect (containsText (w, revertMicText()), "F-10-5 on S-04");
            expect (containsText (w, ja ("ノイズ抑制")), "Discord recommendation");
            expect (containsText (w, ja ("ハウリング")), "howling note");
            expect (! c.getSettings().setupDone);
            next->onClick();
            expectEquals (finished, 1);
            expect (completed, "完了して始める reports completed (the caller starts the tour, F-13-1)");
            expect (c.getSettings().setupDone, "setupDone after 完了して始める");

            c.updateSettings ([] (Settings& s) { s.setupDone = false; });
            SetupWizard later (c, nav, [&] (bool done) { ++finished; completed = done; });
            if (auto* l = find<juce::Button> (later, "setup.later")) l->onClick();
            expectEquals (finished, 2);
            expect (! completed, "あとで設定する reports not completed (no tour)");
            expect (c.getSettings().setupDone, "setupDone after あとで設定する");
        }

        beginTest ("Help pages: Discord steps, revert, licenses (F-10-4, F-10-5, spec 11), close");
        {
            freshUiDataDir();
            AppController c (false);
            FakeNavigator nav;
            const Navigator::HelpTopic topics[] = { Navigator::HelpTopic::discordSetup, Navigator::HelpTopic::revertMic, Navigator::HelpTopic::licenses };
            for (auto topic : topics)
            {
                auto panel = createHelpPanel (topic, c, nav);
                expect (panel != nullptr);
                for (auto win : kWindows)
                {
                    panel->setBounds (overlayArea (win, *panel));
                    checkLayout (*this, *panel, panel->getComponentID() + " " + win.name);
                }
                if (topic == Navigator::HelpTopic::discordSetup) expect (containsText (*panel, "CABLE Output") && containsText (*panel, ja ("エコー除去")));
                if (topic == Navigator::HelpTopic::revertMic) expect (containsText (*panel, revertMicText()));
                if (topic == Navigator::HelpTopic::licenses)
                    for (auto* s : { "AGPL-3.0", "JUCE", "Raw Material Software", "BSD-3-Clause", "Jean-Marc Valin", "MIT License", "Signalsmith Audio" })
                        expect (containsText (*panel, s), s);
                const int before = nav.calls.size();
                if (auto* close = find<juce::Button> (*panel, "overlay.close")) close->onClick();
                expect (nav.calls.size() == before + 1 && nav.calls[before] == "closeOverlay");
            }
        }
    }

private:
    /** Shown inside root: the component and every parent up to root are visible. */
    static bool shown (juce::Component* comp, juce::Component& root)
    {
        if (comp == nullptr) return false;
        for (auto* p = comp; p != nullptr; p = p->getParentComponent())
        {
            if (p == &root) return true;
            if (! p->isVisible()) return false;
        }
        return false;
    }

    /** The control with this id in any section (each section is built once, but only the shown one is parented). */
    static juce::Component* findInSections (SettingsView& v, const juce::String& id)
    {
        for (auto s : kSections)
        {
            v.showSection (s);
            if (auto* comp = findById (v, id)) return comp;
        }
        return nullptr;
    }

    /** Turns a settings control to another value the way a click / drag would. */
    void operate (juce::Component* comp, const juce::String& key)
    {
        if (auto* t = dynamic_cast<ToggleSwitch*> (comp))
        {
            t->setToggleState (! t->getToggleState(), juce::dontSendNotification);
            t->onClick();
        }
        else if (auto* seg = dynamic_cast<Segmented*> (comp))
        {
            const int n = seg->getNumChildComponents();
            seg->getButton ((seg->getSelected() + 1) % n)->onClick();
        }
        else if (auto* cb = dynamic_cast<juce::ComboBox*> (comp)) cb->setSelectedId (cb->getSelectedId() == 1 ? 2 : 1, juce::sendNotificationSync);
        else if (auto* sl = dynamic_cast<juce::Slider*> (comp))
            sl->setValue (sl->getValue() >= sl->getMaximum() ? sl->getMinimum() : sl->getMaximum(), juce::sendNotificationSync);
        else expect (false, key + ": unknown control");
    }

    void detailedSettings()
    {
        beginTest ("S-03 wave 4: every detailed setting has a row whose control changes the Settings value (INTERFACES.md 7.2)");
        {
            freshUiDataDir();
            AppController c (false);
            c.startup();
            FakeNavigator nav;
            SettingsView v (c, nav);
            v.setSize (Theme::defaultWidth - Theme::space5, Theme::defaultHeight - Theme::space5 * 3);
            struct Field { const char* key; std::function<double (const Settings&)> get; };
#define KOE_FIELD(k) Field { #k, [] (const Settings& s) { return double (s.k); } }
            const Field fields[] = {
                // audio (wave4/audio)
                KOE_FIELD (converterQuality), KOE_FIELD (pitchMinHz), KOE_FIELD (pitchMaxHz), KOE_FIELD (highPassOn), KOE_FIELD (highPassHz),
                KOE_FIELD (agcOn), KOE_FIELD (agcTargetDb), KOE_FIELD (agcMaxGainDb), KOE_FIELD (limiterCeilingDb), KOE_FIELD (limiterReleaseMs),
                KOE_FIELD (presetCrossfadeMs), KOE_FIELD (soundboardMaxVoices), KOE_FIELD (soundFadeMs), KOE_FIELD (duckAttackMs),
                KOE_FIELD (duckReleaseMs), KOE_FIELD (monitorIncludeSoundboard),
                // devices, hotkeys, app (wave4/platform)
                KOE_FIELD (wasapiExclusive), KOE_FIELD (inputChannel), KOE_FIELD (monitorLatency), KOE_FIELD (reconnectSeconds),
                KOE_FIELD (pushToTalk), KOE_FIELD (pttReleaseMs), KOE_FIELD (hotkeyToasts), KOE_FIELD (favoriteWrap), KOE_FIELD (startupVoice),
                KOE_FIELD (startupLastPreset), KOE_FIELD (closeAction), KOE_FIELD (trayNotifications), KOE_FIELD (logLevel), KOE_FIELD (logKeepDays),
                // screen (wave4/ui)
                KOE_FIELD (uiScalePercent), KOE_FIELD (alwaysOnTop), KOE_FIELD (animations), KOE_FIELD (meterFps), KOE_FIELD (meterPeakHoldMs),
                KOE_FIELD (tooltipDelayMs), KOE_FIELD (knobSensitivity), KOE_FIELD (knobWheel),
                // updates (wave4/update)
                KOE_FIELD (autoUpdate), KOE_FIELD (updateIncludePrerelease),
            };
#undef KOE_FIELD
            expectEquals (int (std::size (fields)), 38 + 2, "38 rows + settingsShowDetails (the disclosure) = 39; 2 update toggles");
            c.updateSettings ([] (Settings& s) { s.settingsShowDetails = true; });
            c.dispatchPendingMessages();
            for (auto& f : fields)
            {
                auto* comp = findInSections (v, juce::String ("settings.") + f.key);
                expect (comp != nullptr, juce::String ("row for ") + f.key);
                if (comp == nullptr) continue;
                expect (shown (comp, v), juce::String (f.key) + " shown with 「詳細な設定」 open");
                const double before = f.get (c.getSettings());
                operate (comp, f.key);
                expect (f.get (c.getSettings()) != before, juce::String (f.key) + " changed by its control");
                if (auto* s = dynamic_cast<juce::Slider*> (comp)) expect (s->getTextFromValue (s->getValue()).isNotEmpty());
            }
        }

        beginTest ("S-03 wave 4: 「詳細な設定」 hides detailed rows, opens every card, and the state is saved (settingsShowDetails)");
        {
            freshUiDataDir();
            AppController c (false);
            c.startup();
            FakeNavigator nav;
            {
                SettingsView v (c, nav);
                v.setSize (Theme::defaultWidth, Theme::defaultHeight);
                v.showSection (Navigator::SettingsSection::environment);
                expect (shown (findById (v, "settings.outputGain"), v), "everyday row");
                expect (! shown (findById (v, "settings.agcOn"), v), "detailed rows hidden by default");
                auto* d = find<PillButton> (v, "settings.details.environment");
                expect (d != nullptr && shown (d, v));
                if (d == nullptr) return;
                expect (d->getButtonText().startsWith (ja ("詳細な設定（")), d->getButtonText());
                d->onClick();
                expect (c.getSettings().settingsShowDetails, "saved in the settings");
                c.dispatchPendingMessages();
                expect (shown (findById (v, "settings.agcOn"), v), "opened");
                expectEquals (d->getButtonText(), ja ("詳細な設定を閉じる"));
                v.showSection (Navigator::SettingsSection::appearance);
                expect (shown (findById (v, "settings.alwaysOnTop"), v), "one state for every card");
                for (auto win : kWindows)
                    for (int s = 0; s < 7; ++s)
                    {
                        v.setBounds (pageArea (win, false).withPosition (0, 0));
                        v.showSection (kSections[s]);
                        checkLayout (*this, v, juce::String ("S-03 details open ") + kSectionNames[s] + " " + win.name);
                    }
            }
            SettingsView again (c, nav);
            again.setSize (Theme::defaultWidth, Theme::defaultHeight);
            again.showSection (Navigator::SettingsSection::environment);
            expect (shown (findById (again, "settings.agcOn"), again), "a new S-03 opens with the details open");
        }

        beginTest ("S-03 wave 4: search finds detailed rows by name and alias, groups by section, empty message, Esc / Ctrl+F");
        {
            freshUiDataDir();
            AppController c (false);
            c.startup();
            FakeNavigator nav;
            SettingsView v (c, nav);
            v.setSize (Theme::defaultWidth, Theme::defaultHeight);
            v.showSection (Navigator::SettingsSection::devices);
            auto only = [&] (const char* query, std::initializer_list<const char*> expected, std::initializer_list<const char*> absent)
            {
                v.setSearchText (juce::String::fromUTF8 (query));
                for (auto* id : expected) expect (shown (findById (v, juce::String ("settings.") + id), v), juce::String::fromUTF8 (query) + " -> " + id);
                for (auto* id : absent) expect (! shown (findById (v, juce::String ("settings.") + id), v), juce::String::fromUTF8 (query) + " hides " + id);
            };
            only ("PTT", { "pushToTalk", "pttReleaseMs" }, { "agcOn", "outputGain" });
            only ("プッシュトゥトーク", { "pushToTalk" }, { "wasapiExclusive" });
            only ("ぷっしゅ", { "pushToTalk" }, {});
            only ("排他", { "wasapiExclusive" }, { "pushToTalk", "agcOn" });
            only ("AGC", { "agcOn", "agcTargetDb", "agcMaxGainDb" }, { "highPassOn" });
            only ("ＡＧＣ", { "agcOn" }, {});
            only ("自動音量", { "agcOn" }, { "wasapiExclusive" });
            only ("アップデート", { "autoUpdate", "updateIncludePrerelease", "checkUpdates" }, { "agcOn" });
            only ("遅延", { "wasapiExclusive", "converterQuality", "bufferSize" }, { "knobWheel" });
            expect (containsText (v, ja ("デバイス")) && containsText (v, ja ("環境設定")) && containsText (v, ja ("詳細")), "section names shown");
            for (auto win : kWindows)
            {
                v.setBounds (pageArea (win, false).withPosition (0, 0));
                checkLayout (*this, v, juce::String ("S-03 search ") + win.name);
            }
            v.setSearchText ("zzzz-no-such-setting");
            auto* empty = find<TextLabel> (v, "settings.search.empty");
            expect (empty != nullptr && shown (empty, v) && empty->getText().contains (ja ("見つかりませんでした")), "friendly empty result");
            expect (press (&v, juce::KeyPress (juce::KeyPress::escapeKey)), "Esc clears the search");
            auto* search = find<juce::TextEditor> (v, "settings.search");
            expect (search != nullptr && search->getText().isEmpty());
            expect (shown (findById (v, "settings.output"), v), "back to the section");
            expect (! shown (findById (v, "settings.wasapiExclusive"), v), "details closed again outside the search");
            expect (press (&v, juce::KeyPress ('f', juce::ModifierKeys::ctrlModifier, 0)), "Ctrl+F goes to the search box");
            v.setSearchText ("PTT");
            v.showSection (Navigator::SettingsSection::startup);
            expect (search->getText().isEmpty() && shown (findById (v, "settings.closeAction"), v), "a section button leaves the search");
        }

        beginTest ("S-03 wave 4: 書き出し / 読み込み report in a toast, すべて初期化 asks first; updates row; S-04 asks about updates");
        {
            freshUiDataDir();
            AppController c (false);
            c.startup();
            FakeNavigator nav;
            SettingsView v (c, nav);
            v.setSize (Theme::defaultWidth, Theme::defaultHeight);
            v.showSection (Navigator::SettingsSection::advanced);
            const auto file = paths::dataDir().getChildFile ("exported-settings.json");
            juce::StringArray asked;
            settingsFileChooserForTests() = [&] (bool save) { asked.add (save ? "save" : "open"); return file; };
            if (auto* b = find<juce::Button> (v, "settings.export")) b->onClick();
            else expect (false, "settings.export");
            expectEquals (asked.joinIntoString (","), juce::String ("save"));
            expectEquals (nav.toasts.size(), 1, "書き出し reports (success or the controller's error)");
            if (auto* b = find<juce::Button> (v, "settings.import")) b->onClick();
            else expect (false, "settings.import");
            expectEquals (nav.toasts.size(), 2, "読み込み reports");
            settingsFileChooserForTests() = [&] (bool) { return juce::File(); }; // cancel: nothing happens
            if (auto* b = find<juce::Button> (v, "settings.import")) b->onClick();
            expectEquals (nav.toasts.size(), 2);
            settingsFileChooserForTests() = nullptr;

            if (auto* b = find<juce::Button> (v, "settings.reset")) b->onClick();
            expect (nav.calls.contains ("overlay:settings.resetConfirm"), "a confirmation first");
            juce::Button* ok = nullptr;
            if (nav.overlay != nullptr)
                collect (*nav.overlay, [&] (juce::Component& comp)
                {
                    if (auto* b = dynamic_cast<juce::Button*> (&comp); b != nullptr && b->getButtonText() == ja ("すべて初期化")) ok = b;
                });
            expect (ok != nullptr, "すべて初期化 in the confirmation");
            if (ok != nullptr) ok->onClick();
            expect (nav.toasts.contains (ja ("設定を最初の状態に戻しました。")));

            v.showSection (Navigator::SettingsSection::startup);
            auto* state = find<TextLabel> (v, "settings.updateState");
            expect (state != nullptr && state->getText().isNotEmpty(), "update state line");
            expect (containsText (v, juce::String ("v") + KOELOOM_VERSION_STRING), "current version");
            if (auto* b = find<juce::Button> (v, "settings.checkUpdates")) b->onClick(); // stub today: no network in any case
            else expect (false, "settings.checkUpdates");

            SetupWizard w (c, nav, [] (bool) {});
            w.setSize (Theme::defaultWidth, Theme::defaultHeight);
            if (auto* next = find<juce::Button> (w, "setup.next")) next->onClick();
            auto* t = find<ToggleSwitch> (w, "setup.autoUpdate");
            expect (t != nullptr && ! t->getToggleState(), "S-04 asks, OFF by default");
            if (t != nullptr)
            {
                t->setToggleState (true, juce::dontSendNotification);
                t->onClick();
                expect (c.getSettings().autoUpdate);
            }
            for (auto win : kWindows)
            {
                w.setSize (win.w, win.h);
                checkLayout (*this, w, juce::String ("S-04 step 2 with the update question ") + win.name);
            }
        }
    }
};

static UiScreensTests uiScreensTests;

// =============================================================================================== S-03 外観 配色 / theme editor (wave5/themes)
class UiThemeScreensTests : public juce::UnitTest
{
public:
    UiThemeScreensTests() : juce::UnitTest ("UiScreens themes", "UiScreens") {}

    static bool visibleIn (juce::Component* comp, juce::Component& root)
    {
        for (auto* p = comp; p != nullptr; p = p->getParentComponent())
        {
            if (p == &root) return true;
            if (! p->isVisible()) return false;
        }
        return false;
    }

    static void click (juce::Component* comp)
    {
        if (auto* b = dynamic_cast<juce::Button*> (comp); b != nullptr && b->onClick) b->onClick();
    }

    void runTest() override
    {
        beginTest ("S-03 外観: 画面の配置 and 配色 change Settings; この案の配色にもする; Studio-only rows; search aliases");
        {
            freshUiDataDir();
            Theme::clearPalette();
            AppController c (false);
            c.startup();
            FakeNavigator nav;
            SettingsView v (c, nav);
            v.setSize (Theme::defaultWidth, Theme::defaultHeight);
            v.showSection (Navigator::SettingsSection::appearance);
            auto* layout = find<Segmented> (v, "settings.layoutStyle");
            auto* match = find<PillButton> (v, "settings.layoutPalette");
            auto* combo = find<juce::ComboBox> (v, "settings.themeId");
            auto* edit = find<juce::Button> (v, "settings.themeEdit");
            expect (layout != nullptr && match != nullptr && combo != nullptr && edit != nullptr, "rows exist");
            if (layout == nullptr || match == nullptr || combo == nullptr || edit == nullptr) return;
            expect (! match->isVisible() && combo->getNumItems() == 3 && combo->getSelectedId() == 1, "A Studio, Studio / Paper / Mono");
            expect (! edit->isEnabled(), "編集 only for a user theme");

            layout->setSelected (1, true);
            c.dispatchPendingMessages();
            expectEquals (c.getSettings().layoutStyle, 1);
            expect (c.getSettings().themeId.isEmpty(), "the layout alone keeps the palette");
            expect (match->isVisible(), "B offers its palette");
            click (match);
            c.dispatchPendingMessages();
            expectEquals (c.getSettings().themeId, juce::String (ThemeLibrary::paperId));
            expect (! match->isVisible() && combo->getSelectedId() == 2, "offer gone once applied; 配色 shows Paper");
            for (auto* id : { "settings.theme", "settings.accent", "settings.tone" })
                expect (! findById (v, id)->isEnabled(), juce::String (id) + " only while 配色 is Studio");
            layout->setSelected (2, true);
            c.dispatchPendingMessages();
            click (match);
            c.dispatchPendingMessages();
            expectEquals (c.getSettings().themeId, juce::String (ThemeLibrary::monoId), "C -> Mono");

            combo->setSelectedId (1, juce::sendNotificationSync);
            c.dispatchPendingMessages();
            expect (c.getSettings().themeId.isEmpty() && findById (v, "settings.accent")->isEnabled(), "Studio again");
            checkLayout (*this, v, "S-03 appearance with a layout offer");

            ThemeData t { "Mine", true, Theme::dark() };
            juce::String err;
            const auto file = ThemeLibrary::saveNew (t, err);
            c.updateSettings ([] (Settings&) {});
            c.dispatchPendingMessages();
            expectEquals (combo->getNumItems(), 4, "user themes are listed");
            combo->setSelectedId (4, juce::sendNotificationSync);
            c.dispatchPendingMessages();
            expectEquals (c.getSettings().themeId, ThemeLibrary::userId (file));
            expect (edit->isEnabled());
            click (edit);
            auto* editor = dynamic_cast<ThemeEditor*> (nav.overlay.get());
            expect (editor != nullptr && editor->editingFile() == file, "編集 opens the editor on that theme");
            click (findById (v, "settings.themeNew"));
            editor = dynamic_cast<ThemeEditor*> (nav.overlay.get());
            expect (editor != nullptr && editor->editingFile().isEmpty(), "新しく作る opens a new theme");

            for (auto* q : { "スキン", "テーマ", "配色", "レイアウト", "デザイン", "案" })
            {
                v.setSearchText (juce::String::fromUTF8 (q));
                expect (visibleIn (layout, v) && visibleIn (combo, v), juce::String::fromUTF8 (q) + " finds 画面の配置 and 配色");
            }
            v.setSearchText ({});
            for (auto win : kWindows)
            {
                v.setBounds (pageArea (win, false).withPosition (0, 0));
                checkLayout (*this, v, juce::String ("S-03 appearance ") + win.name);
            }
        }

        beginTest ("Theme editor: starts from the current palette, picks apply live, contrast warnings, save uses the theme");
        {
            freshUiDataDir();
            Theme::clearPalette();
            Theme::setDark (true);
            AppController c (false);
            c.startup();
            FakeNavigator nav;
            ThemeEditor ed (c, nav);
            ed.setBounds (overlayArea (kWindows[0], ed));
            expect (samePalette (ed.working().colours, Theme::colours()), "starts from the current palette");
            expect (ed.editingFile().isEmpty());
            auto* summary = find<TextLabel> (ed, "themeEditor.contrastSummary");
            expect (summary != nullptr && summary->getTone() == Tone::ok, "the Studio palette passes");

            auto* selector = find<juce::ColourSelector> (ed, "themeEditor.selector");
            expect (selector != nullptr);
            click (findById (ed, "themeEditor.role.accent"));
            expect (selector->getCurrentColour() == Theme::colours().accent, "the selector shows the role's colour");
            const auto pink = *Theme::parseHex ("#FF5FA2");
            selector->setCurrentColour (pink);
            selector->dispatchPendingMessages();
            expect (ed.working().colours.accent == pink, "a pick in the selector changes the edited palette");
            ed.setRoleColour (5, ed.working().colours.bg); // text = background
            expect (summary->getTone() == Tone::warn && summary->getText().contains (ja ("足りない")), "contrast warning: " + summary->getText());
            expect (Theme::colours().text != ed.working().colours.text, "the app keeps its colours until saved");

            auto* nameField = find<juce::TextEditor> (ed, "themeEditor.name");
            nameField->setText (ja ("読みにくい"));
            click (findById (ed, "themeEditor.save"));
            const auto file = ed.editingFile();
            expect (file.isNotEmpty() && ThemeLibrary::fileFor (file).existsAsFile(), "saved despite the warning");
            expectEquals (c.getSettings().themeId, ThemeLibrary::userId (file), "and used");
            expect (summary->getTone() == Tone::warn, "the warning stays after saving");
            auto* message = find<TextLabel> (ed, "themeEditor.message");
            expect (message != nullptr && message->getTone() == Tone::warn && message->getText().contains (ja ("保存")), message->getText());
            expect (findById (ed, "themeEditor.item." + file) != nullptr, "listed under 自分のテーマ");

            ed.setRoleColour (5, *Theme::parseHex ("#FFFFFF"));
            click (findById (ed, "themeEditor.save"));
            expect (ed.editingFile() == file, "保存 again overwrites the same file");
            juce::String err;
            expect (ThemeLibrary::resolve (ThemeLibrary::userId (file), err)->colours.text == *Theme::parseHex ("#FFFFFF"));

            nameField->setText ("   ");
            click (findById (ed, "themeEditor.save"));
            expect (message->getTone() == Tone::danger, "an empty name is refused");
            for (auto win : kWindows)
            {
                ed.setBounds (overlayArea (win, ed).withPosition (0, 0));
                checkLayout (*this, ed, juce::String ("theme editor ") + win.name);
            }
        }

        beginTest ("Theme editor: rename, duplicate, delete (asks first), export / import through the file chooser seam");
        {
            freshUiDataDir();
            Theme::clearPalette();
            AppController c (false);
            c.startup();
            FakeNavigator nav;
            ThemeData t { "Alpha", true, Theme::mono() };
            juce::String err;
            const auto file = ThemeLibrary::saveNew (t, err);
            c.updateSettings ([file] (Settings& s) { s.themeId = ThemeLibrary::userId (file); });
            ThemeEditor ed (c, nav, file);
            ed.setBounds (overlayArea (kWindows[0], ed));
            expect (ed.editingFile() == file && samePalette (ed.working().colours, Theme::mono()), "opened on the saved theme");

            click (findById (ed, "themeEditor.rename." + file));
            auto* prompt = find<InlinePrompt> (ed, "themeEditor.prompt");
            expect (prompt != nullptr && prompt->getEditor() != nullptr);
            prompt->getEditor()->setText ("Beta");
            prompt->ok();
            expectEquals (ThemeLibrary::resolve (ThemeLibrary::userId (file), err)->name, juce::String ("Beta"));
            expectEquals (find<juce::TextEditor> (ed, "themeEditor.name")->getText(), juce::String ("Beta"), "the edited theme's name follows");

            click (findById (ed, "themeEditor.duplicate." + file));
            expectEquals (int (ThemeLibrary::list().size()), 2);

            const auto out = paths::dataDir().getChildFile ("theme-out.json");
            int asked = 0;
            themeFileChooserForTests() = [&] (bool save) { ++asked; return save ? out : out; };
            click (findById (ed, "themeEditor.export." + file));
            expect (out.existsAsFile() && asked == 1, "書き出し writes the chosen file");
            click (findById (ed, "themeEditor.import"));
            expectEquals (int (ThemeLibrary::list().size()), 3, "読み込み adds a copy");
            const auto bad = paths::dataDir().getChildFile ("theme-bad.json");
            bad.replaceWithText ("{ \"colours\": { \"bg\": \"blue\" } }");
            themeFileChooserForTests() = [&] (bool) { return bad; };
            click (findById (ed, "themeEditor.import"));
            auto* message = find<TextLabel> (ed, "themeEditor.message");
            expect (message->getTone() == Tone::danger && message->getText().contains (ja ("読み込めませんでした")), message->getText());
            themeFileChooserForTests() = [] (bool) { return juce::File(); };
            click (findById (ed, "themeEditor.import"));
            expectEquals (int (ThemeLibrary::list().size()), 3, "cancelled chooser does nothing");
            themeFileChooserForTests() = nullptr;

            click (findById (ed, "themeEditor.delete." + file));
            prompt = find<InlinePrompt> (ed, "themeEditor.prompt");
            expect (prompt != nullptr && prompt->isVisible(), "削除 asks first");
            expect (ThemeLibrary::fileFor (file).existsAsFile(), "nothing deleted before the answer");
            prompt->ok();
            expect (! ThemeLibrary::fileFor (file).exists(), "deleted");
            expect (c.getSettings().themeId.isEmpty(), "the theme in use was deleted: back to Studio");
            expect (ed.editingFile().isEmpty(), "the colours stay as a new theme");
            expectEquals (int (ThemeLibrary::list().size()), 2);
            Theme::clearPalette();
        }
    }

    static bool samePalette (const Palette& a, const Palette& b)
    {
        for (int r = 0; r < Theme::numRoles; ++r)
            if (Theme::role (a, r) != Theme::role (b, r)) return false;
        return true;
    }
};

static UiThemeScreensTests uiThemeScreensTests;

// =============================================================================================== snapshots
class UiScreensSnapshots : public juce::UnitTest
{
public:
    UiScreensSnapshots() : juce::UnitTest ("UiScreens snapshots", "Diag") {}

    struct Host : juce::Component
    {
        bool dim = false;
        void paint (juce::Graphics& g) override
        {
            g.fillAll (Theme::colours().bg);
            if (dim) g.fillAll (Theme::colours().overlay);
        }
    };

    void save (juce::Component& host, const juce::File& file)
    {
        const auto img = host.createComponentSnapshot (host.getLocalBounds(), true, 1.0f);
        file.deleteFile();
        juce::FileOutputStream out (file);
        expect (out.openedOk() && juce::PNGImageFormat().writeImageToStream (img, out), file.getFullPathName());
    }

    void page (juce::Component& screen, WinSize win, bool bottomBar, const juce::File& file)
    {
        Host host;
        host.setSize (win.w, win.h);
        host.addAndMakeVisible (screen);
        screen.setBounds (pageArea (win, bottomBar));
        save (host, file);
        host.removeChildComponent (&screen);
    }

    void overlay (juce::Component& panel, WinSize win, const juce::File& file)
    {
        Host host;
        host.dim = true;
        host.setSize (win.w, win.h);
        const auto pref = panel.getBounds();
        host.addAndMakeVisible (panel);
        panel.setBounds (overlayArea (win, panel));
        save (host, file);
        host.removeChildComponent (&panel);
        panel.setBounds (pref);
    }

    void runTest() override
    {
        beginTest ("PNG snapshots of the ui-screens screens (dark and light)");
        const auto env = juce::SystemStats::getEnvironmentVariable ("KOELOOM_SNAPSHOT_DIR", {});
        const auto dir = env.isNotEmpty() ? juce::File (env) : juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("KoeLoomUiSnapshots");
        dir.createDirectory();
        for (bool dark : { true, false })
        {
            Theme::setDark (dark);
            KoeLookAndFeel lnf;
            juce::LookAndFeel::setDefaultLookAndFeel (&lnf);
            const juce::String theme = dark ? "dark" : "light";
            {
                freshUiDataDir();
                AppController c (false);
                c.startup();
                fillSoundboard (c);
                juce::String why;
                c.toggleFavorite ("character-demon-king", why);
                c.loadPreset ("character-demon-king");
                FakeNavigator nav;
                for (auto win : kWindows)
                {
                    const juce::String sz = win.w == Theme::defaultWidth ? "wide" : "min";
                    {
                        SettingsView v (c, nav);
                        for (int s = 0; s < 7; ++s)
                        {
                            v.showSection (kSections[s]);
                            page (v, win, false, dir.getChildFile ("s03-" + juce::String (kSectionNames[s]) + "-" + sz + "-" + theme + ".png"));
                        }
                        c.setOutputGainDb (8.0f);
                        v.showSection (Navigator::SettingsSection::environment);
                        page (v, win, false, dir.getChildFile ("s03-environment-warn-" + sz + "-" + theme + ".png"));
                        c.setOutputGainDb (0.0f);
                    }
                    {
                        c.updateSettings ([] (Settings& s) { s.soundboardHintShown = false; });
                        SoundboardView v (c, nav);
                        v.setVisible (true);
                        page (v, win, true, dir.getChildFile ("s02-hint-" + sz + "-" + theme + ".png"));
                        c.updateSettings ([] (Settings& s) { s.soundboardHintShown = true; });
                    }
                    {
                        SoundboardView v (c, nav);
                        v.setVisible (true);
                        page (v, win, true, dir.getChildFile ("s02-" + sz + "-" + theme + ".png"));
                        if (auto* card = find<juce::Button> (v, "soundboard.slot.4")) card->onClick();
                        if (nav.overlay != nullptr) overlay (*nav.overlay, win, dir.getChildFile ("s02-slot-settings-" + sz + "-" + theme + ".png"));
                    }
                    {
                        PresetBrowser b (c, nav);
                        overlay (b, win, dir.getChildFile ("s06-" + sz + "-" + theme + ".png"));
                        if (auto* del = find<juce::Button> (b, "presets.saveNew")) del->onClick();
                        overlay (b, win, dir.getChildFile ("s06-prompt-" + sz + "-" + theme + ".png"));
                    }
                    {
                        SetupWizard w (c, nav, [] (bool) {});
                        for (int s = 0; s < 3; ++s)
                        {
                            w.setSize (win.w, win.h);
                            Host host;
                            host.setSize (win.w, win.h);
                            host.addAndMakeVisible (w);
                            save (host, dir.getChildFile ("s04-step" + juce::String (s + 1) + "-" + sz + "-" + theme + ".png"));
                            host.removeChildComponent (&w);
                            if (auto* next = find<juce::Button> (w, "setup.next"); next != nullptr && s < 2) next->onClick();
                        }
                    }
                    const char* helpNames[] = { "discord", "revert", "licenses" };
                    const Navigator::HelpTopic topics[] = { Navigator::HelpTopic::discordSetup, Navigator::HelpTopic::revertMic, Navigator::HelpTopic::licenses };
                    for (int t = 0; t < 3; ++t)
                    {
                        auto panel = createHelpPanel (topics[t], c, nav);
                        overlay (*panel, win, dir.getChildFile ("help-" + juce::String (helpNames[t]) + "-" + sz + "-" + theme + ".png"));
                    }
                    {
                        // wave5/themes: the theme editor, then scrolled to its contrast table with a too faint sub text
                        ThemeEditor ed (c, nav);
                        overlay (ed, win, dir.getChildFile ("theme-editor-" + sz + "-" + theme + ".png"));
                        ed.setRoleColour (6, ed.working().colours.trackOff);
                        Host host;
                        host.dim = true;
                        host.setSize (win.w, win.h);
                        host.addAndMakeVisible (ed);
                        ed.setBounds (overlayArea (win, ed));
                        auto* summary = findById (ed, "themeEditor.contrastSummary");
                        if (auto* vp = summary != nullptr ? summary->findParentComponentOfClass<juce::Viewport>() : nullptr)
                            vp->setViewPosition (0, summary->getY() - Theme::space5 - Theme::space3);
                        save (host, dir.getChildFile ("theme-editor-contrast-" + sz + "-" + theme + ".png"));
                        host.removeChildComponent (&ed);
                    }
                }
                if (dark)
                {
                    // wave5/themes: S-01 (layout A) in the built-in Paper and Mono palettes
                    c.updateSettings ([] (Settings& s) { s.setupDone = true; s.tourStep = 7; });
                    for (auto* id : { ThemeLibrary::paperId, ThemeLibrary::monoId })
                    {
                        c.updateSettings ([id] (Settings& s) { s.themeId = id; });
                        {
                            MainComponent mc (c);
                            mc.setSize (Theme::defaultWidth, Theme::defaultHeight);
                            if (auto* v = dynamic_cast<mainui::VoicePage*> (findById (mc, "page.voice"))) v->tick();
                            save (mc, dir.getChildFile ("S01-wide-" + juce::String (id).fromFirstOccurrenceOf (":", false, false) + ".png"));
                        }
                        c.updateSettings ([] (Settings& s) { s.themeId = {}; });
                        Theme::clearPalette();
                        Theme::setDark (dark);
                        lnf.refreshColours();
                    }
                }
            }
            juce::LookAndFeel::setDefaultLookAndFeel (nullptr);
        }
        Theme::setDark (true);
        logMessage ("  snapshots in " + dir.getFullPathName());
    }
};

static UiScreensSnapshots uiScreensSnapshots;
} // namespace koe

// ui-screens checks (category "UiScreens"): layout at 1120x720 and 800x560, settings reaching the
// AppController, hotkey capture, preset list counts / search / CRUD, setup steps, soundboard options,
// help pages. "UiScreens snapshots" (category "Diag", not in the normal run) writes PNGs of every
// screen in both themes to %KOELOOM_SNAPSHOT_DIR% (or %TEMP%\KoeLoomUiSnapshots) for a visual check.
// Nothing here opens a window, a device, a file chooser or the browser.

#include "App/AppController.h"
#include "Core/Constants.h"
#include "Core/Paths.h"
#include "Platform/Hotkeys.h"
#include "UI/Screens.h"
#include "UI/Widgets.h"
#include "UI/screens/Common.h"

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
            || dynamic_cast<juce::Slider*> (&parent) != nullptr || dynamic_cast<juce::ScrollBar*> (&parent) != nullptr)
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
            expectEquals (count ("all"), 61);
            expectEquals (count ("natural"), 11);
            expectEquals (count ("character"), 22);
            expectEquals (count ("device"), 12);
            expectEquals (count ("space"), 8);
            expectEquals (count ("layered"), 8);
            expectEquals (count ("user"), 0);
            expectEquals (count ("favorites"), 0);

            if (auto* all = find<juce::Button> (b, "presets.cat.all")) all->onClick();
            expectEquals (visibleRows (b).size(), 61);
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
};

static UiScreensTests uiScreensTests;

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

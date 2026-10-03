#pragma once

// Screen classes owned by the ui-screens worker (S-02, S-03, S-04, S-06, help pages).
// The public constructors and methods below are the contract used by MainComponent (ui-main);
// private members may be added freely.

#include "App/AppController.h"
#include "UI/Navigator.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <memory>

namespace koe::ui
{
/** S-03 設定: デバイス / 環境設定 / ホットキー / 起動と常駐 / 外観 / 詳細 / 診断 (docs/mockups/A-S03*.dc.html).
    A search box above the sections; each card's 「詳細な設定」 opens the detailed rows (INTERFACES.md §7). */
class SettingsView : public juce::Component
{
public:
    SettingsView (AppController& controller, Navigator& nav);
    ~SettingsView() override;
    void showSection (Navigator::SettingsSection section);
    /** The search box's text (as if typed); empty returns to the current section. */
    void setSearchText (const juce::String& text);
    /** Ctrl+F focuses the search box, Esc clears it (MainComponent forwards these while S-03 is shown). */
    bool keyPressed (const juce::KeyPress& key) override;
    void resized() override;
    void paint (juce::Graphics& g) override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

/** Tests only: replaces S-03's file chooser for 書き出し (save = true) / 読み込み. Returns the file, or {} to cancel. */
std::function<juce::File (bool save)>& settingsFileChooserForTests();

/** S-02 サウンドボード: 12 slots (4x3) (docs/mockups/A-S02.dc.html). */
class SoundboardView : public juce::Component
{
public:
    SoundboardView (AppController& controller, Navigator& nav);
    ~SoundboardView() override;
    void resized() override;
    void paint (juce::Graphics& g) override;
    void visibilityChanged() override; // first open shows the one-time hint (F-13-7)

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

/** S-06 プリセット一覧 overlay: category filter, search, favourites, CRUD, import/export (docs/mockups/A-S06.dc.html). */
class PresetBrowser : public juce::Component
{
public:
    PresetBrowser (AppController& controller, Navigator& nav);
    ~PresetBrowser() override;
    void resized() override;
    void paint (juce::Graphics& g) override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

/** S-04 初回セットアップ (docs/mockups/A-S04.dc.html). Both 完了して始める and あとで設定する set
    setupDone and call onFinished: completed = true for 完了して始める, false for あとで設定する. */
class SetupWizard : public juce::Component
{
public:
    SetupWizard (AppController& controller, Navigator& nav, std::function<void (bool completed)> onFinished);
    ~SetupWizard() override;
    void resized() override;
    void paint (juce::Graphics& g) override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

/** [?] menu pages: Discord の設定手順 (F-10-4), 元のマイクに戻す方法 (F-10-5), ライセンス表示 (§11). */
std::unique_ptr<juce::Component> createHelpPanel (Navigator::HelpTopic topic, AppController& controller, Navigator& nav);
} // namespace koe::ui

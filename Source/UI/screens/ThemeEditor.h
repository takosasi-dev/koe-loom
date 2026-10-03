#pragma once

// S-03 外観 「配色」 → 「新しく作る」 / 「編集」 (INTERFACES.md §8.4): the user's own theme, as an overlay panel.
// Starts from the current palette (or a saved theme); a colour per role (juce::ColourSelector: hue, a
// saturation / brightness square and a hex field); a live sample in the edited colours; the contrast table
// (pairs under 4.5:1 / 3:1 are marked 「不足」, saving is still allowed and the warning stays); name + 保存
// (saves and uses it); and the saved themes with 編集 / 名前変更 / 複製 / 削除 (asks first) / 書き出し and 読み込み.

#include "App/AppController.h"
#include "UI/Navigator.h"
#include "UI/screens/Common.h"

namespace koe::ui
{
struct ThemeData;

class ThemeEditor : public screens::OverlayPanel
{
public:
    /** file: the user theme to edit (file name without .json, ThemeLibrary), or "" for a new one from the current palette. */
    ThemeEditor (AppController& controller, Navigator& nav, const juce::String& file = {});
    ~ThemeEditor() override;

    const ThemeData& working() const;
    /** The saved file being edited ("" until a new theme is saved). */
    juce::String editingFile() const;
    /** Picks a colour for a role as the selector would (the sample and the contrast table follow at once). */
    void setRoleColour (int role, juce::Colour colour);

    bool keyPressed (const juce::KeyPress& k) override;
    void lookAndFeelChanged() override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

/** Tests only: replaces the theme editor's file chooser for 書き出し (save = true) / 読み込み. Returns the file, or {} to cancel. */
std::function<juce::File (bool save)>& themeFileChooserForTests();
} // namespace koe::ui

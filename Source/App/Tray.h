#pragma once

#include "App/AppController.h"

#include <juce_gui_extra/juce_gui_extra.h>

namespace koe
{
/** Task-tray icon and its menu (S-05, F-09-1): 開く / ボイチェン ON/OFF / マイクミュート / お気に入り / 終了.
    Double-click opens the window. */
class TrayIcon final : public juce::SystemTrayIconComponent
{
public:
    explicit TrayIcon (AppController& controller);
    void mouseDown (const juce::MouseEvent& e) override;
    void mouseDoubleClick (const juce::MouseEvent& e) override;
    /** Updates the tooltip (state in text, never colour alone). */
    void refresh();

private:
    AppController& controller;
};
} // namespace koe

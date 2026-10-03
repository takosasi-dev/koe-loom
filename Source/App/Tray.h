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
    /** Updates the tooltip (state in text, never colour alone); with trayNotifications on, a new danger notice
        pops a tray balloon while KoeLoom is not the foreground app (in a game, the window hidden). */
    void refresh();

private:
    AppController& controller;
    juce::StringArray announced;
};

/** Danger notices not in seen; seen becomes the current danger keys (one that goes and comes back is new again). */
std::vector<Notice> newDangerNotices (const std::vector<Notice>& now, juce::StringArray& seen);
} // namespace koe

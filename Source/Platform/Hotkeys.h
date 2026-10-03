#pragma once

#include "Model/Settings.h"

#include <juce_core/juce_core.h>

#include <functional>
#include <memory>
#include <vector>

namespace koe
{
/**
    Global hotkeys with Win32 RegisterHotKey (F-07-1, D-6: never a low-level keyboard hook).
    Message thread only. Action ids (F-07-2):
      voiceToggle, muteToggle, favoriteNext, favoritePrev, favorite.1 .. favorite.9,
      slot.1 .. slot.10, sound.1 .. sound.12, soundStopAll, freezeToggle, looperRecPlay, looperClear
*/
class Hotkeys
{
public:
    Hotkeys();
    ~Hotkeys();

    /** Unregisters everything, then registers the given bindings. Returns the ones that failed
        (taken by another app, F-07-3). Bindings with virtualKey == 0 are skipped. */
    std::vector<HotkeyBinding> apply (const std::vector<HotkeyBinding>& bindings);
    void unregisterAll();

    /** Called on the message thread with the action id when a hotkey fires. */
    std::function<void (const juce::String& action)> onHotkey;

    /** "Ctrl+Shift+F1" style text for the UI and failure notices. */
    static juce::String describe (int modifiers, int virtualKey);
    /** Every action id in UI order. */
    static juce::StringArray allActions();
    /** Japanese label, e.g. "ボイチェン ON/OFF", "お気に入り 3", "スロット 7 の ON/OFF". */
    static juce::String actionLabel (const juce::String& action);

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
} // namespace koe

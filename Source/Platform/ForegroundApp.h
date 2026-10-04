#pragma once

// アプリごとの自動切り替え (INTERFACES.md §10.3, owner wave8/automation): which program is in front, and which have windows.
// Tests replace both through the hooks below and never look at real windows.

#include <juce_core/juce_core.h>

#include <functional>

namespace koe::foreground
{
/** File name of the program owning the foreground window, e.g. "VALORANT.exe" ("" = unknown). */
juce::String programInFront();
/** Programs with a visible, unowned, titled top-level window: file names, sorted, unique, without KoeLoom itself. */
juce::StringArray visiblePrograms();
/** KoeLoom's own exe name (also true for "KoeLoom.exe"), compared ignoring case. */
bool isSelf (const juce::String& fileName);

inline std::function<juce::String()> programInFrontForTests;
inline std::function<juce::StringArray()> visibleProgramsForTests;
} // namespace koe::foreground

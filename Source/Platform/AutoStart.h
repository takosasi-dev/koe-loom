#pragma once

#include <juce_core/juce_core.h>

namespace koe::autostart
{
/** HKCU\Software\Microsoft\Windows\CurrentVersion\Run, value "KoeLoom" = "<exe>" --autostart (F-09-3).
    No administrator rights needed. Returns false and sets error (Japanese) on failure. */
bool setEnabled (bool enabled, juce::String& error);
bool isEnabled();
/** The Run value for an exe: "\"<full path>\" --autostart" (pure; for tests). */
juce::String commandFor (const juce::File& exe);
} // namespace koe::autostart

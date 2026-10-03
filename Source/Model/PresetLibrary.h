#pragma once

// Built-in presets (embedded JSON, read-only) + user presets (%APPDATA%\KoeLoom\presets\, one file each).
// Message thread only.

#include "Model/Preset.h"

#include <juce_core/juce_core.h>

#include <vector>

namespace koe
{
class PresetLibrary
{
public:
    /** userDir: where user presets live (created on demand). */
    explicit PresetLibrary (juce::File userDir);

    /** (Re)loads built-ins from BinaryData and user presets from disk.
        Broken user files are skipped and counted (E-09). Returns notices to show (Japanese, may be empty). */
    juce::StringArray reload();

    const std::vector<Preset>& all() const { return presets; } // built-ins first (presets.md §4 order), then user by name
    const Preset* find (const std::string& id) const;

    /** CRUD for user presets (F-05-2). Built-ins can only be duplicated. All return false + error on failure. */
    bool saveNew (Preset preset, const juce::String& name, std::string& newIdOut, juce::String& error); // assigns a fresh "user-..." id
    bool overwrite (const Preset& preset, juce::String& error);                                        // user only
    bool duplicate (const std::string& id, std::string& newIdOut, juce::String& error);
    bool rename (const std::string& id, const juce::String& newName, juce::String& error);             // user only
    bool remove (const std::string& id, juce::String& error);                                          // user only

    /** F-05-4 import (rejects > 64 KB, applies the loader rules; imported preset becomes a user preset). */
    bool importFile (const juce::File& file, std::string& newIdOut, PresetLoadReport& report);
    bool exportFile (const std::string& id, const juce::File& destination, juce::String& error) const;

    /** Number of built-ins that failed to parse (must be 0; tested by AC-44). */
    int builtinErrors() const { return builtinErrorCount; }

private:
    juce::File userDir;
    std::vector<Preset> presets;
    int builtinErrorCount = 0;
};
} // namespace koe

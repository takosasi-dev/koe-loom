#pragma once

// S-03 外観 「配色」: the built-in Paper / Mono palettes and the user's own themes (INTERFACES.md §8.4).
// A user theme is paths::themesDir()/<file>.json, Settings::themeId "user:<file>":
//   {"name": "...", "dark": true, "colours": {"bg": "#0E1217", "accent": "#4CC9F0", ...}}
// with the Palette roles by name (Theme::roleKey). Missing roles are derived from bg / text / accent
// (Theme::complete). Broken files are left out of list(); loading one gives the reason in Japanese.

#include "UI/Theme.h"

namespace koe::ui
{
struct ThemeData
{
    juce::String name;
    bool dark = true;
    Palette colours;
};

struct ThemeLibrary
{
    static constexpr const char* paperId = "builtin:paper";
    static constexpr const char* monoId = "builtin:mono";
    static constexpr int maxNameLength = 32;
    static juce::String userId (const juce::String& file) { return "user:" + file; }
    /** The file name of a "user:<file>" id, or "" for any other id. */
    static juce::String userFile (const juce::String& themeId);

    /** The theme for a Settings::themeId: Paper, Mono or a user theme ("" is Studio and has none here).
        Unknown or broken -> nullopt and a Japanese reason. */
    static std::optional<ThemeData> resolve (const juce::String& themeId, juce::String& error);

    struct Entry { juce::String file, name; };
    /** The readable themes in paths::themesDir(), by name. */
    static std::vector<Entry> list();

    static std::optional<ThemeData> parse (const juce::String& json, juce::String& error);
    static juce::String toJson (const ThemeData& t);
    /** A file anywhere (import); a file without a name gets its file name. */
    static std::optional<ThemeData> load (const juce::File& f, juce::String& error);
    static juce::File fileFor (const juce::String& file);

    /** Saves t as a new file named after it; returns the file name, or "" and the reason. */
    static juce::String saveNew (const ThemeData& t, juce::String& error);
    static bool save (const juce::String& file, const ThemeData& t, juce::String& error);
    /** Changes the name inside the file (the file name, and so the theme id, stays). */
    static bool rename (const juce::String& file, const juce::String& newName, juce::String& error);
    static juce::String duplicate (const juce::String& file, juce::String& error);
    static bool remove (const juce::String& file, juce::String& error);
    /** Checks the file and copies it into the themes folder; returns the new file name, or "". */
    static juce::String importFile (const juce::File& source, juce::String& error);
    static bool exportFile (const juce::String& file, const juce::File& dest, juce::String& error);

    /** The trimmed name, or "" and the reason (empty, longer than maxNameLength). */
    static juce::String checkName (const juce::String& name, juce::String& error);
};
} // namespace koe::ui

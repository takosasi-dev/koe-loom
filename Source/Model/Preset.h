#pragma once

// Preset model and JSON format (koeloom_presets.md §3, spec F-05, E-09..E-11, E-21..E-24, E-30).

#include <juce_core/juce_core.h>

#include <optional>
#include <string>
#include <vector>

namespace koe
{
struct LayerDef
{
    enum class Mode { fixed, scale };
    Mode mode = Mode::fixed;
    float pitchSt = 0.0f;   // fixed: -12..12, relative to the input
    int degree = 2;         // scale: -7..7 except 0 (+2 = a third up)
    int key = 0;            // scale: 0=C .. 11=B
    bool minor = false;     // scale: major / minor
    float formantSt = 0.0f; // -6..6
    float levelDb = -6.0f;  // -24..0
    bool enabled = true;    // per-voice ON (S-01 mockup). Not in koeloom_presets.md §3: optional key, default true

    bool operator== (const LayerDef&) const = default;
};

struct SlotDef
{
    std::string type;          // koeloom_effects.md §2 type
    bool enabled = true;
    std::vector<float> params; // one per ParamSpec, in registry order, spec units, choice = index
    std::string file;          // "convolution" only: a file name inside paths::irDir(), no folders (INTERFACES.md §9). JSON key "file", optional

    bool operator== (const SlotDef&) const = default;
};

struct Preset
{
    std::string id;            // ^[a-z0-9]+(-[a-z0-9]+)*$ ; user presets start with "user-"
    juce::String name;         // 1..32 chars
    bool builtin = false;
    bool hasShifter = false;   // false = 変換 OFF (R-P3)
    float pitchSt = 0.0f;
    float formantSt = 0.0f;
    std::vector<LayerDef> layers; // 0..2, only when hasShifter
    std::vector<SlotDef> chain;   // 0..10, processing order
    float outputTrimDb = 0.0f;    // -12..6

    /** "natural" "character" "device" "space" "layered" or "user" (from the id prefix, F-05-6). */
    juce::String category() const;
    bool operator== (const Preset&) const = default;
};

/** What the loader had to change or drop. Anything non-zero is shown to the user (E-21..E-24, E-30). */
struct PresetLoadReport
{
    bool rejected = false;      // nothing was loaded
    juce::String rejectReason;  // Japanese, e.g. "新しい版のプリセットです" (E-11) / "64 KB を超えています"
    int unknownTypesSkipped = 0;     // E-22
    int slotsOverLimitDropped = 0;   // E-21
    int duplicateFreezeLooperDropped = 0; // E-24
    int heavyTurnedOff = 0;          // E-23
    int layersDropped = 0;           // E-30 (over 2, or no shifter)
    int unknownLayerModesSkipped = 0;// E-30
    int valuesClamped = 0;           // E-10 (also counts invalid choice ids reset to default)

    bool hasNotices() const;
    juce::String toJapanese() const; // one line per notice, empty when nothing to say
};

/** Parses one preset file's text. Applies, in order: size limit, schemaVersion check, required keys,
    E-22 -> E-21 -> E-24 -> E-23 on the chain, E-30 on layers, clamping (E-10). Unknown keys are ignored.
    Returns nullopt when rejected (report.rejected/rejectReason set). */
std::optional<Preset> parsePreset (const juce::String& jsonText, PresetLoadReport& report);

/** Inverse of parsePreset. Writes every parameter; choice params as their json id string. */
juce::String serializePreset (const Preset& preset);

/** A slot of the given type with all parameters at their defaults (nullopt for unknown types). */
std::optional<SlotDef> makeDefaultSlot (const std::string& type);

/** id must match ^[a-z0-9]+(-[a-z0-9]+)*$ */
bool isValidPresetId (const std::string& id);
} // namespace koe

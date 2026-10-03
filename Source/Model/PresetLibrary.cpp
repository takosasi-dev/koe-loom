#include "Model/PresetLibrary.h"

#include "BinaryData.h" // KoeLoomPresetData (juce_add_binary_data in CMakeLists.txt)
#include "Core/Constants.h"

#include <algorithm>
#include <iterator>

namespace koe
{
namespace
{
/** Source literals are UTF-8 (/utf-8); juce::String (const char*) would treat them as Latin-1. */
juce::String ja (const char* s) { return juce::String::fromUTF8 (s); }

// koeloom_presets.md §4 table order.
const char* const kBuiltinOrder[] = {
    "natural-asis", "natural-clear", "natural-slightly-high", "natural-slightly-low", "natural-calm", "natural-bright",
    "natural-doubling", "natural-announcer", "natural-radio-dj", "natural-ikebo", "natural-deessed",
    "character-female", "character-male", "character-boy", "character-girl", "character-old-man", "character-old-woman",
    "character-helium", "character-hamster", "character-giant", "character-demon", "character-demon-king",
    "character-masked-villain", "character-robot", "character-cyborg", "character-alien", "character-ghost",
    "character-fairy", "character-crowd", "character-whisper", "character-kero", "character-vocoder-robot",
    "character-talkbox",
    "device-telephone", "device-am-radio", "device-megaphone", "device-military-radio", "device-8bit",
    "device-broken-speaker", "device-old-tape", "device-radio-noise", "device-wah-voice", "device-vowel-filter",
    "device-slicer-trance", "device-stutter-glitch",
    "space-cave", "space-bath", "space-cathedral", "space-yamabiko", "space-underwater", "space-behind-wall",
    "space-tape-echo", "space-reverse-echo",
    "layered-harmony-fifth", "layered-harmony-octave", "layered-choir-3", "layered-drunk", "layered-trembling",
    "layered-phaser-voice", "layered-rotary-voice", "layered-harmony-scale",
};

int builtinRank (const std::string& id)
{
    const auto it = std::find_if (std::begin (kBuiltinOrder), std::end (kBuiltinOrder), [&id] (const char* s) { return id == s; });
    return int (std::distance (std::begin (kBuiltinOrder), it)); // unknown ids sort last
}

bool isUserId (const std::string& id) { return id.rfind ("user-", 0) == 0; }

void sortUsers (std::vector<Preset>& v)
{
    auto firstUser = std::find_if (v.begin(), v.end(), [] (const Preset& p) { return ! p.builtin; });
    std::stable_sort (firstUser, v.end(), [] (const Preset& a, const Preset& b) {
        const int c = a.name.compareNatural (b.name);
        return c != 0 ? c < 0 : a.id < b.id;
    });
}

bool checkName (juce::String& name, juce::String& error)
{
    name = name.trim();
    if (name.isNotEmpty() && name.length() <= kPresetNameMaxChars) return true;
    error = ja ("名前は 1〜32 文字にしてください");
    return false;
}

/** Writes a user preset to "<id>.json" and puts it in the list. Stores exactly what a later load
    will see (limits and ranges applied by a parse of the serialised text). */
bool storeUser (std::vector<Preset>& presets, const juce::File& userDir, const Preset& preset, juce::String& error)
{
    PresetLoadReport report;
    auto p = parsePreset (serializePreset (preset), report);
    if (! p || ! isUserId (p->id))
    {
        error = p ? ja ("ユーザープリセットの ID ではありません") : report.rejectReason;
        return false;
    }
    userDir.createDirectory();
    if (! userDir.getChildFile (p->id + ".json").replaceWithText (serializePreset (*p), false, false, "\n"))
    {
        error = ja ("プリセットを保存できません");
        return false;
    }
    auto it = std::find_if (presets.begin(), presets.end(), [&] (const Preset& x) { return x.id == p->id; });
    if (it != presets.end()) *it = std::move (*p);
    else presets.push_back (std::move (*p));
    sortUsers (presets);
    return true;
}
} // namespace

PresetLibrary::PresetLibrary (juce::File dir) : userDir (std::move (dir)) {}

juce::StringArray PresetLibrary::reload()
{
    presets.clear();
    builtinErrorCount = 0;
    juce::StringArray notices;

    for (int i = 0; i < KoeLoomPresetData::namedResourceListSize; ++i)
    {
        const char* resource = KoeLoomPresetData::namedResourceList[i];
        int size = 0;
        const char* data = KoeLoomPresetData::getNamedResource (resource, size);
        PresetLoadReport report;
        auto p = parsePreset (juce::String::fromUTF8 (data, size), report);
        // a built-in that needed any fixing is a defect too (AC-44)
        if (! p || report.hasNotices() || p->id + ".json" != KoeLoomPresetData::getNamedResourceOriginalFilename (resource))
        {
            ++builtinErrorCount;
            continue;
        }
        p->builtin = true;
        presets.push_back (std::move (*p));
    }
    std::stable_sort (presets.begin(), presets.end(), [] (const Preset& a, const Preset& b) { return builtinRank (a.id) < builtinRank (b.id); });
    if (builtinErrorCount > 0) notices.add (ja ("内蔵プリセット ") + juce::String (builtinErrorCount) + ja (" 件を読み込めませんでした"));

    // user presets: one "<id>.json" each; the id must start with "user-" and be unique (E-09 otherwise)
    int broken = 0;
    for (auto& f : userDir.findChildFiles (juce::File::findFiles, false, "*.json"))
    {
        PresetLoadReport report;
        auto p = f.getSize() > kPresetMaxBytes ? std::nullopt : parsePreset (f.loadFileAsString(), report);
        if (! p || ! isUserId (p->id) || p->id != f.getFileNameWithoutExtension().toStdString() || find (p->id) != nullptr)
        {
            ++broken;
            juce::Logger::writeToLog ("preset skipped: " + f.getFileName() + " " + report.rejectReason);
            continue;
        }
        if (report.hasNotices()) notices.add (ja ("「") + p->name + ja ("」: ") + report.toJapanese().replace ("\n", ja ("、")));
        presets.push_back (std::move (*p));
    }
    sortUsers (presets);
    if (broken > 0) notices.add (ja ("読み込めないユーザープリセットが ") + juce::String (broken) + ja (" 件あります（一覧には出しません）"));
    return notices;
}

const Preset* PresetLibrary::find (const std::string& id) const
{
    for (auto& p : presets)
        if (p.id == id) return &p;
    return nullptr;
}

bool PresetLibrary::saveNew (Preset preset, const juce::String& name, std::string& newIdOut, juce::String& error)
{
    auto n = name;
    if (! checkName (n, error)) return false;

    const std::string base = "user-" + std::to_string (juce::Time::currentTimeMillis());
    std::string id = base;
    for (int k = 2; find (id) != nullptr || userDir.getChildFile (id + ".json").exists(); ++k)
        id = base + "-" + std::to_string (k);

    preset.id = id;
    preset.name = n;
    if (! storeUser (presets, userDir, preset, error)) return false;
    newIdOut = id;
    return true;
}

bool PresetLibrary::overwrite (const Preset& preset, juce::String& error)
{
    const auto* existing = find (preset.id);
    if (existing == nullptr || existing->builtin)
    {
        error = existing == nullptr ? ja ("プリセットが見つかりません") : ja ("内蔵プリセットは変更できません。複製してから編集してください");
        return false;
    }
    return storeUser (presets, userDir, preset, error);
}

bool PresetLibrary::duplicate (const std::string& id, std::string& newIdOut, juce::String& error)
{
    const auto* src = find (id);
    if (src == nullptr)
    {
        error = ja ("プリセットが見つかりません");
        return false;
    }
    const juce::String suffix = ja (" のコピー");
    return saveNew (*src, src->name.substring (0, kPresetNameMaxChars - suffix.length()) + suffix, newIdOut, error);
}

bool PresetLibrary::rename (const std::string& id, const juce::String& newName, juce::String& error)
{
    const auto* p = find (id);
    if (p == nullptr)
    {
        error = ja ("プリセットが見つかりません");
        return false;
    }
    auto n = newName;
    if (! checkName (n, error)) return false;
    auto copy = *p;
    copy.name = n;
    return overwrite (copy, error);
}

bool PresetLibrary::remove (const std::string& id, juce::String& error)
{
    const auto* p = find (id);
    if (p == nullptr || p->builtin)
    {
        error = p == nullptr ? ja ("プリセットが見つかりません") : ja ("内蔵プリセットは削除できません");
        return false;
    }
    if (! userDir.getChildFile (id + ".json").deleteFile())
    {
        error = ja ("プリセットを削除できません");
        return false;
    }
    presets.erase (presets.begin() + (p - presets.data()));
    return true;
}

bool PresetLibrary::importFile (const juce::File& file, std::string& newIdOut, PresetLoadReport& report)
{
    report = {};
    if (! file.existsAsFile())
    {
        report.rejected = true;
        report.rejectReason = ja ("ファイルが見つかりません");
        return false;
    }
    if (file.getSize() > kPresetMaxBytes)
    {
        report.rejected = true;
        report.rejectReason = ja ("64 KB を超えています");
        return false;
    }
    auto p = parsePreset (file.loadFileAsString(), report);
    if (! p) return false;
    juce::String error;
    if (! saveNew (*p, p->name, newIdOut, error))
    {
        report.rejected = true;
        report.rejectReason = error;
        return false;
    }
    return true;
}

bool PresetLibrary::exportFile (const std::string& id, const juce::File& destination, juce::String& error) const
{
    const auto* p = find (id);
    if (p == nullptr)
    {
        error = ja ("プリセットが見つかりません");
        return false;
    }
    if (! destination.replaceWithText (serializePreset (*p), false, false, "\n"))
    {
        error = ja ("ファイルに書き込めません");
        return false;
    }
    return true;
}
} // namespace koe

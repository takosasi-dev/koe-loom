#include "Model/Preset.h"

#include "Core/Constants.h"
#include "Effects/EffectRegistry.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdlib>

namespace koe
{
namespace
{
/** Source literals are UTF-8 (/utf-8); juce::String (const char*) would treat them as Latin-1. */
juce::String ja (const char* s) { return juce::String::fromUTF8 (s); }

const char* const kKeyNames[] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };

std::optional<double> number (const juce::var& v)
{
    if (v.isInt() || v.isInt64() || v.isDouble()) return double (v);
    return std::nullopt;
}

/** Reads a numeric key, clamps it with clampFn; counts a change (or a wrong type) in `clamped`. */
template <typename ClampFn>
float readNumber (const juce::var& obj, const char* key, float def, ClampFn clampFn, int& clamped)
{
    const auto& v = obj[key];
    if (v.isVoid()) return def;
    const auto n = number (v);
    if (! n || ! std::isfinite (*n))
    {
        ++clamped;
        return def;
    }
    const float c = clampFn (float (*n));
    if (c != float (*n)) ++clamped;
    return c;
}

float readRange (const juce::var& obj, const char* key, const Range& r, int& clamped)
{
    return readNumber (obj, key, r.def, [&r] (float x) { return r.clamp (x); }, clamped);
}

/** Choice given as its string id; unknown id or wrong type -> default, counted. */
int readChoice (const juce::var& obj, const char* key, const char* const* ids, int count, int def, int& clamped)
{
    const auto& v = obj[key];
    if (v.isVoid()) return def;
    if (v.isString())
        for (int i = 0; i < count; ++i)
            if (v.toString() == ids[i]) return i;
    ++clamped;
    return def;
}

/** Shortest decimal that round-trips the float, so files read "0.15" rather than "0.150000006". */
juce::var toVar (float f)
{
    if (! std::isfinite (f)) return {};
    if (f == std::round (f) && std::abs (f) < 1.0e9f) return int (f);
    char buf[32];
    auto [end, ec] = std::to_chars (buf, buf + sizeof (buf), f);
    *end = 0;
    return std::strtod (buf, nullptr);
}

/** A file name inside paths::irDir() (INTERFACES.md §9.3): no folders, no "..", nothing Windows refuses. */
bool isValidIrFileName (const juce::String& name)
{
    if (name.isEmpty() || name.getNumBytesAsUTF8() > 255 || name.contains ("..") || name.containsAnyOf ("/\\<>:\"|?*")
        || name.trim() != name || name.endsWithChar ('.'))
        return false;
    for (auto p = name.getCharPointer(); ! p.isEmpty(); ++p)
        if (*p < 0x20) return false;
    return true;
}

bool isHeavy (const SlotDef& s)
{
    auto* info = findEffectInfo (s.type);
    return info != nullptr && info->weight == EffectWeight::heavy;
}

std::optional<SlotDef> parseSlot (const juce::var& v, int& clamped)
{
    if (! v.isObject() || ! v["type"].isString()) return std::nullopt;
    const auto type = v["type"].toString().toStdString();
    auto slot = makeDefaultSlot (type);
    if (! slot) return std::nullopt;

    const auto* info = findEffectInfo (type);
    if (const auto& e = v["enabled"]; ! e.isVoid())
    {
        if (e.isBool()) slot->enabled = bool (e);
        else ++clamped;
    }

    if (type == "convolution") // the only type with a file (INTERFACES.md §9.3); other types ignore the key
        if (const auto& f = v["file"]; ! f.isVoid())
        {
            if (f.isString() && isValidIrFileName (f.toString())) slot->file = f.toString().toStdString();
            else if (! (f.isString() && f.toString().isEmpty())) ++clamped; // folders, "..", wrong type: dropped
        }

    if (const auto& w = v["wet"]; ! w.isVoid())
    {
        const bool number = w.isInt() || w.isInt64() || w.isDouble();
        const float x = number ? float (double (w)) : 1.0f;
        slot->wet = std::isfinite (x) ? std::clamp (x, 0.0f, 1.0f) : 1.0f;
        if (! number || slot->wet != x) ++clamped;
    }

    const auto& params = v["params"];
    if (! params.isObject()) return slot;
    for (size_t i = 0; i < info->params.size(); ++i)
    {
        const auto& spec = info->params[i];
        if (spec.isChoice())
        {
            std::vector<const char*> ids;
            for (auto& c : spec.choices) ids.push_back (c.first);
            slot->params[i] = float (readChoice (params, spec.id, ids.data(), int (ids.size()), int (spec.def), clamped));
        }
        else
        {
            slot->params[i] = readNumber (params, spec.id, spec.def, [&spec] (float x) { return spec.clamp (x); }, clamped);
        }
    }
    return slot;
}

/** nullopt = unknown mode (E-30). */
std::optional<LayerDef> parseLayer (const juce::var& v, int& clamped)
{
    if (! v.isObject()) return std::nullopt;
    LayerDef l;
    if (const auto& m = v["mode"]; ! m.isVoid())
    {
        if (m.toString() == "scale" && m.isString()) l.mode = LayerDef::Mode::scale;
        else if (! (m.isString() && m.toString() == "fixed")) return std::nullopt;
    }
    l.pitchSt = readRange (v, "pitchSt", kPitchSt, clamped);
    l.degree = int (readNumber (v, "degree", kLayerDegree.def, [] (float x) {
        const float d = std::round (kLayerDegree.clamp (x));
        return d == 0.0f ? kLayerDegree.def : d;
    }, clamped));
    l.key = readChoice (v, "key", kKeyNames, 12, 0, clamped);
    const char* const scales[] = { "major", "minor" };
    l.minor = readChoice (v, "scale", scales, 2, 0, clamped) == 1;
    l.formantSt = readRange (v, "formantSt", kFormantSt, clamped);
    l.levelDb = readRange (v, "levelDb", kLayerLevelDb, clamped);
    if (const auto& e = v["enabled"]; ! e.isVoid())
    {
        if (e.isBool()) l.enabled = bool (e);
        else ++clamped;
    }
    return l;
}

std::optional<Preset> reject (PresetLoadReport& report, const juce::String& reason)
{
    report.rejected = true;
    report.rejectReason = reason;
    return std::nullopt;
}
} // namespace

juce::String Preset::category() const
{
    for (auto* c : { "natural", "character", "device", "space", "layered" })
        if (id.rfind (std::string (c) + "-", 0) == 0) return c;
    return "user";
}

bool isValidPresetId (const std::string& id)
{
    if (id.empty() || id.front() == '-' || id.back() == '-') return false;
    for (size_t i = 0; i < id.size(); ++i)
    {
        const char c = id[i];
        if (c == '-' ? id[i - 1] == '-' : ! ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))) return false;
    }
    return true;
}

std::optional<SlotDef> makeDefaultSlot (const std::string& type)
{
    auto* info = findEffectInfo (type);
    if (info == nullptr) return std::nullopt;
    SlotDef s;
    s.type = type;
    for (auto& p : info->params) s.params.push_back (p.def);
    return s;
}

bool PresetLoadReport::hasNotices() const
{
    return rejected || unknownTypesSkipped || slotsOverLimitDropped || duplicateFreezeLooperDropped || heavyTurnedOff
           || layersDropped || unknownLayerModesSkipped || valuesClamped;
}

juce::String PresetLoadReport::toJapanese() const
{
    juce::StringArray lines;
    auto add = [&lines] (int n, const juce::String& text) { if (n > 0) lines.add (text.replace ("%n", juce::String (n))); };
    if (rejected) lines.add (rejectReason);
    add (unknownTypesSkipped, ja ("この版にないエフェクト %n 個を読み飛ばしました"));
    add (slotsOverLimitDropped, ja ("スロットは 10 個までのため、%n 個を捨てました"));
    add (duplicateFreezeLooperDropped, ja ("フリーズとルーパーは各 1 個までのため、%n 個を読み飛ばしました"));
    add (heavyTurnedOff, ja ("重いエフェクトは ON 2 個までのため、%n 個を OFF にしました"));
    add (layersDropped, ja ("重ねる声 %n 件を捨てました（2 件まで。変換 OFF のときは使えません）"));
    add (unknownLayerModesSkipped, ja ("この版で使えない重ねる声 %n 件を読み飛ばしました"));
    add (valuesClamped, ja ("範囲外または不正な値 %n 個を直しました"));
    return lines.joinIntoString ("\n");
}

std::optional<Preset> parsePreset (const juce::String& jsonText, PresetLoadReport& report)
{
    report = {};
    if (jsonText.getNumBytesAsUTF8() > size_t (kPresetMaxBytes)) return reject (report, ja ("64 KB を超えています"));

    juce::var root;
    if (juce::JSON::parse (jsonText, root).failed() || ! root.isObject()) return reject (report, ja ("JSON として読めません"));

    const auto version = number (root["schemaVersion"]);
    if (! version || *version < 1) return reject (report, ja ("schemaVersion がありません"));
    if (*version > kPresetSchemaVersion) return reject (report, ja ("新しい版のプリセットです"));

    Preset p;
    p.id = root["id"].isString() ? root["id"].toString().toStdString() : std::string();
    if (! isValidPresetId (p.id)) return reject (report, ja ("ID がない、または形式が正しくありません"));
    if (! root["name"].isString() || root["name"].toString().isEmpty()) return reject (report, ja ("名前がありません"));
    p.name = root["name"].toString();
    if (p.name.length() > kPresetNameMaxChars)
    {
        p.name = p.name.substring (0, kPresetNameMaxChars);
        ++report.valuesClamped;
    }

    int& clamped = report.valuesClamped;
    if (const auto& sh = root["shifter"]; sh.isObject())
    {
        p.hasShifter = true;
        p.pitchSt = readRange (sh, "pitchSt", kPitchSt, clamped);
        p.formantSt = readRange (sh, "formantSt", kFormantSt, clamped);
    }

    // chain: E-22 -> E-21 -> E-24 -> E-23 (spec §7, last paragraph)
    if (auto* arr = root["chain"].getArray())
    {
        for (auto& v : *arr)
        {
            if (auto slot = parseSlot (v, clamped)) p.chain.push_back (std::move (*slot));
            else ++report.unknownTypesSkipped;
        }
        if (int (p.chain.size()) > kMaxSlots)
        {
            report.slotsOverLimitDropped = int (p.chain.size()) - kMaxSlots;
            p.chain.resize (size_t (kMaxSlots));
        }
        bool seenFreeze = false, seenLooper = false;
        std::vector<SlotDef> kept;
        for (auto& s : p.chain)
        {
            bool* seen = s.type == "freeze" ? &seenFreeze : (s.type == "looper" ? &seenLooper : nullptr);
            if (seen != nullptr && *seen)
            {
                ++report.duplicateFreezeLooperDropped;
                continue;
            }
            if (seen != nullptr) *seen = true;
            kept.push_back (std::move (s));
        }
        p.chain = std::move (kept);
        int heavyOn = 0;
        for (auto& s : p.chain)
            if (s.enabled && isHeavy (s) && ++heavyOn > kMaxHeavyOn)
            {
                s.enabled = false;
                ++report.heavyTurnedOff;
            }
    }

    // layers (E-30): no shifter -> drop all; unknown modes skipped; then the first 2 are kept
    if (auto* arr = root["layers"].getArray())
    {
        if (! p.hasShifter)
            report.layersDropped = arr->size();
        else
            for (auto& v : *arr)
            {
                auto l = parseLayer (v, clamped);
                if (! l) ++report.unknownLayerModesSkipped;
                else if (int (p.layers.size()) >= kMaxLayers) ++report.layersDropped;
                else p.layers.push_back (*l);
            }
    }

    p.outputTrimDb = readRange (root, "outputTrimDb", kTrimDb, clamped);
    return p;
}

juce::String serializePreset (const Preset& preset)
{
    auto* o = new juce::DynamicObject();
    juce::var root (o);
    o->setProperty ("schemaVersion", kPresetSchemaVersion);
    o->setProperty ("id", juce::String (preset.id));
    o->setProperty ("name", preset.name);

    if (preset.hasShifter)
    {
        auto* sh = new juce::DynamicObject();
        sh->setProperty ("pitchSt", toVar (preset.pitchSt));
        sh->setProperty ("formantSt", toVar (preset.formantSt));
        o->setProperty ("shifter", juce::var (sh));

        juce::Array<juce::var> layers;
        for (auto& l : preset.layers)
        {
            // every field is written whatever the mode, so save -> load is exact (AC-09)
            auto* lo = new juce::DynamicObject();
            lo->setProperty ("mode", l.mode == LayerDef::Mode::scale ? "scale" : "fixed");
            lo->setProperty ("pitchSt", toVar (l.pitchSt));
            lo->setProperty ("degree", l.degree);
            lo->setProperty ("key", kKeyNames[juce::jlimit (0, 11, l.key)]);
            lo->setProperty ("scale", l.minor ? "minor" : "major");
            lo->setProperty ("formantSt", toVar (l.formantSt));
            lo->setProperty ("levelDb", toVar (l.levelDb));
            lo->setProperty ("enabled", l.enabled);
            layers.add (juce::var (lo));
        }
        if (! layers.isEmpty()) o->setProperty ("layers", layers);
    }

    juce::Array<juce::var> chain;
    for (auto& s : preset.chain)
    {
        auto* info = findEffectInfo (s.type);
        if (info == nullptr) continue;
        auto* so = new juce::DynamicObject();
        so->setProperty ("type", juce::String (s.type));
        so->setProperty ("enabled", s.enabled);
        auto* po = new juce::DynamicObject();
        for (size_t i = 0; i < info->params.size(); ++i)
        {
            const auto& spec = info->params[i];
            const float v = i < s.params.size() && std::isfinite (s.params[i]) ? s.params[i] : spec.def;
            if (spec.isChoice())
                po->setProperty (spec.id, spec.choices[size_t (spec.clamp (v))].first);
            else
                po->setProperty (spec.id, toVar (v));
        }
        so->setProperty ("params", juce::var (po));
        if (s.type == "convolution" && ! s.file.empty()) so->setProperty ("file", juce::String::fromUTF8 (s.file.c_str()));
        if (s.wet < 1.0f) so->setProperty ("wet", s.wet);
        chain.add (juce::var (so));
    }
    o->setProperty ("chain", chain);
    o->setProperty ("outputTrimDb", toVar (preset.outputTrimDb));
    return juce::JSON::toString (root);
}
} // namespace koe

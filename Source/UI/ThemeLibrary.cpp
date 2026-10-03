#include "UI/ThemeLibrary.h"

#include "Core/Paths.h"
#include "UI/Widgets.h"

#include <algorithm>

namespace koe::ui
{
namespace
{
constexpr juce::int64 kMaxFileBytes = 64 * 1024;

/** A file name we made (or could have made): no folders, no characters Windows refuses. */
bool legalFile (const juce::String& file) { return file.isNotEmpty() && juce::File::createLegalFileName (file) == file; }
} // namespace

juce::String ThemeLibrary::userFile (const juce::String& themeId)
{
    if (! themeId.startsWith ("user:")) return {};
    const auto file = themeId.fromFirstOccurrenceOf ("user:", false, false);
    return legalFile (file) ? file : juce::String();
}

juce::File ThemeLibrary::fileFor (const juce::String& file) { return paths::themesDir().getChildFile (file + ".json"); }

juce::String ThemeLibrary::checkName (const juce::String& name, juce::String& error)
{
    const auto n = name.trim();
    if (n.isEmpty()) error = ja ("名前を入れてください。");
    else if (n.length() > maxNameLength) error = ja ("名前は ") + juce::String (maxNameLength) + ja (" 文字までです。");
    else return n;
    return {};
}

std::optional<ThemeData> ThemeLibrary::parse (const juce::String& json, juce::String& error)
{
    juce::var root;
    if (juce::JSON::parse (json, root).failed() || ! root.isObject())
    {
        error = ja ("JSON の形が正しくありません。");
        return std::nullopt;
    }
    auto* colours = root["colours"].getDynamicObject();
    if (colours == nullptr)
    {
        error = ja ("色の一覧（colours）がありません。");
        return std::nullopt;
    }
    std::array<std::optional<juce::Colour>, Theme::numRoles> given;
    for (int r = 0; r < Theme::numRoles; ++r)
    {
        const auto v = colours->getProperty (Theme::roleKey (r));
        if (v.isVoid()) continue; // derived below
        const auto col = v.isString() ? Theme::parseHex (v.toString()) : std::nullopt;
        if (! col)
        {
            error = ja ("「") + Theme::roleName (r) + ja ("」（") + Theme::roleKey (r) + ja ("）の色 ") + juce::JSON::toString (v, true)
                    + ja (" を読めません。\"#RRGGBB\" の形で書いてください。");
            return std::nullopt;
        }
        given[size_t (r)] = col;
    }
    ThemeData t;
    t.name = root["name"].toString().trim().substring (0, maxNameLength);
    const auto& dark = root["dark"];
    t.dark = dark.isBool() ? bool (dark) : ! given[0] || given[0]->getPerceivedBrightness() < 0.5f; // no flag: judge by the background
    t.colours = Theme::complete (given, t.dark);
    return t;
}

juce::String ThemeLibrary::toJson (const ThemeData& t)
{
    auto* colours = new juce::DynamicObject();
    for (int r = 0; r < Theme::numRoles; ++r) colours->setProperty (Theme::roleKey (r), Theme::toHex (Theme::role (t.colours, r)));
    auto* o = new juce::DynamicObject();
    o->setProperty ("name", t.name);
    o->setProperty ("dark", t.dark);
    o->setProperty ("colours", juce::var (colours));
    return juce::JSON::toString (juce::var (o));
}

std::optional<ThemeData> ThemeLibrary::load (const juce::File& f, juce::String& error)
{
    const auto fail = [&] (const juce::String& why)
    {
        error = ja ("「") + f.getFileName() + ja ("」を読めません: ") + why;
        return std::nullopt;
    };
    if (! f.existsAsFile()) return fail (ja ("ファイルが見つかりません。"));
    if (f.getSize() > kMaxFileBytes) return fail (ja ("テーマのファイルにしては大きすぎます。"));
    juce::String why;
    auto t = parse (f.loadFileAsString(), why);
    if (! t) return fail (why);
    if (t->name.isEmpty()) t->name = f.getFileNameWithoutExtension().substring (0, maxNameLength);
    return t;
}

std::optional<ThemeData> ThemeLibrary::resolve (const juce::String& themeId, juce::String& error)
{
    if (themeId == paperId) return ThemeData { "Paper", false, Theme::paper() };
    if (themeId == monoId) return ThemeData { "Mono", true, Theme::mono() };
    const auto file = userFile (themeId);
    if (file.isEmpty())
    {
        error = ja ("知らない配色です（") + themeId + ja ("）。");
        return std::nullopt;
    }
    return load (fileFor (file), error);
}

std::vector<ThemeLibrary::Entry> ThemeLibrary::list()
{
    std::vector<Entry> out;
    for (const auto& f : paths::themesDir().findChildFiles (juce::File::findFiles, false, "*.json"))
    {
        juce::String why;
        if (const auto t = load (f, why); t && legalFile (f.getFileNameWithoutExtension())) out.push_back ({ f.getFileNameWithoutExtension(), t->name });
    }
    std::sort (out.begin(), out.end(), [] (const Entry& a, const Entry& b)
    {
        const int byName = a.name.compareNatural (b.name);
        return byName != 0 ? byName < 0 : a.file < b.file;
    });
    return out;
}

bool ThemeLibrary::save (const juce::String& file, const ThemeData& t, juce::String& error)
{
    if (! legalFile (file))
    {
        error = ja ("テーマのファイル名が正しくありません。");
        return false;
    }
    paths::themesDir().createDirectory();
    if (fileFor (file).replaceWithText (toJson (t), false, false, "\n")) return true;
    error = ja ("テーマを保存できませんでした（") + fileFor (file).getFullPathName() + ja ("）。");
    return false;
}

juce::String ThemeLibrary::saveNew (const ThemeData& t, juce::String& error)
{
    auto data = t;
    data.name = checkName (t.name, error);
    if (data.name.isEmpty()) return {};
    auto base = juce::File::createLegalFileName (data.name).trim();
    if (base.isEmpty() || base.containsOnly (".")) base = "theme";
    auto file = base;
    for (int n = 2; fileFor (file).exists(); ++n) file = base + " (" + juce::String (n) + ")";
    return save (file, data, error) ? file : juce::String();
}

bool ThemeLibrary::rename (const juce::String& file, const juce::String& newName, juce::String& error)
{
    auto t = load (fileFor (file), error);
    if (! t) return false;
    const auto name = checkName (newName, error);
    if (name.isEmpty()) return false;
    t->name = name;
    return save (file, *t, error);
}

juce::String ThemeLibrary::duplicate (const juce::String& file, juce::String& error)
{
    auto t = load (fileFor (file), error);
    if (! t) return {};
    const auto suffix = ja (" のコピー");
    t->name = t->name.substring (0, maxNameLength - suffix.length()) + suffix;
    return saveNew (*t, error);
}

bool ThemeLibrary::remove (const juce::String& file, juce::String& error)
{
    if (legalFile (file) && fileFor (file).deleteFile()) return true;
    error = ja ("テーマを削除できませんでした。");
    return false;
}

juce::String ThemeLibrary::importFile (const juce::File& source, juce::String& error)
{
    const auto t = load (source, error);
    return t ? saveNew (*t, error) : juce::String();
}

bool ThemeLibrary::exportFile (const juce::String& file, const juce::File& dest, juce::String& error)
{
    const auto t = load (fileFor (file), error);
    if (! t) return false;
    if (dest.replaceWithText (toJson (*t), false, false, "\n")) return true;
    error = ja ("書き出せませんでした（") + dest.getFullPathName() + ja ("）。");
    return false;
}
} // namespace koe::ui

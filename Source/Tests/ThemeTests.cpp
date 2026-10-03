// Theme checks (category "UiScreens", INTERFACES.md §8.4): the built-in Paper / Mono palettes keep the contrast
// rules, theme files round-trip, broken files are refused with a reason, missing roles are derived, and
// MainComponent applies Settings::themeId (unknown -> Studio with a toast). No window, no audio.

#include "App/AppController.h"
#include "Core/Paths.h"
#include "UI/MainComponent.h"
#include "UI/ThemeLibrary.h"
#include "UI/main/Shell.h"

namespace koe::ui
{
namespace
{
void freshThemeDataDir()
{
    auto d = paths::dataDir();
    if (d.getFullPathName().contains ("KoeLoomTests")) d.deleteRecursively();
    d.createDirectory();
}

bool samePalette (const Palette& a, const Palette& b)
{
    for (int r = 0; r < Theme::numRoles; ++r)
        if (Theme::role (a, r) != Theme::role (b, r)) return false;
    return true;
}

/** Puts the Theme globals back to the Studio defaults the other tests expect. */
void resetTheme()
{
    Theme::clearPalette();
    Theme::setDark (true);
    Theme::setVariant (0, 0);
}

juce::String toastText (MainComponent& mc)
{
    for (auto* ch : mc.getChildren())
        if (auto* t = dynamic_cast<mainui::ToastView*> (ch)) return t->currentText();
    return {};
}
} // namespace

class ThemeTests : public juce::UnitTest
{
public:
    ThemeTests() : juce::UnitTest ("Theme", "UiScreens") {}

    void runTest() override
    {
        beginTest ("Paper and Mono: every contrast rule (AC-53) holds");
        {
            const std::pair<const char*, const Palette*> palettes[] = { { "Paper", &Theme::paper() }, { "Mono", &Theme::mono() } };
            for (auto& [label, p] : palettes)
            {
                double worst = 100.0;
                juce::String worstWhat;
                for (auto& k : Theme::contrastChecks (*p))
                {
                    const auto what = juce::String (label) + ": " + Theme::roleKey (k.fg) + " on " + Theme::roleKey (k.bg) + " " + juce::String (k.ratio, 2);
                    expect (k.ratio >= k.min, what);
                    if (k.ratio / k.min < worst) { worst = k.ratio / k.min; worstWhat = what; }
                }
                logMessage ("contrast " + juce::String (label) + ": worst " + worstWhat);
            }
            expectEquals (int (Theme::contrastChecks (Theme::paper()).size()), 23);
            expect (Theme::mono().onAccent.getPerceivedBrightness() < 0.2f, "Mono: dark ink on the white accent");
            expect (contrastRatio (Theme::mono().accent, Theme::mono().bg) >= 3.0, "Mono: focus ring (accent) visible on the background");
        }

        beginTest ("Hex colours and missing roles derived from bg / text / accent");
        {
            expect (Theme::parseHex ("#0E1217") == Theme::dark().bg);
            expect (Theme::parseHex ("0e1217") == Theme::dark().bg, "no # and lower case");
            expect (Theme::parseHex ("#b3000000") == Theme::dark().overlay, "alpha");
            for (auto bad : { "", "#12345", "#GGGGGG", "red", "#1234567" }) expect (! Theme::parseHex (bad).has_value(), bad);
            expectEquals (Theme::toHex (Theme::dark().bg), juce::String ("#0E1217"));
            expectEquals (Theme::toHex (Theme::dark().overlay), juce::String ("#B3000000"));
            for (int r = 0; r < Theme::numRoles; ++r) expect (Theme::parseHex (Theme::toHex (Theme::role (Theme::mono(), r))) == Theme::role (Theme::mono(), r));

            std::array<std::optional<juce::Colour>, Theme::numRoles> none;
            expect (samePalette (Theme::complete (none, true), Theme::dark()), "nothing given = the dark mock");
            expect (samePalette (Theme::complete (none, false), Theme::light()), "nothing given = the light mock");

            std::array<std::optional<juce::Colour>, Theme::numRoles> base;
            base[0] = Theme::parseHex ("#101820"); // bg
            base[5] = Theme::parseHex ("#F0F0E0"); // text
            base[7] = Theme::parseHex ("#FFCC00"); // accent
            const auto p = Theme::complete (base, true);
            expect (p.bg == *base[0] && p.text == *base[5] && p.accent == *base[7], "given roles kept");
            for (auto derived : { p.surface, p.raised, p.border, p.divider, p.textSub, p.trackOff })
                expect (derived != p.bg && derived != p.text, "derived between bg and text");
            expect (p.surface.getPerceivedBrightness() > p.bg.getPerceivedBrightness() && p.raised.getPerceivedBrightness() > p.surface.getPerceivedBrightness(),
                    "dark: surfaces step towards the text");
            expectGreaterOrEqual (contrastRatio (p.onAccent, p.accent), 4.5, "ink on the accent picked for contrast");
            expectGreaterOrEqual (contrastRatio (p.border, p.bg), 3.0, "derived border");
            expectGreaterOrEqual (contrastRatio (p.textSub, p.bg), 4.5, "derived sub text");

            std::array<std::optional<juce::Colour>, Theme::numRoles> lightBg;
            lightBg[0] = Theme::parseHex ("#EEF2F6");
            const auto l = Theme::complete (lightBg, false);
            expect (l.raised == Theme::light().raised && l.text == Theme::light().text, "light: white raised, the mock's ink");
        }

        beginTest ("Theme files: round trip, list, rename, duplicate, delete, export / import");
        {
            freshThemeDataDir();
            ThemeData t;
            t.name = juce::String::fromUTF8 ("夕焼け");
            t.dark = false;
            t.colours = Theme::paper();
            t.colours.accent = *Theme::parseHex ("#A03A2A");
            t.colours.overlay = *Theme::parseHex ("#66000000");
            juce::String err;
            const auto back = ThemeLibrary::parse (ThemeLibrary::toJson (t), err);
            expect (back.has_value(), err);
            expect (back->name == t.name && back->dark == t.dark && samePalette (back->colours, t.colours), "toJson -> parse keeps every role");

            const auto file = ThemeLibrary::saveNew (t, err);
            expect (file.isNotEmpty(), err);
            expect (ThemeLibrary::fileFor (file).existsAsFile());
            const auto json = juce::JSON::parse (ThemeLibrary::fileFor (file));
            expectEquals (json["name"].toString(), t.name);
            expectEquals (json["colours"]["accent"].toString(), juce::String ("#A03A2A"));
            expect (ThemeLibrary::saveNew (t, err) != file, "same name: a second file");
            auto list = ThemeLibrary::list();
            expectEquals (int (list.size()), 2);
            const auto loaded = ThemeLibrary::resolve (ThemeLibrary::userId (file), err);
            expect (loaded.has_value() && samePalette (loaded->colours, t.colours), "resolve user:<file>");

            expect (ThemeLibrary::rename (file, juce::String::fromUTF8 ("  朝焼け "), err), err);
            expectEquals (ThemeLibrary::resolve (ThemeLibrary::userId (file), err)->name, juce::String::fromUTF8 ("朝焼け"));
            expect (! ThemeLibrary::rename (file, "   ", err) && err.isNotEmpty(), "empty name refused");
            expect (! ThemeLibrary::rename (file, juce::String::repeatedString ("a", 33), err), "33 characters refused");

            const auto copy = ThemeLibrary::duplicate (file, err);
            expect (copy.isNotEmpty(), err);
            expect (ThemeLibrary::resolve (ThemeLibrary::userId (copy), err)->name.endsWith (juce::String::fromUTF8 ("のコピー")));
            expectEquals (int (ThemeLibrary::list().size()), 3);

            const auto out = paths::dataDir().getChildFile ("exported.json");
            expect (ThemeLibrary::exportFile (file, out, err), err);
            expect (ThemeLibrary::remove (file, err), err);
            expect (! ThemeLibrary::fileFor (file).exists());
            expect (! ThemeLibrary::resolve (ThemeLibrary::userId (file), err).has_value() && err.isNotEmpty(), "deleted theme no longer resolves");
            const auto imported = ThemeLibrary::importFile (out, err);
            expect (imported.isNotEmpty(), err);
            expect (samePalette (ThemeLibrary::resolve (ThemeLibrary::userId (imported), err)->colours, t.colours), "export -> import keeps the colours");

            expect (! ThemeLibrary::resolve ("user:../settings", err).has_value(), "no paths outside the themes folder");
            expect (! ThemeLibrary::resolve ("builtin:nope", err).has_value() && err.isNotEmpty());
            expect (samePalette (ThemeLibrary::resolve (ThemeLibrary::paperId, err)->colours, Theme::paper()) && ! ThemeLibrary::resolve (ThemeLibrary::paperId, err)->dark);
            expect (samePalette (ThemeLibrary::resolve (ThemeLibrary::monoId, err)->colours, Theme::mono()) && ThemeLibrary::resolve (ThemeLibrary::monoId, err)->dark);
        }

        beginTest ("Theme files: broken ones are left out of the list and give a Japanese reason; missing roles are filled");
        {
            freshThemeDataDir();
            auto dir = paths::themesDir();
            dir.createDirectory();
            const std::pair<const char*, const char*> broken[] = {
                { "not-json", "{ \"name\": \"x\", " },
                { "no-colours", "{ \"name\": \"x\", \"dark\": true }" },
                { "bad-hex", "{ \"name\": \"x\", \"colours\": { \"bg\": \"#12\" } }" },
                { "wrong-type", "{ \"name\": \"x\", \"colours\": { \"accent\": 12 } }" },
                { "array", "[1, 2, 3]" },
            };
            for (auto& [stem, text] : broken)
            {
                const auto f = dir.getChildFile (juce::String (stem) + ".json");
                expect (f.replaceWithText (text));
                juce::String err;
                expect (! ThemeLibrary::load (f, err).has_value(), stem);
                expect (err.contains (juce::String::fromUTF8 ("読めません")) && err.contains (f.getFileName()), juce::String (stem) + ": " + err);
            }
            juce::String err;
            ThemeLibrary::load (dir.getChildFile ("bad-hex.json"), err);
            expect (err.contains ("bg") && err.contains ("#RRGGBB"), "names the role and the expected form: " + err);

            expect (dir.getChildFile ("partial.json").replaceWithText ("{ \"colours\": { \"bg\": \"#F7F3EA\", \"accent\": \"#2A4DA0\" } }"));
            const auto list = ThemeLibrary::list();
            expectEquals (int (list.size()), 1, "only the readable file is listed");
            expectEquals (list[0].file, juce::String ("partial"));
            expectEquals (list[0].name, juce::String ("partial"), "no name: the file name");
            const auto t = ThemeLibrary::resolve ("user:partial", err);
            expect (t.has_value(), err);
            expect (! t->dark, "no dark flag: judged from the light background");
            expect (t->colours.bg == *Theme::parseHex ("#F7F3EA") && t->colours.accent == *Theme::parseHex ("#2A4DA0"));
            expect (t->colours.text == Theme::light().text && t->colours.raised == Theme::light().raised, "missing roles from the light base");
            expect (! ThemeLibrary::resolve ("user:bad-hex", err).has_value());
        }

        beginTest ("MainComponent applies Settings::themeId; unknown or broken -> Studio with a toast");
        {
            freshThemeDataDir();
            resetTheme();
            AppController c (false);
            c.startup();
            c.updateSettings ([] (Settings& s) { s.setupDone = true; s.tourStep = 7; });
            c.dispatchPendingMessages();
            {
                c.updateSettings ([] (Settings& s) { s.themeId = ThemeLibrary::paperId; });
                MainComponent mc (c);
                expect (samePalette (Theme::colours(), Theme::paper()) && ! Theme::isDark() && Theme::themeId() == ThemeLibrary::paperId, "Paper on start");
                c.updateSettings ([] (Settings& s) { s.themeId = ThemeLibrary::monoId; });
                c.dispatchPendingMessages();
                expect (samePalette (Theme::colours(), Theme::mono()) && Theme::isDark(), "switched to Mono");
                c.updateSettings ([] (Settings& s) { s.darkTheme = false; s.accentColour = 2; });
                c.dispatchPendingMessages();
                expect (samePalette (Theme::colours(), Theme::mono()), "the Studio rows do not change another 配色");
                c.updateSettings ([] (Settings& s) { s.themeId = {}; });
                c.dispatchPendingMessages();
                expect (samePalette (Theme::colours(), Theme::make (false, 2, 0)) && Theme::themeId().isEmpty(), "back to Studio (light, violet)");
                c.updateSettings ([] (Settings& s) { s.darkTheme = true; s.accentColour = 0; });
                c.dispatchPendingMessages();
                expect (samePalette (Theme::colours(), Theme::dark()));

                ThemeData t;
                t.name = "Mine";
                t.dark = true;
                t.colours = Theme::dark();
                t.colours.accent = *Theme::parseHex ("#FF88AA");
                juce::String err;
                const auto file = ThemeLibrary::saveNew (t, err);
                c.updateSettings ([file] (Settings& s) { s.themeId = ThemeLibrary::userId (file); });
                c.dispatchPendingMessages();
                expect (Theme::colours().accent == t.colours.accent, "user theme applied");
                t.colours.accent = *Theme::parseHex ("#88FFAA");
                ThemeLibrary::save (file, t, err);
                Theme::invalidate();
                c.updateSettings ([] (Settings&) {});
                c.dispatchPendingMessages();
                expect (Theme::colours().accent == t.colours.accent, "the saved file again after invalidate()");

                expect (ThemeLibrary::fileFor (file).replaceWithText ("{ broken"));
                Theme::invalidate();
                c.updateSettings ([] (Settings&) {});
                c.dispatchPendingMessages();
                expect (samePalette (Theme::colours(), Theme::dark()) && c.getSettings().themeId.isEmpty(), "broken file -> Studio, setting cleared");
                expect (toastText (mc).contains ("Studio"), "toast: " + toastText (mc));
            }
            {
                c.updateSettings ([] (Settings& s) { s.themeId = "user:missing"; });
                MainComponent mc (c);
                expect (samePalette (Theme::colours(), Theme::dark()) && Theme::themeId().isEmpty(), "missing file on start -> Studio");
                expect (c.getSettings().themeId.isEmpty());
                expect (toastText (mc).contains (juce::String::fromUTF8 ("missing.json")), "the toast names the file: " + toastText (mc));
            }
            resetTheme();
        }
    }
};

static ThemeTests themeTests;
} // namespace koe::ui

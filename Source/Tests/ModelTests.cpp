#include "Effects/EffectRegistry.h"
#include "Model/Preset.h"
#include "Model/PresetLibrary.h"
#include "Model/Settings.h"

#include <juce_core/juce_core.h>

#include <map>
#include <set>

namespace koe
{
namespace
{
juce::File makeTempDir()
{
    auto d = juce::File::getSpecialLocation (juce::File::tempDirectory).getNonexistentChildFile ("KoeLoomModelTests", "", false);
    d.createDirectory();
    return d;
}

/** A slot whose every parameter is moved off its default (choices: last option). */
SlotDef unusualSlot (const EffectInfo& info, bool enabled)
{
    SlotDef s { info.type, enabled, {} };
    for (auto& p : info.params)
        s.params.push_back (p.isChoice() ? p.max : p.clamp (p.min + (p.max - p.min) * 0.37f));
    return s;
}

/** JSON text for a preset with the given chain / layers items (raw JSON fragments). */
juce::String presetJson (const juce::StringArray& chainItems, const juce::String& extra = {})
{
    return "{\"schemaVersion\":1,\"id\":\"user-test\",\"name\":\"t\"" + extra + ",\"chain\":[" + chainItems.joinIntoString (",") + "]}";
}

juce::String slotJson (const juce::String& type, const juce::String& params = {}, bool enabled = true)
{
    return "{\"type\":\"" + type + "\",\"enabled\":" + (enabled ? "true" : "false") + ",\"params\":{" + params + "}}";
}

int phaseOf (const Preset& p) // koeloom_presets.md R-P9
{
    for (auto& l : p.layers)
        if (l.mode == LayerDef::Mode::scale) return 5;
    for (auto& s : p.chain)
        if (findEffectInfo (s.type)->phase == 5) return 5;
    return p.chain.empty() && p.layers.empty() ? 1 : 2;
}
} // namespace

class ModelTests : public juce::UnitTest
{
public:
    ModelTests() : juce::UnitTest ("Presets and settings", "Model") {}

    void initialise() override { tmp = makeTempDir(); }
    void shutdown() override { tmp.deleteRecursively(); }

    void runTest() override
    {
        testIdsAndCategories();
        testRoundTrip();
        testLibraryCrud();
        testLoaderRules();
        testBuiltins();
        testSettings();
    }

private:
    juce::File tmp;

    std::optional<Preset> parse (const juce::String& text, PresetLoadReport& r)
    {
        auto p = parsePreset (text, r);
        expect (r.hasNotices() == r.toJapanese().isNotEmpty(), "toJapanese matches hasNotices");
        return p;
    }

    /** Fails (instead of crashing) when a parse result is missing or too small to index. */
    bool shaped (const std::optional<Preset>& p, size_t chain, size_t layers = 0)
    {
        const bool ok = p && p->chain.size() >= chain && p->layers.size() >= layers;
        expect (ok, "unexpected parse result shape");
        return ok;
    }

    void testIdsAndCategories()
    {
        beginTest ("preset id format and category (R-P4, F-05-6)");
        expect (isValidPresetId ("natural-asis"));
        expect (isValidPresetId ("user-1727900000000-2"));
        expect (isValidPresetId ("device-8bit"));
        for (auto* bad : { "", "-a", "a-", "a--b", "Natural", "a_b", "a b", "../x", "a.b" })
            expect (! isValidPresetId (bad), bad);
        Preset p;
        for (auto [id, cat] : std::map<std::string, juce::String> { { "natural-x", "natural" }, { "character-x", "character" },
                                                                    { "device-x", "device" }, { "space-x", "space" },
                                                                    { "layered-x", "layered" }, { "user-1", "user" } })
        {
            p.id = id;
            expectEquals (p.category(), cat);
        }
    }

    void testRoundTrip()
    {
        beginTest ("AC-09: every parameter survives save -> load (all 30 types, layers, trim)");
        const auto& infos = allEffectInfos();
        for (size_t start = 0; start < infos.size(); start += size_t (kMaxSlots))
        {
            Preset p;
            p.id = "user-roundtrip-" + std::to_string (start);
            p.name = juce::String::fromUTF8 ("往復テスト");
            p.hasShifter = true;
            p.pitchSt = 3.7f;
            p.formantSt = -2.2f;
            p.layers.push_back ({ LayerDef::Mode::fixed, -0.2f, 2, 0, false, 1.5f, -4.0f });
            p.layers.push_back ({ LayerDef::Mode::scale, 0.0f, -3, 9, true, 2.0f, -6.5f });
            p.outputTrimDb = -3.5f;
            for (size_t i = start; i < start + size_t (kMaxSlots) && i < infos.size(); ++i)
                // vocoder is the third heavy one in the last group: keep it off (E-23), plus one more off slot
                p.chain.push_back (unusualSlot (infos[i], infos[i].type != std::string ("vocoder") && i % 4 != 1));

            PresetLoadReport r;
            auto back = parse (serializePreset (p), r);
            expect (back.has_value());
            expect (! r.hasNotices(), r.toJapanese());
            expect (back && *back == p, "round trip differs for group " + juce::String (int (start)));
        }

        beginTest ("serialised text uses choice ids and short numbers");
        Preset q;
        q.id = "user-x";
        q.name = "x";
        q.chain.push_back (*makeDefaultSlot ("echo"));
        q.chain[0].params[0] = 1.0f; // tape
        q.chain[0].params[4] = 0.15f;
        const auto text = serializePreset (q);
        expect (text.contains ("\"tape\""), text);
        expect (text.contains ("0.15") && ! text.contains ("0.1500"), text);
        expect (! text.contains ("shifter"), "converter off writes no shifter");
    }

    void testLibraryCrud()
    {
        beginTest ("AC-09 / F-05-2: user preset CRUD, built-ins duplicate only, reload from disk");
        const auto dir = tmp.getChildFile ("presets");
        PresetLibrary lib (dir);
        expect (lib.reload().isEmpty());
        const int builtinCount = int (lib.all().size());
        if (lib.builtinErrors() != 0 || builtinCount != 81) return expect (false, "built-ins missing; CRUD test skipped");

        Preset p = *lib.find ("character-demon-king");
        p.pitchSt = -8.5f;
        std::string id;
        juce::String err;
        expect (lib.saveNew (p, juce::String::fromUTF8 ("わたしの魔王"), id, err), err);
        expect (id.rfind ("user-", 0) == 0 && isValidPresetId (id));
        expect (dir.getChildFile (id + ".json").existsAsFile());
        const Preset saved = *lib.find (id);
        expect (! saved.builtin);
        expectEquals (saved.pitchSt, -8.5f);

        std::string id2;
        expect (lib.saveNew (p, "b", id2, err));
        expect (id2 != id, "fresh ids do not collide");

        {
            PresetLibrary again (dir);
            expect (again.reload().isEmpty());
            expect (again.find (id) != nullptr && *again.find (id) == saved, "reloaded preset equals the saved one");
            expectEquals (int (again.all().size()), builtinCount + 2);
        }

        auto changed = saved;
        changed.chain[1].enabled = false;
        changed.layers.clear();
        expect (lib.overwrite (changed, err), err);
        expect (lib.rename (id, juce::String::fromUTF8 ("改名"), err), err);
        expect (! lib.rename (id, "   ", err), "empty name refused");
        expect (! lib.rename (id, juce::String::repeatedString ("a", 33), err), "33 chars refused");
        {
            PresetLibrary again (dir);
            again.reload();
            auto* r = again.find (id);
            expect (r != nullptr && r->name == juce::String::fromUTF8 ("改名") && ! r->chain[1].enabled && r->layers.empty());
        }

        expect (! lib.overwrite (*lib.find ("natural-clear"), err), "built-in overwrite refused");
        expect (! lib.rename ("natural-clear", "x", err), "built-in rename refused");
        expect (! lib.remove ("natural-clear", err), "built-in delete refused");
        std::string dupId;
        expect (lib.duplicate ("natural-clear", dupId, err), err);
        auto* dup = lib.find (dupId);
        expect (dup != nullptr && ! dup->builtin && dup->chain == lib.find ("natural-clear")->chain);
        expect (dup != nullptr && dup->name == juce::String::fromUTF8 ("クリア補正 のコピー"));

        // users after built-ins, sorted by name
        const auto& all = lib.all();
        for (int i = 0; i < builtinCount; ++i) expect (all[size_t (i)].builtin);
        for (size_t i = size_t (builtinCount) + 1; i < all.size(); ++i)
            expect (all[i - 1].name.compareNatural (all[i].name) <= 0, "user presets sorted by name");

        beginTest ("F-05-4: export then import gives a new user preset with the same contents");
        const auto exported = tmp.getChildFile ("exported.json");
        expect (lib.exportFile ("character-talkbox", exported, err), err);
        std::string impId;
        PresetLoadReport rep;
        expect (lib.importFile (exported, impId, rep), rep.toJapanese());
        expect (! rep.hasNotices());
        auto imported = *lib.find (impId);
        expect (impId != "character-talkbox" && ! imported.builtin);
        imported.id = "character-talkbox";
        imported.builtin = true;
        expect (imported == *lib.find ("character-talkbox"));

        expect (lib.remove (id, err), err);
        expect (lib.find (id) == nullptr && ! dir.getChildFile (id + ".json").exists());

        beginTest ("E-09: broken / foreign user files are skipped and counted");
        dir.getChildFile ("user-broken.json").replaceWithText ("{ not json");
        dir.getChildFile ("natural-asis.json").replaceWithText (serializePreset (*lib.find ("natural-asis")));
        PresetLibrary again (dir);
        const auto notices = again.reload();
        expect (notices.joinIntoString ("\n").contains ("2"), notices.joinIntoString ("\n"));
        expect (again.find ("user-broken") == nullptr);
        expect (again.find ("natural-asis")->builtin);
    }

    void testLoaderRules()
    {
        PresetLoadReport r;

        beginTest ("AC-26: over 64 KB is rejected");
        {
            const auto big = "{\"schemaVersion\":1,\"id\":\"user-a\",\"name\":\"a\",\"pad\":\"" + juce::String::repeatedString ("x", 70000) + "\"}";
            expect (! parse (big, r).has_value());
            expect (r.rejected && r.rejectReason.contains ("64 KB"));
            const auto f = tmp.getChildFile ("big.json");
            f.replaceWithText (big);
            PresetLibrary lib (tmp.getChildFile ("imports"));
            std::string id;
            PresetLoadReport ir;
            expect (! lib.importFile (f, id, ir) && ir.rejected);
        }

        beginTest ("AC-26: out-of-range values are clamped to the ends, unknown keys ignored");
        {
            auto p = parse ("{\"schemaVersion\":1,\"id\":\"user-a\",\"name\":\"a\",\"future\":{\"x\":1},\"outputTrimDb\":-40,"
                            "\"shifter\":{\"pitchSt\":99,\"formantSt\":-99},"
                            "\"chain\":[" + slotJson ("eq", "\"hpfHz\":5,\"highDb\":40,\"unknownParam\":3") + ","
                            + slotJson ("echo", "\"mode\":\"warp\",\"timeMs\":\"long\"") + "]}", r);
            expect (p.has_value() && r.hasNotices());
            if (! shaped (p, 2)) return;
            expectEquals (r.valuesClamped, 7); // 5 out of range + unknown choice id + wrong type
            expectEquals (p->pitchSt, 12.0f);
            expectEquals (p->formantSt, -6.0f);
            expectEquals (p->outputTrimDb, -12.0f);
            expectEquals (p->chain[0].params[0], 20.0f);
            expectEquals (p->chain[0].params[5], 12.0f);
            expectEquals (p->chain[1].params[0], 0.0f);   // unknown choice id -> default (digital)
            expectEquals (p->chain[1].params[1], 300.0f); // wrong type -> default
        }

        beginTest ("AC-26: newer schemaVersion is rejected with a notice (E-11)");
        expect (! parse ("{\"schemaVersion\":2,\"id\":\"user-a\",\"name\":\"a\"}", r).has_value());
        expect (r.rejected && r.rejectReason == juce::String::fromUTF8 ("新しい版のプリセットです") && r.hasNotices());
        expect (! parse ("{\"id\":\"user-a\",\"name\":\"a\"}", r).has_value(), "schemaVersion is required");
        expect (! parse ("{\"schemaVersion\":1,\"id\":\"Bad Id\",\"name\":\"a\"}", r).has_value(), "id format checked");
        expect (! parse ("[1,2]", r).has_value() && r.rejected);

        beginTest ("AC-26: 11 slots -> the first 10 (E-21)");
        {
            juce::StringArray items;
            for (int i = 0; i < 11; ++i) items.add (slotJson ("eq", "\"hpfHz\":" + juce::String (100 + i)));
            auto p = parse (presetJson (items), r);
            if (! shaped (p, 10, 0)) return;
            expectEquals (int (p->chain.size()), 10);
            expectEquals (r.slotsOverLimitDropped, 1);
            expectEquals (p->chain[9].params[0], 109.0f);
        }

        beginTest ("AC-26: unknown type -> only that slot skipped (E-22)");
        {
            auto p = parse (presetJson ({ slotJson ("eq"), slotJson ("hyperdrive"), slotJson ("reverb") }), r);
            if (! shaped (p, 2, 0)) return;
            expectEquals (int (p->chain.size()), 2);
            expectEquals (r.unknownTypesSkipped, 1);
            expect (p->chain[1].type == "reverb" && r.hasNotices());
        }

        beginTest ("AC-26: 11 slots with an unknown 3rd -> all 10 known slots load");
        {
            juce::StringArray items;
            for (int i = 0; i < 11; ++i) items.add (i == 2 ? slotJson ("future-fx") : slotJson ("eq", "\"hpfHz\":" + juce::String (100 + i)));
            auto p = parse (presetJson (items), r);
            if (! shaped (p, 10, 0)) return;
            expectEquals (int (p->chain.size()), 10);
            expectEquals (r.unknownTypesSkipped, 1);
            expectEquals (r.slotsOverLimitDropped, 0);
            expectEquals (p->chain[2].params[0], 103.0f);
            expectEquals (p->chain[9].params[0], 110.0f);
        }

        beginTest ("AC-26: three heavy ON -> the first two stay ON (E-23)");
        {
            // autopitch is the only heavy type since the weights follow the measured CPU (2026-10-03)
            auto p = parse (presetJson ({ slotJson ("autopitch"), slotJson ("eq"), slotJson ("autopitch"), slotJson ("autopitch"),
                                          slotJson ("autopitch", {}, false) }), r);
            if (! shaped (p, 5)) return;
            expectEquals (int (p->chain.size()), 5);
            expect (p->chain[0].enabled && p->chain[2].enabled && ! p->chain[3].enabled && ! p->chain[4].enabled);
            expectEquals (r.heavyTurnedOff, 1);
        }

        beginTest ("AC-26: two loopers -> the first one only (E-24)");
        {
            auto p = parse (presetJson ({ slotJson ("looper", "\"levelDb\":-3"), slotJson ("freeze"), slotJson ("looper"), slotJson ("freeze") }), r);
            if (! shaped (p, 2, 0)) return;
            expectEquals (int (p->chain.size()), 2);
            expectEquals (r.duplicateFreezeLooperDropped, 2);
            expectEquals (p->chain[0].params[0], -3.0f);
        }

        beginTest ("AC-26: three layers -> the first two (E-30)");
        {
            const juce::String shifter = ",\"shifter\":{\"pitchSt\":0,\"formantSt\":0}";
            auto p = parse (presetJson ({}, shifter + ",\"layers\":[{\"pitchSt\":1},{\"pitchSt\":2},{\"pitchSt\":3}]"), r);
            if (! shaped (p, 0, 2)) return;
            expectEquals (int (p->layers.size()), 2);
            expectEquals (r.layersDropped, 1);
            expectEquals (p->layers[1].pitchSt, 2.0f);

            p = parse (presetJson ({}, ",\"layers\":[{\"pitchSt\":1}]"), r);
            if (! shaped (p, 0)) return;
            expect (p->layers.empty() && ! p->hasShifter);
            expectEquals (r.layersDropped, 1);

            p = parse (presetJson ({}, shifter + ",\"layers\":[{\"mode\":\"canon\"},{\"mode\":\"scale\",\"key\":\"A\",\"scale\":\"minor\",\"degree\":-3}]"), r);
            if (! shaped (p, 0, 1)) return;
            expectEquals (int (p->layers.size()), 1);
            expectEquals (r.unknownLayerModesSkipped, 1);
            expect (p->layers[0].mode == LayerDef::Mode::scale && p->layers[0].key == 9 && p->layers[0].minor && p->layers[0].degree == -3);
        }
    }

    void testBuiltins()
    {
        // 61 from koeloom_presets.md sec. 4 + 11 device and 9 space presets of wave 7 (INTERFACES.md §9.5)
        beginTest ("AC-44: all 81 built-ins load with no errors, in koeloom_presets.md sec. 4 order");
        PresetLibrary lib (tmp.getChildFile ("empty-user-dir"));
        expect (lib.reload().isEmpty());
        expectEquals (lib.builtinErrors(), 0);
        const auto& all = lib.all();
        expectEquals (int (all.size()), 81);
        if (all.size() != 81 || lib.builtinErrors() != 0) return;
        expect (all.front().id == "natural-asis" && all[11].id == "character-female" && all[33].id == "device-telephone"
                && all[56].id == "space-cave" && all[73].id == "layered-harmony-fifth" && all.back().id == "layered-harmony-scale");

        std::map<juce::String, int> perCategory;
        std::map<int, int> perPhase;
        std::set<std::string> phase2TypesUsed;
        juce::String lastCategory;
        std::set<juce::String> seenCategories;
        for (auto& p : all)
        {
            const juce::String who (p.id);
            expect (p.builtin && isValidPresetId (p.id), who);
            expect (p.name.isNotEmpty() && p.name.length() <= kPresetNameMaxChars, who);
            // outputTrimDb comes from the Phase 2 level calibration (tools/apply_trims.py), within the format's range
            expect (kTrimDb.clamp (p.outputTrimDb) == p.outputTrimDb, who);
            expect (kPitchSt.clamp (p.pitchSt) == p.pitchSt && kFormantSt.clamp (p.formantSt) == p.formantSt, who);
            expect (int (p.chain.size()) <= 4, who + ": built-ins have at most 4 slots");
            expect (int (p.layers.size()) <= kMaxLayers && (p.hasShifter || p.layers.empty()), who);
            int heavyOn = 0, freeze = 0, looper = 0;
            for (auto& s : p.chain)
            {
                auto* info = findEffectInfo (s.type);
                expect (info != nullptr, who);
                heavyOn += s.enabled && info->weight == EffectWeight::heavy ? 1 : 0;
                freeze += s.type == "freeze" ? 1 : 0;
                looper += s.type == "looper" ? 1 : 0;
                expect (s.enabled, who + ": every listed slot is ON");
                for (size_t i = 0; i < info->params.size(); ++i)
                    expect (info->params[i].clamp (s.params[i]) == s.params[i], who + "." + info->params[i].id);
            }
            expect (heavyOn <= kMaxHeavyOn && freeze <= 1 && looper <= 1, who);

            // the category blocks are contiguous and in table order
            if (p.category() != lastCategory)
            {
                expect (seenCategories.insert (p.category()).second, who + ": category block split");
                lastCategory = p.category();
            }
            ++perCategory[p.category()];
            const int phase = phaseOf (p);
            ++perPhase[phase];
            if (phase < 5)
                for (auto& s : p.chain) phase2TypesUsed.insert (s.type);

            PresetLoadReport r;
            auto back = parsePreset (serializePreset (p), r);
            expect (back && ! r.hasNotices(), who);
            if (back)
            {
                back->builtin = true;
                expect (*back == p, who + ": serialise round trip");
            }
        }
        expectEquals (perCategory["natural"], 11);
        expectEquals (perCategory["character"], 22);
        expectEquals (perCategory["device"], 23);
        expectEquals (perCategory["space"], 17);
        expectEquals (perCategory["layered"], 8);
        expectEquals (perPhase[1], 2);
        expectEquals (perPhase[2], 74);
        expectEquals (perPhase[1] + perPhase[2], 76);
        expectEquals (perPhase[5], 5);

        beginTest ("AC-44: the Phase 2 effect types match koeloom_effects.md sec. 2, and Phase 1+2 presets use only those");
        const std::set<std::string> expected { "compressor", "deesser", "enhancer", "eq", "peq", "autowah", "filtersweep", "isolator",
                                               "formantfilter", "saturator", "distortion", "bitcrusher", "noise", "voicechar", "ringmod",
                                               "modulation", "phaser", "rotary", "ensemble", "tremolo", "slicer", "stutter", "echo", "reverb" };
        std::set<std::string> registryPhase2;
        for (auto& i : allEffectInfos())
            if (i.phase == 2) registryPhase2.insert (i.type);
        expect (registryPhase2 == expected);
        for (auto& t : phase2TypesUsed) expect (expected.count (t) == 1, t);

        beginTest ("spot checks of the table mapping");
        auto param = [&lib] (const char* id, size_t slot, const char* paramId) {
            auto* p = lib.find (id);
            auto* info = findEffectInfo (p->chain[slot].type);
            return p->chain[slot].params[size_t (info->paramIndex (paramId))];
        };
        expectEquals (param ("natural-clear", 1, "midHz"), 3000.0f);
        expectEquals (param ("character-crowd", 1, "type"), 1.0f);       // hall
        expectEquals (param ("space-tape-echo", 0, "mode"), 1.0f);       // tape
        expectEquals (param ("device-radio-noise", 2, "kind"), 4.0f);    // static
        expectEquals (param ("device-radio-noise", 1, "kind"), 1.0f);    // radio
        expectEquals (param ("device-slicer-trance", 0, "division"), 1.0f); // 1/16
        expectEquals (param ("character-talkbox", 0, "chord"), 1.0f);    // major_chord
        expect (! lib.find ("natural-asis")->hasShifter && lib.find ("character-robot")->hasShifter);
        const auto& scaleLayer = lib.find ("layered-harmony-scale")->layers.at (0);
        expect (scaleLayer.mode == LayerDef::Mode::scale && scaleLayer.degree == 2 && scaleLayer.key == 0 && ! scaleLayer.minor
                && scaleLayer.formantSt == 2.0f && scaleLayer.levelDb == -6.0f);
    }

    void testSettings()
    {
        const auto file = tmp.getChildFile ("settings").getChildFile ("settings.json");
        SettingsLoadResult res;

        beginTest ("F-11-1: missing file -> defaults, save -> load is exact");
        auto s = loadSettings (file, res);
        expect (res.fileMissing && s == Settings());
        s.inputDevice = juce::String::fromUTF8 ("マイク (USB)");
        s.outputGainDb = -3.5f;
        s.gateAttackMs = 0.1f;
        s.favorites = { "natural-clear", "user-1" };
        s.hotkeys.push_back ({ "favorite.3", 3, 0x70 });
        s.tourStep = 4;
        s.darkTheme = false;
        expect (saveSettings (s, file));
        expect (! file.getSiblingFile ("settings.json.tmp").exists());
        auto back = loadSettings (file, res);
        expect (back == s && ! res.corrupted && res.clampedKeys.isEmpty(), res.clampedKeys.joinIntoString (","));

        beginTest ("detailed settings: every new key round-trips; out-of-range choices fall back to defaults");
        {
            Settings d;
            d.converterQuality = 2; d.pitchMinHz = 80.0f; d.pitchMaxHz = 600.0f; d.highPassOn = true; d.highPassHz = 120.0f;
            d.agcOn = true; d.agcTargetDb = -20.0f; d.agcMaxGainDb = 6.0f; d.limiterCeilingDb = -3.0f; d.limiterReleaseMs = 200.0f;
            d.presetCrossfadeMs = 100.0f; d.soundboardMaxVoices = 3; d.soundFadeMs = 50.0f; d.duckAttackMs = 5.0f;
            d.duckReleaseMs = 800.0f; d.monitorIncludeSoundboard = false; d.wasapiExclusive = true; d.inputChannel = 2;
            d.monitorLatency = 2; d.reconnectSeconds = 5; d.pushToTalk = 1; d.pttReleaseMs = 0.0f; d.hotkeyToasts = true;
            d.favoriteWrap = false; d.startupVoice = 2; d.startupLastPreset = false; d.closeAction = 1; d.trayNotifications = true;
            d.logLevel = 2; d.logKeepDays = 30; d.uiScalePercent = 125; d.alwaysOnTop = true; d.animations = 2; d.meterFps = 60;
            d.meterPeakHoldMs = 0.0f; d.tooltipDelayMs = 1000.0f; d.knobSensitivity = 0; d.knobWheel = false; d.settingsShowDetails = true;
            d.autoUpdate = true; d.updateIncludePrerelease = false; d.updateSkippedVersion = "0.2.0";
            d.layoutStyle = 2; d.themeId = "user:my-theme";
            expect (clampSettings (d) == d);
            expect (saveSettings (d, file));
            expect (loadSettings (file, res) == d && res.clampedKeys.isEmpty(), res.clampedKeys.joinIntoString (","));

            file.replaceWithText ("{\"converterQuality\": 5, \"uiScalePercent\": 77, \"meterFps\": 45, \"pitchMinHz\": 300,"
                                  " \"pitchMaxHz\": 300, \"soundboardMaxVoices\": 0, \"logKeepDays\": 99, \"limiterCeilingDb\": 3}");
            const auto c = loadSettings (file, res);
            const Settings def;
            expect (c.converterQuality == def.converterQuality && c.uiScalePercent == def.uiScalePercent && c.meterFps == def.meterFps);
            expect (c.pitchMinHz == kPitchMinHz.def && c.pitchMaxHz == kPitchMaxHz.def && c.soundboardMaxVoices == def.soundboardMaxVoices);
            expect (c.logKeepDays == def.logKeepDays && c.limiterCeilingDb == kLimiterCeilingSetDb.max);
            for (auto* k : { "converterQuality", "uiScalePercent", "meterFps", "pitchMinHz", "soundboardMaxVoices", "logKeepDays", "limiterCeilingDb" })
                expect (res.clampedKeys.contains (k), k);
        }

        beginTest ("AC-25: invalid JSON -> defaults, original kept as settings.json.bak");
        file.replaceWithText ("{ \"outputGainDb\": 3, oops");
        back = loadSettings (file, res);
        expect (res.corrupted && back == Settings());
        expect (res.backupFile == file.getSiblingFile ("settings.json.bak") && res.backupFile.existsAsFile());
        expect (res.backupFile.loadFileAsString().contains ("oops") && ! file.exists());

        beginTest ("AC-25 / E-29: output gain 99 -> +12, other out-of-range values clamped, unknown keys ignored");
        file.replaceWithText ("{\"outputGainDb\": 99, \"inputGainDb\": -100, \"gateHoldMs\": \"long\", \"newFeature\": true,"
                              "\"favorites\": [\"a\",\"b\",\"a\",\"c\",\"d\",\"e\",\"f\",\"g\",\"h\",\"i\",\"j\",\"\"], \"darkTheme\": false}");
        back = loadSettings (file, res);
        expect (! res.corrupted);
        expectEquals (back.outputGainDb, 12.0f);
        expectEquals (back.inputGainDb, -24.0f);
        expectEquals (back.gateHoldMs, kGateHoldMs.def);
        expect (! back.darkTheme);
        expect (back.favorites == juce::StringArray ({ "a", "b", "c", "d", "e", "f", "g", "h", "i" }), back.favorites.joinIntoString (","));
        for (auto* k : { "outputGainDb", "inputGainDb", "gateHoldMs", "favorites" })
            expect (res.clampedKeys.contains (k), k);

        beginTest ("F-12-1: output gain snaps to 0.5 dB steps");
        Settings g;
        juce::StringArray keys;
        g.outputGainDb = 3.3f;
        expectEquals (clampSettings (g, &keys).outputGainDb, 3.5f);
        g.outputGainDb = -0.2f;
        expectEquals (clampSettings (g).outputGainDb, 0.0f);
        expect (keys.contains ("outputGainDb"));
        expect (clampSettings (Settings(), &keys) == Settings());

        beginTest ("E-19: unwritable location returns false");
        const auto blocker = tmp.getChildFile ("not-a-dir");
        blocker.replaceWithText ("x");
        expect (! saveSettings (Settings(), blocker.getChildFile ("settings.json")));
    }
};

static ModelTests modelTests;
} // namespace koe

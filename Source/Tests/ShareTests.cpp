// プリセットの共有コード and サウンドボードに録音を登録 (INTERFACES.md §11.3, owner wave9/share). Category "Share".
// No window, no sound, no audio device, no clipboard: the controller's makeShareCode / importShareCode are tested directly;
// the take is recorded through getProcessorForTests() with CaptureData::assumeDevicesRunningForTests (like CaptureTests).

#include "App/AppController.h"
#include "Core/Paths.h"
#include "Effects/EffectRegistry.h"
#include "Tests/TestUtil.h"
#include "Tools/Calibrate.h"
#include "UI/MainComponent.h"
#include "UI/Screens.h"
#include "UI/main/Common.h"
#include "UI/main/ToolsView.h"

#include <juce_audio_formats/juce_audio_formats.h>

namespace koe
{
namespace
{
using namespace test;
constexpr int kBlock = 480;

void freshDataDir()
{
    auto d = paths::dataDir();
    if (d.getFullPathName().contains ("KoeLoomTests")) d.deleteRecursively();
    d.createDirectory();
}

struct AssumeRunning
{
    AssumeRunning() { CaptureData::assumeDevicesRunningForTests = true; }
    ~AssumeRunning() { CaptureData::assumeDevicesRunningForTests = false; }
};

std::unique_ptr<AppController> makeController()
{
    auto c = std::make_unique<AppController> (false);
    c->startup();
    c->updateSettings ([] (Settings& s)
    {
        s.setupDone = true;
        s.tourStep = 7;
        s.monitorDevice = "Headphones (test)";
    });
    c->setVoiceChangerOn (true);
    c->loadPreset ("character-demon-king");
    c->reprepareForTests (kSr, kBlock);
    c->dispatchPendingMessages();
    return c;
}

void run (AppController& c, const std::vector<float>& in)
{
    auto& vp = c.getProcessorForTests();
    std::vector<float> out (kBlock);
    for (size_t pos = 0; pos + kBlock <= in.size(); pos += kBlock) vp.process (in.data() + pos, out.data(), nullptr, kBlock);
}

/** The same packing as makeShareCode, for hand-made payloads (withChecksum false: an old-style code without the check). */
juce::String encode (const juce::String& json, bool withChecksum = true)
{
    juce::uint32 h = 2166136261u;
    for (auto* p = json.toRawUTF8(); *p != 0; ++p) h = (h ^ juce::uint8 (*p)) * 16777619u;
    const auto text = withChecksum ? json + "#" + juce::String::toHexString (int (h)).paddedLeft ('0', 8) : json;
    juce::MemoryOutputStream packed;
    {
        juce::GZIPCompressorOutputStream zip (packed, 9);
        zip.write (text.toRawUTF8(), text.getNumBytesAsUTF8());
    }
    return "KL1:" + juce::Base64::toBase64 (packed.getData(), packed.getDataSize()).replaceCharacter ('+', '-').replaceCharacter ('/', '_').trimCharactersAtEnd ("=");
}

int userCount (AppController& c)
{
    int n = 0;
    for (auto& p : c.getPresetLibrary().all()) n += p.builtin ? 0 : 1;
    return n;
}

bool toastsContain (const juce::StringArray& toasts, const char* utf8)
{
    for (auto& t : toasts)
        if (t.contains (juce::String::fromUTF8 (utf8))) return true;
    return false;
}

struct ToastNavigator final : ui::Navigator
{
    juce::StringArray toasts;
    void showPage (Page) override {}
    void showSettings (SettingsSection) override {}
    void showPresetBrowser() override {}
    void showEffectPicker (int) override {}
    void showSlotDetail (int) override {}
    void showSetupWizard() override {}
    void showHelp (HelpTopic) override {}
    void startTour (bool) override {}
    void showOverlay (std::unique_ptr<juce::Component>) override {}
    void closeOverlay() override {}
    void showToast (const juce::String& t) override { toasts.add (t); }
};
} // namespace

class ShareTests : public juce::UnitTest
{
public:
    ShareTests() : juce::UnitTest ("Preset share codes and the take into the soundboard (wave9/share)", "Share") {}

    void runTest() override
    {
        roundTripEveryBuiltin();
        roundTripUserPreset();
        codeInsideText();
        brokenCodes();
        reportAndNames();
        renderTake();
        soundboardFromTheRecordTool();
        screens();
    }

private:
    // ---------------------------------------------------------------------------------------------
    void roundTripEveryBuiltin()
    {
        beginTest ("Share code: every built-in comes back as the same preset (new user id), short enough for one Discord message");
        freshDataDir();
        auto c = makeController();
        int count = 0, longest = 0;
        long long total = 0;
        juce::String longestId;
        std::vector<Preset> builtins;
        for (auto& p : c->getPresetLibrary().all())
            if (p.builtin) builtins.push_back (p);
        for (auto& orig : builtins)
        {
            juce::String why;
            const auto code = c->makeShareCode (orig.id, why);
            expect (code.startsWith ("KL1:") && why.isEmpty(), juce::String (orig.id) + " " + why);
            expect (code.containsOnly ("KL1:ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_"), "URL-safe: " + orig.id);
            std::string newId;
            PresetLoadReport report;
            expect (c->importShareCode (code, newId, report), juce::String (orig.id) + " " + report.rejectReason);
            expect (! report.hasNotices(), juce::String (orig.id) + ": " + report.toJapanese());
            const auto* back = c->getPresetLibrary().find (newId);
            expect (back != nullptr && juce::String (newId).startsWith ("user-") && newId != orig.id, orig.id);
            if (back == nullptr) continue;
            auto same = *back;
            same.id = orig.id;
            same.builtin = true;
            expect (same == orig, "same preset except the id: " + orig.id);
            ++count;
            total += code.length();
            if (code.length() > longest) { longest = code.length(); longestId = orig.id; }
        }
        expectEquals (count, 81);
        expectLessThan (longest, 2000, "fits a 2000-character Discord message");
        logMessage ("share code length over " + juce::String (count) + " built-ins: average " + juce::String (double (total) / juce::jmax (1, count), 0)
                    + ", longest " + juce::String (longest) + " (" + longestId + ")");
        juce::String why;
        expect (c->makeShareCode ("no-such-preset", why).isEmpty() && why.isNotEmpty(), "unknown preset refused");
    }

    // ---------------------------------------------------------------------------------------------
    void roundTripUserPreset()
    {
        beginTest ("Share code: layers, wet, mod, OFF slots and a convolution file name travel; the IR file itself does not");
        freshDataDir();
        auto c = makeController();
        Preset p;
        p.id = "user-1";
        p.name = juce::String::fromUTF8 ("ぼくの声 \"テスト\"");
        p.hasShifter = true;
        p.pitchSt = 3.5f;
        p.formantSt = -1.5f;
        LayerDef a;
        a.pitchSt = -12.0f;
        a.levelDb = -9.0f;
        LayerDef b;
        b.mode = LayerDef::Mode::scale;
        b.degree = -3;
        b.key = 9;
        b.minor = true;
        b.enabled = false;
        p.layers = { a, b };
        auto echo = makeDefaultSlot ("echo");
        auto ir = makeDefaultSlot ("convolution");
        auto reverb = makeDefaultSlot ("reverb");
        expect (echo && ir && reverb);
        if (! echo || ! ir || ! reverb) return;
        echo->wet = 0.25f;
        echo->modTarget = "wet";
        echo->modDepth = -0.5f;
        ir->file = juce::String::fromUTF8 ("大聖堂 IR.wav").toStdString();
        reverb->enabled = false;
        p.chain = { *echo, *ir, *reverb };
        p.outputTrimDb = -2.5f;
        juce::String err;
        std::string id;
        expect (c->getPresetLibrary().saveNew (p, p.name, id, err), err);
        p = *c->getPresetLibrary().find (id);

        juce::String why;
        const auto code = c->makeShareCode (id, why);
        expect (! code.contains ("user-"), "the id is not carried");
        std::string newId;
        PresetLoadReport report;
        expect (c->importShareCode (code, newId, report), report.rejectReason);
        const auto* back = c->getPresetLibrary().find (newId);
        expect (back != nullptr && newId != id);
        if (back == nullptr) return;
        auto same = *back;
        same.id = p.id;
        expect (same == p, "the whole preset comes back");
        expect (back->chain[1].file == p.chain[1].file, "the IR file name is kept (the file is not)");
        logMessage ("user preset with 3 slots and 2 layers: " + juce::String (code.length()) + " characters");
    }

    // ---------------------------------------------------------------------------------------------
    void codeInsideText()
    {
        beginTest ("Share code: spaces, line breaks and words around the code are fine");
        freshDataDir();
        auto c = makeController();
        juce::String why;
        const auto code = c->makeShareCode ("character-demon-king", why);
        const juce::String texts[] = { "  \r\n" + code + "\n\n  ",
                                       juce::String::fromUTF8 ("これ使ってみて→") + code + juce::String::fromUTF8 ("　よろしく！"),
                                       "preset: " + code + ". thanks",
                                       juce::String::fromUTF8 ("「") + code + juce::String::fromUTF8 ("」") };
        for (auto& t : texts)
        {
            std::string newId;
            PresetLoadReport report;
            expect (c->importShareCode (t, newId, report), t.substring (0, 24) + " " + report.rejectReason);
            const auto* back = c->getPresetLibrary().find (newId);
            expect (back != nullptr && back->name == c->getPresetLibrary().find ("character-demon-king")->name);
        }
    }

    // ---------------------------------------------------------------------------------------------
    void brokenCodes()
    {
        beginTest ("Share code: missing, cut, garbled, not a preset, too big -> rejected in Japanese, nothing added");
        freshDataDir();
        auto c = makeController();
        juce::String why;
        const auto code = c->makeShareCode ("character-demon-king", why);
        expect (code.isNotEmpty(), why);
        juce::String hugeJson ("{\"schemaVersion\":1,\"id\":\"x\",\"name\":\"x\",\"pad\":\"");
        hugeJson << juce::String::repeatedString ("a", kPresetMaxBytes + 100) << "\"}";
        const struct { juce::String text; const char* reason; } cases[] = {
            { {}, "見つかりません" },
            { juce::String::fromUTF8 ("こんにちは"), "見つかりません" },
            { "kl1:" + code.substring (4), "見つかりません" },
            { "KL1:", "壊れています" },
            { "KL1:!!!!", "壊れています" },
            { code.substring (0, code.length() / 2), "壊れています" },
            { encode ("{\"schemaVersion\":1,\"id\":\"x\",\"name\":\"x\"}", false), "壊れています" }, // no check
            { code.substring (0, 40) + "AAAA" + code.substring (44), "壊れています" },
            { "KL1:" + juce::Base64::toBase64 (juce::String ("hello, not zlib")).replaceCharacter ('+', '-').trimCharactersAtEnd ("="), "壊れています" },
            { encode ("[1, 2, 3]"), "" },                                            // JSON but not a preset: parsePreset's reason
            { encode ("{\"schemaVersion\": 99, \"id\": \"a\", \"name\": \"a\"}"), "新しい版" }, // E-11
            { encode (hugeJson), "64 KB" },
        };
        const int before = userCount (*c);
        for (auto& k : cases)
        {
            std::string newId = "untouched";
            PresetLoadReport report;
            expect (! c->importShareCode (k.text, newId, report), k.text.substring (0, 30));
            expect (report.rejected && report.rejectReason.isNotEmpty(), k.text.substring (0, 30));
            expect (report.rejectReason.contains (juce::String::fromUTF8 (k.reason)), k.text.substring (0, 30) + " -> " + report.rejectReason);
            expect (newId == "untouched", "no id handed out");
        }
        expectEquals (userCount (*c), before, "nothing added");
        expectLessThan (encode (hugeJson).length(), 2000, "(the oversized one is a short code: the limit is on the unpacked size)");
    }

    // ---------------------------------------------------------------------------------------------
    void reportAndNames()
    {
        beginTest ("Share code: the E-10.. report like importPreset; the same code twice gives two presets with the same name");
        freshDataDir();
        auto c = makeController();
        std::string id1, id2;
        PresetLoadReport report;
        const auto clamped = encode ("{\"schemaVersion\":1,\"id\":\"x\",\"name\":\"clamped\",\"outputTrimDb\":99,"
                                     "\"chain\":[{\"type\":\"no-such-effect\",\"params\":{}},{\"type\":\"echo\",\"params\":{}}]}");
        expect (c->importShareCode (clamped, id1, report), report.rejectReason);
        expect (report.valuesClamped >= 1 && report.unknownTypesSkipped == 1 && report.hasNotices(), report.toJapanese());
        const auto* p = c->getPresetLibrary().find (id1);
        expect (p != nullptr && p->outputTrimDb == kTrimDb.max && p->chain.size() == 1);

        juce::String why;
        const auto code = c->makeShareCode (id1, why);
        expect (c->importShareCode (code, id2, report), report.rejectReason);
        expect (! report.hasNotices(), "a clean code has nothing to report");
        const auto* q = c->getPresetLibrary().find (id2);
        expect (q != nullptr && id2 != id1 && q->name == p->name, "same name, another id (importPreset does the same)");

        // the library on disk: both are there after a reload
        c->getPresetLibrary().reload();
        expect (c->getPresetLibrary().find (id1) != nullptr && c->getPresetLibrary().find (id2) != nullptr, "saved as user presets");
    }

    // ---------------------------------------------------------------------------------------------
    void renderTake()
    {
        beginTest ("加工して保存: refusals; the take through the working preset, latency cut, as long as the take, 24-bit mono WAV");
        freshDataDir();
        AssumeRunning running;
        auto c = makeController();
        juce::String why;
        expect (c->renderTestTakeToFile (why) == juce::File() && why.contains (juce::String::fromUTF8 ("試し録り")), "no take: " + why);

        c->setInputGainDb (3.0f);
        expect (c->startTestTake (why), why);
        const auto voice = synthVoice (1.2);
        run (*c, voice);
        why = {};
        expect (c->renderTestTakeToFile (why) == juce::File() && why.contains (juce::String::fromUTF8 ("録音中")), "while recording: " + why);
        c->stopTestTake();
        const int len = int (c->getTestTakeSeconds() * kSr + 0.5);
        expectEquals (len, int (voice.size()));

        why = {};
        const auto f = c->renderTestTakeToFile (why);
        expect (f.existsAsFile(), why);
        expect (f.getParentDirectory() == paths::recordingsDir());
        expect (f.getFileNameWithoutExtension().matchesWildcard ("KoeLoom ????-??-?? ??-??-??*", false), f.getFileName());
        expect (c->getLastWavFile() == f, "the 録音 tool's last file");

        juce::WavAudioFormat fmt;
        std::unique_ptr<juce::AudioFormatReader> r (fmt.createReaderFor (f.createInputStream().release(), true));
        expect (r != nullptr);
        if (r == nullptr) return;
        expect (r->sampleRate == kSr && r->numChannels == 1 && r->bitsPerSample == 24, "format");
        expectEquals (int (r->lengthInSamples), len, "as long as the take");
        juce::AudioBuffer<float> b (1, int (r->lengthInSamples));
        r->read (&b, 0, int (r->lengthInSamples), 0, true, false);
        std::vector<float> got (b.getReadPointer (0), b.getReadPointer (0) + b.getNumSamples());
        expect (allFinite (got));
        expect (rmsDb (got) > -40.0f, "processed voice in it: " + juce::String (rmsDb (got)));

        // the same as rendering it by hand: input gain, working preset with its effective trim, latency removed
        std::vector<float> in (voice.size() + size_t (kSr), 0.0f);
        const float g = juce::Decibels::decibelsToGain (3.0f);
        for (size_t i = 0; i < voice.size(); ++i) in[i] = voice[i] * g;
        auto preset = c->getCurrentPreset();
        preset.outputTrimDb = c->getEffectiveTrimDb();
        int lat = 0;
        const auto ref = tools::renderPreset (preset, in, lat, kSr);
        expect (lat > 0, "the demon king has latency");
        float err = 0.0f;
        for (int i = 0; i < len; ++i) err = std::max (err, std::abs (got[size_t (i)] - ref[size_t (lat + i)]));
        expectLessThan (err, 1.0e-6f, "aligned with the input (24-bit rounding only)");

        // while playing it can be saved too; a second file does not overwrite the first
        c->getMonitorForTests().prepareForTest (kSr, kSr, kBlock);
        expect (c->playTestTake (why), why);
        const auto f2 = c->renderTestTakeToFile (why);
        expect (f2.existsAsFile() && f2 != f && f.existsAsFile(), why);
        c->stopTestTake();
        c->clearTestTake();
        expect (c->renderTestTakeToFile (why) == juce::File(), "cleared take: refused");
        for (int i = 0; i < 10; ++i) c->tickForTests();
    }

    // ---------------------------------------------------------------------------------------------
    void soundboardFromTheRecordTool()
    {
        using namespace ui;
        beginTest ("ツール: 加工して保存 -> 録音の「サウンドボードに入れる」 fills the first empty slot; a full board is refused");
        freshDataDir();
        AssumeRunning running;
        auto c = makeController();
        const bool wasOff = mainui::animationsOff();
        mainui::animationsOff() = true;
        {
            MainComponent mc (*c);
            mc.setSize (Theme::minWidth, Theme::minHeight);
            mc.showPage (Navigator::Page::tools);
            auto* view = dynamic_cast<ToolsView*> (mainui::findById (&mc, "page.tools"));
            expect (view != nullptr);
            if (view == nullptr) return;
            auto button = [&mc] (const char* id) { return dynamic_cast<juce::Button*> (mainui::findById (&mc, id)); };

            view->showTool (ToolsView::Tool::take);
            expect (button ("take.render") != nullptr && ! button ("take.render")->isEnabled(), "nothing to save yet");
            button ("take.record")->onClick();
            run (*c, synthVoice (0.5));
            expect (! button ("take.render")->isEnabled(), "not while recording");
            button ("take.record")->onClick();
            expect (button ("take.render")->isEnabled(), "a take to save");
            button ("take.render")->onClick();
            const auto file = c->getLastWavFile();
            expect (file.existsAsFile(), "saved");

            view->showTool (ToolsView::Tool::record);
            auto* sbButton = button ("record.soundboard");
            expect (sbButton != nullptr && sbButton->isEnabled(), "the saved take can go into the soundboard");
            if (sbButton == nullptr) return;
            auto& sb = c->getSoundboard();
            sb.assignFile (0, file); // slot 1 is taken
            sbButton->onClick();
            expect (sb.getSlotDef (1).file == file.getFullPathName(), "the first empty slot (2): " + sb.getSlotDef (1).file);
            for (int i = 2; i < kSoundboardSlots; ++i) expect (sb.getSlotDef (i).file.isEmpty());
            expect (paths::soundboardFile().loadFileAsString().contains (file.getFileName()), "saveSoundboard() was called");

            for (int i = 0; i < kSoundboardSlots; ++i) sb.assignFile (i, file);
            sbButton->onClick(); // full and not on screen: no menu, nothing replaced
            for (int i = 0; i < kSoundboardSlots; ++i) expect (sb.getSlotDef (i).file == file.getFullPathName());

        }
        mainui::animationsOff() = wasOff;

        // the toasts, through a tool with a navigator of our own
        {
            auto c2 = makeController();
            ToastNavigator nav;
            auto rec = makeRecordTool (*c2, nav);
            rec->setSize (560, 380);
            rec->setVisible (true);
            auto* b = dynamic_cast<juce::Button*> (mainui::findById (rec.get(), "record.soundboard"));
            expect (b != nullptr && ! b->isEnabled(), "no file yet: disabled");
            juce::String why;
            expect (c2->startTestTake (why), why);
            run (*c2, synthVoice (0.3));
            c2->stopTestTake();
            const auto f = c2->renderTestTakeToFile (why);
            expect (f.existsAsFile(), why);
            for (int i = 0; i < kSoundboardSlots; ++i) c2->getSoundboard().assignFile (i, f);
            rec->setVisible (false);
            rec->setVisible (true); // refresh
            if (b != nullptr) b->onClick();
            expect (toastsContain (nav.toasts, "空いている枠がありません"), nav.toasts.joinIntoString (" | "));
        }
    }

    // ---------------------------------------------------------------------------------------------
    void screens()
    {
        using namespace ui;
        beginTest ("S-06 and the tools: the new buttons are there, inside, not overlapping, at 1120x720 and 800x560");
        freshDataDir();
        AssumeRunning running;
        auto c = makeController();
        ToastNavigator nav;
        PresetBrowser b (*c, nav);
        for (auto size : { juce::Point<int> (Theme::defaultWidth, Theme::defaultHeight), juce::Point<int> (Theme::minWidth, Theme::minHeight) })
        {
            const auto label = juce::String (size.x) + "x" + juce::String (size.y);
            b.setBounds (juce::Rectangle<int> (size.x, size.y).withSizeKeepingCentre (juce::jmin (b.getWidth(), size.x - Theme::space5 * 2),
                                                                                    juce::jmin (b.getHeight(), size.y - Theme::space5 * 2)));
            for (auto* id : { "presets.shareCopy", "presets.shareImport" })
            {
                auto* comp = mainui::findById (&b, id);
                expect (comp != nullptr && comp->isVisible(), label + " " + id);
                if (comp == nullptr) continue;
                auto* parent = comp->getParentComponent();
                expect (parent->getLocalBounds().contains (comp->getBounds()), label + ": " + id + " inside");
                expect (comp->getHeight() >= Theme::touchMin, label + ": " + id + " tall enough");
                for (auto* sib : parent->getChildren())
                    if (sib != comp && sib->isVisible() && ! sib->getBounds().isEmpty() && sib->getComponentID() != "presets.prompt")
                        expect (! sib->getBounds().intersects (comp->getBounds()), label + ": " + id + " overlaps " + sib->getComponentID());
            }
        }
        // the copy button works on the selected preset: here only through the controller (no clipboard in tests)
        auto* copy = dynamic_cast<juce::Button*> (mainui::findById (&b, "presets.shareCopy"));
        expect (copy != nullptr && copy->isEnabled(), "the working preset is selected");

        const bool wasOff = mainui::animationsOff();
        mainui::animationsOff() = true;
        {
            MainComponent mc (*c);
            for (auto size : { juce::Point<int> (Theme::defaultWidth, Theme::defaultHeight), juce::Point<int> (Theme::minWidth, Theme::minHeight) })
            {
                mc.setSize (size.x, size.y);
                mc.showPage (Navigator::Page::tools);
                auto* view = dynamic_cast<ToolsView*> (mainui::findById (&mc, "page.tools"));
                if (view == nullptr) { expect (false); break; }
                const auto label = juce::String (size.x) + "x" + juce::String (size.y);
                for (auto [tool, page, id] : { std::tuple { ToolsView::Tool::take, "tools.take", "take.render" },
                                               std::tuple { ToolsView::Tool::record, "tools.record", "record.soundboard" } })
                {
                    view->showTool (tool);
                    auto* card = mainui::findById (&mc, page);
                    auto* comp = card != nullptr ? mainui::findById (card, id) : nullptr;
                    expect (comp != nullptr, label + " " + id);
                    if (comp == nullptr) continue;
                    expect (card->getLocalBounds().contains (comp->getBounds()), label + ": " + id + " inside the card");
                    expect (comp->getWidth() >= comp->getHeight() * 2, label + ": " + id + " not squeezed " + comp->getBounds().toString());
                    for (auto* sib : card->getChildren())
                        if (sib != comp && sib->isVisible()) expect (! sib->getBounds().intersects (comp->getBounds()), label + ": " + id + " overlaps " + sib->getComponentID());
                }
            }
        }
        mainui::animationsOff() = wasOff;
    }
};

static ShareTests shareTests;
} // namespace koe

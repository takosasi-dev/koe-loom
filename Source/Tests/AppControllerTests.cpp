// AppController regressions found in review (lead), and the wave-4 detailed platform settings
// (push-to-talk, startup state, favourite wrap, hotkey toasts, close action, reconnect, export / import / reset).
// Category "Integration".

#include "App/AppController.h"
#include "Core/Paths.h"
#include "Platform/Hotkeys.h"
#include "Tests/TestUtil.h"

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
} // namespace

class AppControllerTests : public juce::UnitTest
{
public:
    AppControllerTests() : juce::UnitTest ("AppController review", "Integration") {}

    void runTest() override
    {
        beginTest ("Save as new with the converter OFF: the layers it drops (E-30) stay silent when the converter is turned ON again");
        {
            freshDataDir();
            AppController c (false);
            c.startup();
            c.setVoiceChangerOn (true);
            auto& vp = c.getProcessorForTests();
            const auto in = synthVoice (1.0, 7, kSr, false);
            std::vector<float> out (size_t (kBlock), 0.0f);
            auto render = [&]
            {
                for (size_t pos = 0; pos + kBlock <= in.size(); pos += kBlock) vp.process (in.data() + pos, out.data(), nullptr, kBlock);
            };
            juce::String why;
            expect (c.addLayer (why));
            render();
            expect (vp.anyLayerRunning(), "the added layer sounds");
            c.setShifterEnabled (false);
            render();
            expect (! vp.anyLayerRunning(), "silent while the converter is OFF");
            expect (c.saveCurrentAsNew (juce::String::fromUTF8 ("変換 OFF"), why), why);
            expectEquals (c.getNumLayers(), 0); // the working preset is now the saved one, without layers
            c.setShifterEnabled (true);
            render();
            expect (! vp.anyLayerRunning(), "a layer the working preset no longer has must not sound");
            c.shutdown();
        }

        beginTest ("F-01-6: a monitor device saved as the virtual cable (hand-edited settings.json) is not kept");
        {
            freshDataDir();
            Settings s;
            s.monitorDevice = "CABLE Input (VB-Audio Virtual Cable)";
            expect (saveSettings (s, paths::settingsFile()));
            AppController c (false);
            c.startup();
            expect (c.getSettings().monitorDevice.isEmpty(), c.getSettings().monitorDevice);
            c.shutdown();
        }

        beginTest ("F-01-4 for the monitor: a lost device shows OFF with a notice, comes back by itself, OFF stops waiting");
        {
            freshDataDir();
            AppController c (false);
            c.startup();
            auto hasNotice = [&c]
            {
                for (auto& n : c.getNotices()) if (n.key == "monitor.lost") return true;
                return false;
            };
            auto ticks = [&c] (int k) { for (int i = 0; i < k; ++i) c.tickForTests(); };
            c.setMonitorDevice ("Headphones (Test)");
            c.setMonitorOn (true);
            expect (c.isMonitorOn());

            c.getMonitorForTests().simulateDeviceLostForTest();
            ticks (1); // within 2 s
            expect (! c.isMonitorOn());
            expect (! c.getStatus().monitorOn);
            expect (hasNotice());
            ticks (30); // ~1 s later it retries; AppController (false) opens no device, so the retry succeeds
            expect (c.isMonitorOn());
            expect (! hasNotice());

            c.getMonitorForTests().simulateDeviceLostForTest();
            ticks (1);
            expect (! c.isMonitorOn());
            c.setMonitorOn (true); // the toggle retries at once
            expect (c.isMonitorOn());
            expect (! hasNotice());

            c.getMonitorForTests().simulateDeviceLostForTest();
            ticks (1);
            c.setMonitorOn (false); // stop waiting
            expect (! hasNotice());
            ticks (60);
            expect (! c.isMonitorOn());
            c.shutdown();
        }

        platformSettings();
    }

private:
    /** Output level (dBFS RMS of the last 0.15 s) of 0.3 s of voice through the controller's processor. */
    static float voiceLevel (AppController& c)
    {
        auto& vp = c.getProcessorForTests();
        const auto in = synthVoice (0.3, 7, kSr, false);
        std::vector<float> out (in.size(), 0.0f);
        for (size_t pos = 0; pos + kBlock <= in.size(); pos += kBlock) vp.process (in.data() + pos, out.data() + pos, nullptr, kBlock);
        return rmsDb (out.data() + out.size() / 2, int (out.size() / 2));
    }

    static bool hasNonAscii (const juce::String& s)
    {
        for (int i = 0; i < s.length(); ++i)
            if (s[i] > 127) return true;
        return false;
    }

    // ---------------------------------------------------------------- wave 4: detailed platform settings (INTERFACES.md §7.3)
    void platformSettings()
    {
        auto ticks = [] (AppController& c, int k) { for (int i = 0; i < k; ++i) c.tickForTests(); };
        auto u8 = [] (const char* s) { return juce::String::fromUTF8 (s); };

        beginTest ("pushToTalk: off = the key does nothing; 1 = muted unless held; 2 = muted while held; pttReleaseMs tail after the key is up");
        {
            freshDataDir();
            AppController c (false);
            c.startup();
            bool keyDown = false;
            Hotkeys::keyStateForTests = [&keyDown] (int vk) { return vk == 'T' && keyDown; };
            juce::String why;
            expect (c.setHotkey ("pushToTalk", 2, 'T', why), why);
            auto isOpen = [&] { return voiceLevel (c) > -50.0f; };
            auto isMuted = [&] { return voiceLevel (c) < -80.0f; };
            expect (isOpen(), "default: talking");

            keyDown = true;
            c.performAction ("pushToTalk"); // pushToTalk = 0
            ticks (c, 1);
            expect (isOpen());
            keyDown = false;
            ticks (c, 10);
            expect (isOpen());

            c.updateSettings ([] (Settings& s) { s.pushToTalk = 1; s.pttReleaseMs = 200.0f; });
            expect (isMuted(), "mode 1: muted while the key is up");
            keyDown = true;
            c.performAction ("pushToTalk");
            expect (isOpen(), "mode 1: talking at once");
            ticks (c, 20);
            expect (isOpen(), "mode 1: still talking while held");
            keyDown = false;
            ticks (c, 1); // the release is seen: 200 ms tail = 6 ticks
            ticks (c, 5);
            expect (isOpen(), "mode 1: the tail still talks");
            ticks (c, 1);
            expect (isMuted(), "mode 1: muted after the tail");
            expect (! c.isMicMuted(), "push-to-talk is not the user's mute");

            keyDown = true;
            c.performAction ("pushToTalk");
            c.setMicMuted (true);
            expect (isMuted(), "the user's mute wins while held");
            c.setMicMuted (false);
            expect (isOpen());
            keyDown = false;

            c.updateSettings ([] (Settings& s) { s.pushToTalk = 2; s.pttReleaseMs = 0.0f; });
            ticks (c, 2);
            expect (isOpen(), "mode 2: talking while the key is up");
            keyDown = true;
            c.performAction ("pushToTalk");
            expect (isMuted(), "mode 2: muted while held");
            keyDown = false;
            ticks (c, 1);
            expect (isOpen(), "mode 2: pttReleaseMs 0 = back at the next tick");

            c.updateSettings ([] (Settings& s) { s.pushToTalk = 1; });
            expect (isMuted());
            c.updateSettings ([] (Settings& s) { s.pushToTalk = 0; });
            expect (isOpen(), "off again: never muted by push-to-talk");
            Hotkeys::keyStateForTests = nullptr;
            c.shutdown();
        }

        beginTest ("startup: pushToTalk 1 starts muted; startupVoice ON / OFF / as left; startupLastPreset; logs older than logKeepDays deleted");
        {
            freshDataDir();
            Settings s;
            s.pushToTalk = 1;
            s.voiceChangerOn = false;
            s.startupVoice = 1;
            s.currentPresetId = "natural-clear";
            s.startupLastPreset = false;
            s.logKeepDays = 3;
            expect (saveSettings (s, paths::settingsFile()));
            const auto logs = paths::logsDir();
            logs.createDirectory();
            const auto oldLog = logs.getChildFile ("old.log"), newLog = logs.getChildFile ("koeloom.log");
            oldLog.replaceWithText ("x");
            newLog.replaceWithText ("x");
            oldLog.setLastModificationTime (juce::Time::getCurrentTime() - juce::RelativeTime::days (4));
            {
                AppController c (false);
                c.startup();
                expect (voiceLevel (c) < -80.0f);
                expect (c.isVoiceChangerOn());
                expect (c.getCurrentPreset().id == "natural-asis", c.getCurrentPreset().id);
                expect (! oldLog.exists() && newLog.exists());
                c.shutdown();
            }
            s = {};
            s.voiceChangerOn = true;
            s.startupVoice = 2;
            s.currentPresetId = "natural-clear";
            expect (saveSettings (s, paths::settingsFile()));
            {
                AppController c (false);
                c.startup();
                expect (! c.isVoiceChangerOn());
                expect (c.getCurrentPreset().id == "natural-clear", c.getCurrentPreset().id);
                c.shutdown();
            }
            s = {};
            s.voiceChangerOn = false; // startupVoice 0 = as left
            expect (saveSettings (s, paths::settingsFile()));
            {
                AppController c (false);
                c.startup();
                expect (! c.isVoiceChangerOn());
                c.shutdown();
            }
        }

        beginTest ("favoriteWrap: next / prev wrap by default and stop at the ends when off");
        {
            freshDataDir();
            AppController c (false);
            c.startup();
            juce::String why;
            for (auto* id : { "natural-asis", "natural-clear", "natural-calm" }) expect (c.toggleFavorite (id, why), why);
            c.loadFavorite (2);
            c.performAction ("favoriteNext");
            expect (c.getCurrentPreset().id == "natural-asis", "wraps to the first");
            c.performAction ("favoritePrev");
            expect (c.getCurrentPreset().id == "natural-calm", "wraps to the last");
            c.updateSettings ([] (Settings& st) { st.favoriteWrap = false; });
            c.performAction ("favoriteNext");
            expect (c.getCurrentPreset().id == "natural-calm", "stays at the last");
            c.loadFavorite (0);
            c.performAction ("favoritePrev");
            expect (c.getCurrentPreset().id == "natural-asis", "stays at the first");
            c.performAction ("favoriteNext");
            expect (c.getCurrentPreset().id == "natural-clear");
            c.shutdown();
        }

        beginTest ("hotkeyToasts: hotkeys stay silent by default; on, they say what changed");
        {
            freshDataDir();
            AppController c (false);
            c.startup();
            juce::String why;
            expect (c.toggleFavorite ("natural-clear", why), why);
            c.takeToasts();
            c.performAction ("voiceToggle");
            c.performAction ("favorite.1");
            expect (c.takeToasts().isEmpty());
            c.updateSettings ([] (Settings& st) { st.hotkeyToasts = true; });
            c.loadPreset ("natural-asis");
            c.takeToasts();
            c.performAction ("voiceToggle");
            c.performAction ("muteToggle");
            c.performAction ("favorite.1");
            const auto t = c.takeToasts();
            juce::StringArray expected;
            expected.add (u8 ("ボイチェン ON"));
            expected.add (u8 ("マイクミュート ON"));
            expected.add (u8 ("プリセット: ") + c.getCurrentPreset().name);
            expect (t == expected, t.joinIntoString (" | "));
            c.shutdown();
        }

        beginTest ("closeAction: 0 = the close button leaves it in the tray, 1 = quit (through the usual quit request)");
        {
            freshDataDir();
            AppController c (false);
            c.startup();
            int quits = 0;
            c.onQuitRequest = [&quits] { ++quits; };
            c.performAction ("closeWindow");
            expectEquals (quits, 0);
            c.updateSettings ([] (Settings& st) { st.closeAction = 1; });
            c.performAction ("closeWindow");
            expectEquals (quits, 1);
            c.shutdown();
        }

        beginTest ("reconnectSeconds: the monitor device is retried every n seconds");
        {
            freshDataDir();
            AppController c (false);
            c.startup();
            c.updateSettings ([] (Settings& st) { st.reconnectSeconds = 3; });
            c.setMonitorDevice ("Headphones (Test)");
            c.setMonitorOn (true);
            c.getMonitorForTests().simulateDeviceLostForTest();
            ticks (c, 1);
            expect (! c.isMonitorOn());
            ticks (c, 85); // the 1 s default would have retried after 30
            expect (! c.isMonitorOn());
            ticks (c, 5);
            expect (c.isMonitorOn());
            c.shutdown();
        }

        beginTest ("export / import: settings.json's shape, back exactly; invalid JSON or another JSON refused in Japanese, the file untouched; clamped");
        {
            freshDataDir();
            AppController c (false);
            c.startup();
            juce::String why;
            expect (c.toggleFavorite ("natural-clear", why), why);
            expect (c.setHotkey ("voiceToggle", 2, 'V', why), why);
            c.setInputDevice ("Mic (Test)");
            c.setInputGainDb (6.0f);
            c.updateSettings ([] (Settings& st) { st.pushToTalk = 2; st.logLevel = 2; st.darkTheme = false; st.reconnectSeconds = 4; });
            const auto file = paths::dataDir().getChildFile (u8 ("書き出し テスト.json"));
            juce::String error;
            expect (c.exportSettings (file, error), error);
            SettingsLoadResult lr;
            const auto exported = loadSettings (file, lr);
            expect (exported == c.getSettings() && lr.clampedKeys.isEmpty());

            c.resetSettings();
            c.updateSettings ([] (Settings& st) { st.monitorLatency = 0; });
            expect (! (c.getSettings() == exported));
            expect (c.importSettings (file, error), error);
            expect (c.getSettings() == exported);

            auto refused = [&] (const juce::File& f)
            {
                juce::String e;
                const auto before = c.getSettings();
                expect (! c.importSettings (f, e));
                expect (hasNonAscii (e), e);
                expect (c.getSettings() == before);
                return e;
            };
            const auto broken = paths::dataDir().getChildFile ("broken.json");
            broken.replaceWithText ("{ \"darkTheme\": true, oops");
            const auto e1 = refused (broken);
            expect (e1.contains (u8 ("壊れて")), e1);
            expect (broken.existsAsFile() && ! broken.getSiblingFile ("broken.json.bak").exists(), "the user's file is never moved");
            const auto preset = paths::dataDir().getChildFile ("preset.json");
            preset.replaceWithText ("{ \"name\": \"x\", \"chain\": [] }");
            expect (refused (preset).contains (u8 ("設定ファイルではありません")));
            expect (refused (paths::dataDir().getChildFile ("missing.json")).contains (u8 ("見つかりません")));

            const auto odd = paths::dataDir().getChildFile ("odd.json");
            odd.replaceWithText ("{ \"currentPresetId\": \"natural-asis\", \"logKeepDays\": 99, \"inputGainDb\": -100, \"pushToTalk\": 1 }");
            expect (c.importSettings (odd, error), error);
            expectEquals (c.getSettings().logKeepDays, Settings().logKeepDays);
            expectEquals (c.getSettings().inputGainDb, kInputGainDb.min);
            expectEquals (c.getSettings().pushToTalk, 1);
            expect (voiceLevel (c) < -80.0f, "imported push-to-talk is applied");
            c.shutdown();
        }

        beginTest ("reset: everything back to defaults except devices, favourites, hotkeys, setupDone, tourStep (and the current voice)");
        {
            freshDataDir();
            AppController c (false);
            c.startup();
            juce::String why;
            expect (c.toggleFavorite ("natural-clear", why), why);
            expect (c.setHotkey ("muteToggle", 2, 'M', why), why);
            c.setInputDevice ("Mic (Test)");
            c.setOutputDevice ("CABLE Input (VB-Audio Virtual Cable)");
            c.setMonitorDevice ("Headphones (Test)");
            c.loadPreset ("natural-calm");
            c.setInputGainDb (6.0f);
            c.setMonitorVolumeDb (-12.0f);
            c.updateSettings ([] (Settings& st)
            {
                st.setupDone = true; st.tourStep = 3; st.darkTheme = false; st.pushToTalk = 1; st.wasapiExclusive = true;
                st.inputChannel = 2; st.logKeepDays = 30; st.converterQuality = 2; st.uiScalePercent = 125; st.confirmOnExit = false;
            });
            const auto kept = c.getSettings();
            c.resetSettings();
            Settings expected;
            expected.hasSavedDevices = kept.hasSavedDevices;
            expected.inputDevice = kept.inputDevice;
            expected.outputDevice = kept.outputDevice;
            expected.monitorDevice = kept.monitorDevice;
            expected.favorites = kept.favorites;
            expected.hotkeys = kept.hotkeys;
            expected.setupDone = true;
            expected.tourStep = 3;
            expected.currentPresetId = "natural-calm";
            expect (c.getSettings() == expected);
            expect (c.getCurrentPreset().id == "natural-calm");
            expect (voiceLevel (c) > -50.0f, "push-to-talk off again");
            c.shutdown();
        }
    }
};

static AppControllerTests appControllerTests;
} // namespace koe

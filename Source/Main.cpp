#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_gui_extra/juce_gui_extra.h>

#include "App/AppController.h"
#include "App/Tray.h"
#include "Core/Paths.h"
#include "Tools/Calibrate.h"
#include "UI/MainComponent.h"
#include "UI/Theme.h"
#include "UI/Widgets.h"

#include <cstdlib>

namespace
{
juce::String u8 (const char* s) { return juce::String::fromUTF8 (s); }

bool hasArg (const juce::String& a) { return juce::JUCEApplicationBase::getCommandLineParameterArray().contains (a); }
juce::String argAfter (const juce::String& a)
{
    const auto args = juce::JUCEApplicationBase::getCommandLineParameterArray();
    const int i = args.indexOf (a);
    return i >= 0 ? args[i + 1].unquoted() : juce::String();
}

/** Tools never touch the user's real %APPDATA%\KoeLoom. */
void isolateDataDir (const char* name)
{
    if (juce::SystemStats::getEnvironmentVariable ("KOELOOM_DATA_DIR", {}).isNotEmpty()) return;
    auto tmp = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile (name);
    tmp.createDirectory();
    _putenv_s ("KOELOOM_DATA_DIR", tmp.getFullPathName().toRawUTF8());
}

/** KoeLoom.exe --run-tests [category]  -> test-results.txt next to the exe, exit code = failures (spec §9.4). */
int runUnitTests (const juce::String& category)
{
    isolateDataDir ("KoeLoomTests");
    struct Runner : juce::UnitTestRunner
    {
        juce::String log;
        void logMessage (const juce::String& m) override { log << m << "\n"; }
    } runner;
    runner.setAssertOnFailure (false);
    runner.setPassesAreLogged (false);

    if (category.isNotEmpty()) runner.runTestsInCategory (category);
    else
    {
        juce::Array<juce::UnitTest*> tests; // everything except ad-hoc diagnostics
        for (auto* t : juce::UnitTest::getAllTests())
            if (t->getCategory() != "Diag") tests.add (t);
        runner.runTests (tests);
    }

    int failures = 0, passes = 0;
    for (int i = 0; i < runner.getNumResults(); ++i)
    {
        failures += runner.getResult (i)->failures;
        passes += runner.getResult (i)->passes;
    }
    runner.log << "\n==== " << passes << " passed, " << failures << " failed ====\n";
    auto out = juce::File::getSpecialLocation (juce::File::currentExecutableFile).getSiblingFile ("test-results.txt");
    out.replaceWithText (runner.log, false, false, "\n");
    return failures;
}

// =============================================================================================
class MainWindow final : public juce::DocumentWindow, private juce::ChangeListener
{
public:
    MainWindow (koe::AppController& c, juce::LookAndFeel& lnf)
        : juce::DocumentWindow ("KoeLoom", koe::ui::Theme::colours().bg, juce::DocumentWindow::allButtons), controller (c)
    {
        setLookAndFeel (&lnf);
        setUsingNativeTitleBar (true);
        content = new koe::ui::MainComponent (controller); // applies S-03 拡大率 (Desktop scale) before the sizes below
        setContentOwned (content, false);
        setResizable (true, false);
        // F-14-11, in logical pixels: with 拡大率 the smallest window grows by the same factor (ui::minimumWindowSize)
        setResizeLimits (koe::ui::Theme::minWidth, koe::ui::Theme::minHeight, 10000, 10000);
        centreWithSize (koe::ui::Theme::defaultWidth, koe::ui::Theme::defaultHeight);
        setAlwaysOnTop (controller.getSettings().alwaysOnTop);
        controller.addChangeListener (this);
    }

    ~MainWindow() override
    {
        controller.removeChangeListener (this);
        setLookAndFeel (nullptr);
    }

    /** Closing the window keeps KoeLoom running in the tray (F-09-1), unless S-03 「× ボタンの動き」 is 終了する:
        then it quits the way the tray's 終了 does (the 「終了時に確認」 question included). */
    void closeButtonPressed() override
    {
        if (controller.getSettings().closeAction == 1 && controller.onQuitRequest) controller.onQuitRequest();
        else setVisible (false);
    }

    void changeListenerCallback (juce::ChangeBroadcaster*) override
    {
        if (isAlwaysOnTop() != controller.getSettings().alwaysOnTop) setAlwaysOnTop (controller.getSettings().alwaysOnTop); // S-03 常に手前
    }

    void applyRenderer (bool software)
    {
        // F-14-7: Direct2D by default, software rendering for weak integrated GPUs
        if (auto* peer = getPeer())
        {
            const auto engines = peer->getAvailableRenderingEngines();
            for (int i = 0; i < engines.size(); ++i)
                if (engines[i].containsIgnoreCase (software ? "Software" : "Direct2D")) peer->setCurrentRenderingEngine (i);
        }
    }

    void bringToFront()
    {
        setVisible (true);
        setMinimised (false);
        toFront (true);
    }

private:
    koe::AppController& controller;
    koe::ui::MainComponent* content = nullptr;
};
} // namespace

// =============================================================================================
class KoeLoomApplication final : public juce::JUCEApplication
{
public:
    const juce::String getApplicationName() override { return "KoeLoom"; }
    const juce::String getApplicationVersion() override { return KOELOOM_VERSION_STRING; }
    /** F-09-4: one instance; tools (tests, snapshots, calibration) may run beside a running app. */
    bool moreThanOneInstanceAllowed() override
    {
        // after an update the new exe waits here for the old one to exit, then takes the single-instance lock
        koe::updater::waitForOldProcess (juce::JUCEApplicationBase::getCommandLineParameterArray());
        return hasArg ("--run-tests") || hasArg ("--snapshot") || hasArg ("--calibrate-presets");
    }

    void anotherInstanceStarted (const juce::String&) override
    {
        if (window != nullptr) window->bringToFront();
    }

    void initialise (const juce::String&) override
    {
        if (hasArg ("--run-tests"))
        {
            auto category = argAfter ("--run-tests");
            setApplicationReturnValue (runUnitTests (category.startsWith ("--") ? juce::String() : category) == 0 ? 0 : 1);
            quit();
            return;
        }
        if (hasArg ("--snapshot"))
        {
            isolateDataDir ("KoeLoomSnapshots");
            const auto dir = juce::File::getCurrentWorkingDirectory().getChildFile (argAfter ("--snapshot"));
            setApplicationReturnValue (koe::ui::renderSnapshots (dir) > 0 ? 0 : 1);
            quit();
            return;
        }
        if (hasArg ("--calibrate-presets"))
        {
            isolateDataDir ("KoeLoomCalibrate");
            setApplicationReturnValue (koe::tools::calibratePresets (argAfter ("--calibrate-presets")));
            quit();
            return;
        }

        // ---- the app ----
        koe::paths::logsDir().createDirectory();
        logger.reset (new juce::FileLogger (koe::paths::logsDir().getChildFile ("koeloom.log"), "KoeLoom " + getApplicationVersion(), 512 * 1024));
        juce::Logger::setCurrentLogger (logger.get());

        controller = std::make_unique<koe::AppController> (true);
        controller->startup();
        koe::ui::Theme::setDark (controller->getSettings().darkTheme);
        lnf = std::make_unique<koe::ui::KoeLookAndFeel>();
        juce::LookAndFeel::setDefaultLookAndFeel (lnf.get());

        window = std::make_unique<MainWindow> (*controller, *lnf);
        tray = std::make_unique<TrayHolder> (*controller);
        controller->onShowWindowRequest = [this] { if (window != nullptr) window->bringToFront(); };
        controller->onQuitRequest = [this] { requestQuitFromTray(); };
        controller->addChangeListener (tray.get());
        koe::updater::removeOldExe (juce::File::getSpecialLocation (juce::File::currentExecutableFile)); // a finished update

        const bool hidden = controller->getSettings().startMinimized || hasArg ("--autostart"); // F-09-2
        window->setVisible (! hidden);
        if (! hidden) window->applyRenderer (controller->getSettings().softwareRenderer);
    }

    void shutdown() override
    {
        if (controller != nullptr)
        {
            controller->removeChangeListener (tray.get());
            controller->shutdown();
        }
        tray.reset();
        window.reset();
        controller.reset();
        juce::LookAndFeel::setDefaultLookAndFeel (nullptr);
        lnf.reset();
        juce::Logger::setCurrentLogger (nullptr);
        logger.reset();
    }

    /** Windows shutdown / sign-out: never block it with a dialog (F-09-6). */
    void systemRequestedQuit() override { quit(); }

private:
    struct TrayHolder final : juce::ChangeListener
    {
        explicit TrayHolder (koe::AppController& c) : icon (c) {}
        void changeListenerCallback (juce::ChangeBroadcaster*) override { icon.refresh(); }
        koe::TrayIcon icon;
    };

    /** F-09-6: confirm before quitting while the virtual mic is in use. */
    void requestQuitFromTray()
    {
        const auto& s = controller->getSettings();
        const bool cable = koe::AppController::isCableInputName (s.outputDevice) && controller->getStatus().running;
        if (! cable || ! s.confirmOnExit)
        {
            quit();
            return;
        }
        auto* aw = new juce::AlertWindow (u8 ("KoeLoom を終了しますか"),
                                          u8 ("終了すると、仮想マイクに声が届かなくなります。使っているアプリ（Discord、ゲーム内ボイス、会議アプリ）の入力デバイスを、元のマイクに戻してください。"),
                                          juce::MessageBoxIconType::WarningIcon);
        auto* dontAsk = new juce::ToggleButton (u8 ("今後は表示しない"));
        dontAsk->setSize (260, 28);
        aw->addCustomComponent (dontAsk);
        aw->addButton (u8 ("終了する"), 1, juce::KeyPress (juce::KeyPress::returnKey));
        aw->addButton (u8 ("キャンセル"), 0, juce::KeyPress (juce::KeyPress::escapeKey));
        if (window != nullptr) window->bringToFront();
        aw->enterModalState (true, juce::ModalCallbackFunction::create ([this, aw, dontAsk] (int result)
        {
            if (dontAsk->getToggleState() && result == 1) controller->updateSettings ([] (koe::Settings& st) { st.confirmOnExit = false; });
            delete dontAsk;
            juce::ignoreUnused (aw);
            if (result == 1) quit();
        }), true);
    }

    std::unique_ptr<juce::FileLogger> logger;
    std::unique_ptr<koe::AppController> controller;
    std::unique_ptr<koe::ui::KoeLookAndFeel> lnf;
    std::unique_ptr<MainWindow> window;
    std::unique_ptr<TrayHolder> tray;
};

START_JUCE_APPLICATION (KoeLoomApplication)

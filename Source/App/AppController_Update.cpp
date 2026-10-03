// Updates from GitHub Releases (INTERFACES.md §7.4, owner wave4/update). OFF by default: no network at all
// unless settings.autoUpdate is on or the user presses 「今すぐ確認」 (F-11-4 / AC-27 hold by default).
// Never restarts by itself: a ready update replaces the exe when the app quits (finishUpdateOnQuit), and only
// 「今すぐ再起動」 (applyUpdateNow) quits and starts the new version.
#include "App/AppController.h"

namespace koe
{
namespace
{
juce::String u8 (const char* s) { return juce::String::fromUTF8 (s); }
constexpr int kAutoCheckTicks = 90; // ~3 s after startup (30 Hz timer)

using US = AppController::UpdateState::Status;
static_assert (int (US::idle) == int (Updater::Status::idle) && int (US::ready) == int (Updater::Status::ready)
               && int (US::failed) == int (Updater::Status::failed), "the two status enums share their order");
} // namespace

void AppController::setUpdaterForTests (std::unique_ptr<Updater> u)
{
    updater = std::move (u);
    shownUpdateStatus = Updater::Status::idle;
}

void AppController::checkForUpdates (bool userInitiated)
{
    if (! userInitiated && ! settings.autoUpdate) return;
    if (updater == nullptr)
    {
        if (! allowDevices) return; // snapshots / UI tests never reach the network
        updater = std::make_unique<Updater> (Updater::httpsNet(), juce::File::getSpecialLocation (juce::File::currentExecutableFile));
    }
    if (updater->isBusy() || updater->getState().status == Updater::Status::ready) return;
    updater->start (KOELOOM_VERSION_STRING, settings.updateIncludePrerelease, settings.updateSkippedVersion);
    sendChangeMessage();
}

AppController::UpdateState AppController::getUpdateState() const
{
    UpdateState r;
    if (updater == nullptr) return r;
    const auto s = updater->getState();
    r.status = US (int (s.status));
    r.version = s.version;
    r.releaseUrl = s.releaseUrl;
    r.progress = s.progress;
    r.error = s.error;
    return r;
}

void AppController::applyUpdateNow()
{
    if (updater == nullptr || updater->getState().status != Updater::Status::ready) return;
    restartAfterUpdate = true;
    if (allowDevices) juce::JUCEApplicationBase::quit(); // the app's shutdown() -> shutdown() -> finishUpdateOnQuit()
}

void AppController::skipUpdateVersion()
{
    if (updater == nullptr) return;
    const auto version = updater->getState().version;
    if (version.isEmpty()) return;
    updater->discard(); // stops a download, deletes KoeLoom.exe.new: nothing is replaced at quit
    shownUpdateStatus = Updater::Status::idle;
    removeNotice ("update.ready");
    removeNotice ("update.failed");
    updateSettings ([version] (Settings& s) { s.updateSkippedVersion = version; });
}

void AppController::pollUpdater()
{
    if (! updateAutoChecked && ticks >= kAutoCheckTicks)
    {
        updateAutoChecked = true;
        checkForUpdates (false); // nothing unless settings.autoUpdate
    }
    if (updater == nullptr) return;
    const auto s = updater->getState();
    if (s.status == shownUpdateStatus) return; // download progress: the UI polls getUpdateState()
    shownUpdateStatus = s.status;
    removeNotice ("update.ready");
    removeNotice ("update.failed");
    if (s.status == Updater::Status::ready)
        addNotice ({ "update.ready", NoticeLevel::info, "v" + s.version + u8 (" に更新できます。次の起動から新しい版になります。"), true,
                     u8 ("今すぐ再起動"), "update.apply" });
    else if (s.status == Updater::Status::failed && s.manualInstall)
        addNotice ({ "update.failed", NoticeLevel::warning, s.error, true, u8 ("Releases を開く"), "update.openReleases" });
    sendChangeMessage();
}

void AppController::finishUpdateOnQuit()
{
    if (updater == nullptr) return;
    const auto s = updater->getState();
    if (s.status == Updater::Status::ready)
    {
        const auto exe = updater->getExe();
        juce::String error;
        if (updater::swapInNewExe (exe, error)) juce::Logger::writeToLog ("update: replaced the exe with v" + s.version);
        else juce::Logger::writeToLog ("update: " + error);
        // after a failed swap this starts the old version again, which is still what the user asked for
        if (restartAfterUpdate && allowDevices && ! updater::launchUpdated (exe, error)) juce::Logger::writeToLog ("update: " + error);
    }
    updater.reset(); // cancels a running check / download (its partial file is deleted)
}
} // namespace koe

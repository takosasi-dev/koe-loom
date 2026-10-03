#pragma once

// Updates from GitHub Releases (INTERFACES.md §7.4). The free functions are pure or work on the files
// they are given (tests use a temp folder); Updater runs one check + download on a background thread
// through a Net that tests replace with a fake (tests never touch the network).

#include <juce_core/juce_core.h>

#include <atomic>
#include <functional>
#include <mutex>
#include <thread>

namespace koe::updater
{
juce::String releasesApiUrl();   // https://api.github.com/repos/takosasi-dev/koe-loom/releases
juce::String releasesPageUrl();  // https://github.com/takosasi-dev/koe-loom/releases

// ---- versions (semver 2.0 precedence; a leading "v" is ignored, build metadata too) ----
bool isValidVersion (const juce::String& v);
/** <0 if a < b, 0 if equal, >0 if a > b. An invalid version sorts below every valid one. */
int compareVersions (const juce::String& a, const juce::String& b);

// ---- the releases JSON (array from GET /repos/{owner}/{repo}/releases) ----
juce::String assetNameFor (const juce::String& version); // "KoeLoom-v0.2.0-win-x64.exe"

struct Release
{
    juce::String version;      // "0.2.0" (tag without the v)
    juce::String pageUrl;      // html_url
    bool prerelease = false;
    juce::String assetUrl;     // browser_download_url of the exe
    juce::int64 assetSize = 0;
    juce::String sha256;       // lowercase hex from the asset's "digest", empty if GitHub gave none
    juce::String sha256Url;    // the "<asset>.sha256" asset of the same release, empty if none
};

/** The newest release newer than currentVersion that carries the exe asset. Drafts are skipped,
    pre-releases only with includePrerelease, skippedVersion never. False with error empty: nothing newer;
    false with error set (Japanese): the JSON could not be read. */
bool pickRelease (const juce::String& json, const juce::String& currentVersion, bool includePrerelease,
                  const juce::String& skippedVersion, Release& out, juce::String& error);

/** The first 64-hex-digit token of a .sha256 file ("<hex>  <name>" or just "<hex>"), lowercase; empty if none. */
juce::String parseSha256Text (const juce::String& content);
juce::String sha256OfFile (const juce::File& file);     // lowercase hex, empty if unreadable
bool verifyFile (const juce::File& file, const juce::String& expectedHex);

// ---- the files next to the exe ----
juce::File newFileFor (const juce::File& exe);          // KoeLoom.exe.new (the download)
juce::File oldFileFor (const juce::File& exe);          // KoeLoom.exe.old (the replaced version)
bool canWriteTo (const juce::File& dir);
/** exe -> exe.old, exe.new -> exe (a running exe may be renamed, not overwritten). If the second step
    fails the first is undone. False + error (Japanese) on failure; exe is then still the old version. */
bool swapInNewExe (const juce::File& exe, juce::String& error);

// ---- start-up helpers for Main.cpp ----
/** "--wait-pid <pid>" in the arguments: waits (up to timeoutMs) for that process, i.e. the previous
    version that started this one, to exit, so this instance can take JUCE's single-instance lock. */
void waitForOldProcess (const juce::StringArray& args, int timeoutMs = 20000);
/** Deletes exe.old left by an update (retries ~2 s while the old process lets go of it). */
void removeOldExe (const juce::File& exe);
/** Starts exe with "--updated --wait-pid <this process>". */
bool launchUpdated (const juce::File& exe, juce::String& error);
} // namespace koe::updater

namespace koe
{
/** One check + download at a time on a background thread. Thread-safe getState(). */
class Updater
{
public:
    struct Net
    {
        /** GET url (at most maxBytes) into out. False + error (Japanese) on failure. */
        std::function<bool (const juce::String& url, juce::int64 maxBytes, juce::MemoryBlock& out, juce::String& error)> get;
        /** GET url into dest (at most maxBytes); progress (0..1) returns false to cancel. */
        std::function<bool (const juce::String& url, const juce::File& dest, juce::int64 maxBytes,
                            const std::function<bool (float)>& progress, juce::String& error)> download;
    };
    static Net httpsNet();   // juce::URL (WinINet), User-Agent + Accept headers, timeouts

    enum class Status { idle, checking, upToDate, downloading, ready, failed };
    struct State
    {
        Status status = Status::idle;
        juce::String version, releaseUrl, error;
        float progress = 0.0f;
        bool manualInstall = false;   // failed because the exe folder is not writable: open the Releases page
    };

    Updater (Net net, juce::File exe);
    ~Updater();   // cancels and joins

    /** Starts a check (+ download) unless one is running. */
    void start (const juce::String& currentVersion, bool includePrerelease, const juce::String& skippedVersion);
    State getState() const;
    bool isBusy() const;
    /** Cancels a running job, deletes the download, back to idle. */
    void discard();
    const juce::File& getExe() const { return exe; }

private:
    void run (juce::String currentVersion, bool includePrerelease, juce::String skippedVersion);
    void set (const std::function<void (State&)>& change);
    void stopAndJoin();

    Net net;
    juce::File exe;
    mutable std::mutex lock;
    State state;
    std::thread worker;
    std::atomic<bool> stop { false }, busy { false };
};
} // namespace koe

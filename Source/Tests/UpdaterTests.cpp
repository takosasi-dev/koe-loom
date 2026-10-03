// Updates from GitHub Releases (INTERFACES.md §7.4, wave4/update). Category "Platform".
// Never touches the network (a fake Updater::Net), the real exe or dist: every exe here is a text file in
// %TEMP%\KoeLoomTests\updater-tests. Never starts a process.

#include "App/AppController.h"
#include "Core/Paths.h"
#include "Platform/Updater.h"

#include <atomic>
#include <iterator>

#ifndef NOMINMAX
 #define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace koe
{
namespace
{
juce::File freshDir()
{
    auto data = paths::dataDir();
    if (data.getFullPathName().contains ("KoeLoomTests")) data.deleteRecursively();
    auto d = data.getChildFile ("updater-tests");
    d.createDirectory();
    return d;
}

juce::String asset (const juce::String& version, const juce::String& digest = {})
{
    const auto name = updater::assetNameFor (version);
    juce::String a = "{\"name\":\"" + name + "\",\"size\":123,\"browser_download_url\":\"https://example.invalid/" + name + "\"";
    if (digest.isNotEmpty()) a << ",\"digest\":\"sha256:" << digest << "\"";
    return a + "}";
}

juce::String release (const juce::String& tag, bool pre, bool draft, const juce::String& assets)
{
    return "{\"tag_name\":\"" + tag + "\",\"prerelease\":" + (pre ? "true" : "false") + ",\"draft\":" + (draft ? "true" : "false")
           + ",\"html_url\":\"https://example.invalid/tag/" + tag + "\",\"assets\":[" + assets + "]}";
}

juce::String array (const juce::StringArray& items) { return "[" + items.joinIntoString (",") + "]"; }

const juce::String kHashA = "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"; // sha256 ("abc")
const juce::String kHashZero = juce::String::repeatedString ("0", 64);

/** A GitHub that answers from memory and counts what it was asked. */
struct FakeGitHub
{
    juce::String json, shaText;
    std::string payload = "new exe";
    bool failGet = false;
    std::atomic<int> gets { 0 }, downloads { 0 };

    Updater::Net net()
    {
        Updater::Net n;
        n.get = [this] (const juce::String& url, juce::int64, juce::MemoryBlock& out, juce::String& error)
        {
            ++gets;
            if (failGet) { error = "offline"; return false; }
            const auto text = url.endsWith (".sha256") ? shaText : json;
            out.replaceAll (text.toRawUTF8(), text.getNumBytesAsUTF8());
            return true;
        };
        n.download = [this] (const juce::String&, const juce::File& dest, juce::int64, const std::function<bool (float)>& progress, juce::String&)
        {
            ++downloads;
            progress (0.5f);
            dest.replaceWithData (payload.data(), payload.size());
            return progress (1.0f);
        };
        return n;
    }
};

bool waitDone (const Updater& u)
{
    for (int i = 0; i < 1000 && u.isBusy(); ++i) juce::Thread::sleep (5);
    return ! u.isBusy();
}

juce::String hashOf (const std::string& bytes, const juce::File& scratch)
{
    scratch.replaceWithData (bytes.data(), bytes.size());
    return updater::sha256OfFile (scratch);
}
} // namespace

class UpdaterTests final : public juce::UnitTest
{
public:
    UpdaterTests() : juce::UnitTest ("Updater (GitHub Releases)", "Platform") {}

    void runTest() override
    {
        using namespace updater;

        beginTest ("Versions: semver precedence, a leading v, pre-release suffixes, invalid ones");
        {
            expect (compareVersions ("0.2.0", "0.1.0") > 0);
            expect (compareVersions ("0.10.0", "0.9.9") > 0);
            expect (compareVersions ("1.0.0", "0.99.99") > 0);
            expectEquals (compareVersions ("v0.2.0", "0.2.0"), 0);
            expectEquals (compareVersions ("1.0.0+build.5", "1.0.0"), 0);
            const char* chain[] = { "1.0.0-alpha", "1.0.0-alpha.1", "1.0.0-alpha.beta", "1.0.0-beta", "1.0.0-beta.2",
                                    "1.0.0-beta.11", "1.0.0-rc.1", "1.0.0" }; // semver.org §11
            for (int i = 0; i + 1 < int (std::size (chain)); ++i)
            {
                expect (compareVersions (chain[i], chain[i + 1]) < 0, juce::String (chain[i]) + " < " + chain[i + 1]);
                expect (compareVersions (chain[i + 1], chain[i]) > 0);
            }
            for (auto* bad : { "", "1.0", "1.0.0.0", "abc", "1.0.0-", "1.x.0", "1.0.0-a..b" })
                expect (! isValidVersion (bad), bad);
            expect (compareVersions ("garbage", "0.0.1") < 0);
            expectEquals (assetNameFor ("v0.2.0"), juce::String ("KoeLoom-v0.2.0-win-x64.exe"));
        }

        beginTest ("Release JSON: pre-release only, mixed, draft, missing asset, digest vs .sha256, skipped, invalid");
        {
            Release r;
            juce::String err;
            const auto preOnly = array ({ release ("v0.2.0", true, false, asset ("0.2.0", kHashA.toUpperCase())) });
            expect (pickRelease (preOnly, "0.1.0", true, {}, r, err));
            expectEquals (r.version, juce::String ("0.2.0"));
            expect (r.prerelease);
            expectEquals (r.sha256, kHashA, "the digest, lowercased");
            expectEquals (r.assetUrl, juce::String ("https://example.invalid/KoeLoom-v0.2.0-win-x64.exe"));
            expectEquals (r.pageUrl, juce::String ("https://example.invalid/tag/v0.2.0"));
            expect (! pickRelease (preOnly, "0.1.0", false, {}, r, err), "stable only: nothing");
            expect (err.isEmpty());

            const auto mixed = array ({ release ("v0.1.0", true, false, asset ("0.1.0", kHashA)),
                                        release ("v0.2.0", false, false, asset ("0.2.0", kHashA)),
                                        release ("v0.3.0-beta.1", false, false, asset ("0.3.0-beta.1", kHashA)), // suffix = pre-release
                                        release ("v0.2.5", true, false, asset ("0.2.5", kHashA)) });
            expect (pickRelease (mixed, "0.1.0", true, {}, r, err));
            expectEquals (r.version, juce::String ("0.3.0-beta.1"));
            expect (pickRelease (mixed, "0.1.0", false, {}, r, err));
            expectEquals (r.version, juce::String ("0.2.0"));
            expect (pickRelease (mixed, "0.1.0", true, "0.3.0-beta.1", r, err), "skipped version");
            expectEquals (r.version, juce::String ("0.2.5"));
            expect (! pickRelease (mixed, "0.3.0", true, {}, r, err), "nothing newer");
            expect (err.isEmpty());

            const auto draft = array ({ release ("v0.4.0", false, true, asset ("0.4.0", kHashA)), release ("v0.2.0", false, false, asset ("0.2.0", kHashA)) });
            expect (pickRelease (draft, "0.1.0", true, {}, r, err));
            expectEquals (r.version, juce::String ("0.2.0"), "the draft is skipped");

            const auto missing = array ({ release ("v0.5.0", false, false, "{\"name\":\"KoeLoom-v0.5.0-linux.zip\",\"browser_download_url\":\"x\"}"),
                                          release ("v0.2.0", false, false, asset ("0.2.0")) });
            expect (pickRelease (missing, "0.1.0", true, {}, r, err));
            expectEquals (r.version, juce::String ("0.2.0"), "a release without the exe is skipped");

            const auto shaAsset = array ({ release ("v0.2.0", false, false,
                                                    asset ("0.2.0") + ",{\"name\":\"KoeLoom-v0.2.0-win-x64.exe.sha256\",\"browser_download_url\":\"https://example.invalid/x.sha256\"}") });
            expect (pickRelease (shaAsset, "0.1.0", true, {}, r, err));
            expect (r.sha256.isEmpty(), "no digest");
            expectEquals (r.sha256Url, juce::String ("https://example.invalid/x.sha256"));

            const auto badDigest = array ({ release ("v0.2.0", false, false, asset ("0.2.0", "1234")) });
            expect (pickRelease (badDigest, "0.1.0", true, {}, r, err) && r.sha256.isEmpty(), "a malformed digest is ignored");

            for (auto* bad : { "{", "{\"message\":\"Not Found\"}", "" })
            {
                expect (! pickRelease (bad, "0.1.0", true, {}, r, err), bad);
                expect (err.isNotEmpty(), "an unreadable answer is an error, not 'up to date'");
            }

            expectEquals (parseSha256Text (kHashA.toUpperCase() + "  KoeLoom-v0.2.0-win-x64.exe\r\n"), kHashA);
            expectEquals (parseSha256Text (kHashA + " *KoeLoom.exe"), kHashA);
            expect (parseSha256Text ("not a hash").isEmpty());
        }

        beginTest ("Hash: SHA-256 of a file, verify good / bad / missing");
        {
            const auto dir = freshDir();
            const auto f = dir.getChildFile ("abc.bin");
            f.replaceWithText ("abc", false, false, nullptr);
            expectEquals (sha256OfFile (f), kHashA);
            expect (verifyFile (f, kHashA));
            expect (verifyFile (f, kHashA.toUpperCase()));
            expect (! verifyFile (f, kHashZero));
            expect (! verifyFile (f, {}));
            expect (! verifyFile (dir.getChildFile ("missing.bin"), kHashA));
            expect (sha256OfFile (dir.getChildFile ("missing.bin")).isEmpty());
        }

        beginTest ("Swap: exe -> .old, .new -> exe, a stale .old replaced, missing .new, rollback when the 2nd step fails");
        {
            const auto dir = freshDir();
            const auto exe = dir.getChildFile ("KoeLoom.exe");
            exe.replaceWithText ("v1");
            newFileFor (exe).replaceWithText ("v2");
            oldFileFor (exe).replaceWithText ("v0");
            juce::String err;
            expect (swapInNewExe (exe, err), err);
            expectEquals (exe.loadFileAsString(), juce::String ("v2"));
            expectEquals (oldFileFor (exe).loadFileAsString(), juce::String ("v1"));
            expect (! newFileFor (exe).exists());

            err.clear();
            expect (! swapInNewExe (exe, err), "no .new");
            expect (err.isNotEmpty());
            expectEquals (exe.loadFileAsString(), juce::String ("v2"));

            newFileFor (exe).replaceWithText ("v3");
            {
                // hold .new open without delete sharing: it cannot be renamed, so the swap must undo step 1
                auto h = CreateFileW (newFileFor (exe).getFullPathName().toWideCharPointer(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                      OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
                expect (h != INVALID_HANDLE_VALUE);
                err.clear();
                expect (! swapInNewExe (exe, err), "rename of .new blocked");
                CloseHandle (h);
            }
            expect (err.isNotEmpty());
            expectEquals (exe.loadFileAsString(), juce::String ("v2"), "rolled back");
            expectEquals (newFileFor (exe).loadFileAsString(), juce::String ("v3"));

            removeOldExe (exe);
            expect (! oldFileFor (exe).exists());
            removeOldExe (exe); // nothing to do
            expect (canWriteTo (dir));
            expect (! canWriteTo (exe.getChildFile ("sub")), "a 'folder' that is a file");

            const auto t0 = juce::Time::getMillisecondCounter();
            waitForOldProcess ({ "--updated" });
            waitForOldProcess ({ "--updated", "--wait-pid", juce::String (juce::int64 (GetCurrentProcessId())) }); // never waits for itself
            waitForOldProcess ({ "--wait-pid", "4000000000" });                                                 // no such process
            expect (juce::Time::getMillisecondCounter() - t0 < 2000);
        }

        beginTest ("Updater: download, verify, ready; .sha256 asset; bad hash; up to date; offline; reuse; discard; unwritable folder");
        {
            const auto dir = freshDir();
            const auto exe = dir.getChildFile ("KoeLoom.exe");
            exe.replaceWithText ("old exe");
            FakeGitHub gh;
            const auto good = hashOf (gh.payload, dir.getChildFile ("scratch.bin"));
            gh.json = array ({ release ("v0.2.0", true, false, asset ("0.2.0", good)) });
            {
                Updater u (gh.net(), exe);
                u.start ("0.1.0", true, {});
                expect (waitDone (u));
                auto s = u.getState();
                expect (s.status == Updater::Status::ready, s.error);
                expectEquals (s.version, juce::String ("0.2.0"));
                expectEquals (s.progress, 1.0f);
                expectEquals (newFileFor (exe).loadFileAsString(), juce::String (gh.payload));
                expectEquals (gh.downloads.load(), 1);

                u.start ("0.1.0", true, {}); // the verified download from before is reused
                expect (waitDone (u));
                expect (u.getState().status == Updater::Status::ready);
                expectEquals (gh.downloads.load(), 1);

                u.discard();
                expect (u.getState().status == Updater::Status::idle);
                expect (! newFileFor (exe).exists());
            }
            gh.gets = 0;
            gh.json = array ({ release ("v0.2.0", false, false, asset ("0.2.0") + ",{\"name\":\"KoeLoom-v0.2.0-win-x64.exe.sha256\",\"browser_download_url\":\"https://example.invalid/a.sha256\"}") });
            gh.shaText = good + "  KoeLoom-v0.2.0-win-x64.exe\n";
            {
                Updater u (gh.net(), exe);
                u.start ("0.1.0", true, {});
                expect (waitDone (u));
                expect (u.getState().status == Updater::Status::ready, u.getState().error);
                expectEquals (gh.gets.load(), 2, "the releases list and the .sha256 file");
                u.discard();
            }
            gh.shaText = kHashZero;
            {
                Updater u (gh.net(), exe);
                u.start ("0.1.0", true, {});
                expect (waitDone (u));
                const auto s = u.getState();
                expect (s.status == Updater::Status::failed && s.error.isNotEmpty());
                expect (! newFileFor (exe).exists(), "a bad download is deleted");
                expect (! s.manualInstall);
            }
            {
                Updater u (gh.net(), exe);
                u.start ("0.2.0", true, {});
                expect (waitDone (u));
                expect (u.getState().status == Updater::Status::upToDate);
                u.start ("0.1.0", true, "0.2.0");
                expect (waitDone (u));
                expect (u.getState().status == Updater::Status::upToDate, "skipped version");
            }
            gh.failGet = true;
            {
                Updater u (gh.net(), exe);
                u.start ("0.1.0", true, {});
                expect (waitDone (u));
                const auto s = u.getState();
                expect (s.status == Updater::Status::failed);
                expect (s.error.contains ("offline"), s.error);
            }
            gh.failGet = false;
            gh.json = array ({ release ("v0.2.0", true, false, asset ("0.2.0", good)) });
            {
                Updater u (gh.net(), exe.getChildFile ("KoeLoom.exe")); // its folder is a file: not writable
                u.start ("0.1.0", true, {});
                expect (waitDone (u));
                const auto s = u.getState();
                expect (s.status == Updater::Status::failed && s.manualInstall);
                expect (s.releaseUrl.isNotEmpty(), "the Releases page to open instead");
            }
            expectEquals (exe.loadFileAsString(), juce::String ("old exe"), "the updater itself never replaces the exe");
        }

        beginTest ("AppController: autoUpdate off -> no fetch; on -> one check after startup, notice, skip; swap at quit");
        {
            for (bool autoOn : { false, true })
            {
                auto dir = freshDir();
                const auto exe = dir.getChildFile ("KoeLoom.exe");
                exe.replaceWithText ("old exe");
                FakeGitHub gh;
                gh.json = array ({ release ("v99.0.0", true, false, asset ("99.0.0", hashOf (gh.payload, dir.getChildFile ("scratch.bin")))) });
                Settings st;
                st.autoUpdate = autoOn;
                expect (saveSettings (st, paths::settingsFile()));
                AppController c (false);
                c.startup();
                c.setUpdaterForTests (std::make_unique<Updater> (gh.net(), exe));
                for (int i = 0; i < 120; ++i) c.tickForTests();
                for (int i = 0; i < 1000 && c.getUpdateState().status == AppController::UpdateState::Status::checking; ++i) juce::Thread::sleep (5);
                for (int i = 0; i < 1000 && c.getUpdateState().status == AppController::UpdateState::Status::downloading; ++i) juce::Thread::sleep (5);
                c.tickForTests();
                auto hasNotice = [&c] (const char* key, juce::String* actionId = nullptr, juce::String* text = nullptr)
                {
                    for (auto& n : c.getNotices())
                        if (n.key == key)
                        {
                            if (actionId != nullptr) *actionId = n.actionId;
                            if (text != nullptr) *text = n.text;
                            return true;
                        }
                    return false;
                };
                if (! autoOn)
                {
                    expectEquals (gh.gets.load(), 0, "autoUpdate off: not a single request");
                    expect (c.getUpdateState().status == AppController::UpdateState::Status::idle);
                    c.checkForUpdates (false);
                    expectEquals (gh.gets.load(), 0, "an automatic check is ignored while off");
                    c.checkForUpdates (true); // 「今すぐ確認」 works even when off
                    for (int i = 0; i < 1000 && c.getUpdateState().status != AppController::UpdateState::Status::ready; ++i) juce::Thread::sleep (5);
                    c.tickForTests();
                    expectEquals (gh.gets.load(), 1);
                    expect (hasNotice ("update.ready"));
                    c.skipUpdateVersion();
                    expectEquals (c.getSettings().updateSkippedVersion, juce::String ("99.0.0"));
                    expect (! hasNotice ("update.ready"));
                    expect (c.getUpdateState().status == AppController::UpdateState::Status::idle);
                    expect (! newFileFor (exe).exists(), "skipping deletes the download");
                    c.shutdown();
                    expectEquals (exe.loadFileAsString(), juce::String ("old exe"), "nothing to replace after a skip");
                    continue;
                }
                expectEquals (gh.gets.load(), 1, "one automatic check");
                const auto s = c.getUpdateState();
                expect (s.status == AppController::UpdateState::Status::ready, s.error);
                expectEquals (s.version, juce::String ("99.0.0"));
                juce::String action, text;
                expect (hasNotice ("update.ready", &action, &text));
                expectEquals (action, juce::String ("update.apply"));
                expectEquals (text, juce::String::fromUTF8 ("v99.0.0 に更新できます。次の起動から新しい版になります。"));
                for (int i = 0; i < 120; ++i) c.tickForTests();
                expectEquals (gh.gets.load(), 1, "only once per run");
                c.applyUpdateNow(); // no devices: marks the restart, never quits or starts a process
                expectEquals (exe.loadFileAsString(), juce::String ("old exe"), "replaced only when the app quits");
                c.shutdown();
                expectEquals (exe.loadFileAsString(), juce::String (gh.payload), "swapped at quit");
                expectEquals (oldFileFor (exe).loadFileAsString(), juce::String ("old exe"));
                expect (! newFileFor (exe).exists());
            }
        }
    }
};

static UpdaterTests updaterTests;
} // namespace koe

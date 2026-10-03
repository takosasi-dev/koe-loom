#include "Platform/Updater.h"

#ifndef NOMINMAX
 #define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <bcrypt.h>
#pragma comment(lib, "bcrypt.lib")

namespace koe::updater
{
namespace
{
juce::String u8 (const char* s) { return juce::String::fromUTF8 (s); }

juce::String stripV (juce::String v)
{
    v = v.trim();
    return v.startsWithIgnoreCase ("v") ? v.substring (1) : v;
}

bool allDigits (const juce::String& s) { return s.isNotEmpty() && s.containsOnly ("0123456789"); }

struct Semver
{
    juce::int64 core[3] {};
    juce::StringArray pre;   // empty = a normal release
};

bool parse (const juce::String& text, Semver& out)
{
    auto v = stripV (text).upToFirstOccurrenceOf ("+", false, false);
    const auto core = v.upToFirstOccurrenceOf ("-", false, false);
    const auto parts = juce::StringArray::fromTokens (core, ".", "");
    if (parts.size() != 3 || core.endsWithChar ('.')) return false;
    for (int i = 0; i < 3; ++i)
    {
        if (! allDigits (parts[i]) || parts[i].length() > 9) return false;
        out.core[i] = parts[i].getLargeIntValue();
    }
    out.pre.clear();
    if (v.containsChar ('-'))
    {
        const auto pre = v.fromFirstOccurrenceOf ("-", false, false);
        out.pre = juce::StringArray::fromTokens (pre, ".", "");
        if (pre.isEmpty() || pre.endsWithChar ('.') || pre.contains ("..")) return false;
        for (auto& id : out.pre)
            if (id.isEmpty() || ! id.containsOnly ("0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz-")) return false;
    }
    return true;
}

bool isHex64 (const juce::String& s) { return s.length() == 64 && s.containsOnly ("0123456789abcdefABCDEF"); }
} // namespace

juce::String releasesApiUrl() { return "https://api.github.com/repos/takosasi-dev/koe-loom/releases"; }
juce::String releasesPageUrl() { return "https://github.com/takosasi-dev/koe-loom/releases"; }

bool isValidVersion (const juce::String& v)
{
    Semver s;
    return parse (v, s);
}

int compareVersions (const juce::String& a, const juce::String& b)
{
    Semver x, y;
    const bool okA = parse (a, x), okB = parse (b, y);
    if (! okA || ! okB) return int (okA) - int (okB);
    for (int i = 0; i < 3; ++i)
        if (x.core[i] != y.core[i]) return x.core[i] < y.core[i] ? -1 : 1;
    // semver §11: a pre-release is lower than the release; then identifier by identifier
    if (x.pre.isEmpty() || y.pre.isEmpty()) return int (x.pre.isEmpty()) - int (y.pre.isEmpty());
    for (int i = 0; i < juce::jmin (x.pre.size(), y.pre.size()); ++i)
    {
        const auto& p = x.pre[i];
        const auto& q = y.pre[i];
        const bool np = allDigits (p), nq = allDigits (q);
        if (np && nq)
        {
            if (p.getLargeIntValue() != q.getLargeIntValue()) return p.getLargeIntValue() < q.getLargeIntValue() ? -1 : 1;
        }
        else if (np != nq) return np ? -1 : 1;   // numeric identifiers are lower
        else if (const int c = p.compare (q); c != 0) return c < 0 ? -1 : 1;
    }
    return x.pre.size() == y.pre.size() ? 0 : (x.pre.size() < y.pre.size() ? -1 : 1);
}

juce::String assetNameFor (const juce::String& version) { return "KoeLoom-v" + stripV (version) + "-win-x64.exe"; }

bool pickRelease (const juce::String& json, const juce::String& currentVersion, bool includePrerelease,
                  const juce::String& skippedVersion, Release& out, juce::String& error)
{
    error.clear();
    const auto root = juce::JSON::parse (json);
    if (! root.isArray())
    {
        error = u8 ("リリースの情報を読み取れませんでした。");
        return false;
    }
    bool found = false;
    for (auto& r : *root.getArray())
    {
        if (! r.isObject() || bool (r["draft"])) continue;
        const auto version = stripV (r["tag_name"].toString());
        if (! isValidVersion (version) || compareVersions (version, currentVersion) <= 0) continue;
        if (skippedVersion.isNotEmpty() && compareVersions (version, skippedVersion) == 0) continue;
        const bool pre = bool (r["prerelease"]) || version.upToFirstOccurrenceOf ("+", false, false).containsChar ('-');
        if (pre && ! includePrerelease) continue;
        if (found && compareVersions (version, out.version) <= 0) continue;

        Release rel;
        rel.version = version;
        rel.pageUrl = r["html_url"].toString();
        rel.prerelease = pre;
        const auto name = assetNameFor (version);
        if (auto* assets = r["assets"].getArray())
            for (auto& a : *assets)
            {
                if (a["name"].toString() == name)
                {
                    rel.assetUrl = a["browser_download_url"].toString();
                    rel.assetSize = juce::int64 (a["size"]);
                    const auto digest = a["digest"].toString();
                    if (digest.startsWithIgnoreCase ("sha256:") && isHex64 (digest.substring (7))) rel.sha256 = digest.substring (7).toLowerCase();
                }
                else if (a["name"].toString() == name + ".sha256")
                    rel.sha256Url = a["browser_download_url"].toString();
            }
        if (rel.assetUrl.isEmpty()) continue;   // a release without the Windows exe
        out = rel;
        found = true;
    }
    return found;
}

juce::String parseSha256Text (const juce::String& content)
{
    for (auto& t : juce::StringArray::fromTokens (content, " \t\r\n", ""))
        if (isHex64 (t)) return t.toLowerCase();
    return {};
}

juce::String sha256OfFile (const juce::File& file)
{
    juce::FileInputStream in (file);
    if (! in.openedOk()) return {};
    BCRYPT_ALG_HANDLE alg = nullptr;
    if (BCryptOpenAlgorithmProvider (&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0) return {};
    juce::String result;
    BCRYPT_HASH_HANDLE hash = nullptr;
    if (BCryptCreateHash (alg, &hash, nullptr, 0, nullptr, 0, 0) == 0)
    {
        constexpr int chunk = 1 << 16;
        juce::HeapBlock<unsigned char> buf (chunk);
        bool ok = true;
        for (int n = 0; ok && (n = in.read (buf.get(), chunk)) > 0;)
            ok = BCryptHashData (hash, buf.get(), ULONG (n), 0) == 0;
        unsigned char digest[32] {};
        if (ok && BCryptFinishHash (hash, digest, sizeof (digest), 0) == 0) result = juce::String::toHexString (digest, int (sizeof (digest)), 0);
        BCryptDestroyHash (hash);
    }
    BCryptCloseAlgorithmProvider (alg, 0);
    return result;
}

bool verifyFile (const juce::File& file, const juce::String& expectedHex)
{
    return isHex64 (expectedHex) && sha256OfFile (file) == expectedHex.toLowerCase();
}

juce::File newFileFor (const juce::File& exe) { return exe.getSiblingFile (exe.getFileName() + ".new"); }
juce::File oldFileFor (const juce::File& exe) { return exe.getSiblingFile (exe.getFileName() + ".old"); }

bool canWriteTo (const juce::File& dir)
{
    const auto probe = dir.getNonexistentChildFile ("KoeLoom-write-test", ".tmp", false);
    if (! probe.create()) return false;
    probe.deleteFile();
    return true;
}

bool swapInNewExe (const juce::File& exe, juce::String& error)
{
    const auto fresh = newFileFor (exe), old = oldFileFor (exe);
    if (! fresh.existsAsFile())
    {
        error = u8 ("新しい版のファイルがありません。");
        return false;
    }
    if (old.exists() && ! old.deleteFile())
    {
        error = u8 ("前の古い版のファイルを消せません。");
        return false;
    }
    if (! exe.moveFileTo (old))
    {
        error = u8 ("今の版のファイルを移動できません。");
        return false;
    }
    if (fresh.moveFileTo (exe)) return true;
    old.moveFileTo (exe); // undo the first step
    error = u8 ("新しい版のファイルを置けませんでした。元の版のままです。");
    return false;
}

void waitForOldProcess (const juce::StringArray& args, int timeoutMs)
{
    const int i = args.indexOf ("--wait-pid");
    const auto pid = i >= 0 ? DWORD (args[i + 1].getLargeIntValue()) : DWORD (0);
    if (pid == 0 || pid == GetCurrentProcessId()) return;
    if (auto h = OpenProcess (SYNCHRONIZE, FALSE, pid)) // null: already gone
    {
        WaitForSingleObject (h, DWORD (timeoutMs));
        CloseHandle (h);
    }
}

void removeOldExe (const juce::File& exe)
{
    const auto old = oldFileFor (exe);
    for (int i = 0; i < 20; ++i)
    {
        if (! old.exists() || old.deleteFile()) return;
        juce::Thread::sleep (100);
    }
    juce::Logger::writeToLog ("update: could not delete " + old.getFileName());
}

bool launchUpdated (const juce::File& exe, juce::String& error)
{
    if (exe.startAsProcess ("--updated --wait-pid " + juce::String (juce::int64 (GetCurrentProcessId())))) return true;
    error = u8 ("新しい版を起動できませんでした。");
    return false;
}
} // namespace koe::updater

// ================================================================================================ Updater
namespace koe
{
namespace
{
juce::String u8 (const char* s) { return juce::String::fromUTF8 (s); }

std::unique_ptr<juce::InputStream> openUrl (const juce::String& url, const char* accept, juce::String& error)
{
    int status = 0;
    juce::String headers;
    headers << "User-Agent: KoeLoom/" << KOELOOM_VERSION_STRING << "\r\nAccept: " << accept << "\r\n";
    auto in = juce::URL (url).createInputStream (juce::URL::InputStreamOptions (juce::URL::ParameterHandling::inAddress)
                                                     .withExtraHeaders (headers)
                                                     .withConnectionTimeoutMs (15000)
                                                     .withNumRedirectsToFollow (5)
                                                     .withStatusCode (&status));
    if (in != nullptr && status == 200) return in;
    error = status > 0 ? "HTTP " + juce::String (status) : u8 ("接続できません");
    return nullptr;
}
} // namespace

Updater::Net Updater::httpsNet()
{
    Net n;
    n.get = [] (const juce::String& url, juce::int64 maxBytes, juce::MemoryBlock& out, juce::String& error)
    {
        auto in = openUrl (url, "application/vnd.github+json", error);
        if (in == nullptr) return false;
        out.reset();
        in->readIntoMemoryBlock (out, juce::pointer_sized_int (maxBytes + 1));
        if (juce::int64 (out.getSize()) <= maxBytes) return true;
        error = u8 ("応答が大きすぎます");
        return false;
    };
    n.download = [] (const juce::String& url, const juce::File& dest, juce::int64 maxBytes,
                     const std::function<bool (float)>& progress, juce::String& error)
    {
        auto in = openUrl (url, "application/octet-stream", error);
        if (in == nullptr) return false;
        const auto total = in->getTotalLength();
        if (total > maxBytes) { error = u8 ("ファイルが大きすぎます"); return false; }
        dest.deleteFile();
        juce::FileOutputStream out (dest);
        if (! out.openedOk()) { error = u8 ("ファイルを書き込めません"); return false; }
        juce::HeapBlock<char> buf (1 << 16);
        juce::int64 done = 0;
        for (int n = 0; (n = in->read (buf.get(), 1 << 16)) > 0;)
        {
            done += n;
            if (done > maxBytes) { error = u8 ("ファイルが大きすぎます"); return false; }
            if (! out.write (buf.get(), size_t (n))) { error = u8 ("ファイルを書き込めません"); return false; }
            if (! progress (total > 0 ? float (double (done) / double (total)) : 0.0f)) { error = u8 ("中止しました"); return false; }
        }
        if (total > 0 && done != total) { error = u8 ("途中で切れました"); return false; }
        return true;
    };
    return n;
}

Updater::Updater (Net n, juce::File e) : net (std::move (n)), exe (std::move (e)) {}

Updater::~Updater() { stopAndJoin(); }

void Updater::stopAndJoin()
{
    stop = true;
    if (worker.joinable()) worker.join();
    stop = false;
}

void Updater::start (const juce::String& currentVersion, bool includePrerelease, const juce::String& skippedVersion)
{
    if (busy) return;
    if (worker.joinable()) worker.join();   // the previous job has finished
    busy = true;
    set ([] (State& s) { s = {}; s.status = Status::checking; });
    worker = std::thread ([this, currentVersion, includePrerelease, skippedVersion] { run (currentVersion, includePrerelease, skippedVersion); });
}

Updater::State Updater::getState() const
{
    std::lock_guard<std::mutex> g (lock);
    return state;
}

bool Updater::isBusy() const { return busy; }

void Updater::discard()
{
    stopAndJoin();
    busy = false;
    updater::newFileFor (exe).deleteFile();
    set ([] (State& s) { s = {}; });
}

void Updater::set (const std::function<void (State&)>& change)
{
    std::lock_guard<std::mutex> g (lock);
    change (state);
}

void Updater::run (juce::String currentVersion, bool includePrerelease, juce::String skippedVersion)
{
    constexpr juce::int64 kMaxJsonBytes = 4 * 1024 * 1024, kMaxShaBytes = 4096, kMaxExeBytes = 256 * 1024 * 1024; // the exe is ~11 MB
    struct Done { std::atomic<bool>& b; ~Done() { b = false; } } done { busy };
    auto fail = [this] (const juce::String& e, bool manual = false)
    {
        if (stop) return;   // discarded: discard() resets the state
        set ([&] (State& s) { s.status = Status::failed; s.error = e; s.manualInstall = manual; });
    };

    juce::MemoryBlock json;
    juce::String err;
    if (! net.get (updater::releasesApiUrl(), kMaxJsonBytes, json, err))
        return fail (u8 ("更新を確認できませんでした（") + err + u8 ("）。"));
    updater::Release rel;
    if (! updater::pickRelease (json.toString(), currentVersion, includePrerelease, skippedVersion, rel, err))
    {
        if (err.isNotEmpty()) return fail (err);
        if (! stop) set ([] (State& s) { s.status = Status::upToDate; });
        return;
    }
    set ([&] (State& s) { s.version = rel.version; s.releaseUrl = rel.pageUrl.isNotEmpty() ? rel.pageUrl : updater::releasesPageUrl(); });

    if (! updater::canWriteTo (exe.getParentDirectory()))
        return fail (u8 ("KoeLoom のフォルダに書き込めないため、自動で更新できません。Releases のページから新しい版を入れてください。"), true);
    if (rel.assetSize > kMaxExeBytes) return fail (u8 ("新しい版のファイルが大きすぎます。"));

    auto sha = rel.sha256;
    if (sha.isEmpty() && rel.sha256Url.isNotEmpty())
    {
        juce::MemoryBlock text;
        if (! net.get (rel.sha256Url, kMaxShaBytes, text, err)) return fail (u8 ("確認用のハッシュを取得できませんでした（") + err + u8 ("）。"));
        sha = updater::parseSha256Text (text.toString());
    }
    if (sha.isEmpty()) return fail (u8 ("新しい版の確認用のハッシュが見つかりません。"));

    const auto dest = updater::newFileFor (exe);
    if (! updater::verifyFile (dest, sha))   // a download from an earlier run is reused
    {
        set ([] (State& s) { s.status = Status::downloading; s.progress = 0.0f; });
        auto progress = [this] (float p)
        {
            set ([p] (State& s) { s.progress = p; });
            return ! stop.load();
        };
        if (! net.download (rel.assetUrl, dest, kMaxExeBytes, progress, err))
        {
            dest.deleteFile();
            return fail (u8 ("新しい版をダウンロードできませんでした（") + err + u8 ("）。"));
        }
        if (! updater::verifyFile (dest, sha))
        {
            dest.deleteFile();
            return fail (u8 ("ダウンロードしたファイルが壊れています（ハッシュが一致しません）。"));
        }
    }
    if (! stop) set ([] (State& s) { s.status = Status::ready; s.progress = 1.0f; });
}
} // namespace koe

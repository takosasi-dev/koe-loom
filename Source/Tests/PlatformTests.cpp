// Platform tests (platform): hotkey names, the autostart command, the monitor's adaptive resampler (R-2)
// and the soundboard. Category "Platform".
// Never opens an audio device, registers a hotkey, writes the registry or makes a sound.

#include "Core/Constants.h"
#include "Core/Paths.h"
#include "Dsp/Building.h"
#include "Engine/MonitorOutput.h"
#include "Engine/Soundboard.h"
#include "Platform/AutoStart.h"
#include "Platform/Hotkeys.h"
#include "Tests/Mp3Fixture.h"
#include "Tests/TestUtil.h"

#include <juce_audio_formats/juce_audio_formats.h>

#include <cmath>
#include <complex>
#include <functional>

#ifndef NOMINMAX
 #define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace koe
{
namespace
{
using namespace test;
using Status = SoundSlotState::Status;

juce::String u8 (const char* s) { return juce::String::fromUTF8 (s); }

bool hasNonAscii (const juce::String& s)
{
    for (int i = 0; i < s.length(); ++i)
        if (s[i] > 127) return true;
    return false;
}

juce::File testDir()
{
    auto d = paths::dataDir().getChildFile ("platform-tests");
    d.createDirectory();
    return d;
}

// ============================================================================ monitor helpers
constexpr double kMainRate = 48000.0;
constexpr int kMainBlock = 480;
constexpr double kToneHz = 500.0;   // a whole number of periods per 10 ms block, at 48 and 44.1 kHz
constexpr float kToneAmp = 0.5f;
constexpr double kTwoPi = juce::MathConstants<double>::twoPi;

float toneAt (long long mainSample)
{
    return kToneAmp * float (std::sin (kTwoPi * std::fmod (kToneHz * double (mainSample) / kMainRate, 1.0)));
}

struct DriftStats
{
    int underruns = 0, overruns = 0;
    double ppmLastMinute = 0.0;                      // mean getRatioPpm() over the last 60 s
    double maxResidual = 0.0;                        // sine predictor residual after 1 s (a skip, repeat or click)
    float minPeak = 1.0e9f, maxPeak = 0.0f;          // per-block peak after 1 s
    double delayMinMs = 1.0e9, delayMaxMs = -1.0e9;  // delay through the FIFO after 1 s, relative to its value at 1 s
    double steadyMinMs = 1.0e9, steadyMaxMs = -1.0e9; // ... over the last 5 minutes
};

/** Main device (48 kHz, 480-sample pushes) and monitor device (monRate nominal, its clock off by ppm) run
    offline on their own clocks, both callbacks late by up to jitterMs. The main side pushes a 500 Hz sine. */
DriftStats runDrift (double monRate, double ppm, int monBlock, double seconds, double jitterMs)
{
    DriftStats st;
    MonitorOutput m;
    m.prepareForTest (kMainRate, monRate, monBlock);
    m.setVolumeDb (0.0f);
    m.setEnabled (true);

    const double monActual = monRate * (1.0 + ppm * 1.0e-6);  // the monitor device's real clock
    const double mainPeriod = kMainBlock / kMainRate, monPeriod = monBlock / monActual;
    const double wOut = kTwoPi * kToneHz / monActual;         // rad per output sample, real time
    const double c2 = 2.0 * std::cos (wOut);
    const auto rot = std::polar (1.0, -wOut);
    std::vector<float> in (static_cast<size_t> (kMainBlock)), out (static_cast<size_t> (monBlock));
    juce::Random rng (20261003);
    auto late = [&] { return rng.nextDouble() * jitterMs * 1.0e-3; };
    long long pushed = 0, nMain = 0, nMon = 0, ppmCount = 0;
    double jMain = late(), jMon = late();
    double y1 = 0.0, y2 = 0.0, lastPhase = 0.0, unwrapped = 0.0, ppmSum = 0.0;
    bool tracking = false;

    while (double (nMon) * monPeriod < seconds)
    {
        if (double (nMain + 1) * mainPeriod + jMain <= double (nMon) * monPeriod + jMon)
        {
            for (int i = 0; i < kMainBlock; ++i) in[size_t (i)] = toneAt (pushed + i);
            m.push (in.data(), kMainBlock);
            pushed += kMainBlock;
            ++nMain;
            jMain = late();
            continue;
        }

        m.pullForTest (out.data(), monBlock);
        const double t0 = double (nMon) * monPeriod; // device clock time of this block's first sample
        ++nMon;
        jMon = late();
        const bool settled = t0 >= 1.0;
        auto p = std::polar (1.0, -kTwoPi * std::fmod (kToneHz * t0, 1.0));
        std::complex<double> z {};
        float peak = 0.0f;
        for (int j = 0; j < monBlock; ++j)
        {
            const double y = out[size_t (j)];
            if (settled) st.maxResidual = std::max (st.maxResidual, std::abs (y - c2 * y1 + y2));
            y2 = y1;
            y1 = y;
            z += y * p;
            p *= rot;
            peak = std::max (peak, std::abs (out[size_t (j)]));
        }
        if (! settled) continue;
        st.minPeak = std::min (st.minPeak, peak);
        st.maxPeak = std::max (st.maxPeak, peak);

        // the tone's phase against the device clock moves exactly as the delay through the FIFO does
        const double ph = std::arg (z);
        if (tracking)
        {
            double d = ph - lastPhase;
            d -= kTwoPi * std::round (d / kTwoPi);
            unwrapped += d;
        }
        tracking = true;
        lastPhase = ph;
        const double delayMs = -unwrapped / (kTwoPi * kToneHz) * 1000.0;
        st.delayMinMs = std::min (st.delayMinMs, delayMs);
        st.delayMaxMs = std::max (st.delayMaxMs, delayMs);
        if (t0 >= seconds - 300.0)
        {
            st.steadyMinMs = std::min (st.steadyMinMs, delayMs);
            st.steadyMaxMs = std::max (st.steadyMaxMs, delayMs);
        }
        if (t0 >= seconds - 60.0)
        {
            ppmSum += m.getRatioPpm();
            ++ppmCount;
        }
    }
    st.underruns = m.getUnderruns();
    st.overruns = m.getOverruns();
    st.ppmLastMinute = ppmCount > 0 ? ppmSum / double (ppmCount) : 0.0;
    return st;
}

/** Main and monitor both at 48 kHz, driven block by block; records the monitor output. */
struct Lockstep
{
    MonitorOutput m;
    long long pushed = 0;
    std::vector<float> rec, buf = std::vector<float> (size_t (kMainBlock));

    Lockstep()
    {
        m.prepareForTest (kMainRate, kMainRate, kMainBlock);
        m.setVolumeDb (0.0f);
        m.setEnabled (true);
        rec.reserve (48000 * 10);
    }
    void push()
    {
        for (int i = 0; i < kMainBlock; ++i) buf[size_t (i)] = toneAt (pushed + i);
        m.push (buf.data(), kMainBlock);
        pushed += kMainBlock;
    }
    void pull()
    {
        m.pullForTest (buf.data(), kMainBlock);
        rec.insert (rec.end(), buf.begin(), buf.end());
    }
    void run (int blocks)
    {
        for (int b = 0; b < blocks; ++b)
        {
            push();
            pull();
        }
    }
};

float maxStep (const std::vector<float>& v, size_t from)
{
    float m = 0.0f;
    for (size_t i = std::max<size_t> (from, 1); i < v.size(); ++i) m = std::max (m, std::abs (v[i] - v[i - 1]));
    return m;
}

float peakIn (const std::vector<float>& v, size_t from, size_t to)
{
    float m = 0.0f;
    for (size_t i = from; i < std::min (to, v.size()); ++i) m = std::max (m, std::abs (v[i]));
    return m;
}

// ============================================================================ soundboard helpers
std::vector<float> dc (double seconds, float value, double sr = kSr)
{
    return std::vector<float> (size_t (std::llround (seconds * sr)), value);
}

/** Polls until the slot is no longer loading (10 s cap; a slot still loading then fails the test). */
SoundSlotState waitLoaded (Soundboard& sb, int slot)
{
    const auto until = juce::Time::getMillisecondCounterHiRes() + 10000.0;
    auto st = sb.getSlotState (slot);
    while (st.status == Status::loading && juce::Time::getMillisecondCounterHiRes() < until)
    {
        juce::Thread::sleep (2);
        st = sb.getSlotState (slot);
    }
    return st;
}

/** Runs this thread's message loop (the tests run on the message thread) until done() or 10 s. */
bool pumpUntil (const std::function<bool()>& done)
{
    const auto until = juce::Time::getMillisecondCounterHiRes() + 10000.0;
    while (! done())
    {
        if (juce::Time::getMillisecondCounterHiRes() > until) return false;
        MSG msg;
        while (PeekMessageW (&msg, nullptr, 0, 0, PM_REMOVE))
        {
            TranslateMessage (&msg);
            DispatchMessageW (&msg);
        }
        juce::Thread::sleep (5);
    }
    return true;
}

/** Renders the soundboard like the audio thread (480-sample blocks), counting allocations inside render(). */
struct SbRender
{
    Soundboard& sb;
    long long allocations = 0;
    std::vector<float> out, mon;

    void run (int n)
    {
        const size_t start = out.size();
        out.resize (start + size_t (n), 0.0f);
        mon.resize (start + size_t (n), 0.0f);
        for (int pos = 0; pos < n; pos += kMainBlock)
        {
            const int k = std::min (kMainBlock, n - pos);
            AllocationCounter ac;
            sb.render (out.data() + start + size_t (pos), mon.data() + start + size_t (pos), k);
            allocations += ac.count();
        }
    }
    void clear()
    {
        out.clear();
        mon.clear();
    }
};

/** Mono file through one of JUCE's writers (FLAC, Ogg Vorbis). */
bool writeWith (juce::AudioFormat& format, const juce::File& file, const std::vector<float>& samples, double sr, int bits, int quality)
{
    file.deleteFile();
    std::unique_ptr<juce::FileOutputStream> os (file.createOutputStream());
    if (os == nullptr) return false;
    std::unique_ptr<juce::AudioFormatWriter> w (format.createWriterFor (os.get(), sr, 1, bits, {}, quality));
    if (w == nullptr) return false;
    os.release();
    const float* ch[] = { samples.data() };
    return w->writeFromFloatArrays (ch, 1, int (samples.size()));
}

/** 2^(k-12): exact in float and 24-bit, so any sum of distinct slots tells which slots sound. */
float level (int k) { return std::ldexp (1.0f, k - 12); }

// ============================================================================ hotkeys / autostart
class PlatformNameTests final : public juce::UnitTest
{
public:
    PlatformNameTests() : juce::UnitTest ("Hotkey names and autostart command", "Platform") {}

    void runTest() override
    {
        beginTest ("Hotkeys::describe: Ctrl+Alt+Shift+Win order and readable key names");
        expectEquals (Hotkeys::describe (MOD_CONTROL | MOD_SHIFT, VK_F1), juce::String ("Ctrl+Shift+F1"));
        expectEquals (Hotkeys::describe (MOD_WIN | MOD_SHIFT | MOD_ALT | MOD_CONTROL, '5'), juce::String ("Ctrl+Alt+Shift+Win+5"));
        expectEquals (Hotkeys::describe (MOD_ALT, 'K'), juce::String ("Alt+K"));
        expectEquals (Hotkeys::describe (MOD_CONTROL | MOD_NOREPEAT, VK_F24), juce::String ("Ctrl+F24"));
        expectEquals (Hotkeys::describe (0, VK_NUMPAD7), u8 ("テンキー7"));
        expectEquals (Hotkeys::describe (MOD_CONTROL, VK_ADD), u8 ("Ctrl+テンキー+"));
        expectEquals (Hotkeys::describe (0, VK_DIVIDE), u8 ("テンキー/"));
        expectEquals (Hotkeys::describe (0, VK_DECIMAL), u8 ("テンキー."));
        expectEquals (Hotkeys::describe (MOD_SHIFT, VK_SPACE), juce::String ("Shift+Space"));
        expectEquals (Hotkeys::describe (0, VK_PRIOR), juce::String ("PageUp"));
        expectEquals (Hotkeys::describe (MOD_ALT, VK_NONCONVERT), u8 ("Alt+無変換"));
        expectEquals (Hotkeys::describe (0, VK_MEDIA_PLAY_PAUSE), u8 ("再生/一時停止"));
        // symbol keys come from the keyboard layout; these three are the same on US and JIS keyboards
        expectEquals (Hotkeys::describe (MOD_CONTROL, VK_OEM_COMMA), juce::String ("Ctrl+,"));
        expectEquals (Hotkeys::describe (0, VK_OEM_PERIOD), juce::String ("."));
        expectEquals (Hotkeys::describe (0, VK_OEM_MINUS), juce::String ("-"));
        for (int vk : { VK_OEM_1, VK_OEM_2, VK_OEM_3, VK_OEM_4, VK_OEM_5, VK_OEM_6, VK_OEM_7, VK_OEM_PLUS })
        {
            const auto d = Hotkeys::describe (0, vk);
            expect (d.length() == 1, "symbol key 0x" + juce::String::toHexString (vk) + " -> " + d);
        }
        expectEquals (Hotkeys::describe (MOD_CONTROL, 0), juce::String());
        expect (Hotkeys::describe (0, 0x07).startsWith ("0x"));

        beginTest ("Hotkeys::allActions: 39 ids in the header's order");
        const auto actions = Hotkeys::allActions();
        juce::StringArray expected { "voiceToggle", "muteToggle", "favoriteNext", "favoritePrev" };
        for (int i = 1; i <= 9; ++i) expected.add ("favorite." + juce::String (i));
        for (int i = 1; i <= 10; ++i) expected.add ("slot." + juce::String (i));
        for (int i = 1; i <= 12; ++i) expected.add ("sound." + juce::String (i));
        expected.addArray (juce::StringArray { "soundStopAll", "freezeToggle", "looperRecPlay", "looperClear" });
        expectEquals (actions.size(), 39);
        expect (actions == expected, actions.joinIntoString (","));

        beginTest ("Hotkeys::actionLabel: Japanese, distinct, never empty");
        juce::StringArray labels;
        for (auto& a : actions)
        {
            const auto l = Hotkeys::actionLabel (a);
            expect (l.isNotEmpty() && hasNonAscii (l), a + " -> " + l);
            labels.addIfNotAlreadyThere (l);
        }
        expectEquals (labels.size(), actions.size());
        expectEquals (Hotkeys::actionLabel ("voiceToggle"), u8 ("ボイチェン ON/OFF"));
        expectEquals (Hotkeys::actionLabel ("favorite.3"), u8 ("お気に入り 3"));
        expectEquals (Hotkeys::actionLabel ("slot.7"), u8 ("スロット 7 の ON/OFF"));

        beginTest ("autostart::commandFor: quoted full path + --autostart (spaces, Japanese)");
        expectEquals (autostart::commandFor (juce::File ("C:\\Program Files\\KoeLoom\\KoeLoom.exe")),
                      juce::String ("\"C:\\Program Files\\KoeLoom\\KoeLoom.exe\" --autostart"));
        const juce::File ja (u8 ("D:\\ツール\\声 ルーム (試用)\\KoeLoom.exe"));
        expectEquals (autostart::commandFor (ja), u8 ("\"D:\\ツール\\声 ルーム (試用)\\KoeLoom.exe\" --autostart"));
    }
};

// ============================================================================ monitor output
class PlatformMonitorTests final : public juce::UnitTest
{
public:
    PlatformMonitorTests() : juce::UnitTest ("Monitor output resampler", "Platform") {}

    void runTest() override
    {
        struct Case { double rate, ppm; int block; };
        for (auto c : { Case { 48000.0, 300.0, 480 }, Case { 48000.0, -300.0, 480 },
                        Case { 44100.0, 300.0, 441 }, Case { 44100.0, -300.0, 441 } })
        {
            beginTest ("R-2: 10 min, main 48 kHz vs monitor " + juce::String (c.rate, 0) + " Hz "
                       + (c.ppm > 0 ? "+" : "") + juce::String (c.ppm, 0) + " ppm, 3 ms callback jitter");
            const auto st = runDrift (c.rate, c.ppm, c.block, 600.0, 3.0);
            const double expectedPpm = (1.0 / (1.0 + c.ppm * 1.0e-6) - 1.0) * 1.0e6;
            logMessage ("    underruns " + juce::String (st.underruns) + ", overruns " + juce::String (st.overruns)
                        + ", ppm (last minute) " + juce::String (st.ppmLastMinute, 1) + " (clock " + juce::String (expectedPpm, 1) + ")"
                        + ", delay swing " + juce::String (st.delayMaxMs - st.delayMinMs, 2) + " ms over 1 s..10 min ("
                        + juce::String ((st.delayMaxMs - st.delayMinMs) * kMainRate / 1000.0, 0) + " samples), "
                        + juce::String (st.steadyMaxMs - st.steadyMinMs, 2) + " ms over the last 5 min"
                        + ", max residual " + juce::String (st.maxResidual, 6)
                        + ", peak " + juce::String (st.minPeak, 4) + ".." + juce::String (st.maxPeak, 4));
            expectEquals (st.underruns, 0);
            expectEquals (st.overruns, 0);
            expectWithinAbsoluteError (st.ppmLastMinute, expectedPpm, 30.0);
            // a dropped or repeated input sample leaves a residual of about A * 0.065 = 0.03
            expectLessThan (st.maxResidual, 0.005);
            expectGreaterThan (st.minPeak, 0.49f);
            expectLessThan (st.maxPeak, 0.51f);
            // same frequency in and out: the delay settles instead of running away
            expectLessThan (st.steadyMaxMs - st.steadyMinMs, 5.0);
        }

        beginTest ("an underrun is counted, faded out and back in (no click)");
        {
            Lockstep s;
            s.run (200);
            for (int i = 0; i < 10; ++i) s.pull(); // the main device stalls for 100 ms
            expectEquals (s.m.getUnderruns(), 1);
            s.run (200);
            expectEquals (s.m.getOverruns(), 0);
            // the sine itself steps by up to A * 2 pi 500 / 48000 = 0.033; a hard cut steps by up to 0.5
            expectLessThan (maxStep (s.rec, 4800), 0.045f);
            expectWithinAbsoluteError (peakIn (s.rec, s.rec.size() - 48000, s.rec.size()), kToneAmp, 0.01f);
        }

        beginTest ("an overrun is counted, faded, the surplus dropped");
        {
            Lockstep s;
            s.run (200);
            for (int i = 0; i < 120; ++i) s.push(); // the monitor device stalls for 1.2 s (the FIFO holds 1 s)
            expectGreaterThan (s.m.getOverruns(), 0);
            s.run (200);
            expectEquals (s.m.getUnderruns(), 0);
            expectLessThan (maxStep (s.rec, 4800), 0.045f);
            expectWithinAbsoluteError (peakIn (s.rec, s.rec.size() - 48000, s.rec.size()), kToneAmp, 0.01f);
        }

        beginTest ("on/off and volume ramp in about 30 ms");
        {
            Lockstep s;
            s.run (100);
            size_t from = s.rec.size();
            s.m.setEnabled (false);
            s.run (10);
            expectEquals (peakIn (s.rec, from + 1920, s.rec.size()), 0.0f); // silent 40 ms after off
            expectGreaterThan (peakIn (s.rec, from, from + 480), 0.1f);     // ... but not cut at once
            from = s.rec.size();
            s.m.setEnabled (true);
            s.run (10);
            expectWithinAbsoluteError (peakIn (s.rec, from + 1920, s.rec.size()), kToneAmp, 0.005f);
            from = s.rec.size();
            s.m.setVolumeDb (-6.0f);
            s.run (10);
            expectWithinAbsoluteError (peakIn (s.rec, from + 1920, s.rec.size()), kToneAmp * dsp::dbToGain (-6.0f), 0.005f);
            from = s.rec.size();
            s.m.setVolumeDb (-100.0f); // clamped to -40 dB
            s.run (10);
            expectWithinAbsoluteError (peakIn (s.rec, from + 1920, s.rec.size()), kToneAmp * dsp::dbToGain (-40.0f), 0.0005f);
            expectLessThan (maxStep (s.rec, 4800), 0.045f);
        }

        beginTest ("push / pull never allocate");
        {
            MonitorOutput m;
            m.prepareForTest (kMainRate, 44100.0, 441);
            m.setEnabled (true);
            std::vector<float> in (size_t (kMainBlock), 0.1f), out (441);
            AllocationCounter ac;
            for (int i = 0; i < 1000; ++i)
            {
                m.push (in.data(), kMainBlock);
                m.pullForTest (out.data(), 441);
            }
            expectEquals (ac.count(), 0LL);
        }

        beginTest ("monitor OFF: once faded out, push() stops filling the FIFO; ON again resumes cleanly");
        {
            Lockstep s;
            s.run (100);
            s.m.setEnabled (false);
            s.run (20); // fades out
            const int overrunsBefore = s.m.getOverruns();
            for (int b = 0; b < 300; ++b) s.push(); // 3 s with nothing pulling: 3x the FIFO if copied
            expectEquals (s.m.getOverruns(), overrunsBefore);
            s.m.setEnabled (true);
            const size_t from = s.rec.size();
            s.run (100);
            expectWithinAbsoluteError (peakIn (s.rec, from + 4800 * 5, s.rec.size()), kToneAmp, 0.01f);
            expectLessThan (maxStep (s.rec, from), 0.045f);
        }

        beginTest ("monitor device lost: reported once, not when closed on purpose");
        {
            MonitorOutput m;
            expect (! m.fetchDeviceLost());
            m.simulateDeviceLostForTest();
            expect (m.fetchDeviceLost());
            expect (! m.fetchDeviceLost());
            m.close();
            expect (! m.fetchDeviceLost());
        }
    }
};

// ============================================================================ soundboard
class PlatformSoundboardTests final : public juce::UnitTest
{
public:
    PlatformSoundboardTests() : juce::UnitTest ("Soundboard", "Platform") {}

    void runTest() override
    {
        const auto dir = testDir();
        loading (dir);
        formats (dir);
        totalLimit (dir);
        playback (dir);
        persistence (dir);
        dir.deleteRecursively();
    }

private:
    void loading (const juce::File& dir)
    {
        beginTest ("load -> ready (Japanese file name), onStateChanged on the message thread");
        const auto ok = dir.getChildFile (u8 ("効果音 テスト.wav"));
        expect (writeWav (ok, sine (1000.0, 1.0, 0.25f), 48000.0));
        {
            Soundboard sb;
            int calls = 0;
            bool onMessageThread = true;
            sb.onStateChanged = [&]
            {
                ++calls;
                onMessageThread = onMessageThread && juce::MessageManager::getInstance()->isThisTheMessageThread();
            };
            sb.assignFile (0, ok);
            expect (pumpUntil ([&] { return calls > 0 && sb.getSlotState (0).status != Status::loading; }), "no onStateChanged");
            expect (onMessageThread);
            const auto st = sb.getSlotState (0);
            expect (st.status == Status::ready, st.error);
            expectEquals (st.fileName, ok.getFileName());
            expectWithinAbsoluteError (st.lengthSeconds, 1.0, 1.0e-6);
            expect (! st.playing);
            expectEquals (sb.getTotalBytes(), (long long) (48000 * sizeof (float)));
        }

        beginTest ("E-13: over 60 s is refused, exactly 60 s is accepted");
        Soundboard sb;
        const auto f61 = dir.getChildFile ("61s.wav"), f60 = dir.getChildFile ("60s.wav");
        expect (writeWav (f61, silence (61.0, 8000.0), 8000.0));
        expect (writeWav (f60, silence (60.0, 8000.0), 8000.0));
        sb.assignFile (1, f61);
        sb.assignFile (2, f60);
        auto st = waitLoaded (sb, 1);
        expect (st.status == Status::error && st.error.contains ("60"), st.error);
        st = waitLoaded (sb, 2);
        expect (st.status == Status::ready, st.error);
        expectWithinAbsoluteError (st.lengthSeconds, 60.0, 1.0e-6);

        beginTest ("E-14: broken and 0-byte files are errors; E-12: a missing file is 'not found' and never plays");
        juce::MemoryBlock junk (4096);
        juce::Random rng (99);
        rng.fillBitsRandomly (junk.getData(), junk.getSize());
        const auto broken = dir.getChildFile ("broken.wav"), empty = dir.getChildFile ("empty.wav");
        expect (broken.replaceWithData (junk.getData(), junk.getSize()));
        empty.deleteFile();
        expect (empty.create() && empty.getSize() == 0);
        const auto missing = dir.getChildFile (u8 ("消えたファイル.wav"));
        missing.deleteFile();
        sb.assignFile (3, broken);
        sb.assignFile (4, empty);
        sb.assignFile (5, missing);
        st = waitLoaded (sb, 3);
        expect (st.status == Status::error && st.error.isNotEmpty(), st.error);
        st = waitLoaded (sb, 4);
        expect (st.status == Status::error && st.error.isNotEmpty(), st.error);
        st = waitLoaded (sb, 5);
        expect (st.status == Status::missing && st.error.contains (u8 ("見つかりません")), st.error);
        expectEquals (st.fileName, missing.getFileName());

        // a RIFF/WAVE header over garbage, and a valid WAV cut short: ready or error, never a crash
        juce::MemoryOutputStream riff;
        riff.write ("RIFF", 4);
        riff.writeInt (100000);
        riff.write ("WAVEfmt ", 8);
        riff.write (junk.getData(), 2000);
        const auto fakeRiff = dir.getChildFile ("fake-riff.wav"), truncated = dir.getChildFile ("truncated.wav");
        expect (fakeRiff.replaceWithData (riff.getData(), riff.getDataSize()));
        juce::MemoryBlock whole;
        expect (ok.loadFileAsData (whole));
        expect (truncated.replaceWithData (whole.getData(), 1000));
        sb.assignFile (6, fakeRiff);
        sb.assignFile (7, truncated);
        for (int s : { 6, 7 })
        {
            st = waitLoaded (sb, s);
            expect (st.status == Status::ready || st.status == Status::error, "slot " + juce::String (s));
        }

        SbRender r { sb };
        sb.trigger (5);
        sb.trigger (3);
        sb.trigger (4);
        r.run (4800);
        expectEquals (peakIn (r.out, 0, r.out.size()), 0.0f);
        expectEquals (r.allocations, 0LL);
    }

    void formats (const juce::File& dir)
    {
        // a 44.1 kHz file into the 48 kHz device rate: the tone must come out at 440 Hz (479 Hz if not resampled)
        const auto tone = sine (440.0, 1.0, 0.5f, 44100.0);
        const float toneRmsDb = 20.0f * std::log10 (0.5f / std::sqrt (2.0f));
        juce::FlacAudioFormat flac;
        juce::OggVorbisAudioFormat ogg;
        const auto wavFile = dir.getChildFile ("tone.wav"), flacFile = dir.getChildFile ("tone.flac"),
                   oggFile = dir.getChildFile ("tone.ogg"), mp3File = dir.getChildFile (u8 ("トーン.mp3"));
        expect (writeWav (wavFile, tone, 44100.0));
        expect (writeWith (flac, flacFile, tone, 44100.0, 24, 5));
        expect (writeWith (ogg, oggFile, tone, 44100.0, 16, 4)); // 128 kbps
        expect (mp3File.replaceWithData (kMp3Tone440, sizeof (kMp3Tone440)));

        struct Case { const char* name; juce::File file; double lengthTolerance; float levelTolerance; };
        // MP3 through Windows Media ignores LAME's gapless info: the Info frame, the encoder delay and the last
        // frame's padding are all counted, so this 1 s file reads as 41 frames x 1152 = 1.071 s
        const Case cases[] = { { "WAV", wavFile, 1.0e-6, 0.05f }, { "FLAC", flacFile, 1.0e-6, 0.05f },
                               { "Ogg Vorbis", oggFile, 1.0e-3, 0.3f }, { "MP3", mp3File, 0.1, 0.5f } };
        Soundboard sb;
        sb.prepare (48000.0);
        SbRender r { sb };
        for (int k = 0; k < 4; ++k)
        {
            const auto& tc = cases[k];
            beginTest (juce::String ("decode ") + tc.name + ": 1 s, 440 Hz at -9 dB after resampling 44.1 -> 48 kHz");
            sb.assignFile (k, tc.file);
            const auto st = waitLoaded (sb, k);
            expect (st.status == Status::ready, tc.name + (": " + st.error));
            if (st.status != Status::ready) continue;
            expectWithinAbsoluteError (st.lengthSeconds, 1.0, tc.lengthTolerance);
            r.clear();
            sb.trigger (k);
            r.run (48000 + 4800);
            expectWithinAbsoluteError (estimateF0 (r.out.data() + 9600, 28800), 440.0, 1.0);
            expectWithinAbsoluteError (rmsDb (r.out.data() + 9600, 28800), toneRmsDb, tc.levelTolerance);
            expect (! sb.getSlotState (k).playing, tc.name + juce::String (" still playing after 1.1 s"));
        }

        beginTest ("decode: random bytes named .mp3 are an error in Japanese, never a crash or a sound");
        juce::MemoryBlock junk (8192);
        juce::Random rng (7);
        rng.fillBitsRandomly (junk.getData(), junk.getSize());
        const auto broken = dir.getChildFile ("broken.mp3");
        expect (broken.replaceWithData (junk.getData(), junk.getSize()));
        sb.assignFile (4, broken);
        const auto st = waitLoaded (sb, 4);
        expect (st.status == Status::error, "status " + juce::String (int (st.status)));
        expect (hasNonAscii (st.error), st.error);
        r.clear();
        sb.trigger (4);
        r.run (4800);
        expectEquals (peakIn (r.out, 0, r.out.size()), 0.0f);
        expectEquals (r.allocations, 0LL);
    }

    void totalLimit (const juce::File& dir)
    {
        // Decoded sounds are mono float at the device rate. At 768 kHz a 60 s sound is 184 MB, so the limit
        // is crossed with tiny 1 kHz files and only ~30 MB of real memory.
        beginTest ("E-13: the 200 MB total (decoded bytes) refuses the file that would cross it");
        Soundboard sb;
        sb.prepare (768000.0);
        const auto f9 = dir.getChildFile ("9s-1k.wav"), f60 = dir.getChildFile ("60s-1k.wav"), f1 = dir.getChildFile ("1s-1k.wav");
        expect (writeWav (f9, silence (9.0, 1000.0), 1000.0));
        expect (writeWav (f60, silence (60.0, 1000.0), 1000.0));
        expect (writeWav (f1, silence (1.0, 1000.0), 1000.0));
        constexpr long long perSecond = 768000LL * (long long) sizeof (float);

        sb.assignFile (0, f9);
        auto st = waitLoaded (sb, 0);
        expect (st.status == Status::ready, st.error);
        expectEquals (sb.getTotalBytes(), 9 * perSecond);

        sb.assignFile (1, f60); // 27.6 MB + 184.3 MB = 211.9 MB > 200 MB (209.7 million bytes)
        st = waitLoaded (sb, 1);
        expect (st.status == Status::error && st.error.contains ("200 MB"), st.error);
        expectEquals (sb.getTotalBytes(), 9 * perSecond);

        sb.assignFile (2, f1);
        st = waitLoaded (sb, 2);
        expect (st.status == Status::ready, st.error);
        expectEquals (sb.getTotalBytes(), 10 * perSecond);
        expectLessOrEqual (sb.getTotalBytes(), kSoundboardMaxTotalBytes);

        sb.clearSlot (0);
        expect (sb.getSlotState (0).status == Status::empty);
        expectEquals (sb.getTotalBytes(), perSecond);
    }

    void playback (const juce::File& dir)
    {
        Soundboard sb;
        SbRender r { sb };
        // slots 0..11: 1 s of DC at 2^(k-12)
        for (int k = 0; k < kSoundboardSlots; ++k)
        {
            const auto f = dir.getChildFile ("dc" + juce::String (k) + ".wav");
            expect (writeWav (f, dc (1.0, level (k))));
            sb.assignFile (k, f);
        }
        for (int k = 0; k < kSoundboardSlots; ++k) expect (waitLoaded (sb, k).status == Status::ready);
        auto setDef = [&] (int slot, std::function<void (SoundboardSlotDef&)> change)
        {
            auto d = sb.getSlotDef (slot);
            change (d);
            sb.setSlotDef (slot, d);
        };

        beginTest ("F-06-9: the 9th sound stops the oldest");
        for (int k = 0; k < 9; ++k)
        {
            sb.trigger (k);
            r.run (480);
        }
        r.run (960); // the oldest has faded out (5 ms)
        float expected = 0.0f;
        for (int k = 1; k < 9; ++k) expected += level (k);
        expectWithinAbsoluteError (r.out.back(), expected, 1.0e-5f);
        expect (! sb.getSlotState (0).playing);
        expect (sb.getSlotState (1).playing && sb.getSlotState (8).playing);
        sb.stopAll();
        r.run (960);
        expectEquals (r.out.back(), 0.0f);

        beginTest ("F-06-9: a burst of 17 triggers in one block keeps the 8 newest (voice pool full)");
        for (int k = 0; k < kSoundboardSlots; ++k) setDef (k, [] (auto& d) { d.retrigger = 2; });
        r.clear();
        for (int k : { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 0, 1, 2, 3, 4 }) sb.trigger (k);
        r.run (1440);
        expected = 0.0f;
        for (int k : { 9, 10, 11, 0, 1, 2, 3, 4 }) expected += level (k);
        expectWithinAbsoluteError (r.out.back(), expected, 1.0e-5f);
        sb.stopAll();
        r.run (960);

        beginTest ("F-06-4: retrigger restart / ignore / overlap");
        for (int mode = 0; mode < 3; ++mode)
        {
            setDef (11, [mode] (auto& d) { d.retrigger = mode; }); // level 0.5
            r.clear();
            sb.trigger (11);
            r.run (24000);
            sb.trigger (11);
            r.run (72000);
            const auto& o = r.out;
            const float a = level (11);
            if (mode == 0) // restart: one voice, ends 1 s after the second trigger
            {
                expectWithinAbsoluteError (o[36000], a, 1.0e-5f);
                expectWithinAbsoluteError (o[48000], a, 1.0e-5f);
                expectWithinAbsoluteError (o[71999], a, 1.0e-5f);
                expectEquals (o[72000], 0.0f);
            }
            else if (mode == 1) // ignore: the second trigger does nothing
            {
                expectWithinAbsoluteError (o[36000], a, 1.0e-5f);
                expectWithinAbsoluteError (o[47999], a, 1.0e-5f);
                expectEquals (o[48000], 0.0f);
            }
            else // overlap: two voices from 0.5 s to 1 s
            {
                expectWithinAbsoluteError (o[12000], a, 1.0e-5f);
                expectWithinAbsoluteError (o[36000], 2.0f * a, 1.0e-5f);
                expectWithinAbsoluteError (o[50000], a, 1.0e-5f);
                expectWithinAbsoluteError (o[71999], a, 1.0e-5f);
                expectEquals (o[72000], 0.0f);
            }
        }

        beginTest ("F-06-4: loop plays seamlessly until stopAll");
        setDef (10, [] (auto& d) { d.loop = true; d.retrigger = 0; });
        r.clear();
        sb.trigger (10);
        r.run (48000 * 3);
        float lo = 1.0f, hi = 0.0f;
        for (auto x : r.out) { lo = std::min (lo, x); hi = std::max (hi, x); }
        expectWithinAbsoluteError (lo, level (10), 1.0e-5f);
        expectWithinAbsoluteError (hi, level (10), 1.0e-5f);
        expect (sb.getSlotState (10).playing);
        const size_t stopAt = r.out.size();
        sb.stopAll();
        r.run (960);
        expectEquals (peakIn (r.out, stopAt + 240, r.out.size()), 0.0f); // 5 ms fade
        expect (! sb.getSlotState (10).playing);
        setDef (10, [] (auto& d) { d.loop = false; });

        beginTest ("F-06-4: volume -24 / +6 dB within 1 dB");
        const auto sineFile = dir.getChildFile ("sine.wav");
        expect (writeWav (sineFile, sine (1000.0, 1.0, 0.25f)));
        sb.assignFile (9, sineFile);
        expect (waitLoaded (sb, 9).status == Status::ready);
        auto levelAt = [&] (float db)
        {
            setDef (9, [db] (auto& d) { d.volumeDb = db; d.retrigger = 0; });
            r.clear();
            sb.trigger (9);
            r.run (24000);
            sb.stopAll();
            r.run (960);
            return rmsDb (r.out.data() + 4800, 14400);
        };
        const float ref = levelAt (0.0f);
        expectWithinAbsoluteError (levelAt (-24.0f) - ref, -24.0f, 1.0f);
        expectWithinAbsoluteError (levelAt (6.0f) - ref, 6.0f, 1.0f);
        setDef (9, [] (auto& d) { d.volumeDb = 0.0f; });

        beginTest ("F-06-5: monitor routing per slot");
        for (bool toMon : { true, false })
        {
            setDef (9, [toMon] (auto& d) { d.toMonitor = toMon; });
            r.clear();
            sb.trigger (9);
            r.run (9600);
            sb.stopAll();
            r.run (960);
            expectGreaterThan (peakIn (r.out, 0, 9600), 0.2f);
            if (toMon) expect (r.mon == r.out);
            else expectEquals (peakIn (r.mon, 0, r.mon.size()), 0.0f);
        }

        beginTest ("F-06-7: ducking lowers the voice while a sound plays, then recovers");
        expectEquals (sb.voiceDuckGain(), 1.0f);
        sb.setDuckingDb (-12.0f);
        r.clear();
        sb.trigger (0);
        r.run (9600);
        expectWithinAbsoluteError (sb.voiceDuckGain(), dsp::dbToGain (-12.0f), 0.01f);
        r.run (48000 * 3); // the sound ended at 1 s; 300 ms release
        expectWithinAbsoluteError (sb.voiceDuckGain(), 1.0f, 0.01f);
        sb.setDuckingDb (0.0f);
        sb.trigger (0);
        r.run (9600);
        expectWithinAbsoluteError (sb.voiceDuckGain(), 1.0f, 0.001f); // off: no ducking (the release above is still converging)
        sb.stopAll();
        r.run (960);

        beginTest ("F-10-3: test tone 440 Hz, -18 dBFS, output only");
        sb.setTestTone (true);
        r.clear();
        r.run (24000);
        expectWithinAbsoluteError (peakDb (r.out.data() + 4800, 19200), -18.0f, 0.1f);
        expectWithinAbsoluteError (estimateF0 (r.out.data() + 4800, 19200), 440.0, 1.0);
        expectEquals (peakIn (r.mon, 0, r.mon.size()), 0.0f);
        sb.setTestTone (false);
        r.run (960); // 10 ms fade
        r.clear();
        r.run (4800);
        expectEquals (peakIn (r.out, 0, r.out.size()), 0.0f);

        beginTest ("triggers queued while nothing renders (device closed) are dropped");
        sb.trigger (0);
        juce::Thread::sleep (400);
        r.clear();
        r.run (4800);
        expectEquals (peakIn (r.out, 0, r.out.size()), 0.0f);
        sb.trigger (0);
        r.run (480);
        expectGreaterThan (peakIn (r.out, 4800, r.out.size()), 0.0f);
        sb.stopAll();
        r.run (960);

        beginTest ("a sound shorter than one device sample still loops safely");
        const auto tiny = dir.getChildFile ("tiny.wav");
        expect (writeWav (tiny, dc (1.0 / 768000.0, 0.5f, 768000.0), 768000.0));
        sb.assignFile (8, tiny);
        expect (waitLoaded (sb, 8).status == Status::ready);
        setDef (8, [] (auto& d) { d.loop = true; });
        r.clear();
        sb.trigger (8);
        r.run (4800);
        expect (allFinite (r.out));
        sb.stopAll();
        r.run (960);

        beginTest ("S-02: positionSeconds follows the newest voice of the slot, wraps when looping, 0 when stopped");
        setDef (11, [] (auto& d) { d.retrigger = 2; d.loop = false; }); // 1 s
        r.clear();
        expectEquals (sb.getSlotState (11).positionSeconds, 0.0);
        sb.trigger (11);
        r.run (24000);
        auto pos = sb.getSlotState (11);
        expect (pos.playing);
        expectWithinAbsoluteError (pos.positionSeconds, 0.5, 1.0e-9);
        sb.trigger (11); // overlap: the second voice is the newest
        r.run (4800);
        expectWithinAbsoluteError (sb.getSlotState (11).positionSeconds, 0.1, 1.0e-9);
        r.run (24000); // the first voice has ended
        pos = sb.getSlotState (11);
        expect (pos.playing);
        expectWithinAbsoluteError (pos.positionSeconds, 0.6, 1.0e-9);
        r.run (24000);
        pos = sb.getSlotState (11);
        expect (! pos.playing);
        expectEquals (pos.positionSeconds, 0.0);
        setDef (11, [] (auto& d) { d.retrigger = 0; d.loop = true; });
        sb.trigger (11);
        r.run (48000 + 12000);
        expectWithinAbsoluteError (sb.getSlotState (11).positionSeconds, 0.25, 1.0e-9);
        sb.stopAll();
        r.run (960);
        expectEquals (sb.getSlotState (11).positionSeconds, 0.0);
        setDef (11, [] (auto& d) { d.loop = false; });

        beginTest ("prepare() at a new rate reloads the sounds at that rate");
        sb.prepare (44100.0);
        auto st = waitLoaded (sb, 0);
        expect (st.status == Status::ready, st.error);
        expectWithinAbsoluteError (st.lengthSeconds, 1.0, 1.0e-6);
        r.clear();
        sb.trigger (0);
        r.run (48000);
        int sounding = 0;
        for (auto x : r.out) sounding += std::abs (x) > 1.0e-7f ? 1 : 0;
        expect (std::abs (sounding - 44100) <= 2, juce::String (sounding));

        beginTest ("render() never allocates");
        expectEquals (r.allocations, 0LL);
    }

    void persistence (const juce::File& dir)
    {
        beginTest ("soundboard.json: save -> load gives the same slots; corrupt or missing = empty board");
        const auto json = dir.getChildFile ("soundboard.json");
        const auto existing = dir.getChildFile ("sine.wav");
        {
            Soundboard a;
            a.setSlotDef (0, { existing.getFullPathName(), -7.5f, true, 2, false });
            a.setSlotDef (5, { dir.getChildFile (u8 ("無い 音.wav")).getFullPathName(), 6.0f, false, 1, true });
            a.setSlotDef (11, { {}, -24.0f, true, 0, false });
            expect (a.save (json));

            Soundboard b;
            b.load (json);
            for (int s = 0; s < kSoundboardSlots; ++s)
                expect (b.getSlotDef (s) == a.getSlotDef (s), "slot " + juce::String (s));
            expect (waitLoaded (b, 0).status == Status::ready);
            expect (waitLoaded (b, 5).status == Status::missing);
        }

        expect (json.replaceWithText (R"({"version":1,"slots":[{"file":"","volumeDb":99,"loop":true,"retrigger":"bogus","toMonitor":false}]})"));
        {
            Soundboard c;
            c.load (json);
            const auto d = c.getSlotDef (0);
            expectEquals (d.volumeDb, kSoundboardVolumeDb.max);
            expectEquals (d.retrigger, 0);
            expect (d.loop && ! d.toMonitor);
        }

        for (const char* text : { "{ not json", "" })
        {
            expect (json.replaceWithText (text));
            Soundboard c;
            c.setSlotDef (3, { existing.getFullPathName(), 1.0f, true, 1, false });
            c.load (json);
            for (int s = 0; s < kSoundboardSlots; ++s) expect (c.getSlotDef (s) == SoundboardSlotDef {});
        }
        json.deleteFile();
        {
            Soundboard c;
            c.load (json);
            expect (c.getSlotDef (0) == SoundboardSlotDef {});
            expect (c.getSlotState (0).status == Status::empty);
        }
    }
};

PlatformNameTests platformNameTests;
PlatformMonitorTests platformMonitorTests;
PlatformSoundboardTests platformSoundboardTests;
} // namespace
} // namespace koe

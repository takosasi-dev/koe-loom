// Offline soak test (complements AC-24, which needs real devices and stays a manual check).
// Pushes hours of audio through the path the app runs - AppController without devices: VoiceProcessor +
// EffectChain + Soundboard, and the monitor sink feeding a MonitorOutput whose device side the test pulls -
// while a seeded script works the controls the way a user would. No XRUN figure: that needs a real device.
//   "Soak (offline AC-24)", category Diag: KOELOOM_SOAK_HOURS hours of audio (default 8)
//   "Soak (short)", category Integration: 3 minutes of audio, plus the regressions the soak found

#include "App/AppController.h"
#include "Core/Paths.h"
#include "Effects/EffectRegistry.h"
#include "Engine/MonitorOutput.h"
#include "Tests/TestUtil.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <xmmintrin.h>

#ifndef NOMINMAX
 #define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
 #define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <psapi.h>

namespace koe
{
namespace
{
using namespace test;

struct Mem { double privateMb = 0.0, workingSetMb = 0.0; };

Mem processMemory()
{
    PROCESS_MEMORY_COUNTERS_EX pmc {};
    pmc.cb = sizeof (pmc);
    K32GetProcessMemoryInfo (GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*> (&pmc), sizeof (pmc));
    return { double (pmc.PrivateUsage) / 1048576.0, double (pmc.WorkingSetSize) / 1048576.0 };
}

/** CPU cycles this thread has used: unaffected by other programs competing for the CPU. */
ULONG64 threadCycles()
{
    ULONG64 c = 0;
    QueryThreadCycleTime (GetCurrentThread(), &c);
    return c;
}

/** The tests run on the message thread: let the controller's and the soundboard's timers fire. */
void pumpMessages()
{
    MSG msg;
    while (PeekMessageW (&msg, nullptr, 0, 0, PM_REMOVE))
    {
        TranslateMessage (&msg);
        DispatchMessageW (&msg);
    }
}

/** Monitor sink between the processor and a MonitorOutput (pulled by the test as the device would).
    Also notes whether flush-to-zero and denormals-are-zero were set inside process(). */
struct MonitorTap final : IMonitorSink
{
    MonitorOutput out;
    bool ftzDaz = true;
    void push (const float* x, int n) override
    {
        if ((_mm_getcsr() & 0x8040u) != 0x8040u) ftzDaz = false;
        out.push (x, n);
    }
};

/** Cost of one block in thread CPU cycles, 1 kcycle bins (preallocated: no growth while measuring). */
struct CostStats
{
    std::vector<int> bins = std::vector<int> (40000, 0);
    double sum = 0.0, wallUs = 0.0;
    long long count = 0;
    void add (ULONG64 cycles, double us)
    {
        ++bins[std::min (size_t (cycles / 1000), bins.size() - 1)];
        sum += double (cycles);
        wallUs += us;
        ++count;
    }
    double meanK() const { return count > 0 ? sum / double (count) / 1000.0 : 0.0; }
    double meanUs() const { return count > 0 ? wallUs / double (count) : 0.0; }
    double percentileK (double p) const
    {
        const auto want = (long long) std::ceil (p * double (count));
        long long seen = 0;
        for (size_t i = 0; i < bins.size(); ++i)
            if ((seen += bins[i]) >= want) return double (i + 1);
        return double (bins.size());
    }
};

enum class Seg { voice, silence, dc, loud };

/** Looped synthetic speech with long silences, a DC-offset stretch, loud/clipped stretches and short noise bursts. */
struct SoakInput
{
    double cycle = 600.0;
    std::vector<float> voice48 = synthVoice (61.3, 11, 48000.0), voice44 = synthVoice (61.3, 11, 44100.0);
    std::vector<float> noise = whiteNoise (1.0, 0.99f, 5);
    size_t vpos = 0, npos = 0;

    Seg segmentAt (double t) const
    {
        const double f = std::fmod (t, cycle) / cycle;
        if (f < 0.62) return Seg::voice;
        if (f < 0.82) return Seg::silence;
        if (f < 0.88) return Seg::dc;
        if (f < 0.92) return Seg::loud;
        return Seg::voice;
    }

    Seg fill (float* x, int n, double t, double sr)
    {
        const auto& v = sr < 46000.0 ? voice44 : voice48;
        if (vpos >= v.size()) vpos = 0;
        const Seg s = segmentAt (t);
        const bool burst = s == Seg::voice && std::fmod (t, 37.0) < 0.05;  // 50 ms of near full-scale noise
        const bool clipped = std::fmod (t, cycle) / cycle < 0.90;         // first half of "loud": clipped at ±1
        for (int i = 0; i < n; ++i)
        {
            float y = v[vpos];
            if (++vpos >= v.size()) vpos = 0;
            if (s == Seg::silence) y = 0.0f;
            else if (s == Seg::dc) y = 0.5f * y + 0.25f;
            else if (s == Seg::loud) y = clipped ? std::clamp (4.0f * y, -1.0f, 1.0f) : 4.0f * y;
            if (burst)
            {
                y = noise[npos];
                if (++npos >= noise.size()) npos = 0;
            }
            x[i] = y;
        }
        return s;
    }
};

SoundSlotState waitLoaded (Soundboard& sb, int slot)
{
    const auto until = juce::Time::getMillisecondCounterHiRes() + 10000.0;
    auto st = sb.getSlotState (slot);
    while (st.status == SoundSlotState::Status::loading && juce::Time::getMillisecondCounterHiRes() < until)
    {
        juce::Thread::sleep (2);
        pumpMessages();
        st = sb.getSlotState (slot);
    }
    return st;
}

juce::String mb (double v) { return juce::String (v, 1) + " MB"; }

void runSoak (juce::UnitTest& ut, double totalSeconds, bool longRun)
{
    auto dir = paths::dataDir();
    if (dir.getFullPathName().contains ("KoeLoomTests")) dir.deleteRecursively();
    dir.createDirectory();
    const auto progress = juce::File::getSpecialLocation (juce::File::currentExecutableFile).getSiblingFile ("soak-progress.txt");
    if (longRun) progress.replaceWithText ("");
    auto note = [&] (const juce::String& line)
    {
        ut.logMessage (line);
        if (longRun) progress.appendText (line + "\n");
    };

    // ---- soundboard sounds: a loop at 44.1 kHz, one-shots for restart / overlap, speech at 22.05 kHz ----
    auto sounds = dir.getChildFile ("soak-sounds");
    sounds.createDirectory();
    {
        auto pad = sine (220.0, 7.3, 0.15f, 44100.0);
        const auto p2 = sine (277.2, 7.3, 0.15f, 44100.0), p3 = sine (329.6, 7.3, 0.15f, 44100.0);
        for (size_t i = 0; i < pad.size(); ++i) pad[i] += p2[i] + p3[i];
        writeWav (sounds.getChildFile ("pad-loop.wav"), pad, 44100.0);
        writeWav (sounds.getChildFile ("blip.wav"), concat ({ sine (880.0, 0.3, 0.5f), whiteNoise (0.5, 0.3f, 9) }));
        writeWav (sounds.getChildFile ("hit.wav"), concat ({ whiteNoise (0.05, 0.9f, 4), sine (110.0, 1.45, 0.6f) }));
        writeWav (sounds.getChildFile ("speech.wav"), synthVoice (4.0, 21, 22050.0), 22050.0);
    }

    MonitorTap tap; // outlives the controller, which points the processor at it
    AppController c (false);
    c.startup();
    auto& vp = c.getProcessorForTests();
    auto& sb = c.getSoundboard();
    double sr = 48000.0;
    int block = 480;
    constexpr double monRate = 44100.0; // headphones at 44.1 kHz: the adaptive resampler converts
    vp.setMonitorSink (&tap);
    tap.out.prepareForTest (sr, monRate, 441);
    tap.out.setEnabled (true);
    tap.out.setVolumeDb (-6.0f);

    auto slotDef = [&] (int s, const char* file, bool loop, int retrigger, float volumeDb)
    {
        SoundboardSlotDef d;
        d.file = sounds.getChildFile (file).getFullPathName();
        d.loop = loop;
        d.retrigger = retrigger;
        d.volumeDb = volumeDb;
        sb.setSlotDef (s, d);
    };
    slotDef (0, "pad-loop.wav", true, 1, -12.0f); // loop, retrigger ignored
    slotDef (1, "blip.wav", false, 0, -6.0f);     // restart
    slotDef (2, "hit.wav", false, 2, -9.0f);      // overlap
    slotDef (3, "speech.wav", false, 0, -6.0f);
    for (int s = 0; s < 4; ++s) ut.expect (waitLoaded (sb, s).status == SoundSlotState::Status::ready, "sound " + juce::String (s) + " did not load");
    c.updateSettings ([] (Settings& s) { s.duckingDb = -6.0f; });

    std::vector<juce::String> presetIds;
    for (auto& p : c.getPresetLibrary().all())
        if (p.builtin) presetIds.push_back (juce::String (p.id));
    ut.expectEquals (int (presetIds.size()), 81);
    juce::String why;
    for (size_t i = 0; i < presetIds.size(); i += 7) c.toggleFavorite (presetIds[i].toStdString(), why);
    std::vector<std::string> types;
    for (auto& i : allEffectInfos())
        if (hasEffectFactory (i.type)) types.push_back (i.type);

    // ---- schedule ----
    SoakInput input;
    input.cycle = std::min (600.0, totalSeconds / 3.0);
    const double presetEvery = longRun ? 30.0 : totalSeconds / double (presetIds.size() + 3); // 64 for the original 61
    const double actionEvery = longRun ? 1.0 : 0.5;
    const double bucketSeconds = longRun ? 3600.0 : totalSeconds / 3.0;
    const double warmupSeconds = longRun ? 600.0 : totalSeconds * 0.1;
    struct Reprepare { double at, rate; int block; };
    std::vector<Reprepare> reprepares;
    {
        const double away = std::max (5.0, totalSeconds * 0.03);
        const Reprepare other[] = { { 0.25, 44100.0, 441 }, { 0.5, 44100.0, 1024 }, { 0.75, 48000.0, 960 } };
        for (auto& r : other)
        {
            reprepares.push_back ({ totalSeconds * r.at, r.rate, r.block });
            reprepares.push_back ({ totalSeconds * r.at + away, 48000.0, 480 });
        }
    }
    const int numBuckets = std::max (1, int (std::ceil (totalSeconds / bucketSeconds - 1.0e-9)));
    std::vector<CostStats> buckets ((size_t) numBuckets);
    CostStats silenceCost, speechCost;

    // ---- state ----
    const float ceiling = dsp::dbToGain (kLimiterCeilingDb);
    std::vector<float> in (2048), outL (2048), outR (2048), mon (2048);
    juce::Random rng (20261003);
    double t = 0.0, monAcc = 0.0, nextPresetAt = 0.0, nextActionAt = 1.0, lastStopAll = -1.0e9;
    long long blocks = 0, allocs = 0, nonFinite = 0, lrMismatch = 0, overCeiling = 0, monBad = 0, firstAllocBlock = -1;
    float worstPeak = 0.0f, worstMon = 0.0f;
    juce::String firstAllocWhere, presetId;
    size_t presetIdx = 0, reprepIdx = 0;
    long long presetLoadBlock = -1000;
    bool windowValid = true;
    float windowPeak = 0.0f;
    int silentWindows = 0, posErrors = 0, latencyMismatches = 0, monUnder = 0, monOver = 0;
    juce::StringArray silentPresets;
    std::map<juce::String, float> latencyByPreset;
    float latMin = 1.0e9f, latMax = 0.0f;
    std::set<juce::String> presetsSeen;
    double loopRun = 0.0, maxLoopRun = 0.0;
    int actionCounts[14] {};
    Mem memWarm {}, memPeak {};
    bool warmDone = false;
    int bucketNow = 0;
    struct Ramp { bool on = false; int slot = 0, param = 0; std::string type; float from = 0, to = 0; int left = 0, total = 1; } ramp;
    struct PitchRamp { bool on = false; float from = 0, to = 0; int left = 0, total = 1; } pitchRamp;

    auto loadNextPreset = [&]
    {
        presetId = presetIds[presetIdx++ % presetIds.size()];
        c.loadPreset (presetId.toStdString());
        c.setVoiceChangerOn (true);
        c.setMicMuted (false);
        presetsSeen.insert (presetId);
        presetLoadBlock = blocks;
        windowValid = true;
        windowPeak = 0.0f;
        ramp.on = pitchRamp.on = false;
    };

    auto reprepare = [&] (double rate, int blk)
    {
        c.reprepareForTests (rate, blk);
        monUnder += tap.out.getUnderruns();
        monOver += tap.out.getOverruns();
        tap.out.prepareForTest (rate, monRate, 441);
        sr = rate;
        block = blk;
        monAcc = 0.0;
        for (int s = 0; s < 4; ++s) waitLoaded (sb, s); // decoded again for the new rate
        c.performAction ("sound.1");
    };

    auto randomIn = [&] (float lo, float hi) { return lo + rng.nextFloat() * (hi - lo); };

    auto doAction = [&]
    {
        const auto& chain = c.getChain();
        const int n = int (chain.size());
        const int kind = rng.nextInt (100);
        if (kind < 6) { ++actionCounts[0]; c.setVoiceChangerOn (! c.isVoiceChangerOn()); }
        else if (kind < 10) { ++actionCounts[1]; c.setMicMuted (! c.isMicMuted()); }
        else if (kind < 20)
        {
            ++actionCounts[2];
            if (n > 0)
            {
                const int i = rng.nextInt (n);
                if (rng.nextBool()) c.setSlotEnabled (i, ! chain[size_t (i)].enabled, why);
                else c.performAction ("slot." + juce::String (i + 1)); // hotkey path
            }
        }
        else if (kind < 28) { ++actionCounts[3]; c.addEffect (types[size_t (rng.nextInt (int (types.size())))], why); }
        else if (kind < 34) { ++actionCounts[4]; if (n > 0) c.removeSlot (rng.nextInt (n)); }
        else if (kind < 40) { ++actionCounts[5]; if (n > 1) c.moveSlot (rng.nextInt (n), rng.nextInt (n)); }
        else if (kind < 55)
        {
            ++actionCounts[6];
            if (n == 0) return;
            const int i = rng.nextInt (n);
            const auto* info = findEffectInfo (chain[size_t (i)].type);
            if (info == nullptr || info->params.empty()) return;
            const int p = rng.nextInt (int (info->params.size()));
            const auto& spec = info->params[size_t (p)];
            const float target = randomIn (spec.min, spec.max);
            if (spec.isChoice() || spec.integer) { c.setSlotParam (i, p, target); return; }
            const auto& vals = chain[size_t (i)].params;
            ramp = { true, i, p, chain[size_t (i)].type, size_t (p) < vals.size() ? vals[size_t (p)] : spec.def, target, 0, 30 + rng.nextInt (170) };
            ramp.left = ramp.total;
        }
        else if (kind < 63)
        {
            ++actionCounts[7];
            const int layers = c.getNumLayers();
            if (layers == 0 || (layers < 2 && rng.nextInt (3) == 0)) { c.addLayer (why); return; }
            const int i = rng.nextInt (layers);
            const int k = rng.nextInt (4);
            if (k == 0) c.removeLayer (i);
            else if (k == 1) c.setLayerEnabled (i, ! c.getLayer (i).enabled);
            else
            {
                auto d = c.getLayer (i);
                d.mode = rng.nextBool() ? LayerDef::Mode::scale : LayerDef::Mode::fixed;
                d.pitchSt = randomIn (-12.0f, 12.0f);
                d.formantSt = randomIn (-6.0f, 6.0f);
                d.levelDb = randomIn (-18.0f, -3.0f);
                d.key = rng.nextInt (12);
                d.minor = rng.nextBool();
                d.degree = rng.nextInt (14) - 7;
                if (d.degree >= 0) ++d.degree; // -7..-1, 1..7
                c.setLayer (i, d);
            }
        }
        else if (kind < 68)
        {
            ++actionCounts[8];
            if (rng.nextInt (3) == 0) c.setFormant (randomIn (-6.0f, 6.0f));
            pitchRamp = { true, c.getPitch(), randomIn (-12.0f, 12.0f), 0, 50 + rng.nextInt (100) };
            pitchRamp.left = pitchRamp.total;
        }
        else if (kind < 72) { ++actionCounts[9]; c.setShifterEnabled (! c.hasShifter()); }
        else if (kind < 84)
        {
            ++actionCounts[10];
            const int k = rng.nextInt (20);
            if (k < 6) c.performAction ("sound.2");
            else if (k < 11) for (int b = 1 + rng.nextInt (3); b > 0; --b) c.performAction ("sound.3");
            else if (k < 14) c.performAction ("sound.4");
            else if (k < 17) c.performAction ("sound.1");
            else if (k < 19)
            {
                auto d = sb.getSlotDef (k - 16);
                d.volumeDb = randomIn (-24.0f, 0.0f);
                sb.setSlotDef (k - 16, d);
            }
            else if (t - lastStopAll > (longRun ? 1800.0 : 60.0))
            {
                c.performAction ("soundStopAll");
                lastStopAll = t;
            }
        }
        else if (kind < 90)
        {
            ++actionCounts[11];
            const char* acts[] = { "freezeToggle", "freezeToggle", "looperRecPlay", "looperClear" };
            const int k = rng.nextInt (6);
            if (k == 0) c.addEffect ("freeze", why);
            else if (k == 1) c.addEffect ("looper", why);
            else c.performAction (acts[k - 2]);
        }
        else if (kind < 97)
        {
            ++actionCounts[12];
            switch (rng.nextInt (6))
            {
                case 0: c.setInputGainDb (randomIn (-6.0f, 12.0f)); break;
                case 1: c.setGate (rng.nextBool(), randomIn (-60.0f, -40.0f), 5.0f, 80.0f, 120.0f); break;
                case 2: c.setNoiseSuppression (! c.getSettings().noiseSuppressionOn, randomIn (0.5f, 1.0f)); break;
                case 3: c.setOutputGainDb (randomIn (-6.0f, 12.0f)); break;
                case 4: { const float d = randomIn (-24.0f, 0.0f); c.updateSettings ([d] (Settings& s) { s.duckingDb = d; }); break; }
                default: { const float v = randomIn (-40.0f, 0.0f); c.setMonitorVolumeDb (v); tap.out.setVolumeDb (v); break; }
            }
        }
        else { ++actionCounts[13]; c.performAction (rng.nextBool() ? "favoriteNext" : "favoritePrev"); }
    };

    auto housekeeping = [&]
    {
        // what the 30 fps UI timer and the controller's timer do
        vp.collectGarbage();
        c.pollMeters();
        c.takeToasts();
        if (ramp.on)
        {
            const auto& chain = c.getChain();
            if (ramp.slot >= int (chain.size()) || chain[size_t (ramp.slot)].type != ramp.type) ramp.on = false;
            else
            {
                ramp.left = std::max (0, ramp.left - 3);
                c.setSlotParam (ramp.slot, ramp.param, ramp.from + (ramp.to - ramp.from) * (1.0f - float (ramp.left) / float (ramp.total)));
                ramp.on = ramp.left > 0;
            }
        }
        if (pitchRamp.on)
        {
            pitchRamp.left = std::max (0, pitchRamp.left - 3);
            c.setPitch (pitchRamp.from + (pitchRamp.to - pitchRamp.from) * (1.0f - float (pitchRamp.left) / float (pitchRamp.total)));
            pitchRamp.on = pitchRamp.left > 0;
        }
        pumpMessages();
    };

    c.performAction ("sound.1");
    const auto wallStart = juce::Time::getMillisecondCounterHiRes();
    auto bucketWallStart = wallStart;
    while (t < totalSeconds)
    {
        // ---- control side (message thread, between callbacks) ----
        if (reprepIdx < reprepares.size() && t >= reprepares[reprepIdx].at)
        {
            reprepare (reprepares[reprepIdx].rate, reprepares[reprepIdx].block);
            ++reprepIdx;
        }
        if (t >= nextPresetAt)
        {
            loadNextPreset();
            nextPresetAt += presetEvery;
        }
        const long long sinceLoad = blocks - presetLoadBlock;
        if (t >= nextActionAt)
        {
            if (sinceLoad >= 100) doAction(); // the first second after a load belongs to the preset checks
            nextActionAt += actionEvery;
        }
        if (blocks % 3 == 0) housekeeping();
        if (blocks % 30 == 0)
        {
            for (int s = 0; s < 4; ++s)
            {
                const auto st = sb.getSlotState (s);
                if (st.playing && (st.positionSeconds < 0.0 || st.positionSeconds > st.lengthSeconds + 0.05)) ++posErrors;
                if (s == 0)
                {
                    loopRun = st.playing ? loopRun + 30.0 * block / sr : 0.0;
                    maxLoopRun = std::max (maxLoopRun, loopRun);
                }
            }
            c.getNotices();
        }

        // ---- audio side ----
        const Seg seg = input.fill (in.data(), block, t, sr);
        const bool nominal = sr == 48000.0 && block == 480;
        ULONG64 cycles = 0;
        long long a = 0;
        int monN = 0;
        const auto w0 = juce::Time::getHighResolutionTicks();
        {
            AllocationCounter counter;
            const ULONG64 c0 = threadCycles();
            vp.process (in.data(), outL.data(), outR.data(), block);
            cycles = threadCycles() - c0;
            monAcc += block * monRate / sr;
            monN = int (monAcc);
            monAcc -= monN;
            tap.out.pullForTest (mon.data(), monN);
            a = counter.count();
        }
        const double us = juce::Time::highResolutionTicksToSeconds (juce::Time::getHighResolutionTicks() - w0) * 1.0e6;

        // ---- checks ----
        if (a > 0)
        {
            allocs += a;
            if (firstAllocBlock < 0)
            {
                firstAllocBlock = blocks;
                firstAllocWhere = presetId + " at " + juce::String (t, 2) + " s";
            }
        }
        float pk = 0.0f;
        for (int i = 0; i < block; ++i)
        {
            const float v = outL[size_t (i)];
            if (! std::isfinite (v)) { ++nonFinite; continue; }
            if (v != outR[size_t (i)]) ++lrMismatch;
            pk = std::max (pk, std::abs (v));
        }
        worstPeak = std::max (worstPeak, pk);
        if (pk > ceiling + 1.0e-4f) ++overCeiling;
        for (int i = 0; i < monN; ++i)
        {
            const float v = mon[size_t (i)];
            if (! std::isfinite (v) || std::abs (v) > ceiling + 1.0e-4f) ++monBad;
            else worstMon = std::max (worstMon, std::abs (v));
        }

        if (sinceLoad >= 30 && sinceLoad < 100)
        {
            if (seg == Seg::silence) windowValid = false;
            windowPeak = std::max (windowPeak, pk);
        }
        if (sinceLoad == 100 && windowValid && windowPeak < 0.003f)
        {
            ++silentWindows;
            silentPresets.addIfNotAlreadyThere (presetId);
        }
        if (sinceLoad == 50 && nominal)
        {
            const float lat = c.getStatus().latencyMs;
            const auto key = presetId + (c.getSettings().noiseSuppressionOn ? " +ns" : "");
            auto it = latencyByPreset.find (key);
            if (it == latencyByPreset.end()) latencyByPreset[key] = lat;
            else if (std::abs (it->second - lat) > 0.01f)
            {
                if (latencyMismatches++ < 5) ut.logMessage ("  latency of " + key + " changed: " + juce::String (it->second, 2) + " -> " + juce::String (lat, 2) + " ms");
            }
            latMin = std::min (latMin, lat);
            latMax = std::max (latMax, lat);
        }

        if (nominal)
        {
            const int bi = std::min (numBuckets - 1, int (t / bucketSeconds));
            buckets[size_t (bi)].add (cycles, us);
            if (seg == Seg::silence) silenceCost.add (cycles, us);
            else if (seg == Seg::voice) speechCost.add (cycles, us);
        }

        t += block / sr;
        ++blocks;

        if (! warmDone && t >= warmupSeconds)
        {
            warmDone = true;
            memWarm = processMemory();
            note ("  after warm-up (" + juce::String (warmupSeconds, 0) + " s of audio): private " + mb (memWarm.privateMb) + ", working set " + mb (memWarm.workingSetMb));
        }
        if (const int bi = std::min (numBuckets - 1, int (t / bucketSeconds)); bi != bucketNow || t >= totalSeconds)
        {
            const auto m = processMemory();
            memPeak.privateMb = std::max (memPeak.privateMb, m.privateMb);
            memPeak.workingSetMb = std::max (memPeak.workingSetMb, m.workingSetMb);
            const auto& b = buckets[size_t (bucketNow)];
            const auto now = juce::Time::getMillisecondCounterHiRes();
            note ("  " + juce::String (longRun ? "hour " : "part ") + juce::String (bucketNow + 1) + ": wall " + juce::String ((now - bucketWallStart) / 1000.0, 1)
                  + " s, block cost mean " + juce::String (b.meanK(), 1) + " kcyc / p99 " + juce::String (b.percentileK (0.99), 0) + " kcyc / p99.9 "
                  + juce::String (b.percentileK (0.999), 0) + " kcyc, wall mean " + juce::String (b.meanUs(), 1) + " us (" + juce::String (b.meanUs() / 100.0, 2)
                  + " % of real time), private " + mb (m.privateMb) + ", working set " + mb (m.workingSetMb));
            bucketWallStart = now;
            bucketNow = bi;
        }
    }
    const double wallSeconds = (juce::Time::getMillisecondCounterHiRes() - wallStart) / 1000.0;
    const auto memEnd = processMemory();

    // ---- after the activity: not stuck silent with voice ON and not muted ----
    juce::String finalLevels;
    for (const char* id : { "natural-asis", "character-helium" })
    {
        c.loadPreset (id);
        c.setVoiceChangerOn (true);
        c.setMicMuted (false);
        c.performAction ("soundStopAll");
        const auto v = synthVoice (3.0, 3, sr);
        std::vector<float> o (v.size());
        for (size_t pos = 0; pos + size_t (block) <= v.size(); pos += size_t (block))
        {
            vp.process (v.data() + pos, o.data() + pos, nullptr, block);
            tap.out.pullForTest (mon.data(), int (block * monRate / sr));
            housekeeping();
        }
        const float level = rmsDb (o.data() + o.size() / 2, int (o.size() / 2));
        finalLevels << id << " " << juce::String (level, 1) << " dBFS  ";
        ut.expectGreaterThan (level, -40.0f, juce::String ("output stuck silent after the soak: ") + id);
    }

    // ---- report ----
    note ("  " + juce::String (t / 3600.0, 2) + " h of audio (" + juce::String (blocks) + " blocks) in " + juce::String (wallSeconds, 0) + " s wall ("
          + juce::String (t / std::max (1.0e-9, wallSeconds), 1) + "x real time)");
    juce::String acts;
    const char* actNames[] = { "voice", "mute", "slotToggle", "add", "remove", "move", "param", "layer", "pitch", "shifter", "sound", "freeze/looper", "env", "favorite" };
    for (int i = 0; i < 14; ++i) acts << actNames[i] << " " << actionCounts[i] << ", ";
    note ("  actions: " + acts + "preset loads " + juce::String ((long long) presetIdx) + " (" + juce::String ((int) presetsSeen.size()) + " distinct), device re-prepares "
          + juce::String ((int) reprepIdx));
    note ("  output peak " + juce::String (dsp::gainToDb (worstPeak), 2) + " dBFS (ceiling " + juce::String (kLimiterCeilingDb, 1) + "), non-finite " + juce::String (nonFinite)
          + ", L!=R " + juce::String (lrMismatch) + ", monitor peak " + juce::String (dsp::gainToDb (worstMon), 1) + " dBFS, monitor bad samples " + juce::String (monBad)
          + ", monitor underruns " + juce::String (monUnder + tap.out.getUnderruns()) + " / overruns " + juce::String (monOver + tap.out.getOverruns()));
    note ("  audio-thread allocations " + juce::String (allocs) + (firstAllocBlock >= 0 ? " (first: " + firstAllocWhere + ")" : juce::String())
          + ", FTZ/DAZ set in process(): " + (tap.ftzDaz ? "yes" : "NO"));
    note ("  latency " + juce::String (latMin, 1) + ".." + juce::String (latMax, 1) + " ms over " + juce::String ((int) latencyByPreset.size()) + " preset/NS combinations, changes "
          + juce::String (latencyMismatches) + "; silent preset windows " + juce::String (silentWindows) + (silentPresets.isEmpty() ? juce::String() : " (" + silentPresets.joinIntoString (", ") + ")")
          + "; soundboard position errors " + juce::String (posErrors) + ", longest unbroken loop " + juce::String (maxLoopRun / 60.0, 1) + " min");
    note ("  block cost in long silence " + juce::String (silenceCost.meanK(), 1) + " kcyc vs speech " + juce::String (speechCost.meanK(), 1) + " kcyc (mean)");
    note ("  memory: private " + mb (memWarm.privateMb) + " -> " + mb (memEnd.privateMb) + " (" + juce::String (memEnd.privateMb - memWarm.privateMb, 1) + " MB), working set "
          + mb (memWarm.workingSetMb) + " -> " + mb (memEnd.workingSetMb) + ", peak private " + mb (memPeak.privateMb));
    note ("  after: " + finalLevels);

    ut.expectEquals (nonFinite, 0LL, "non-finite output samples");
    ut.expectEquals (overCeiling, 0LL, "blocks above the limiter ceiling");
    ut.expectEquals (lrMismatch, 0LL, "L and R differ");
    ut.expectEquals (monBad, 0LL, "monitor samples non-finite or above the limiter ceiling");
    ut.expectEquals (allocs, 0LL, "allocations on the audio thread");
    ut.expect (tap.ftzDaz, "flush-to-zero / denormals-are-zero not set inside process()");
    ut.expectEquals (latencyMismatches, 0, "reported latency changed between loads of the same preset");
    ut.expectEquals (silentWindows, 0, "a freshly loaded preset was silent on speech");
    ut.expectEquals (posErrors, 0, "soundboard position outside the sound");
    ut.expectEquals (int (presetsSeen.size()), int (presetIds.size()));
    ut.expectLessThan (memEnd.privateMb - memWarm.privateMb, 50.0, "private memory growth");
    if (longRun && numBuckets >= 2)
    {
        // same mix of presets every hour (two passes through the 61): later hours must not cost more
        const auto& first = buckets.front();
        const auto& last = buckets.back();
        ut.expectLessOrEqual (last.meanK(), first.meanK() * 1.5, "mean block cost trends upward");
        ut.expectLessOrEqual (last.percentileK (0.99), first.percentileK (0.99) * 2.0, "p99 block cost trends upward");
        ut.expectLessOrEqual (silenceCost.meanK(), speechCost.meanK() * 1.5, "silence costs more than speech (denormals?)");
    }
    c.shutdown();
}

/** Regression probe: records the largest block an effect is handed. */
struct BlockProbe : IEffect
{
    int maxN = 0;
    void prepare (double, int) override {}
    void reset() override {}
    void setParam (int, float) override {}
    void process (float*, int n) override { maxN = std::max (maxN, n); }
};
} // namespace

class SoakShortTests : public juce::UnitTest
{
public:
    SoakShortTests() : juce::UnitTest ("Soak (short)", "Integration") {}

    void runTest() override
    {
        beginTest ("device reopened with a larger buffer: the old chain is never handed more than its block size");
        {
            VoiceProcessor vp;
            vp.setNoiseSuppression (false, 1.0f);
            vp.prepare (kSr, 480);
            auto probe = std::make_unique<BlockProbe>();
            auto* p = probe.get();
            std::vector<EffectChain::TestSlot> fx;
            fx.push_back ({ findEffectInfo ("eq"), std::move (probe) });
            vp.requestChain (EffectChain::createFromEffects (std::move (fx), kSr, 480));
            std::vector<float> in (1024, 0.1f), out (1024);
            for (int b = 0; b < 5; ++b) vp.process (in.data(), out.data(), nullptr, 480);
            vp.prepare (44100.0, 1024); // AudioEngine::open(); the controller builds the new chain only after the device started
            for (int b = 0; b < 5; ++b) vp.process (in.data(), out.data(), nullptr, 1024);
            expectGreaterThan (p->maxN, 0);
            expectLessOrEqual (p->maxN, 480);
        }

        beginTest ("monitor resampler never goes above the ceiling the processor clamped to (Catmull-Rom overshoot)");
        {
            const float ceiling = dsp::dbToGain (kLimiterCeilingDb);
            MonitorOutput m;
            m.prepareForTest (kSr, 44100.0, 441);
            m.setEnabled (true);
            m.setVolumeDb (0.0f);
            std::vector<float> in (480), out (441);
            for (int i = 0; i < 480; ++i) in[size_t (i)] = (i / 2) % 2 == 0 ? ceiling : -ceiling; // A, A, -A, -A: limited, near-square
            float peak = 0.0f;
            for (int b = 0; b < 200; ++b)
            {
                m.push (in.data(), 480);
                m.pullForTest (out.data(), 441);
                if (b > 20) for (auto v : out) peak = std::max (peak, std::abs (v));
            }
            expectGreaterThan (peak, 0.5f * ceiling);
            expectLessOrEqual (peak, ceiling + 1.0e-6f);
        }

        beginTest ("offline soak, 3 minutes of audio: finite, <= ceiling, no audio-thread allocation, memory, latency, soundboard, re-prepares");
        runSoak (*this, 180.0, false);
    }
};

class SoakLongTests : public juce::UnitTest
{
public:
    SoakLongTests() : juce::UnitTest ("Soak (offline AC-24)", "Diag") {}

    void runTest() override
    {
        double hours = juce::SystemStats::getEnvironmentVariable ("KOELOOM_SOAK_HOURS", "8").getDoubleValue();
        if (hours <= 0.0) hours = 8.0;
        beginTest ("offline soak, " + juce::String (hours, 2) + " h of audio (no device: XRUNs are not measurable here)");
        runSoak (*this, hours * 3600.0, true);
    }
};

static SoakShortTests soakShortTests;
static SoakLongTests soakLongTests;
} // namespace koe

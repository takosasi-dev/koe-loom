// T5: shifters single-threaded vs parallel (spec §5.9, F-15-1, Phase 0 (h)). No audio device:
// a dedicated thread runs at the real-time block period (timeBeginPeriod(1) + high-resolution
// waitable timer, the last ~200 us spun) and processes 3 Signalsmith instances per block.
//
//   p0_parallel [--minutes 1] [--buffers 128,256,480] [--modes single,parallel]
//               [--block 1440 --interval 240 --split 0] [--tag name] [--input file.wav]
//
// single  : the paced thread processes main, layer 1, layer 2 in order.
// parallel: 2 worker threads (MMCSS "Pro Audio"), the paced thread does the main voice and hands the
//           layers to the workers. Signals are atomics; waiters spin ~50 us then WaitOnAddress.
//           A layer not finished within 50 % of the buffer time is muted for that block (F-15-4);
//           while its worker is still busy the layer is skipped (counted again).
// expired : block processing time (wall clock, block start -> mix done) > buffer time (both modes).
// Writes results/05-parallel[-tag].csv and results/05-parallel[-tag]-run.txt.

#include "p0_common.h"
#include "p0_shifter.h"

#include <juce_events/juce_events.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <thread>

#include <windows.h>
#include <immintrin.h>
#include <mmsystem.h>

using namespace p0;

namespace
{
const float kPitch[3] = { 4.0f, -4.0f, -9.0f };     // main voice, layer 1, layer 2 (T2 parameter sets)
const float kFormant[3] = { 2.5f, -2.5f, -5.0f };


template <typename Pred>
void spinThenWait (std::atomic<uint32_t>& word, uint32_t notThis, Pred stop, double spinUs, DWORD waitMs)
{
    const double t0 = nowUs();
    while (word.load (std::memory_order_acquire) == notThis && ! stop())
    {
        if (nowUs() - t0 < spinUs) { _mm_pause(); continue; }
        WaitOnAddress (&word, &notThis, sizeof (notThis), waitMs);
    }
}

class Worker
{
public:
    Worker (SignalsmithShifter& s, int maxBlock, size_t maxJobs) : shifter (s)
    {
        in.assign (size_t (maxBlock), 0.0f);
        out.assign (size_t (maxBlock), 0.0f);
        wakeUs.assign (maxJobs, 0.0);
        th = std::thread ([this] { run(); });
        while (! started.load()) std::this_thread::yield();
    }
    ~Worker()
    {
        quit.store (true);
        go.fetch_add (1, std::memory_order_release);
        WakeByAddressSingle (&go);
        th.join();
    }

    bool idle() const { return done.load (std::memory_order_acquire) == go.load (std::memory_order_relaxed); }

    void start (const float* src, int n)
    {
        std::memcpy (in.data(), src, sizeof (float) * size_t (n));
        blockSize = n;
        signalUs.store (nowUs(), std::memory_order_relaxed);
        go.fetch_add (1, std::memory_order_release);
        WakeByAddressSingle (&go);
    }

    /** Wait until done or the absolute deadline (us). Returns true when finished. */
    bool waitUntil (double deadlineUs)
    {
        const uint32_t target = go.load (std::memory_order_relaxed);
        const double t0 = nowUs();
        while (done.load (std::memory_order_acquire) != target)
        {
            const double now = nowUs();
            if (now >= deadlineUs) return false;
            if (now - t0 < 50.0) { _mm_pause(); continue; }
            uint32_t seen = done.load (std::memory_order_acquire);
            if (seen == target) break;
            const DWORD ms = DWORD (std::max (1.0, std::ceil ((deadlineUs - now) / 1000.0)));
            WaitOnAddress (&done, &seen, sizeof (seen), ms);
        }
        return true;
    }

    std::vector<float> in, out;
    std::vector<double> wakeUs;
    size_t jobs = 0;
    bool mmcssOk = false;
    DWORD mmcssError = 0;

private:
    void run()
    {
        ScopedProAudio mmcss;   // entered and reverted on this thread
        mmcssOk = mmcss.ok();
        mmcssError = mmcss.error;
        started.store (true);
        uint32_t seen = 0;
        for (;;)
        {
            spinThenWait (go, seen, [] { return false; }, 50.0, INFINITE);
            seen = go.load (std::memory_order_acquire);
            if (quit.load()) return;
            const double startUs = nowUs();
            if (jobs < wakeUs.size()) wakeUs[jobs] = startUs - signalUs.load (std::memory_order_relaxed);
            ++jobs;
            shifter.process (in.data(), out.data(), blockSize);
            done.store (seen, std::memory_order_release);
            WakeByAddressSingle (&done);
        }
    }

    SignalsmithShifter& shifter;
    std::thread th;
    std::atomic<uint32_t> go { 0 }, done { 0 };
    std::atomic<double> signalUs { 0 };
    std::atomic<bool> quit { false }, started { false };
    int blockSize = 0;
};

std::vector<std::unique_ptr<SignalsmithShifter>> makeShifters (const ShifterConfig& cfg, int buffer)
{
    std::vector<std::unique_ptr<SignalsmithShifter>> v;
    for (int i = 0; i < 3; ++i)
    {
        v.push_back (std::make_unique<SignalsmithShifter> (cfg, i + 1));
        v.back()->prepare (kSr, buffer);
        v.back()->setPitchSemitones (kPitch[i]);
        v.back()->setFormantSemitones (kFormant[i]);
        v.back()->reset();
    }
    return v;
}

struct Result
{
    Stats proc, wake, lateStart;
    long long blocks = 0, expired = 0, layerMuted = 0;
    double cpuSeconds = 0, cycleSeconds = 0, wallSeconds = 0, systemBusy = 0;
    juce::String mmcss;
};

class Pacer
{
public:
    Pacer()
    {
        timer = CreateWaitableTimerExW (nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        highRes = timer != nullptr;
        if (! highRes) timer = CreateWaitableTimerW (nullptr, TRUE, nullptr);
    }
    ~Pacer() { CloseHandle (timer); }
    void waitUntil (double targetUs)
    {
        const double remain = targetUs - nowUs() - 200.0;    // spin the last ~200 us
        if (remain > 0)
        {
            LARGE_INTEGER due;
            due.QuadPart = -(LONGLONG) (remain * 10.0);    // relative, 100 ns units
            SetWaitableTimer (timer, &due, 0, nullptr, nullptr, FALSE);
            WaitForSingleObject (timer, INFINITE);
        }
        while (nowUs() < targetUs) _mm_pause();
    }
    bool highRes = false;

private:
    HANDLE timer = nullptr;
};

Result runCondition (const ShifterConfig& cfg, const std::vector<float>& input, int buffer, bool parallel, double minutes)
{
    Result r;
    auto sh = makeShifters (cfg, buffer);
    const double periodUs = 1.0e6 * buffer / kSr;
    const size_t numBlocks = size_t (minutes * 60.0 * kSr / buffer);
    std::vector<double> proc, late;
    proc.reserve (numBlocks);
    late.reserve (numBlocks);
    std::vector<float> o0 (size_t (buffer), 0.0f), o1 (size_t (buffer), 0.0f), o2 (size_t (buffer), 0.0f), mix (size_t (buffer), 0.0f);
    std::unique_ptr<Worker> w1, w2;
    if (parallel)
    {
        w1 = std::make_unique<Worker> (*sh[1], buffer, numBlocks);
        w2 = std::make_unique<Worker> (*sh[2], buffer, numBlocks);
    }
    ScopedProAudio mainMmcss;
    Pacer pacer;

    p0::SystemLoad load;
    load.start();
    const double cpu0 = processCpuSeconds(), cyc0 = processCycleSeconds();
    const double wall0 = nowUs();
    double next = nowUs() + periodUs;
    size_t pos = 0;
    for (size_t b = 0; b < numBlocks; ++b)
    {
        pacer.waitUntil (next);
        const double t0 = nowUs();
        late.push_back (t0 - next);
        if (pos + size_t (buffer) > input.size()) pos = 0;
        const float* src = input.data() + pos;
        pos += size_t (buffer);

        if (! parallel)
        {
            sh[0]->process (src, o0.data(), buffer);
            sh[1]->process (src, o1.data(), buffer);
            sh[2]->process (src, o2.data(), buffer);
            for (int i = 0; i < buffer; ++i) mix[size_t (i)] = (o0[size_t (i)] + o1[size_t (i)]) + o2[size_t (i)];
        }
        else
        {
            const bool run1 = w1->idle(), run2 = w2->idle();
            if (run1) w1->start (src, buffer);
            if (run2) w2->start (src, buffer);
            sh[0]->process (src, o0.data(), buffer);
            const double deadline = t0 + 0.5 * periodUs;
            const bool ok1 = run1 && w1->waitUntil (deadline);
            const bool ok2 = run2 && w2->waitUntil (deadline);
            r.layerMuted += (ok1 ? 0 : 1) + (ok2 ? 0 : 1);
            for (int i = 0; i < buffer; ++i)
                mix[size_t (i)] = (o0[size_t (i)] + (ok1 ? w1->out[size_t (i)] : 0.0f)) + (ok2 ? w2->out[size_t (i)] : 0.0f);
        }
        const double t1 = nowUs();
        proc.push_back (t1 - t0);
        if (t1 - t0 > periodUs) ++r.expired;
        next += periodUs;
    }
    r.wallSeconds = (nowUs() - wall0) * 1.0e-6;
    r.cpuSeconds = processCpuSeconds() - cpu0;
    r.cycleSeconds = processCycleSeconds() - cyc0;
    r.systemBusy = load.busyPercent();
    r.blocks = (long long) numBlocks;
    for (auto& v : proc) v = 100.0 * v / periodUs;
    r.proc = stats (proc);
    r.lateStart = stats (late);
    r.mmcss = "main:" + juce::String (mainMmcss.ok() ? "ok" : "fail(" + juce::String ((int) mainMmcss.error) + ")");
    if (parallel)
    {
        std::vector<double> wake (w1->wakeUs.begin(), w1->wakeUs.begin() + long (std::min (w1->jobs, w1->wakeUs.size())));
        wake.insert (wake.end(), w2->wakeUs.begin(), w2->wakeUs.begin() + long (std::min (w2->jobs, w2->wakeUs.size())));
        r.wake = stats (wake);
        r.mmcss << " w1:" << (w1->mmcssOk ? "ok" : "fail(" + juce::String ((int) w1->mmcssError) + ")")
                << " w2:" << (w2->mmcssOk ? "ok" : "fail(" + juce::String ((int) w2->mmcssError) + ")");
    }
    return r;
}

/** F-15-3: unpaced, no deadline. Same seeds and order -> outputs must match bit for bit. */
bool outputsIdentical (const ShifterConfig& cfg, const std::vector<float>& input, int buffer, int blocks)
{
    auto a = makeShifters (cfg, buffer), b = makeShifters (cfg, buffer);
    Worker w1 (*b[1], buffer, size_t (blocks)), w2 (*b[2], buffer, size_t (blocks));
    std::vector<float> o0 (size_t (buffer), 0.0f), o1 (size_t (buffer), 0.0f), o2 (size_t (buffer), 0.0f), m1 (size_t (buffer), 0.0f), p0v (size_t (buffer), 0.0f), m2 (size_t (buffer), 0.0f);
    for (int k = 0; k < blocks; ++k)
    {
        const float* src = input.data() + (size_t (k) * size_t (buffer)) % (input.size() - size_t (buffer));
        a[0]->process (src, o0.data(), buffer);
        a[1]->process (src, o1.data(), buffer);
        a[2]->process (src, o2.data(), buffer);
        for (int i = 0; i < buffer; ++i) m1[size_t (i)] = (o0[size_t (i)] + o1[size_t (i)]) + o2[size_t (i)];
        w1.start (src, buffer);
        w2.start (src, buffer);
        b[0]->process (src, p0v.data(), buffer);
        w1.waitUntil (1e300);
        w2.waitUntil (1e300);
        for (int i = 0; i < buffer; ++i) m2[size_t (i)] = (p0v[size_t (i)] + w1.out[size_t (i)]) + w2.out[size_t (i)];
        if (std::memcmp (m1.data(), m2.data(), sizeof (float) * size_t (buffer)) != 0) return false;
    }
    return true;
}
} // namespace

int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    Args args (argc, argv);
    const auto speech = loadSpeech (args);
    const double minutes = args.getDouble ("--minutes", 1.0);
    ShifterConfig cfg;
    cfg.blockSamples = args.getInt ("--block", 1440);
    cfg.intervalSamples = args.getInt ("--interval", 240);
    cfg.splitComputation = args.getInt ("--split", 0) != 0;
    cfg.name = "b" + std::to_string (cfg.blockSamples) + "-i" + std::to_string (cfg.intervalSamples) + (cfg.splitComputation ? "-split" : "");
    auto buffers = juce::StringArray::fromTokens (args.get ("--buffers", "128,256,480"), ",", "");
    auto modes = juce::StringArray::fromTokens (args.get ("--modes", "single,parallel"), ",", "");
    const juce::String tag = args.has ("--tag") ? "-" + args.get ("--tag") : juce::String();

    const UINT tbp = timeBeginPeriod (1);
    std::printf ("config %s, %.3f min per condition, input %s, timeBeginPeriod(1) %s, logical cores %u\n", cfg.name.c_str(), minutes,
                 speech.source.toRawUTF8(), tbp == TIMERR_NOERROR ? "ok" : "fail", std::thread::hardware_concurrency());

    juce::String csv = "mode,buffer,minutes,blocks,proc_p50_pct,proc_p99_pct,proc_max_pct,proc_p50_us,proc_p99_us,proc_max_us,expired,layer_muted,"
                       "wake_p50_us,wake_p99_us,wake_max_us,late_start_p99_us,late_start_max_us,process_cpu_s,wall_s,process_cpu_cores,process_cycle_cores,system_busy_pct,mmcss\n";
    juce::String identical;
    for (auto& bs : buffers)
    {
        const int buf = bs.getIntValue();
        const double periodUs = 1.0e6 * buf / kSr;
        const bool same = outputsIdentical (cfg, speech.x, buf, 2000);
        identical << "buffer " << buf << ": single and parallel outputs identical over 2000 blocks = " << (same ? "yes" : "NO") << "\n";
        std::printf ("buffer %d: F-15-3 identical = %s\n", buf, same ? "yes" : "NO");
        for (auto& m : modes)
        {
            const bool par = m == "parallel";
            auto r = runCondition (cfg, speech.x, buf, par, minutes);
            csv << m << "," << buf << "," << minutes << "," << r.blocks << "," << num (r.proc.p50) << "," << num (r.proc.p99) << "," << num (r.proc.max) << ","
                << num (r.proc.p50 * periodUs / 100.0) << "," << num (r.proc.p99 * periodUs / 100.0) << "," << num (r.proc.max * periodUs / 100.0) << ","
                << r.expired << "," << r.layerMuted << "," << (par ? num (r.wake.p50) : juce::String()) << "," << (par ? num (r.wake.p99) : juce::String()) << ","
                << (par ? num (r.wake.max) : juce::String()) << "," << num (r.lateStart.p99) << "," << num (r.lateStart.max) << "," << num (r.cpuSeconds) << ","
                << num (r.wallSeconds) << "," << num (r.cpuSeconds / r.wallSeconds, 4) << "," << num (r.cycleSeconds / r.wallSeconds, 4) << "," << num (r.systemBusy, 1) << "," << r.mmcss << "\n";
            std::printf ("%-8s %3d: p50 %7.3f%% p99 %7.3f%% max %8.3f%% expired %lld muted %lld wake p99 %8.3f us cpu %.3f cores (cycles %.4f) busy %.1f%% %s\n",
                         m.toRawUTF8(), buf, r.proc.p50, r.proc.p99, r.proc.max, r.expired, r.layerMuted, r.wake.p99,
                         r.cpuSeconds / r.wallSeconds, r.cycleSeconds / r.wallSeconds, r.systemBusy, r.mmcss.toRawUTF8());
        }
    }
    timeEndPeriod (1);

    juce::String log;
    log << "# generated by p0_parallel " << args.commandLine << "\n# date " << timestamp() << ", Release build, 48 kHz mono, input " << speech.source << "\n"
        << "# shifters: Signalsmith " << cfg.name << ", main " << kPitch[0] << "/" << kFormant[0] << ", layer1 " << kPitch[1] << "/" << kFormant[1]
        << ", layer2 " << kPitch[2] << "/" << kFormant[2] << " (pitch st / formant st)\n"
        << "# pacing: timeBeginPeriod(1) " << (tbp == TIMERR_NOERROR ? "ok" : "fail")
        << ", high-resolution waitable timer + ~200 us spin; logical cores " << (int) std::thread::hardware_concurrency() << "\n"
        << identical;
    writeText (resultsDir().getChildFile ("05-parallel" + tag + ".csv"), csv);
    writeText (resultsDir().getChildFile ("05-parallel" + tag + "-run.txt"), log);
    std::printf ("%s", log.toRawUTF8());
    return 0;
}

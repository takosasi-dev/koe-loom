// T4: monitor output on a second device with its own clock (spec §5.4, R-2, Phase 0 (d)).
// Needs VB-CABLE and a second output (headphones). Not run in Phase 0 (VB-CABLE not installed).
//
//   p0_monitor --monitor <headphone name part> [--minutes 30] [--source silence|sine|mic]
//              [--buffer 480] [--monitor-buffer 480] [--target <samples>]
//              [--type "Windows Audio (Low Latency Mode)"] [--monitor-type "Windows Audio"] [--in <mic part>]
//
// Main device (A): mic (only with --source mic) -> "CABLE Input". Its callback pushes the same signal
// into a lock-free SPSC FIFO. Monitor device (B, output only) pulls from the FIFO through an adaptive
// resampler: the read rate is 1 + P*e + I, e = (smoothed fill - target) / target, so the integral term
// converges to the clock ratio of A to B (reported in ppm). Default --source silence: nothing audible,
// the FIFO dynamics are the same. Per-second log: results/raw/monitor_<date>.csv, summary row appended
// to results/04-monitor-drift.csv. Pass condition (R-2): 0 underruns and 0 overflows.

#include "p0_device.h"

#include <atomic>
#include <cmath>
#include <cstdio>

using namespace p0;

namespace
{
constexpr int kCapacity = 48000;   // 1 s

struct Shared
{
    juce::AbstractFifo fifo { kCapacity };
    std::vector<float> data = std::vector<float> (size_t (kCapacity), 0.0f);
    std::atomic<long long> overflows { 0 }, underruns { 0 };
    std::atomic<double> ratioPpm { 0 }, clockPpm { 0 }, fillSmoothed { 0 };
    std::atomic<int> fillMin { kCapacity }, fillMax { 0 };
};

struct MainSide final : juce::AudioIODeviceCallback
{
    Shared& sh;
    int source = 0;                       // 0 silence, 1 sine, 2 mic
    double phase = 0;
    std::vector<float> mono = std::vector<float> (8192, 0.0f);
    explicit MainSide (Shared& s) : sh (s) {}

    void audioDeviceIOCallbackWithContext (const float* const* in, int numIn, float* const* out, int numOut, int n,
                                           const juce::AudioIODeviceCallbackContext&) override
    {
        n = std::min (n, int (mono.size()));
        for (int i = 0; i < n; ++i)
        {
            float s = 0.0f;
            if (source == 1) { s = float (0.1 * std::sin (phase)); phase += 2.0 * juce::MathConstants<double>::pi * 440.0 / 48000.0; }
            else if (source == 2 && numIn > 0) { for (int c = 0; c < numIn; ++c) s += in[c][i]; s /= float (numIn); }
            mono[size_t (i)] = s;
            for (int c = 0; c < numOut; ++c) out[c][i] = s;
        }
        if (phase > 1.0e6) phase = std::fmod (phase, 2.0 * juce::MathConstants<double>::pi);
        if (sh.fifo.getFreeSpace() < n) sh.overflows.fetch_add (1);
        const auto w = sh.fifo.write (std::min (n, sh.fifo.getFreeSpace()));
        std::copy (mono.begin(), mono.begin() + w.blockSize1, sh.data.begin() + w.startIndex1);
        std::copy (mono.begin() + w.blockSize1, mono.begin() + w.blockSize1 + w.blockSize2, sh.data.begin() + w.startIndex2);
    }
    void audioDeviceAboutToStart (juce::AudioIODevice*) override {}
    void audioDeviceStopped() override {}
};

struct MonitorSide final : juce::AudioIODeviceCallback
{
    Shared& sh;
    double target = 1920, kp = 0.002, ki = 7.0e-7;
    double frac = 0, integ = 0, ratio = 1, fillLp = 0;
    bool running = false;
    float h[4] {};
    std::vector<float> scratch = std::vector<float> (16384, 0.0f);
    explicit MonitorSide (Shared& s) : sh (s) {}

    void audioDeviceIOCallbackWithContext (const float* const*, int, float* const* out, int numOut, int n,
                                           const juce::AudioIODeviceCallbackContext&) override
    {
        const int ready = sh.fifo.getNumReady();
        if (! running)
        {
            for (int c = 0; c < numOut; ++c) juce::FloatVectorOperations::clear (out[c], n);
            if (ready >= int (target)) { running = true; fillLp = ready; }
            return;
        }
        // how many input samples this block consumes at the current ratio
        int need = 0;
        double f = frac;
        for (int i = 0; i < n; ++i) { f += ratio; while (f >= 1.0) { f -= 1.0; ++need; } }
        need = std::min (need, int (scratch.size()));
        const int take = std::min (need, ready);
        if (take < need) sh.underruns.fetch_add (1);
        const auto r = sh.fifo.read (take);
        std::copy (sh.data.begin() + r.startIndex1, sh.data.begin() + r.startIndex1 + r.blockSize1, scratch.begin());
        std::copy (sh.data.begin() + r.startIndex2, sh.data.begin() + r.startIndex2 + r.blockSize2, scratch.begin() + r.blockSize1);
        std::fill (scratch.begin() + take, scratch.begin() + need, 0.0f);

        int k = 0;
        for (int i = 0; i < n; ++i)
        {
            frac += ratio;
            while (frac >= 1.0)
            {
                frac -= 1.0;
                h[0] = h[1]; h[1] = h[2]; h[2] = h[3]; h[3] = k < need ? scratch[size_t (k)] : 0.0f;
                ++k;
            }
            // cubic Hermite between h[1] and h[2]
            const float t = float (frac);
            const float c1 = 0.5f * (h[2] - h[0]), c2 = h[0] - 2.5f * h[1] + 2.0f * h[2] - 0.5f * h[3];
            const float c3 = 0.5f * (h[3] - h[0]) + 1.5f * (h[1] - h[2]);
            const float y = ((c3 * t + c2) * t + c1) * t + h[1];
            for (int c = 0; c < numOut; ++c) out[c][i] = y;
        }

        // controller on the smoothed fill (1 s time constant)
        const int fill = sh.fifo.getNumReady();
        const double a = 1.0 - std::exp (-double (n) / 48000.0);
        fillLp += a * (fill - fillLp);
        const double e = (fillLp - target) / target;
        integ = std::clamp (integ + ki * e, -0.002, 0.002);
        ratio = std::clamp (1.0 + kp * e + integ, 0.995, 1.005);
        sh.ratioPpm.store ((ratio - 1.0) * 1.0e6);
        sh.clockPpm.store (integ * 1.0e6);
        sh.fillSmoothed.store (fillLp);
        if (fill < sh.fillMin.load()) sh.fillMin.store (fill);
        if (fill > sh.fillMax.load()) sh.fillMax.store (fill);
    }
    void audioDeviceAboutToStart (juce::AudioIODevice*) override {}
    void audioDeviceStopped() override {}
};
} // namespace

int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    Args args (argc, argv);
    const double minutes = args.getDouble ("--minutes", 30.0);
    const auto srcName = args.get ("--source", "silence");
    const int source = srcName == "sine" ? 1 : (srcName == "mic" ? 2 : 0);
    const auto type = args.get ("--type", "Windows Audio (Low Latency Mode)");
    const auto monType = args.get ("--monitor-type", "Windows Audio");
    const int buffer = args.getInt ("--buffer", 480), monBuffer = args.getInt ("--monitor-buffer", 480);
    if (! args.has ("--monitor")) { std::fprintf (stderr, "--monitor <headphone device name part> is required\n"); return 1; }

    juce::AudioDeviceManager a, b;
    const auto outName = findDeviceName (a, type, args.get ("--out", "CABLE Input"), false);
    const auto inName = source == 2 ? findDeviceName (a, type, args.get ("--in"), true) : juce::String();
    const auto monName = findDeviceName (b, monType, args.get ("--monitor"), false);
    if (outName.isEmpty() || ! outputIsSafe (outName, args)) { std::fprintf (stderr, "main output must be the virtual cable\n"); return 2; }
    if (monName.isEmpty()) { std::fprintf (stderr, "monitor device not found\n"); return 2; }

    Shared sh;
    MainSide mainCb (sh);
    mainCb.source = source;
    MonitorSide monCb (sh);
    monCb.target = args.getDouble ("--target", 2.0 * (buffer + monBuffer));
    if (auto err = openDevice (a, type, inName, outName, buffer); err.isNotEmpty()) { std::fprintf (stderr, "main open failed: %s\n", err.toRawUTF8()); return 3; }
    if (auto err = openDevice (b, monType, {}, monName, monBuffer); err.isNotEmpty()) { std::fprintf (stderr, "monitor open failed: %s\n", err.toRawUTF8()); return 3; }
    const auto sumA = deviceSummary (a), sumB = deviceSummary (b);
    std::printf ("main    %s\nmonitor %s\ntarget fill %.0f samples, source %s\n", sumA.toRawUTF8(), sumB.toRawUTF8(), monCb.target, srcName.toRawUTF8());

    juce::String csv = "elapsed_s,fill_smoothed,fill_min,fill_max,ratio_ppm,clock_estimate_ppm,underruns,overflows\n";
    b.addAudioCallback (&monCb);
    a.addAudioCallback (&mainCb);
    const double start = nowUs();
    for (int sec = 1; sec <= int (minutes * 60.0); ++sec)
    {
        while (nowUs() - start < sec * 1.0e6) juce::Thread::sleep (20);
        csv << sec << "," << num (sh.fillSmoothed.load(), 1) << "," << sh.fillMin.exchange (kCapacity) << "," << sh.fillMax.exchange (0) << ","
            << num (sh.ratioPpm.load()) << "," << num (sh.clockPpm.load()) << "," << sh.underruns.load() << "," << sh.overflows.load() << "\n";
        if (sec % 60 == 0) std::printf ("%d s: clock %.3f ppm, underruns %lld, overflows %lld\n", sec, sh.clockPpm.load(), sh.underruns.load(), sh.overflows.load());
    }
    a.removeAudioCallback (&mainCb);
    b.removeAudioCallback (&monCb);
    a.closeAudioDevice();
    b.closeAudioDevice();
    writeText (rawDir().getChildFile ("monitor_" + juce::Time::getCurrentTime().formatted ("%Y%m%d_%H%M%S") + ".csv"), csv);
    auto summaryFile = resultsDir().getChildFile ("04-monitor-drift.csv");
    juce::String row;
    if (! summaryFile.existsAsFile()) row << "date,minutes,source,target_fill,clock_estimate_ppm,underruns,overflows,main,monitor\n";
    row << timestamp() << "," << minutes << "," << srcName << "," << monCb.target << "," << num (sh.clockPpm.load()) << "," << sh.underruns.load() << ","
        << sh.overflows.load() << ",\"" << sumA << "\",\"" << sumB << "\"\n";
    summaryFile.appendText (row, false, false, "\n");
    std::printf ("done: clock %.3f ppm, underruns %lld, overflows %lld\n", sh.clockPpm.load(), sh.underruns.load(), sh.overflows.load());
    return 0;
}

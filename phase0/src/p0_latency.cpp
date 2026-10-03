// T1-7 / (e): loop latency output "CABLE Input" -> VB-CABLE -> input "CABLE Output", one AudioDeviceManager
// (spec Phase 0 (e), §13.1). Needs VB-CABLE. Not run in Phase 0 (VB-CABLE not installed).
//
//   p0_latency [--buffers 128,256,480] [--clicks 100] [--type "Windows Audio (Low Latency Mode)"]
//
// A 1-sample click (0.5) is written once per second; the first input sample with |x| > 0.1 after it
// is the arrival. Input and output share one callback (JUCE WASAPI drives both sides from one thread),
// so both use the same running sample counter. The result covers output buffer + cable + input
// buffer; the microphone's own input buffer is not included (see the reported latencies).
// Writes results/01-latency.csv (one row per buffer) and results/raw/latency_<buffer>.csv.

#include "p0_device.h"

#include <atomic>
#include <cstdio>

using namespace p0;

namespace
{
struct Clicker final : juce::AudioIODeviceCallback
{
    static constexpr int maxClicks = 1000;
    int wanted = 100;
    long long t = 0;                // samples since start
    long long clickAt = -1;         // pending click position
    long long arrivals[maxClicks] {};
    std::atomic<int> got { 0 };
    std::atomic<int> missed { 0 };

    void audioDeviceIOCallbackWithContext (const float* const* in, int numIn, float* const* out, int numOut, int n,
                                           const juce::AudioIODeviceCallbackContext&) override
    {
        for (int c = 0; c < numOut; ++c) juce::FloatVectorOperations::clear (out[c], n);
        for (int i = 0; i < n; ++i, ++t)
        {
            if (clickAt >= 0 && numIn > 0 && std::abs (in[0][i]) > 0.1f)
            {
                const int k = got.load();
                if (k < maxClicks) arrivals[k] = t - clickAt;
                got.store (k + 1);
                clickAt = -1;
            }
            if (clickAt >= 0 && t - clickAt > 24000) { missed.fetch_add (1); clickAt = -1; }   // 0.5 s without arrival
            if (t >= 96000 && t % 48000 == 0 && got.load() < wanted)                         // after 2 s warm-up
            {
                for (int c = 0; c < numOut; ++c) out[c][i] = 0.5f;
                clickAt = t;
            }
        }
    }
    void audioDeviceAboutToStart (juce::AudioIODevice*) override {}
    void audioDeviceStopped() override {}
};
} // namespace

int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    Args args (argc, argv);
    const auto type = args.get ("--type", "Windows Audio (Low Latency Mode)");
    const int clicks = std::min (Clicker::maxClicks, args.getInt ("--clicks", 100));
    auto summaryFile = resultsDir().getChildFile ("01-latency.csv");
    juce::String rows;
    if (! summaryFile.existsAsFile()) rows << "date,type,buffer,clicks,missed,min_ms,median_ms,max_ms,reported_in_samples,reported_out_samples,opened\n";

    for (auto& b : juce::StringArray::fromTokens (args.get ("--buffers", "128,256,480"), ",", ""))
    {
        const int buffer = b.getIntValue();
        juce::AudioDeviceManager dm;
        const auto outName = findDeviceName (dm, type, "CABLE Input", false);
        const auto inName = findDeviceName (dm, type, "CABLE Output", true);
        if (! outName.containsIgnoreCase ("CABLE Input") || ! inName.containsIgnoreCase ("CABLE Output"))
        {
            std::fprintf (stderr, "VB-CABLE not found\n");
            return 2;
        }
        if (auto err = openDevice (dm, type, inName, outName, buffer); err.isNotEmpty())
        {
            std::fprintf (stderr, "open failed: %s\n", err.toRawUTF8());
            return 3;
        }
        auto* dev = dm.getCurrentAudioDevice();
        const auto summary = deviceSummary (dm);
        std::printf ("%s\n", summary.toRawUTF8());
        Clicker cb;
        cb.wanted = clicks;
        dm.addAudioCallback (&cb);
        const double start = nowUs();
        while (cb.got.load() < clicks && nowUs() - start < (clicks + 10) * 1.0e6) juce::Thread::sleep (100);
        dm.removeAudioCallback (&cb);
        const int inLat = dev->getInputLatencyInSamples(), outLat = dev->getOutputLatencyInSamples();
        dm.closeAudioDevice();

        std::vector<double> ms;
        juce::String raw = "click,latency_samples,latency_ms\n";
        for (int k = 0; k < std::min (cb.got.load(), clicks); ++k)
        {
            ms.push_back (1000.0 * double (cb.arrivals[k]) / 48000.0);
            raw << k << "," << cb.arrivals[k] << "," << num (ms.back()) << "\n";
        }
        writeText (rawDir().getChildFile ("latency_" + juce::String (buffer) + ".csv"), raw);
        std::sort (ms.begin(), ms.end());
        const double med = ms.empty() ? 0 : (ms.size() % 2 ? ms[ms.size() / 2] : 0.5 * (ms[ms.size() / 2 - 1] + ms[ms.size() / 2]));
        rows << timestamp() << ",\"" << type << "\"," << buffer << "," << ms.size() << "," << cb.missed.load() << ","
             << (ms.empty() ? juce::String() : num (ms.front())) << "," << (ms.empty() ? juce::String() : num (med)) << ","
             << (ms.empty() ? juce::String() : num (ms.back())) << "," << inLat << "," << outLat << ",\"" << summary << "\"\n";
        std::printf ("buffer %d: %zu clicks, missed %d, min %.3f median %.3f max %.3f ms\n", buffer, ms.size(), cb.missed.load(),
                     ms.empty() ? 0.0 : ms.front(), med, ms.empty() ? 0.0 : ms.back());
    }
    summaryFile.appendText (rows, false, false, "\n");
    return 0;
}

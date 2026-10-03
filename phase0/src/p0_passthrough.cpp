// T1-2: mic -> virtual cable with input and output as separate devices (spec Phase 0 (a), §13.1).
// Needs VB-CABLE. Not run in Phase 0 (VB-CABLE not installed).
//
//   p0_passthrough --buffer 480 [--minutes 30] [--source mic|sine] [--with-mic] [--type "Windows Audio (Low Latency Mode)"]
//                  [--in <mic name part>] [--out "CABLE Input"] [--allow-any-output]
//
// --source sine plays 440 Hz at -20 dBFS (phase-continuous); record it with p0_capture and check it with
// tools/analyze_sine.py. That checks the output side + cable. --with-mic also opens the microphone (its
// samples are not used) so the input side's drift shows up in the XRUN and zero_head counts in the same
// run. Output must be "CABLE Input" (nothing goes to speakers) unless --allow-any-output.
// Every second a row goes to results/raw/passthrough_<type>_<buffer>_<source>.csv:
//   XRUNs (AudioIODevice::getXRunCount(); JUCE WASAPI counts only capture DATA_DISCONTINUITY flags, -1 if
//   unavailable), longest callback, callbacks, and "zero_head" = callbacks whose input started with >= 16
//   exact zeros followed by signal: JUCE's WASAPI code zero-fills the block start when the input
//   reservoir is short (copyBuffersFromReservoir), which getXRunCount() does not count.
// A summary row is appended to results/01-passthrough.csv.

#include "p0_device.h"

#include <atomic>
#include <cmath>
#include <cstdio>

using namespace p0;

namespace
{
struct Pass final : juce::AudioIODeviceCallback
{
    bool sine = false;
    double phase = 0.0;
    std::atomic<double> maxUs { 0 };
    std::atomic<long long> callbacks { 0 }, zeroHead { 0 };

    void audioDeviceIOCallbackWithContext (const float* const* in, int numIn, float* const* out, int numOut, int n,
                                           const juce::AudioIODeviceCallbackContext&) override
    {
        const double t0 = nowUs();
        constexpr double inc = 2.0 * juce::MathConstants<double>::pi * 440.0 / 48000.0;
        for (int i = 0; i < n; ++i)
        {
            float s = 0.0f;
            if (sine)
            {
                s = float (0.1 * std::sin (phase));   // -20 dBFS peak
                phase += inc;
                if (phase > 2.0 * juce::MathConstants<double>::pi) phase -= 2.0 * juce::MathConstants<double>::pi;
            }
            else if (numIn > 0)
            {
                for (int c = 0; c < numIn; ++c) s += in[c][i];
                s /= float (numIn);
            }
            for (int c = 0; c < numOut; ++c) out[c][i] = s;
        }
        if (numIn > 0)
        {
            int k = 0;
            while (k < n && in[0][k] == 0.0f) ++k;
            if (k >= 16 && k < n) zeroHead.fetch_add (1);
        }
        callbacks.fetch_add (1);
        const double us = nowUs() - t0;
        if (us > maxUs.load()) maxUs.store (us);
    }
    void audioDeviceAboutToStart (juce::AudioIODevice*) override {}
    void audioDeviceStopped() override {}
};
} // namespace

int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    Args args (argc, argv);
    const int buffer = args.getInt ("--buffer", 480);
    const double minutes = args.getDouble ("--minutes", 30.0);
    const bool sine = args.get ("--source", "mic") == "sine";
    const auto type = args.get ("--type", "Windows Audio (Low Latency Mode)");

    juce::AudioDeviceManager dm;
    const auto outName = findDeviceName (dm, type, args.get ("--out", "CABLE Input"), false);
    const auto inName = (sine && ! args.has ("--with-mic")) ? juce::String() : findDeviceName (dm, type, args.get ("--in"), true);
    if (outName.isEmpty() || ! outputIsSafe (outName, args))
    {
        std::fprintf (stderr, "output device not found or not the virtual cable ('%s'). Install VB-CABLE or pass --allow-any-output.\n", outName.toRawUTF8());
        return 2;
    }
    Pass cb;
    cb.sine = sine;
    if (auto err = openDevice (dm, type, inName, outName, buffer); err.isNotEmpty())
    {
        std::fprintf (stderr, "open failed: %s\n", err.toRawUTF8());
        return 3;
    }
    auto* dev = dm.getCurrentAudioDevice();
    const auto summary = deviceSummary (dm);
    std::printf ("%s\n", summary.toRawUTF8());

    const auto tag = type.retainCharacters ("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ") + "_" + juce::String (buffer) + "_" + (sine ? "sine" : "mic");
    juce::String csv = "elapsed_s,xruns_total,callback_max_us,callback_max_pct,callbacks_total,zero_head_total,buffer,sample_rate\n";
    const double bufferUs = 1.0e6 * dev->getCurrentBufferSizeSamples() / dev->getCurrentSampleRate();
    dm.addAudioCallback (&cb);
    const double start = nowUs();
    double worstPct = 0;
    for (int sec = 1; sec <= int (minutes * 60.0); ++sec)
    {
        while (nowUs() - start < sec * 1.0e6) juce::Thread::sleep (20);
        const double m = cb.maxUs.exchange (0.0);
        worstPct = std::max (worstPct, 100.0 * m / bufferUs);
        csv << sec << "," << dev->getXRunCount() << "," << num (m) << "," << num (100.0 * m / bufferUs) << "," << cb.callbacks.load() << ","
            << cb.zeroHead.load() << "," << dev->getCurrentBufferSizeSamples() << "," << num (dev->getCurrentSampleRate(), 0) << "\n";
        if (sec % 60 == 0) std::printf ("%d s: xruns %d zero_head %lld\n", sec, dev->getXRunCount(), cb.zeroHead.load());
    }
    dm.removeAudioCallback (&cb);
    const int xruns = dev->getXRunCount();
    dm.closeAudioDevice();
    writeText (rawDir().getChildFile ("passthrough_" + tag + ".csv"), csv);

    auto summaryFile = resultsDir().getChildFile ("01-passthrough.csv");
    juce::String row;
    if (! summaryFile.existsAsFile()) row << "date,type,buffer,source,minutes,xruns,zero_head_blocks,worst_callback_pct,opened\n";
    row << timestamp() << ",\"" << type << "\"," << buffer << "," << (sine ? "sine" : "mic") << "," << minutes << "," << xruns << ","
        << cb.zeroHead.load() << "," << num (worstPct) << ",\"" << summary << "\"\n";
    summaryFile.appendText (row, false, false, "\n");
    std::printf ("done: xruns %d, zero_head %lld, worst callback %.3f %%\n", xruns, cb.zeroHead.load(), worstPct);
    return 0;
}

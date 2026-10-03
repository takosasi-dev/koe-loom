// T1-3: record the virtual cable's capture side to a WAV (spec Phase 0 (a)). Needs VB-CABLE.
// Not run in Phase 0 (VB-CABLE not installed). Input only: nothing is played.
//
//   p0_capture [--minutes 31] [--in "CABLE Output"] [--type "Windows Audio"] [--buffer 480] [--out file.wav]
//
// Default output: results/raw/capture_<yyyymmdd_hhmmss>.wav (24-bit, left channel). Start this first,
// then p0_passthrough --source sine, then analyse with tools/analyze_sine.py.

#include "p0_device.h"

#include <juce_audio_formats/juce_audio_formats.h>

#include <cstdio>

using namespace p0;

namespace
{
struct Recorder final : juce::AudioIODeviceCallback
{
    juce::AudioFormatWriter::ThreadedWriter* writer = nullptr;
    std::atomic<long long> samples { 0 }, dropped { 0 };

    void audioDeviceIOCallbackWithContext (const float* const* in, int numIn, float* const* out, int numOut, int n,
                                           const juce::AudioIODeviceCallbackContext&) override
    {
        for (int c = 0; c < numOut; ++c) juce::FloatVectorOperations::clear (out[c], n);
        if (numIn > 0 && writer != nullptr)
        {
            if (! writer->write (in, n)) dropped.fetch_add (n);
            samples.fetch_add (n);
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
    const double minutes = args.getDouble ("--minutes", 31.0);
    const auto type = args.get ("--type", "Windows Audio");
    const int buffer = args.getInt ("--buffer", 480);
    juce::File outFile = args.has ("--out") ? juce::File (args.get ("--out"))
                                            : rawDir().getChildFile ("capture_" + juce::Time::getCurrentTime().formatted ("%Y%m%d_%H%M%S") + ".wav");

    juce::AudioDeviceManager dm;
    const auto inName = findDeviceName (dm, type, args.get ("--in", "CABLE Output"), true);
    if (inName.isEmpty() || ! inName.containsIgnoreCase (args.get ("--in", "CABLE Output")))
    {
        std::fprintf (stderr, "capture device '%s' not found (is VB-CABLE installed?)\n", args.get ("--in", "CABLE Output").toRawUTF8());
        return 2;
    }

    outFile.deleteFile();
    std::unique_ptr<juce::FileOutputStream> os (outFile.createOutputStream());
    juce::WavAudioFormat wav;
    std::unique_ptr<juce::AudioFormatWriter> w (os ? wav.createWriterFor (os.get(), 48000.0, 1, 24, {}, 0) : nullptr);
    if (w == nullptr) { std::fprintf (stderr, "cannot write %s\n", outFile.getFullPathName().toRawUTF8()); return 4; }
    os.release();
    juce::TimeSliceThread io ("wav writer");
    io.startThread();
    auto threaded = std::make_unique<juce::AudioFormatWriter::ThreadedWriter> (w.release(), io, 1 << 18);

    Recorder rec;
    rec.writer = threaded.get();
    if (auto err = openDevice (dm, type, inName, {}, buffer); err.isNotEmpty())
    {
        std::fprintf (stderr, "open failed: %s\n", err.toRawUTF8());
        return 3;
    }
    auto* dev = dm.getCurrentAudioDevice();
    std::printf ("%s\nrecording %.2f min to %s\n", deviceSummary (dm).toRawUTF8(), minutes, outFile.getFullPathName().toRawUTF8());
    dm.addAudioCallback (&rec);
    const double start = nowUs();
    while (nowUs() - start < minutes * 60.0e6) juce::Thread::sleep (100);
    dm.removeAudioCallback (&rec);
    const int xruns = dev->getXRunCount();
    dm.closeAudioDevice();
    threaded.reset();   // flushes
    std::printf ("done: %lld samples, %lld dropped by the writer FIFO, capture xruns %d\n", rec.samples.load(), rec.dropped.load(), xruns);
    return rec.dropped.load() == 0 ? 0 : 5;
}

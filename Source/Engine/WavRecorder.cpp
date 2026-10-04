#include "Engine/WavRecorder.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace koe
{
namespace
{
constexpr double kFifoSeconds = 4.0;   // the writer may stall this long (disk busy) before samples are lost
constexpr int kChunk = 8192;
juce::String u8 (const char* s) { return juce::String::fromUTF8 (s); }
} // namespace

std::unique_ptr<WavRecorder> WavRecorder::create (const juce::File& file, double sampleRate, juce::String& error)
{
    if (! file.getParentDirectory().createDirectory())
    {
        error = u8 ("録音のフォルダを作れませんでした（") + file.getParentDirectory().getFullPathName() + u8 ("）。");
        return nullptr;
    }
    std::unique_ptr<juce::FileOutputStream> os (file.createOutputStream());
    if (os == nullptr || ! os->openedOk())
    {
        error = u8 ("録音のファイルを作れませんでした（") + file.getFullPathName() + u8 ("）。フォルダに書き込めるか確認してください。");
        return nullptr;
    }
    std::unique_ptr<juce::AudioFormatWriter> w (juce::WavAudioFormat().createWriterFor (os.get(), sampleRate, 1, 24, {}, 0));
    if (w == nullptr)
    {
        error = u8 ("このサンプルレートでは WAV を書けません（") + juce::String (sampleRate, 0) + " Hz" + u8 ("）。");
        os.reset();
        file.deleteFile();
        return nullptr;
    }
    os.release(); // the writer owns it now
    std::unique_ptr<WavRecorder> r (new WavRecorder (file, sampleRate, std::move (w)));
    r->startThread (juce::Thread::Priority::low); // the owner may be playing a game on this PC
    return r;
}

WavRecorder::WavRecorder (const juce::File& f, double sampleRate, std::unique_ptr<juce::AudioFormatWriter> w)
    : juce::Thread ("KoeLoom WAV"),
      file (f),
      rate (sampleRate),
      writer (std::move (w)),
      fifo (std::max (kChunk, int (sampleRate * kFifoSeconds))),
      ring (size_t (fifo.getTotalSize()), 0.0f),
      chunk (size_t (kChunk), 0.0f)
{
}

WavRecorder::~WavRecorder() { finish(); }

void WavRecorder::push (const float* samples, int numSamples)
{
    if (! accepting.load (std::memory_order_relaxed) || failure.load (std::memory_order_relaxed) != 0) return;
    const auto w = fifo.write (numSamples);
    if (w.blockSize1 > 0) std::memcpy (ring.data() + w.startIndex1, samples, size_t (w.blockSize1) * sizeof (float));
    if (w.blockSize2 > 0) std::memcpy (ring.data() + w.startIndex2, samples + w.blockSize1, size_t (w.blockSize2) * sizeof (float));
    const int written = w.blockSize1 + w.blockSize2;
    pushed.fetch_add (written, std::memory_order_relaxed);
    if (written < numSamples)
    {
        int expected = 0;
        failure.compare_exchange_strong (expected, int (Failure::overflow));
    }
}

void WavRecorder::run()
{
    while (! threadShouldExit())
    {
        wait (20);
        drain();
    }
    drain();
}

void WavRecorder::drain()
{
    for (;;)
    {
        const int ready = std::min (fifo.getNumReady(), kChunk);
        if (ready <= 0) return;
        {
            const auto r = fifo.read (ready);
            if (r.blockSize1 > 0) std::memcpy (chunk.data(), ring.data() + r.startIndex1, size_t (r.blockSize1) * sizeof (float));
            if (r.blockSize2 > 0) std::memcpy (chunk.data() + r.blockSize1, ring.data() + r.startIndex2, size_t (r.blockSize2) * sizeof (float));
        }
        if (failure.load() != 0) continue; // stopped writing: just empty the FIFO
        const float* ch[] = { chunk.data() };
        if (failForTests.exchange (false) || ! writer->writeFromFloatArrays (ch, 1, ready))
        {
            int expected = 0;
            failure.compare_exchange_strong (expected, int (Failure::write));
        }
    }
}

bool WavRecorder::finish()
{
    if (finished) return getFailure() == Failure::none;
    finished = true;
    accepting.store (false);
    signalThreadShouldExit();
    notify();
    stopThread (-1); // run() drains the FIFO before it returns
    if (writer != nullptr && ! writer->flush())
    {
        int expected = 0;
        failure.compare_exchange_strong (expected, int (Failure::write));
    }
    writer.reset(); // closes the file
    return getFailure() == Failure::none;
}
} // namespace koe

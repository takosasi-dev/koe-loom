#include "Engine/VoiceAnalysis.h"

#include "Tools/Calibrate.h"

#include <algorithm>
#include <cmath>

namespace koe
{
namespace
{
float rmsDbOf (const float* x, int n)
{
    double e = 0.0;
    for (int i = 0; i < n; ++i) e += double (x[i]) * x[i];
    return n > 0 ? float (10.0 * std::log10 (e / n + 1.0e-30)) : -300.0f;
}
} // namespace

// ============================================================================ TapRing
TapRing::TapRing (int capacity)
{
    const int size = juce::nextPowerOfTwo (juce::jmax (16, capacity));
    buf = std::make_unique<std::atomic<float>[]> (size_t (size)); // value-initialised: zeros
    mask = size - 1;
}

void TapRing::push (const float* x, int n)
{
    const long long w = written.load (std::memory_order_relaxed);
    for (int i = 0; i < n; ++i)
    {
        const float v = x[i];
        buf[size_t ((w + i) & mask)].store (std::isfinite (v) ? v : 0.0f, std::memory_order_relaxed);
    }
    written.store (w + n, std::memory_order_release);
}

bool TapRing::readLatest (float* dest, int n) const noexcept
{
    // half the ring: the writer stays well clear of what is being copied
    if (n <= 0 || n > (mask + 1) / 2) return false;
    const long long w = written.load (std::memory_order_acquire);
    if (w < n) return false;
    for (int i = 0; i < n; ++i) dest[i] = buf[size_t ((w - n + i) & mask)].load (std::memory_order_relaxed);
    return true;
}

// ============================================================================ PitchMeter
float PitchMeter::measure (const float* x, int n, double sampleRate)
{
    if (n <= 0) return 0.0f;
    const int tail = juce::jmin (n, int (sampleRate * 0.04));
    if (rmsDbOf (x + n - tail, tail) < kQuietDb) return 0.0f;
    if (preparedRate != sampleRate)
    {
        detector.prepare (sampleRate);
        preparedRate = sampleRate;
    }
    else
        detector.reset();
    detector.process (x, n);
    return detector.getFrequencyHz();
}

float PitchMeter::poll (const TapRing& ring, double sampleRate)
{
    const int n = int (sampleRate * 0.048);
    if (int (scratch.size()) < n) scratch.resize (size_t (n));
    const long long total = ring.getWritten();
    const bool fresh = total != lastWritten; // nothing new (device stopped): no pitch rather than the last one
    lastWritten = total;
    const float raw = fresh && ring.readLatest (scratch.data(), n) ? measure (scratch.data(), n, sampleRate) : 0.0f;
    history[size_t (historyPos)] = raw;
    historyPos = (historyPos + 1) % int (history.size());

    float v[3];
    int k = 0;
    for (float h : history)
        if (h > 0.0f) v[k++] = h;
    if (k < 2)
    {
        smoothed = 0.0f;
        return 0.0f;
    }
    std::sort (v, v + k);
    const float target = k == 3 ? v[1] : std::sqrt (v[0] * v[1]);
    smoothed = smoothed > 0.0f ? smoothed * std::sqrt (target / smoothed) : target; // half way, in semitones
    return smoothed;
}

void PitchMeter::reset()
{
    history.fill (0.0f);
    historyPos = 0;
    smoothed = 0.0f;
    lastWritten = -1;
}

juce::String noteNameForHz (float hz)
{
    if (! (hz > 0.0f)) return {};
    static const char* const names[] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
    const int m = int (std::lround (dsp::hzToMidi (hz)));
    return juce::String (names[((m % 12) + 12) % 12]) + juce::String (int (std::floor (m / 12.0)) - 1);
}

// ============================================================================ TakeBuffer
void TakeBuffer::start (int samples)
{
    data.assign (size_t (juce::jmax (0, samples)), 0.0f);
    count.store (0);
}

void TakeBuffer::push (const float* x, int n)
{
    const long long c = count.load (std::memory_order_relaxed);
    const long long room = (long long) data.size() - c;
    if (room <= 0) return;
    const int m = int (std::min<long long> (room, n));
    for (int i = 0; i < m; ++i) data[size_t (c + i)] = std::isfinite (x[i]) ? x[i] : 0.0f;
    count.store (c + m, std::memory_order_release);
}

juce::String checkCalibrationTake (const std::vector<float>& x, double sampleRate)
{
    const int frame = juce::jmax (1, int (sampleRate * 0.02));
    const int frames = int (x.size()) / frame;
    long long clipped = 0;
    for (float v : x)
        if (std::abs (v) >= 0.99f) ++clipped;
    std::vector<float> db (size_t (juce::jmax (0, frames)));
    float loudest = -300.0f;
    for (int f = 0; f < frames; ++f)
        loudest = juce::jmax (loudest, db[size_t (f)] = rmsDbOf (x.data() + size_t (f) * size_t (frame), frame));

    if (loudest < PitchMeter::kQuietDb)
        return juce::String::fromUTF8 ("声が小さすぎて測れませんでした。マイクに近づくか、入力ゲインを上げてください。");
    if (clipped * 1000 > (long long) x.size()) // more than 0.1 % of the samples at full scale
        return juce::String::fromUTF8 ("声が大きすぎて割れていました。入力ゲインを下げるか、マイクから少し離れてください。");
    // voice: frames within 30 dB of the loudest and above the quiet level (room noise does not count)
    const float floorDb = juce::jmax (PitchMeter::kQuietDb, loudest - 30.0f);
    const auto voiced = std::count_if (db.begin(), db.end(), [floorDb] (float d) { return d >= floorDb; });
    if (double (voiced) * frame < 3.0 * sampleRate)
        return juce::String::fromUTF8 ("声の区間が短すぎました。10 秒のあいだに 3 秒以上話してください。");
    return {};
}

// ============================================================================ CalibrationJob
CalibrationJob::CalibrationJob (std::vector<float> t, double sampleRate, Preset ref, std::vector<Preset> list)
    : juce::Thread ("KoeLoom calibration"), take (std::move (t)), rate (sampleRate), reference (std::move (ref)), presets (std::move (list))
{
}

CalibrationJob::~CalibrationJob() { stopThread (-1); }

void CalibrationJob::start() { startThread (juce::Thread::Priority::low); } // the owner may be playing a game on the same PC

void CalibrationJob::run()
{
    const float refDb = tools::presetLevelDb (reference, take, rate);
    done.store (1);
    for (auto& p : presets)
    {
        if (threadShouldExit())
        {
            cancelled.store (true);
            break;
        }
        const float diff = tools::presetLevelDb (p, take, rate) - refDb; // p with its shipped trim
        trims[juce::String (p.id)] = tools::calibratedTrimDb (p.outputTrimDb, diff);
        done.fetch_add (1);
    }
    finished.store (true, std::memory_order_release);
}
} // namespace koe

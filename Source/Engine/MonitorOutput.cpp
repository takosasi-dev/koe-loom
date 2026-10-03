#include "Engine/MonitorOutput.h"

#include "Dsp/Building.h"

#include <algorithm>
#include <cmath>
#include <thread>
#include <vector>

namespace koe
{
namespace
{
// Adaptive resampler (spec §5.4, R-2; Phase 0 T4). The read speed is
//   ratio = (mainRate / monitorRate) * (1 + kp * e + I),   e = (fill_lp - target) / target,   dI/dt = ki * e
// fill_lp is the FIFO fill (main-rate samples, measured after each read) through a 1 s one-pole low-pass.
// I converges to the clock difference of the two devices (getRatioPpm; negative = the monitor clock is faster).
// Tuned offline (10 min, ±300 ppm, 44.1/48 kHz, 4 ms callback jitter): I ripples < ±25 ppm, fill stays > 800 samples.
constexpr double kKp = 0.001;
constexpr double kKiPerSecond = 3.0e-5;
constexpr double kFillTauSeconds = 1.0;
constexpr double kMaxIntegral = 0.002;      // ±2000 ppm
constexpr double kMaxCorrection = 0.005;
constexpr double kTargetBlocks = 1.5;       // target fill = 1.5 * (main block + monitor block); ponytail: fixed, tune from real-device runs
constexpr float kGlitchFadeMs = 2.0f;       // fade to silence on underrun / overrun
constexpr int kChunk = 1024;
// the processor clamps the monitor mix to the limiter ceiling; Catmull-Rom overshoots up to 1.25x on
// such limited (near-square) material, so clamp again after interpolating
const float kCeiling = dsp::dbToGain (kLimiterCeilingDb);

double nearestRate (const juce::Array<double>& rates, double wanted)
{
    double best = rates.isEmpty() ? wanted : rates[0];
    for (auto r : rates)
        if (std::abs (r - wanted) < std::abs (best - wanted)) best = r;
    return best;
}
} // namespace

struct MonitorOutput::Impl final : juce::AudioIODeviceCallback
{
    // ---- shared between the threads ----
    juce::AbstractFifo fifo { 2 };
    std::vector<float> data;
    std::atomic<bool> active { false }, pushing { false }, resync { false };
    std::atomic<int> maxPush { 0 }, underruns { 0 }, overruns { 0 };
    std::atomic<double> ppm { 0.0 };
    std::atomic<bool> enabled { false };
    std::atomic<bool> silent { false };   // pull side: disabled and faded out, so push() may skip
    std::atomic<bool> lost { false };     // the device stopped or failed by itself
    bool stoppingOnPurpose = false;       // message thread
    std::atomic<float> volumeDb { kMonitorVolumeDb.def };

    // ---- device (message thread) ----
    std::unique_ptr<juce::AudioIODeviceType> type;
    std::unique_ptr<juce::AudioIODevice> device;

    // ---- pull side (monitor device thread, or the test) ----
    double nominal = 1.0, ratio = 1.0, monRate = 48000.0;
    int monBlock = 480;
    bool running = false;
    double frac = 0.0, integral = 0.0, fillLp = 0.0;
    float h0 = 0, h1 = 0, h2 = 0, h3 = 0;
    int glitchSteps = 96;
    dsp::Ramp gain, glitch;
    std::vector<float> scratch;

    /** Stops push() from touching the FIFO (Dekker handshake with push(), all seq_cst). */
    void deactivate()
    {
        active.store (false);
        while (pushing.load()) std::this_thread::yield();
    }

    /** Message thread, no callback running. Allocates. */
    void prepareState (double mainRate, double monitorRate, int monitorBlock)
    {
        deactivate();
        const int capacity = std::max (16384, int (mainRate)); // 1 s
        data.assign (size_t (capacity), 0.0f);
        fifo.setTotalSize (capacity);
        monRate = monitorRate > 0.0 ? monitorRate : 48000.0;
        monBlock = std::max (1, monitorBlock);
        nominal = mainRate / monRate;
        ratio = nominal;
        scratch.assign (size_t (std::ceil (kChunk * nominal * (1.0 + 2.0 * kMaxCorrection))) + 8, 0.0f);
        running = false;
        frac = integral = fillLp = 0.0;
        h0 = h1 = h2 = h3 = 0.0f;
        glitchSteps = std::max (1, int (dsp::msToSamples (kGlitchFadeMs, monRate)));
        gain.prepare (monRate, kParamSmoothMs);
        gain.snap (0.0f);
        glitch.prepare (monRate, kGlitchFadeMs);
        glitch.snap (0.0f);
        maxPush.store (0);
        underruns.store (0);
        overruns.store (0);
        ppm.store (0.0);
        resync.store (false);
        silent.store (false);
        active.store (true);
    }

    double targetFill() const noexcept { return kTargetBlocks * (maxPush.load (std::memory_order_relaxed) + monBlock * nominal); }

    void push (const float* x, int n)
    {
        pushing.store (true);
        if (! active.load()) {}
        else if (! enabled.load (std::memory_order_relaxed) && silent.load (std::memory_order_relaxed))
            resync.store (true); // off and faded out: skip the copy; the reader restarts from fresh data once it is on
        else
        {
            if (n > maxPush.load (std::memory_order_relaxed)) maxPush.store (n, std::memory_order_relaxed);
            int s1, n1, s2, n2;
            fifo.prepareToWrite (n, s1, n1, s2, n2);
            if (n1 + n2 < n)
            {
                overruns.fetch_add (1);
                resync.store (true); // the reader fades out and drops the surplus
            }
            std::copy (x, x + n1, data.begin() + s1);
            std::copy (x + n1, x + n1 + n2, data.begin() + s2);
            fifo.finishedWrite (n1 + n2);
        }
        pushing.store (false);
    }

    void discard (int n) { if (n > 0) fifo.finishedRead (std::min (n, fifo.getNumReady())); }

    void pull (float* out, int n)
    {
        if (! active.load())
        {
            std::fill (out, out + n, 0.0f);
            return;
        }
        for (int pos = 0; pos < n; pos += kChunk) pullChunk (out + pos, std::min (kChunk, n - pos));
    }

    void pullChunk (float* out, int n)
    {
        const bool on = enabled.load (std::memory_order_relaxed);
        gain.setTarget (on ? dsp::dbToGain (volumeDb.load (std::memory_order_relaxed)) : 0.0f);
        silent.store (! on && ! gain.isRamping() && gain.value <= 0.0f, std::memory_order_relaxed);
        const double target = targetFill();
        const int ready = fifo.getNumReady();

        if (! running)
        {
            std::fill (out, out + n, 0.0f);
            for (int i = 0; i < n; ++i) gain.next();
            if (resync.exchange (false))
            {
                // a push was dropped: what the FIFO holds ends at the drop and does not join up with the
                // next push, so throw it all away and start again from fresh data
                discard (fifo.getNumReady());
                return;
            }
            if (ready >= target && maxPush.load (std::memory_order_relaxed) > 0)
            {
                discard (ready - int (target)); // stale surplus would only add latency
                running = true;
                frac = 0.0;
                h0 = h1 = h2 = h3 = 0.0f;
                glitch.snap (0.0f);
                glitch.steps = glitchSteps;
                glitch.setTarget (1.0f);
                fillLp = fifo.getNumReady();
            }
            return;
        }

        // input samples this chunk consumes (same arithmetic as the loop below, so k never passes need)
        int need = 0;
        double f = frac;
        for (int i = 0; i < n; ++i)
        {
            f += ratio;
            while (f >= 1.0) { f -= 1.0; ++need; }
        }
        const bool underrun = need > ready;
        const bool overrun = ! underrun && resync.load();
        const int take = std::min (need, ready);
        int s1, n1, s2, n2;
        fifo.prepareToRead (take, s1, n1, s2, n2);
        std::copy (data.begin() + s1, data.begin() + s1 + n1, scratch.begin());
        std::copy (data.begin() + s2, data.begin() + s2 + n2, scratch.begin() + n1);
        fifo.finishedRead (n1 + n2);
        // past the end of the data (underrun) the last sample is held, so the fade below never has to
        // cut short even when the FIFO ran dry exactly at a chunk boundary
        std::fill (scratch.begin() + take, scratch.begin() + need, take > 0 ? scratch[size_t (take - 1)] : h3);

        if (underrun || overrun)
        {
            // fade to silence within this chunk, before the data runs out (underrun) or ahead of the drop (overrun)
            glitch.steps = std::max (1, std::min (glitchSteps, n));
            glitch.setTarget (0.0f);
            if (underrun) underruns.fetch_add (1);
        }

        int k = 0;
        for (int i = 0; i < n; ++i)
        {
            frac += ratio;
            while (frac >= 1.0)
            {
                frac -= 1.0;
                h0 = h1; h1 = h2; h2 = h3; h3 = scratch[size_t (k++)];
            }
            // cubic Hermite (Catmull-Rom) between h1 and h2
            const float t = float (frac);
            const float c1 = 0.5f * (h2 - h0);
            const float c2 = h0 - 2.5f * h1 + 2.0f * h2 - 0.5f * h3;
            const float c3 = 0.5f * (h3 - h0) + 1.5f * (h1 - h2);
            const float y = ((c3 * t + c2) * t + c1) * t + h1;
            out[i] = std::clamp (y, -kCeiling, kCeiling) * glitch.next() * gain.next();
        }

        if (underrun || overrun)
        {
            running = false; // (after an overrun the FIFO is flushed first) wait for the target fill, then fade in
            return;
        }

        const double dt = n / monRate;
        fillLp += (1.0 - std::exp (-dt / kFillTauSeconds)) * (fifo.getNumReady() - fillLp);
        const double e = (fillLp - target) / target;
        integral = std::clamp (integral + kKiPerSecond * dt * e, -kMaxIntegral, kMaxIntegral);
        ratio = nominal * (1.0 + std::clamp (kKp * e + integral, -kMaxCorrection, kMaxCorrection));
        ppm.store (integral * 1.0e6, std::memory_order_relaxed);
    }

    // ---- juce::AudioIODeviceCallback (monitor device thread) ----
    void audioDeviceIOCallbackWithContext (const float* const*, int, float* const* out, int numOut, int n,
                                           const juce::AudioIODeviceCallbackContext&) override
    {
        if (numOut <= 0 || out[0] == nullptr) return;
        pull (out[0], n);
        for (int c = 1; c < numOut; ++c)
            if (out[c] != nullptr) juce::FloatVectorOperations::copy (out[c], out[0], n); // stereo, L = R
    }
    void audioDeviceAboutToStart (juce::AudioIODevice*) override {}
    void audioDeviceStopped() override { if (! stoppingOnPurpose) lost.store (true); } // JUCE closes a removed device
    void audioDeviceError (const juce::String&) override { lost.store (true); }
};

MonitorOutput::MonitorOutput() : impl (std::make_unique<Impl>()) {}
MonitorOutput::~MonitorOutput() { close(); }

juce::String MonitorOutput::open (const juce::String& outputDeviceName, double mainSampleRate)
{
    close();
    juce::String err = juce::String::fromUTF8 ("デバイスが見つかりません");
    // Low-latency shared mode first; plain shared mode accepts more formats. Any device rate works
    // because the resampler converts from the main rate.
    for (auto mode : { juce::WASAPIDeviceMode::sharedLowLatency, juce::WASAPIDeviceMode::shared })
    {
        std::unique_ptr<juce::AudioIODeviceType> t (juce::AudioIODeviceType::createAudioIODeviceType_WASAPI (mode));
        if (t == nullptr) continue;
        t->scanForDevices();
        if (! t->getDeviceNames (false).contains (outputDeviceName)) continue;
        std::unique_ptr<juce::AudioIODevice> d (t->createDevice (outputDeviceName, {})); // output only
        if (d == nullptr) continue;
        juce::BigInteger outCh;
        outCh.setRange (0, std::min (2, d->getOutputChannelNames().size()), true);
        const auto e = d->open ({}, outCh, nearestRate (d->getAvailableSampleRates(), mainSampleRate), d->getDefaultBufferSize());
        if (e.isNotEmpty())
        {
            err = e;
            continue;
        }
        impl->prepareState (mainSampleRate, d->getCurrentSampleRate(), d->getCurrentBufferSizeSamples());
        impl->lost.store (false);
        impl->type = std::move (t);
        impl->device = std::move (d);
        impl->device->start (impl.get());
        return {};
    }
    return err;
}

void MonitorOutput::close()
{
    if (impl->device != nullptr)
    {
        impl->stoppingOnPurpose = true;
        impl->device->stop();
        impl->device->close();
        impl->device.reset();
        impl->stoppingOnPurpose = false;
    }
    impl->type.reset();
    impl->deactivate();
}

bool MonitorOutput::isOpen() const noexcept { return impl->device != nullptr && impl->device->isPlaying(); }
void MonitorOutput::setEnabled (bool on) noexcept { impl->enabled.store (on); }
bool MonitorOutput::fetchDeviceLost() noexcept { return impl->lost.exchange (false); }
void MonitorOutput::simulateDeviceLostForTest() { impl->audioDeviceError ("test"); }
void MonitorOutput::setVolumeDb (float db) noexcept { impl->volumeDb.store (kMonitorVolumeDb.clamp (db)); }
void MonitorOutput::push (const float* samples, int numSamples) { impl->push (samples, numSamples); }
int MonitorOutput::getUnderruns() const noexcept { return impl->underruns.load(); }
int MonitorOutput::getOverruns() const noexcept { return impl->overruns.load(); }
double MonitorOutput::getRatioPpm() const noexcept { return impl->ppm.load(); }

void MonitorOutput::prepareForTest (double mainRate, double monitorRate, int monitorBlock)
{
    close();
    impl->prepareState (mainRate, monitorRate, monitorBlock);
}

void MonitorOutput::pullForTest (float* out, int numSamples) { impl->pull (out, numSamples); }
} // namespace koe

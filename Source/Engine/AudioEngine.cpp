#include "Engine/AudioEngine.h"

#include "Effects/EffectRegistry.h"

#include <algorithm>
#include <cmath>

namespace koe
{
namespace
{
template <typename T>
T nearestOf (const juce::Array<T>& options, T wanted)
{
    if (options.isEmpty()) return wanted;
    T best = options[0];
    for (auto o : options)
        if (std::abs (double (o) - double (wanted)) < std::abs (double (best) - double (wanted))) best = o;
    return best;
}

/** Bridges device-thread notifications to the message thread; cancelled automatically on destruction. */
struct Notifier final : juce::AsyncUpdater
{
    std::function<void()> fn;
    void handleAsyncUpdate() override { if (fn) fn(); }
};
} // namespace

struct AudioEngine::Notifiers
{
    Notifier listChanged, error;
    juce::CriticalSection lock;
    juce::String lastError;
};

AudioEngine::AudioEngine (VoiceProcessor& processor) : vp (processor)
{
    notifiers = std::make_unique<Notifiers>();
    auto& n = notifiers;
    n->listChanged.fn = [this] { if (onDeviceListChanged) onDeviceListChanged(); };
    n->error.fn = [this]
    {
        juce::String msg;
        {
            const juce::ScopedLock sl (notifiers->lock);
            msg = notifiers->lastError;
        }
        if (onDeviceError) onDeviceError (msg);
    };

    type.reset (juce::AudioIODeviceType::createAudioIODeviceType_WASAPI (juce::WASAPIDeviceMode::sharedLowLatency));
    if (type == nullptr) type.reset (juce::AudioIODeviceType::createAudioIODeviceType_WASAPI (juce::WASAPIDeviceMode::shared));
    if (type != nullptr) type->addListener (this);
}

AudioEngine::~AudioEngine()
{
    close();
    if (type != nullptr) type->removeListener (this);
    notifiers.reset();
}

void AudioEngine::scanDevices()
{
    if (type != nullptr) type->scanForDevices();
}

juce::StringArray AudioEngine::getInputNames() const { return type != nullptr ? type->getDeviceNames (true) : juce::StringArray(); }
juce::StringArray AudioEngine::getOutputNames() const { return type != nullptr ? type->getDeviceNames (false) : juce::StringArray(); }
juce::String AudioEngine::getTypeName() const { return type != nullptr ? type->getTypeName() : juce::String(); }

AudioEngine::DeviceCaps AudioEngine::getCaps (const juce::String& input, const juce::String& output) const
{
    DeviceCaps caps;
    if (type == nullptr) return caps;
    std::unique_ptr<juce::AudioIODevice> d (type->createDevice (output, input)); // not opened
    if (d == nullptr) return caps;
    caps.sampleRates = d->getAvailableSampleRates();
    caps.bufferSizes = d->getAvailableBufferSizes();
    caps.defaultBufferSize = d->getDefaultBufferSize();
    return caps;
}

juce::String AudioEngine::open (const juce::String& input, const juce::String& output, double sampleRate, int bufferSize)
{
    close();
    if (type == nullptr) return juce::String::fromUTF8 ("Windows のオーディオ (WASAPI) を使えません");
    std::unique_ptr<juce::AudioIODevice> d (type->createDevice (output, input));
    if (d == nullptr) return juce::String::fromUTF8 ("デバイスが見つかりません");

    juce::BigInteger inCh, outCh;
    inCh.setRange (0, std::min (2, d->getInputChannelNames().size()), true);
    outCh.setRange (0, std::min (2, d->getOutputChannelNames().size()), true);
    const double rate = nearestOf (d->getAvailableSampleRates(), sampleRate);   // E-05: nearest supported
    auto sizes = d->getAvailableBufferSizes();
    const int buffer = sizes.contains (bufferSize) ? bufferSize : nearestOf (sizes, bufferSize > 0 ? bufferSize : d->getDefaultBufferSize());

    const auto err = d->open (inCh, outCh, rate, buffer);
    if (err.isNotEmpty()) return err;

    const double actualRate = d->getCurrentSampleRate();
    const int actualBuffer = d->getCurrentBufferSizeSamples();
    vp.prepare (actualRate, actualBuffer);
    const size_t cap = size_t (std::max (actualBuffer, 4096));
    mono.assign (cap, 0.0f);
    outL.assign (cap, 0.0f);
    outR.assign (cap, 0.0f);
    currentRate.store (actualRate);
    currentBuffer.store (actualBuffer);
    deviceLatency.store (d->getInputLatencyInSamples() + d->getOutputLatencyInSamples());
    overruns.store (0);
    silentSamples.store (0);
    watchdog.reset();

    device = std::move (d);
    running.store (true);
    device->start (this);
    return {};
}

void AudioEngine::close()
{
    if (device != nullptr)
    {
        stoppingOnPurpose = true;
        device->stop();
        device->close();
        device.reset();
        stoppingOnPurpose = false;
    }
    running.store (false);
}

int AudioEngine::getXRunCount() const noexcept
{
    const int dev = device != nullptr ? device->getXRunCount() : 0;
    return std::max (0, dev) + overruns.load();
}

void AudioEngine::audioDeviceIOCallbackWithContext (const float* const* in, int numIn, float* const* out, int numOut, int numSamples,
                                                    const juce::AudioIODeviceCallbackContext&)
{
    const auto t0 = juce::Time::getHighResolutionTicks();
    const double sr = currentRate.load (std::memory_order_relaxed);

    for (int pos = 0; pos < numSamples;)
    {
        const int n = std::min (numSamples - pos, int (mono.size()));
        // mono input: average of the channels (A-12)
        int used = 0;
        std::fill (mono.begin(), mono.begin() + n, 0.0f);
        for (int c = 0; c < numIn; ++c)
            if (in[c] != nullptr)
            {
                ++used;
                for (int i = 0; i < n; ++i) mono[size_t (i)] += in[c][pos + i];
            }
        bool allZero = true;
        if (used > 1)
            for (int i = 0; i < n; ++i) mono[size_t (i)] /= float (used);
        for (int i = 0; i < n && allZero; ++i) allZero = mono[size_t (i)] == 0.0f;
        if (allZero && used > 0) silentSamples.fetch_add (n, std::memory_order_relaxed);
        else silentSamples.store (0, std::memory_order_relaxed);

        vp.process (mono.data(), outL.data(), outR.data(), n);

        for (int c = 0; c < numOut; ++c)
        {
            if (out[c] == nullptr) continue;
            const float* src = c == 0 ? outL.data() : (c == 1 ? outR.data() : nullptr);
            if (src != nullptr) std::copy (src, src + n, out[c] + pos);
            else std::fill (out[c] + pos, out[c] + pos + n, 0.0f);
        }
        pos += n;
    }

    // ---- load, deadline misses, watchdog (§5.6) ----
    const double blockSeconds = numSamples / sr;
    const double elapsed = juce::Time::highResolutionTicksToSeconds (juce::Time::getHighResolutionTicks() - t0);
    const double load = blockSeconds > 0.0 ? elapsed / blockSeconds : 0.0;
    if (load > 1.0) overruns.fetch_add (1, std::memory_order_relaxed);
    cpuLoad.store (float (0.9 * cpuLoad.load (std::memory_order_relaxed) + 0.1 * load), std::memory_order_relaxed);

    auto* chain = vp.getActiveChainAudio();
    int heavySlot = -1;
    if (chain != nullptr)
        for (int i = chain->size() - 1; i >= 0 && heavySlot < 0; --i)
        {
            auto& s = chain->slot (i);
            if (s.running && s.info->weight == EffectWeight::heavy && ! s.autoStopped.load (std::memory_order_relaxed)) heavySlot = i;
        }
    switch (watchdog.update (load, blockSeconds, vp.anyLayerRunning(), heavySlot >= 0))
    {
        case Watchdog::Action::stopLayers:
            vp.autoStopLayers();
            layersStoppedEvent.store (true);
            break;
        case Watchdog::Action::stopHeavySlot:
            chain->autoStop (heavySlot);
            heavyStoppedEvent.store (heavySlot);
            break;
        case Watchdog::Action::notifyOnly:
            overloadEvent.store (true);
            break;
        case Watchdog::Action::none: break;
    }
}

void AudioEngine::audioDeviceAboutToStart (juce::AudioIODevice*) {}

void AudioEngine::audioDeviceStopped()
{
    if (stoppingOnPurpose) return;
    running.store (false);
    if (auto& n = notifiers)
    {
        {
            const juce::ScopedLock sl (n->lock);
            n->lastError = "stopped";
        }
        n->error.triggerAsyncUpdate();
    }
}

void AudioEngine::audioDeviceError (const juce::String& message)
{
    if (auto& n = notifiers)
    {
        {
            const juce::ScopedLock sl (n->lock);
            n->lastError = message;
        }
        n->error.triggerAsyncUpdate();
    }
}

void AudioEngine::audioDeviceListChanged()
{
    if (auto& n = notifiers) n->listChanged.triggerAsyncUpdate();
}
} // namespace koe

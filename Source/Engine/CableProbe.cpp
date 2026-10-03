#include "Engine/CableProbe.h"

#include <juce_audio_devices/juce_audio_devices.h>

#include <atomic>
#include <cmath>

namespace koe
{
struct CableProbe::Impl final : juce::AudioIODeviceCallback
{
    std::unique_ptr<juce::AudioIODeviceType> type;
    std::unique_ptr<juce::AudioIODevice> device;
    std::atomic<float> peak { 0.0f };

    void audioDeviceIOCallbackWithContext (const float* const* in, int numIn, float* const* out, int numOut, int n,
                                           const juce::AudioIODeviceCallbackContext&) override
    {
        float p = 0.0f;
        for (int c = 0; c < numIn; ++c)
            if (in[c] != nullptr)
                for (int i = 0; i < n; ++i) p = std::max (p, std::abs (in[c][i]));
        for (int c = 0; c < numOut; ++c)
            if (out[c] != nullptr) juce::FloatVectorOperations::clear (out[c], n);
        float cur = peak.load (std::memory_order_relaxed);
        while (p > cur && ! peak.compare_exchange_weak (cur, p, std::memory_order_relaxed)) {}
    }
    void audioDeviceAboutToStart (juce::AudioIODevice*) override {}
    void audioDeviceStopped() override {}
};

CableProbe::CableProbe() : impl (std::make_unique<Impl>()) {}
CableProbe::~CableProbe() { stop(); }

juce::String CableProbe::start (const juce::String& inputName)
{
    stop();
    juce::String err = juce::String::fromUTF8 ("「") + inputName + juce::String::fromUTF8 ("」が見つかりません。");
    for (auto mode : { juce::WASAPIDeviceMode::sharedLowLatency, juce::WASAPIDeviceMode::shared })
    {
        impl->type.reset (juce::AudioIODeviceType::createAudioIODeviceType_WASAPI (mode));
        if (impl->type == nullptr) continue;
        impl->type->scanForDevices();
        if (! impl->type->getDeviceNames (true).contains (inputName)) continue;
        std::unique_ptr<juce::AudioIODevice> d (impl->type->createDevice ({}, inputName)); // input only, never an output
        if (d == nullptr) continue;
        juce::BigInteger inCh;
        inCh.setRange (0, std::min (2, d->getInputChannelNames().size()), true);
        const auto rates = d->getAvailableSampleRates();
        const double rate = rates.contains (48000.0) || rates.isEmpty() ? 48000.0 : rates[0];
        const auto e = d->open (inCh, {}, rate, d->getDefaultBufferSize());
        if (e.isNotEmpty())
        {
            err = juce::String::fromUTF8 ("仮想マイクの録音側を開けませんでした（") + e + juce::String::fromUTF8 ("）。");
            continue;
        }
        impl->peak.store (0.0f);
        impl->device = std::move (d);
        impl->device->start (impl.get());
        return {};
    }
    impl->type.reset();
    return err;
}

void CableProbe::stop()
{
    if (impl->device != nullptr)
    {
        impl->device->stop();
        impl->device->close();
        impl->device.reset();
    }
    impl->type.reset();
}

bool CableProbe::isRunning() const { return impl->device != nullptr && impl->device->isPlaying(); }

float CableProbe::fetchPeak() { return impl->peak.exchange (0.0f); }
} // namespace koe

#pragma once

#include "Engine/VoiceProcessor.h"
#include "Engine/Watchdog.h"

#include <juce_audio_devices/juce_audio_devices.h>

#include <atomic>
#include <functional>
#include <memory>
#include <vector>

namespace koe
{
/**
    Main audio device: input = microphone, output = virtual cable (spec §5.4). Uses JUCE's WASAPI
    device type directly (shared low-latency mode preferred, D-11) so nothing is ever opened
    implicitly (D-13: no default devices). Message thread API; the callback drives VoiceProcessor.
*/
class AudioEngine final : private juce::AudioIODeviceCallback, private juce::AudioIODeviceType::Listener
{
public:
    explicit AudioEngine (VoiceProcessor& processor);
    ~AudioEngine() override;

    // ---- devices (message thread) ----
    void scanDevices();
    juce::StringArray getInputNames() const;
    juce::StringArray getOutputNames() const;
    juce::String getTypeName() const;
    /** Called (message thread) when Windows' device list changes (plug / unplug, F-01-4). */
    std::function<void()> onDeviceListChanged;
    /** Called (message thread) when the open device reports an error or stops unexpectedly. */
    std::function<void (const juce::String&)> onDeviceError;

    struct DeviceCaps { juce::Array<double> sampleRates; juce::Array<int> bufferSizes; int defaultBufferSize = 0; };
    DeviceCaps getCaps (const juce::String& input, const juce::String& output) const;

    /** Opens input+output (closing any previous device), prepares the processor and starts.
        Returns an error string (Japanese) or empty on success (E-04/E-05 handled by the caller's text).
        exclusive: WASAPI exclusive mode (S-03 詳細); no fallback here, the caller decides. */
    juce::String open (const juce::String& input, const juce::String& output, double sampleRate, int bufferSize,
                       bool exclusive = false);
    void close();
    bool isRunning() const noexcept { return running.load(); }
    bool isExclusive() const noexcept { return running.load() && openedExclusive; }

    /** Settings::inputChannel: 0 = average of the open channels (the first two; the only one on a mono device),
        1 = left, 2 = right (left on a mono device), 3 = average of both. Any thread. */
    void setInputChannel (int mode) noexcept { inputChannel.store (mode); }
    /** The callback's input mix: n samples from pos of the device channels into mono, by the mode above.
        Returns how many channels were used (0 = no input channel). Public for tests. */
    static int mixInput (const float* const* in, int numIn, int pos, int n, int mode, float* mono) noexcept;

    // ---- status (any thread) ----
    double getSampleRate() const noexcept { return currentRate.load(); }
    int getBufferSize() const noexcept { return currentBuffer.load(); }
    /** Device-reported input + output latency (samples). */
    int getDeviceLatencySamples() const noexcept { return deviceLatency.load(); }
    /** Device XRUNs if the driver reports them, plus callbacks that ran over their deadline. */
    int getXRunCount() const noexcept;
    /** Smoothed callback load 0..1+ (wall time / buffer time, F-08-4). */
    float getCpuLoad() const noexcept { return cpuLoad.load(); }
    /** True while every input sample has been exactly 0 for 10 s with the device open (E-15). */
    bool isInputSilent() const noexcept { return silentSamples.load() >= (long long) (kSilentInputSeconds * currentRate.load()); }

    // ---- watchdog events (message thread polls) ----
    bool fetchLayersAutoStopped() noexcept { return layersStoppedEvent.exchange (false); }
    int fetchHeavySlotStopped() noexcept { return heavyStoppedEvent.exchange (-1); }
    bool fetchOverloadNotice() noexcept { return overloadEvent.exchange (false); }

private:
    void audioDeviceIOCallbackWithContext (const float* const* in, int numIn, float* const* out, int numOut, int numSamples,
                                           const juce::AudioIODeviceCallbackContext&) override;
    void audioDeviceAboutToStart (juce::AudioIODevice* device) override;
    void audioDeviceStopped() override;
    void audioDeviceError (const juce::String& message) override;
    void audioDeviceListChanged() override;

    VoiceProcessor& vp;
    std::unique_ptr<juce::AudioIODeviceType> type, exclusiveType; // exclusiveType: made on first use
    bool openedExclusive = false;
    std::atomic<int> inputChannel { 0 };
    std::unique_ptr<juce::AudioIODevice> device;

    std::vector<float> mono, outL, outR;
    std::atomic<bool> running { false };
    std::atomic<double> currentRate { kSampleRate };
    std::atomic<int> currentBuffer { 0 }, deviceLatency { 0 };
    std::atomic<float> cpuLoad { 0.0f };
    std::atomic<int> overruns { 0 };
    std::atomic<long long> silentSamples { 0 };
    Watchdog watchdog;
    std::atomic<bool> layersStoppedEvent { false }, overloadEvent { false };
    std::atomic<int> heavyStoppedEvent { -1 };
    bool stoppingOnPurpose = false;

    struct Notifiers; // device-thread -> message-thread bridges (AsyncUpdater)
    std::unique_ptr<Notifiers> notifiers;
};
} // namespace koe

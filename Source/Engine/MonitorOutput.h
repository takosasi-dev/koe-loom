#pragma once

#include "Engine/VoiceProcessor.h"

#include <juce_audio_devices/juce_audio_devices.h>

#include <atomic>
#include <memory>

namespace koe
{
/**
    Monitor output on a second, output-only device (headphones; F-01-3, F-08-5, spec §5.4, R-2).
    The main callback push()es the monitor mix into a lock-free FIFO; this device's own callback pulls
    it through an adaptive resampler whose ratio follows the FIFO fill, absorbing the clock difference
    (and a different sample rate) between the two devices. Never opens a virtual cable (F-01-6, checked
    by the caller). Volume and on/off fade smoothly.
*/
class MonitorOutput final : public IMonitorSink
{
public:
    MonitorOutput();
    ~MonitorOutput() override;

    /** Opens the named output device (message thread). mainSampleRate = the rate push() delivers. */
    juce::String open (const juce::String& outputDeviceName, double mainSampleRate);
    void close();
    bool isOpen() const noexcept;

    void setEnabled (bool on) noexcept;      // fades in/out over ~30 ms; once faded out, push() skips the copy
    /** True once after the device stopped or failed by itself (unplugged, F-01-4). Message thread. */
    bool fetchDeviceLost() noexcept;
    void setVolumeDb (float db) noexcept;

    /** Main audio thread. */
    void push (const float* samples, int numSamples) override;

    // ---- diagnostics / tests ----
    int getUnderruns() const noexcept;
    int getOverruns() const noexcept;
    double getRatioPpm() const noexcept;     // measured clock-ratio correction, ppm
    /** Tests: run the pull side without a device (as if the monitor device asked for n samples). */
    void prepareForTest (double mainRate, double monitorRate, int monitorBlock);
    void pullForTest (float* out, int numSamples);
    /** Tests: as if the device reported an error (unplugged). */
    void simulateDeviceLostForTest();

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
} // namespace koe

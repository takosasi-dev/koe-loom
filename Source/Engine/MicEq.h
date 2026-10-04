#pragma once

// マイクの癖の補正 (INTERFACES.md §11.3). Owner: wave9/voice.
// The measurement (long-term average spectrum of the voiced part of a take, compared with a published speech spectrum),
// the 14 peaking filters that apply the result (an IVoicePostProcessor on VoiceProcessor::setInputFilter), and the
// background job that runs the measurement. MicEqFilter::process is the only audio-thread code here.

#include "Core/Constants.h"
#include "Dsp/Building.h"
#include "Engine/VoiceProcessor.h"

#include <juce_core/juce_core.h>

#include <array>
#include <atomic>
#include <vector>

namespace koe
{
using MicEqBands = std::array<float, kMicEqBands>;

/** The target: ANSI S3.5-1997 Table 3, standard speech spectrum level for normal vocal effort (dB, 1/3-octave bands
    160 Hz .. 8 kHz), interpolated on log frequency at kMicEqBandHz. Bands outside 160 Hz .. 8 kHz (125 Hz, 11.2 kHz)
    are NaN: the table says nothing there, so micEqGainsFromLevels continues the nearest band instead of guessing. */
MicEqBands micEqTargetDb();

/** Long-term average spectrum of the voiced frames of x (frames within 30 dB of the loudest and above -50 dBFS, the
    rule of checkCalibrationTake), as the mean power per Hz in each half-octave band (dB, arbitrary offset).
    NaN for a band above 0.45 x sampleRate or with no FFT bin. All NaN when nothing is voiced. */
MicEqBands micEqBandLevelsDb (const std::vector<float>& x, double sampleRate);

/** Correction from measured band levels: target - measured, bands outside the table continue their neighbour, the
    mean taken off (the loudness stays), smoothed with the neighbours (1/4, 1/2, 1/4), within +-kMicEqMaxDb with mean 0. */
std::vector<float> micEqGainsFromLevels (const MicEqBands& levelsDb);

/** Peaking filter gains (dB) that make the 14-filter cascade hit gainsDb at each band centre (the filters overlap, so
    they are solved for, not copied). Bands above 0.45 x sampleRate get 0 (not used). */
MicEqBands micEqFilterGains (const std::vector<float>& gainsDb, double sampleRate);

/** Response (dB) of the cascade designed from gainsDb at hz (tests, and the solver above). */
float micEqResponseDb (const std::vector<float>& gainsDb, double sampleRate, double hz);

/**
    Applies the correction in place. Off (and faded out): returns at once, the samples untouched bit for bit.
    ON/OFF fades over 30 ms; a new set of gains crossfades from the old one over 30 ms. Coefficients are made on the
    message thread (setGains) and handed over without allocation or locks, like VoiceProcessor's chain hand-over.
*/
class MicEqFilter final : public IVoicePostProcessor
{
public:
    MicEqFilter() = default;
    ~MicEqFilter() override;

    /** Message thread. gainsDb: kMicEqBands values at kMicEqBandHz. */
    void setGains (const std::vector<float>& gainsDb, double sampleRate);
    /** Any thread. */
    void setEnabled (bool on) noexcept { enabled.store (on, std::memory_order_release); }
    /** True while off and fully faded out: the owner may take it out of the path. Any thread. */
    bool isIdle() const noexcept { return idle.load (std::memory_order_acquire); }
    /** Message thread: frees coefficient sets the audio thread has let go of. */
    void collectGarbage();

    void process (float* samples, int numSamples) override;

private:
    struct Program
    {
        std::array<dsp::Biquad, kMicEqBands> bands;
        int count = 0;          // bands in use (those below 0.45 x rate)
        int fadeSamples = 1;    // 30 ms
        float run (float x) noexcept
        {
            for (int b = 0; b < count; ++b) x = bands[size_t (b)].process (x);
            return x;
        }
        void reset() noexcept { for (auto& q : bands) q.reset(); }
    };

    std::atomic<bool> enabled { false }, idle { true };
    std::atomic<Program*> pending { nullptr };
    std::array<std::atomic<Program*>, 2> retired {};
    // audio thread only
    Program* current = nullptr;
    Program* previous = nullptr;    // fading out
    Program* toRetire = nullptr;    // no free retire slot yet
    int crossLeft = 0;
    float mix = 0.0f;
    bool running = false;
};

/** Works out the gains from a take on a low-priority thread. Looks at cancel() between steps. */
class MicEqJob final : private juce::Thread
{
public:
    MicEqJob (std::vector<float> take, double sampleRate);
    ~MicEqJob() override; // cancels and waits
    void start();
    void cancel() { signalThreadShouldExit(); }
    bool isFinished() const noexcept { return finished.load (std::memory_order_acquire); }
    bool wasCancelled() const noexcept { return cancelled.load(); }
    /** After isFinished() && ! wasCancelled(): kMicEqBands gains, or empty with error (Japanese). */
    const std::vector<float>& getGains() const noexcept { return gains; }
    const juce::String& getError() const noexcept { return error; }

private:
    void run() override;
    const std::vector<float> take;
    const double rate;
    std::vector<float> gains;
    juce::String error;
    std::atomic<bool> finished { false }, cancelled { false };
};
} // namespace koe

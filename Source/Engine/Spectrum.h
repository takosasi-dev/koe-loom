#pragma once

// 声の見える化 (INTERFACES.md §12.3). Owner: wave10/viz.
// Message thread only: the newest ~85 ms of a TapRing -> Hann window, FFT zero-padded x2 (juce::dsp::FFT) ->
// kSpectrumBands log-spaced bands (kSpectrumLowHz..kSpectrumHighHz) in dB, then smoothed over time (fast up, slow down).
// The audio thread only writes the ring.

#include "Core/Constants.h"
#include "Engine/VoiceAnalysis.h"

#include <juce_dsp/juce_dsp.h>

#include <array>
#include <memory>
#include <vector>

namespace koe
{
using SpectrumBands = std::array<float, kSpectrumBands>;

class SpectrumAnalyser
{
public:
    static constexpr float kFloorDb = -120.0f;    // silence, and bands above Nyquist
    static constexpr float kAttack = 0.6f;        // share of the way to a louder frame (~30 frames/s: ~40 ms)
    static constexpr float kRelease = 0.12f;      // ... to a quieter one (~250 ms): the shape of the voice stays readable
    static constexpr int kStalePolls = 15;        // polls without new samples (~0.5 s) before "no sound data"

    /** Geometric centre of band b, Hz. */
    static float bandCentreHz (int band) noexcept;
    /** The band whose range holds hz; -1 outside kSpectrumLowHz..kSpectrumHighHz. */
    static int bandFor (float hz) noexcept;
    /** Samples one frame looks at: ~85 ms (4096 at 44.1 / 48 kHz, 16384 at 192 kHz). */
    static int windowSize (double sampleRate) noexcept;

    /** One frame, not smoothed: the newest windowSize() samples of x (n at least that) -> dB per band, where a
        full-scale sine reads 0 dB. Bands above Nyquist read kFloorDb. False (dbOut untouched) when n is too short. */
    bool measure (const float* x, int n, double sampleRate, SpectrumBands& dbOut);
    /** When the ring got new samples since the last poll: measures its newest window and smooths it into the held
        spectrum. out = the held spectrum, true; or kFloorDb everywhere and false until a full window arrived after
        reset(), and again once no new samples came for kStalePolls polls (the device stopped). */
    bool poll (const TapRing& ring, double sampleRate, SpectrumBands& out);
    void reset() noexcept;

    /** A made-up voice (in: about 140 Hz, an "a" vowel) and the same voice raised and brightened (out), in the shape
        poll() gives, for snapshots that have no sound. */
    static void demoSpectra (SpectrumBands& in, SpectrumBands& out);

private:
    void prepare (double sampleRate);

    double preparedRate = 0.0;
    int n = 0;                                    // window length; the FFT is 2n long
    std::unique_ptr<juce::dsp::FFT> fft;
    std::vector<float> window, work, frame;
    std::array<int, kSpectrumBands> binLo {}, binHi {}; // bins in the band (inclusive); lo > hi: interpolate at binAt
    std::array<float, kSpectrumBands> binAt {};          // the band centre in bins; < 0 above Nyquist
    float magToAmp = 1.0f;                        // FFT magnitude -> sine amplitude
    SpectrumBands held {};
    bool hasHeld = false;
    long long lastWritten = -1;
    int stale = 0;
};
} // namespace koe

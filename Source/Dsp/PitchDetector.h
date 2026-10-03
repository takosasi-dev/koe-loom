#pragma once

// Fundamental-frequency detector for Phase 5 (spec §5.7). Autopitch, the vocoder (follow) and the
// scale-locked layers each own one (they are never shared).
//
// Two methods, both our own code (no external sources):
//   MPM (McLeod & Wyvill 2005, the default): normalised square difference function (type-II ACF)
//        -> key maxima between zero crossings -> first key maximum >= 0.93 * highest -> parabola.
//   YIN (de Cheveigne & Kawahara 2002): difference function -> cumulative mean normalised difference
//        -> absolute threshold 0.15 -> parabola. The integration window sits on the newest samples.
// Correlations go through juce::dsp::FFT. The input is low-passed at 3 kHz and decimated to about
// 12 kHz (60-1000 Hz needs no more), which makes one detection cost a few microseconds. DC is removed
// per window (mean) instead of with a high-pass: a 40 Hz high-pass rings at every onset and kept a
// 100 Hz onset 20 cents flat for 28 ms. Rumble below 60 Hz is therefore not removed ([暫定]).
//
// Window: with nothing locked the detector looks at 38 ms (enough for 60 Hz: 256 + one 60 Hz period).
// Once voiced it shrinks to 2.3 detected periods (10.7 ms .. 38 ms), so a 100-400 Hz voice is tracked
// within 25 ms (Phase 0 measured that a fixed 38 ms window needs 30-38 ms to follow a note change).
// A drop of more than ~5.5 semitones within one hop falls outside the short window: that hop reads as
// unvoiced and the next one uses the full window again. A quiet start of the window (below -30 dB re its
// peak, i.e. an onset) is left out, so the first reading is not biased by the empty part.
// Unvoiced: silence (newest 5 ms of the raw input below -60 dBFS), noise and unvoiced consonants (no
// clear period).
//
// prepare() allocates; reset() and process() do not (audio thread).

#include "Dsp/Building.h"

#include <memory>
#include <vector>

namespace juce::dsp
{
class FFT;
}

namespace koe::dsp
{
class PitchDetector
{
public:
    enum class Method { mpm, yin };

    struct Settings
    {
        Method method = Method::mpm;
        double fminHz = 60.0, fmaxHz = 1000.0; // spec §5.7 [暫定]
        int hopSamples = 128;                  // one detection per hop (input samples)
    };

    PitchDetector();
    ~PitchDetector();

    void prepare (double sampleRate, const Settings& settings = {});
    void reset();
    /** Feed input of any length; runs one detection every hop. */
    void process (const float* x, int numSamples);

    /** Latest estimate in Hz, 0 when unvoiced. */
    float getFrequencyHz() const noexcept { return f0; }
    bool isVoiced() const noexcept { return f0 > 0.0f; }
    /** Confidence of the latest detection, 0..1 (MPM: NSDF peak, YIN: 1 - CMNDF). */
    float getClarity() const noexcept { return clarity; }
    /** Input samples between detections. */
    int getHopSamples() const noexcept { return hop * decim; }
    /** Input samples the next detection looks at. */
    int getWindowSamples() const noexcept { return curN * decim; }

private:
    void detect();
    float detectMpm (int n, int tauMax);
    float detectYin (int n, int tauMax);
    float vertex (int tau) const noexcept;
    juce::dsp::FFT& fftFor (int n) const noexcept;

    Settings set;
    double rate = 12000.0; // analysis rate
    int decim = 4, decimPhase = 0;
    Biquad aa1, aa2;
    std::vector<float> ring, raw; // filtered / raw input at the analysis rate
    int ringMask = 0, ringW = 0;
    int hop = 32, hopCount = 0;
    int minLag = 12, maxLag = 201, longN = 457, shortN = 128, curN = 457, gateN = 60;
    int minOrder = 8;
    std::vector<std::unique_ptr<juce::dsp::FFT>> ffts;
    std::vector<float> win, bufA, bufB, curve;
    std::vector<double> prefix;
    float f0 = 0.0f, clarity = 0.0f;
};

inline float hzToMidi (float hz) noexcept { return 69.0f + 12.0f * std::log2 (hz / 440.0f); }
inline float midiToHz (float note) noexcept { return 440.0f * std::exp2 ((note - 69.0f) / 12.0f); }

/** Scale choices, in the order of the autopitch `scale` parameter. minor = natural minor. */
enum class Scale { chromatic = 0, major = 1, minor = 2 };

/** Semitones from `midiNote` to the note `degree` scale steps above the scale note nearest to it
    (degree 0 = just snap; F-02-11: +2 = a third up). key 0 = C .. 11 = B. */
float scaleShiftSemitones (float midiNote, int key, Scale scale, int degree = 0) noexcept;
} // namespace koe::dsp

#pragma once

// Shared helpers for juce::UnitTest suites (run with KoeLoom.exe --run-tests).

#include <juce_core/juce_core.h>

#include <cstdint>
#include <initializer_list>
#include <vector>

namespace koe::test
{
inline constexpr double kSr = 48000.0;

std::vector<float> sine (double hz, double seconds, float amplitude = 0.5f, double sr = kSr);
std::vector<float> silence (double seconds, double sr = kSr);
std::vector<float> whiteNoise (double seconds, float amplitude, uint32_t seed = 1, double sr = kSr);
/** Deterministic speech-like signal: gliding glottal pulse train through moving vowel formants,
    syllable envelope, short consonant noise every 0.6 s (40 ms), never silent for > 0.5 s, peak about -6 dBFS.
    consonants = false drops the noise bursts (for click tests over long operations). */
std::vector<float> synthVoice (double seconds, uint32_t seed = 7, double sr = kSr, bool consonants = true);
/** The user's reference speech (koeloom_presets.md §5.1) if %KOELOOM_REFERENCE_WAV% points to it,
    otherwise synthVoice(10 s). *isReal tells which. Mono, 48 kHz. */
std::vector<float> referenceSpeechOrSynth (bool* isReal = nullptr);
std::vector<float> concat (std::initializer_list<std::vector<float>> parts);

float rmsDb (const float* x, int n);
inline float rmsDb (const std::vector<float>& v) { return rmsDb (v.data(), int (v.size())); }
float peakDb (const float* x, int n);
inline float peakDb (const std::vector<float>& v) { return peakDb (v.data(), int (v.size())); }
bool allFinite (const float* x, int n);
inline bool allFinite (const std::vector<float>& v) { return allFinite (v.data(), int (v.size())); }

/** Fundamental frequency estimate (CMNDF/YIN with parabolic interpolation), Hz; 0 if unvoiced. */
double estimateF0 (const float* x, int n, double sr = kSr, double fmin = 50.0, double fmax = 1100.0);
double centsBetween (double f, double reference);

/** Spec §13.2 "クリック判定": max |x[i]-x[i-1]| within [opStart-20 ms, opEnd+20 ms] divided by the
    larger of the same measure over the 100 ms static windows just before and just after. <= 2 passes. */
double clickRatio (const std::vector<float>& out, int opStartSample, int opEndSample, double sr = kSr);

/** Lag (samples, 0..maxLag) at which sig best matches ref (normalised cross-correlation). */
int findLag (const std::vector<float>& ref, const std::vector<float>& sig, int maxLag);

/** Debug aid: write a mono 24-bit WAV. */
bool writeWav (const juce::File& file, const std::vector<float>& samples, double sr = kSr);

/** Counts global operator new calls made on the current thread while alive (AC-39). */
class AllocationCounter
{
public:
    AllocationCounter();
    ~AllocationCounter();
    long long count() const;

private:
    long long startCount;
};
} // namespace koe::test

#pragma once

// Phase 0 shared helpers: timing, statistics, argument parsing, WAV I/O, the synthetic
// speech stand-in, and system-load sampling. Experiment code, not product code.

#include <juce_core/juce_core.h>

#include <cstdint>
#include <string>
#include <vector>

namespace p0
{
inline constexpr double kSr = 48000.0;

//==============================================================================
/** QueryPerformanceCounter in microseconds. */
double nowUs();

struct Stats
{
    double p50 = 0, p99 = 0, max = 0, mean = 0;
    size_t n = 0;
};
/** Percentiles by nearest rank on a sorted copy. */
Stats stats (std::vector<double> v);

//==============================================================================
/** "--name value" / "--flag" parser. */
class Args
{
public:
    Args (int argc, char** argv);
    bool has (const char* name) const;
    juce::String get (const char* name, const juce::String& def = {}) const;
    double getDouble (const char* name, double def) const;
    int getInt (const char* name, int def) const;
    juce::String commandLine;

private:
    juce::StringArray tokens;
};

//==============================================================================
juce::File phase0Dir();   // the phase0/ source folder (compile-time path, ASCII)
juce::File resultsDir();  // phase0/results
juce::File rawDir();      // phase0/results/raw (git-ignored), created on demand

/** Mono mix-down; returns empty on failure. */
std::vector<float> readWavMono (const juce::File& f, double* sampleRate = nullptr);
bool writeWav (const juce::File& f, const std::vector<float>& x, double sr = kSr);

/** Write text as UTF-8 without BOM, LF line ends. */
bool writeText (const juce::File& f, const juce::String& s);

//==============================================================================
/** Speech-like test signal (same recipe as Source/Tests/TestUtil.cpp synthVoice): glottal pulse
    train with gliding f0 (105..175 Hz + 5 Hz vibrato) through moving vowel formants, ~3.7 Hz
    syllable envelope (never silent), short consonant noise bursts, peak -6 dBFS. Deterministic. */
std::vector<float> synthVoice (double seconds, uint32_t seed = 7, double sr = kSr);
/** The f0 trajectory used by synthVoice (Hz at time t seconds). */
double synthVoiceF0 (double t);

struct Speech
{
    std::vector<float> x;
    bool real = false;          // true = reference-speech.wav (the user's voice)
    juce::String source;        // description for result files
};
/** --input <wav>, else phase0/testdata/reference-speech.wav, else synthVoice(10 s). 48 kHz only. */
Speech loadSpeech (const Args& args);

std::vector<float> sine (double hz, double seconds, float amplitude, double sr = kSr);
/** Uniform white noise with the given RMS (dBFS). */
std::vector<float> whiteNoiseRms (double seconds, double rmsDbfs, uint32_t seed, double sr = kSr);

//==============================================================================
double rmsDb (const float* x, size_t n);
inline double rmsDb (const std::vector<float>& v) { return rmsDb (v.data(), v.size()); }

/** Delay of `out` relative to `in` from the cross-correlation of 1 ms RMS envelopes (spec T2 method).
    Returns milliseconds on the 1 ms grid; *score gets the normalised correlation. */
int envelopeLagMs (const std::vector<float>& in, const std::vector<float>& out, int maxLagMs, double* score = nullptr);
/** Sample-accurate delay by waveform cross-correlation, searched in [centre-radius, centre+radius]. */
int waveformLag (const std::vector<float>& in, const std::vector<float>& out, int centre, int radius, double* score = nullptr);

//==============================================================================
/** Whole-machine busy % between start() and busyPercent() (GetSystemTimes). */
class SystemLoad
{
public:
    void start();
    double busyPercent() const;

private:
    uint64_t idle0 = 0, kernel0 = 0, user0 = 0;
};

/** This process's CPU time (user+kernel) in seconds (GetProcessTimes; updated on clock ticks). */
double processCpuSeconds();
/** Same from QueryProcessCycleTime (cycle-accurate) divided by the measured TSC rate. */
double processCycleSeconds();

/** Puts the calling thread in the MMCSS "Pro Audio" class for its lifetime, like JUCE's WASAPI
    audio thread (setMMThreadPriority). Used while timing DSP so the numbers reflect an audio thread. */
class ScopedProAudio
{
public:
    ScopedProAudio();
    ~ScopedProAudio();
    bool ok() const { return handle != nullptr; }
    unsigned long error = 0;

private:
    void* handle = nullptr;
};

/** Formats a double without rounding away information (up to 6 significant decimals). */
juce::String num (double v, int decimals = 3);

/** Current local date-time "YYYY-MM-DD HH:MM:SS". */
juce::String timestamp();
} // namespace p0

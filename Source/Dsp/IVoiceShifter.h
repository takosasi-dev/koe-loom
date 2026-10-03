#pragma once

#include <memory>

namespace koe
{
/**
    Pitch/formant converter (spec §5.5, F-02-6). The main voice and each layer own one instance.
    Mono, 48 kHz, float32. prepare() may allocate; everything else runs on the audio thread and must not.
    process() must not be called with in == out. Output is the input delayed by getLatencySamples()
    (and pitch/formant shifted). Pitch and formant are independent: changing the formant must not move
    the fundamental (AC-04). Parameter changes take effect smoothly (F-02-4).
*/
class IVoiceShifter
{
public:
    virtual ~IVoiceShifter() = default;
    virtual void prepare (double sampleRate, int maxBlockSize) = 0;
    virtual void reset() = 0;
    virtual void setPitchSemitones (float semitones) = 0;
    virtual void setFormantSemitones (float semitones) = 0;
    virtual void process (const float* in, float* out, int numSamples) = 0;
    virtual int getLatencySamples() const = 0;
};

/** Signalsmith Stretch block/interval in samples at 48 kHz. Chosen in Phase 0 (results/02-shifters.md). */
struct ShifterConfig
{
    int blockSamples = 1440;    // 30 ms  [暫定 until Phase 0 decides]
    int intervalSamples = 240;  // 5 ms
    bool splitComputation = false;
};

std::unique_ptr<IVoiceShifter> createSignalsmithShifter (const ShifterConfig& = {});
/** Candidate B: own phase vocoder. FFT size 2^fftOrder, Hann window of windowLength samples (zero padded),
    hop windowLength / 4, latency windowLength - 1. */
std::unique_ptr<IVoiceShifter> createPhaseVocoderShifter (int fftOrder = 11, int windowLength = 1536);
/** Test implementation: pure delay of a fixed latency (AC-35). */
std::unique_ptr<IVoiceShifter> createIdentityShifter (int latencySamples);
} // namespace koe

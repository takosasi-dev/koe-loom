#pragma once

// IVoiceShifter as defined in spec §5.5 (same shape as Source/Dsp/IVoiceShifter.h) and the
// Signalsmith Stretch implementation (candidate A) used by p0_shifters and p0_parallel.
//
// API checked against third_party/signalsmith-stretch/signalsmith-stretch.h (version {1,3,2} in the
// header; third_party/CMakeLists.txt calls it 1.4.0):
//   configure(nChannels, blockSamples, intervalSamples, splitComputation)
//   presetDefault(ch, sr)  = configure(ch, sr*0.12, sr*0.03, split=false)
//   presetCheaper(ch, sr)  = configure(ch, sr*0.10, sr*0.04, split=true)
//   setTransposeSemitones(st), setFormantSemitones(st, compensatePitch), setFormantBase(freq/sr; 0 = detect)
//   inputLatency() = block - analysisOffset (= block/2 for the symmetric Kaiser window)
//   outputLatency() = synthesisOffset (= block/2) + (split ? interval : 0)
//   process(inputs, n, outputs, n)   (in != out)

#include <signalsmith-stretch/signalsmith-stretch.h>

#include <memory>
#include <string>

namespace p0
{
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

struct ShifterConfig
{
    std::string name;          // e.g. "b1440-i240"
    int blockSamples = 1440;
    int intervalSamples = 240;
    bool splitComputation = false;
    float formantBaseHz = 0;   // 0 = let the library estimate f0 (library default)
    bool compensatePitch = true;  // false only for the level-scan diagnosis
};

class SignalsmithShifter final : public IVoiceShifter
{
public:
    explicit SignalsmithShifter (ShifterConfig c, long seed = 1) : cfg (std::move (c)), stretch (seed) {}

    void prepare (double sampleRate, int) override
    {
        sr = sampleRate;
        stretch.configure (1, cfg.blockSamples, cfg.intervalSamples, cfg.splitComputation);
        stretch.setFormantBase (cfg.formantBaseHz > 0 ? float (cfg.formantBaseHz / sr) : 0.0f);
        apply();
    }
    void reset() override { stretch.reset(); }
    void setPitchSemitones (float st) override { pitch = st; apply(); }
    void setFormantSemitones (float st) override { formant = st; apply(); }
    void process (const float* in, float* out, int n) override
    {
        const float* ins[1] = { in };
        float* outs[1] = { out };
        stretch.process (ins, n, outs, n);
    }
    int getLatencySamples() const override { return stretch.inputLatency() + stretch.outputLatency(); }
    int inputLatency() const { return stretch.inputLatency(); }
    int outputLatency() const { return stretch.outputLatency(); }

private:
    void apply()
    {
        stretch.setTransposeSemitones (pitch);
        // compensatePitch = true: formants stay put under a pitch shift unless moved by `formant`,
        // so pitch and formant are independent (F-02-2, AC-04).
        stretch.setFormantSemitones (formant, cfg.compensatePitch);
    }

    ShifterConfig cfg;
    signalsmith::stretch::SignalsmithStretch<float> stretch;
    double sr = 48000.0;
    float pitch = 0, formant = 0;
};
} // namespace p0

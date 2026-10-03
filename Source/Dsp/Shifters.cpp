#include "Dsp/IVoiceShifter.h"

#include "Dsp/Building.h"

#include <signalsmith-stretch/signalsmith-stretch.h>

#include <algorithm>
#include <cmath>
#include <vector>

namespace koe
{
namespace
{
/** Candidate A (spec §5.5): Signalsmith Stretch used as a real-time pitch shifter with
    pitch-compensated formant shifting (pitch and formant independent, F-02-2). */
class SignalsmithShifter final : public IVoiceShifter
{
public:
    explicit SignalsmithShifter (const ShifterConfig& c) : config (c), stretch (12345L) {}

    void prepare (double sr, int maxBlockSize) override
    {
        sampleRate = sr;
        const double scale = sr / 48000.0;
        stretch.configure (1, std::max (64, int (config.blockSamples * scale)), std::max (16, int (config.intervalSamples * scale)),
                           config.splitComputation);
        // 0 = let the library estimate the fundamental for formant analysis
        stretch.setFormantBase (0);
        smoothCoeff = dsp::onePoleCoeff (25.0f, sr / double (std::max (1, maxBlockSize))); // per-block smoothing
        reset();
    }

    void reset() override
    {
        stretch.reset();
        curPitch = targetPitch;
        curFormant = targetFormant;
        applyParams();
    }

    void setPitchSemitones (float st) override { targetPitch = st; }
    void setFormantSemitones (float st) override { targetFormant = st; }

    void process (const float* in, float* out, int n) override
    {
        // glide parameters per block (F-02-4); the phase vocoder itself is click-free on changes
        if (curPitch != targetPitch || curFormant != targetFormant)
        {
            curPitch = targetPitch + smoothCoeff * (curPitch - targetPitch);
            curFormant = targetFormant + smoothCoeff * (curFormant - targetFormant);
            if (std::abs (curPitch - targetPitch) < 0.005f) curPitch = targetPitch;
            if (std::abs (curFormant - targetFormant) < 0.005f) curFormant = targetFormant;
            applyParams();
        }
        const float* ins[1] = { in };
        float* outs[1] = { out };
        stretch.process (ins, n, outs, n);
    }

    int getLatencySamples() const override { return stretch.inputLatency() + stretch.outputLatency(); }

private:
    void applyParams()
    {
        stretch.setTransposeSemitones (curPitch);
        stretch.setFormantSemitones (curFormant, true);
    }

    ShifterConfig config;
    signalsmith::stretch::SignalsmithStretch<float> stretch;
    double sampleRate = 48000.0;
    float targetPitch = 0.0f, targetFormant = 0.0f, curPitch = 0.0f, curFormant = 0.0f;
    float smoothCoeff = 0.0f;
};

/** Test implementation (AC-35): a pure delay. */
class IdentityShifter final : public IVoiceShifter
{
public:
    explicit IdentityShifter (int latency) : latencySamples (std::max (0, latency)) {}
    void prepare (double, int) override { line.prepare (latencySamples + 1); }
    void reset() override { line.reset(); }
    void setPitchSemitones (float) override {}
    void setFormantSemitones (float) override {}
    void process (const float* in, float* out, int n) override
    {
        for (int i = 0; i < n; ++i)
        {
            line.push (in[i]);
            out[i] = line.readInt (latencySamples);
        }
    }
    int getLatencySamples() const override { return latencySamples; }

private:
    int latencySamples;
    dsp::DelayLine line;
};
} // namespace

std::unique_ptr<IVoiceShifter> createSignalsmithShifter (const ShifterConfig& c) { return std::make_unique<SignalsmithShifter> (c); }
std::unique_ptr<IVoiceShifter> createIdentityShifter (int latencySamples) { return std::make_unique<IdentityShifter> (latencySamples); }
} // namespace koe

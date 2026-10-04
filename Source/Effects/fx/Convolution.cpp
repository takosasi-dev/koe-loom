#include "Core/Constants.h"
#include "Dsp/Building.h"
#include "Effects/IEffect.h"

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_dsp/juce_dsp.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <vector>

// convolution 「残響ファイル」 (INTERFACES.md §9.3, owner wave7/ir). Input -> pre-delay (glides like the
// reverb's) -> low cut / high cut (biquads, cutoffs glide) -> juce::dsp::Convolution with the user's impulse
// response -> equal-power mix with the dry voice.
//
// The IR (WAV / FLAC / AIFF, any rate, any channel count) is read in setAssetPath() on the message thread:
// channels are averaged, anything past kIrMaxSeconds and trailing silence are cut, it is resampled to the
// processing rate and scaled to unit energy (times kWetGain), so a wet-only output is about as loud as the dry
// voice, like the reverb. The first load happens in the reset() that EffectChain::create() calls right after setParam()
// (still the message thread), so the IR is active from the first block.
// `lengthPct` keeps the first part of the IR (raised-cosine fade at the cut). Changing it on the audio thread
// only stores the wish: a small worker thread builds the shortened IR and hands it over through `ready`; the
// audio thread passes it to the wait-free Convolution::loadImpulseResponse(AudioBuffer&&), and JUCE builds the
// engine on its own thread and crossfades the engines over 50 ms (no click, no allocation on the audio thread).
// No file, a missing or an unreadable file: the input passes unchanged and getUiState() says so.

namespace koe
{
namespace
{
constexpr float kWetGain = 1.0f;  // unit energy: wet-only speech sits ~1 dB under the dry voice, like the reverb hall
constexpr int kCtrl = 32;          // filter cutoffs are re-designed every 32 samples
constexpr int kHeadSize = 1024;    // non-uniform partitioned convolution (zero latency), see ConvolutionTests CPU

/** getUiState() values. */
enum State { none = 0, loaded = 1, failed = 2 };

class ConvolutionEffect final : public IEffect
{
public:
    ConvolutionEffect() : worker (*this) {}
    ~ConvolutionEffect() override { worker.stopThread (2000); }

    void prepare (double sampleRate, int maxBlockSize) override
    {
        sr = sampleRate;
        maxBlock = std::max (1, maxBlockSize);
        spec = { sr, juce::uint32 (maxBlock), 1 };
        conv.prepare (spec);
        preLine.prepare (int (sr * 0.2) + 8);
        wet.assign (size_t (maxBlock), 0.0f);
        mix.prepare (sr, 40.0f);
        cutCoeff = std::pow (dsp::onePoleCoeff (30.0f, sr), float (kCtrl));
    }

    void setAssetPath (const std::string& utf8Path) override
    {
        worker.stopThread (2000);
        full.clear();
        ready.store (false);
        pendingFirstLoad = false;
        state.store (none);
        if (utf8Path.empty()) return;
        state.store (failed);

        juce::AudioFormatManager formats;
        formats.registerBasicFormats();
        std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (juce::File (juce::String::fromUTF8 (utf8Path.c_str()))));
        if (reader == nullptr || reader->sampleRate <= 0.0 || reader->lengthInSamples <= 0 || reader->numChannels == 0) return;
        const double irRate = reader->sampleRate;
        const int n = int (std::min<juce::int64> (reader->lengthInSamples, juce::int64 (kIrMaxSeconds * irRate)));
        const int ch = int (std::min<unsigned int> (reader->numChannels, 8));
        juce::AudioBuffer<float> buf (ch, n);
        if (! reader->read (buf.getArrayOfWritePointers(), ch, 0, n)) return;

        full.assign (size_t (n), 0.0f);
        for (int c = 0; c < ch; ++c)
            for (int i = 0; i < n; ++i) full[size_t (i)] += buf.getSample (c, i) / float (ch);

        float peak = 0.0f;
        for (float v : full) peak = std::max (peak, std::abs (v));
        if (! std::isfinite (peak) || peak <= 1.0e-6f) { full.clear(); return; }
        size_t end = full.size(); // cut the silent tail (-90 dB of the peak): less work for the engine
        while (end > 1 && std::abs (full[end - 1]) < peak * 3.0e-5f) --end;
        full.resize (end);
        if (std::abs (irRate - sr) > 0.5) resampleFull (irRate);

        double energy = 0.0;
        for (float v : full) energy += double (v) * v;
        if (! (energy > 1.0e-12)) { full.clear(); return; }
        const float g = float (kWetGain / std::sqrt (energy));
        for (auto& v : full) v *= g;
        pendingFirstLoad = true;
    }

    void reset() override
    {
        if (pendingFirstLoad) // the reset() right after setAssetPath(): still the message thread
        {
            pendingFirstLoad = false;
            const int len = juce::roundToInt (wantedPct.load());
            conv.loadImpulseResponse (makeIr (len), sr, juce::dsp::Convolution::Stereo::no, juce::dsp::Convolution::Trim::no,
                                      juce::dsp::Convolution::Normalise::no);
            conv.prepare (spec); // makes the IR active now (juce_Convolution.h, prepare())
            builtPct = len;
            state.store (loaded);
            worker.startThread (juce::Thread::Priority::low);
        }
        conv.reset();
        preLine.reset();
        lowCut.reset();
        highCut.reset();
        mix.snap (mix.target);
        updateMixGains (mix.value);
        lowNow = lowTarget;
        highNow = highTarget;
        designFilters();
        preDelay = preDelayTarget();
        ctrlLeft = 0;
    }

    void setParam (int index, float v) override
    {
        switch (index)
        {
            case 0: mix.setTarget (v); break;
            case 1: preDelayMs = v; break;
            case 2: lowTarget = v; break;
            case 3: highTarget = v; break;
            case 4: wantedPct.store (v); break;
            default: break;
        }
    }

    void process (float* x, int n) override
    {
        if (state.load (std::memory_order_relaxed) != loaded) return; // no IR: pass through
        juce::ScopedNoDenormals noDenormals;
        if (ready.load (std::memory_order_acquire))
        {
            conv.loadImpulseResponse (std::move (pending), sr, juce::dsp::Convolution::Stereo::no, juce::dsp::Convolution::Trim::no,
                                      juce::dsp::Convolution::Normalise::no);
            ready.store (false, std::memory_order_release);
        }
        for (int done = 0; done < n;)
        {
            const int m = std::min (maxBlock, n - done);
            processChunk (x + done, m);
            done += m;
        }
    }

    int getUiState() const override { return state.load(); }

private:
    struct Worker final : juce::Thread
    {
        explicit Worker (ConvolutionEffect& e) : juce::Thread ("KoeLoom IR length"), fx (e) {}
        void run() override
        {
            int last = -1;
            while (! threadShouldExit())
            {
                wait (30);
                const int want = juce::roundToInt (fx.wantedPct.load());
                if (want != last) { last = want; continue; } // wait until the knob rests for one poll
                if (want == fx.builtPct || fx.ready.load (std::memory_order_acquire)) continue;
                fx.pending = fx.makeIr (want);
                fx.builtPct = want;
                fx.ready.store (true, std::memory_order_release);
            }
        }
        ConvolutionEffect& fx;
    };

    /** The first pct % of the IR, faded out over the last quarter (at most 50 ms) when shortened. */
    juce::AudioBuffer<float> makeIr (int pct) const
    {
        const int total = int (full.size());
        const int len = std::clamp (int (std::ceil (total * std::clamp (pct, 1, 100) / 100.0)), 1, total);
        juce::AudioBuffer<float> b (1, len);
        std::copy (full.begin(), full.begin() + len, b.getWritePointer (0));
        if (len < total)
        {
            const int fade = std::max (1, std::min (len / 4, int (sr * 0.05)));
            auto* d = b.getWritePointer (0) + (len - fade);
            for (int i = 0; i < fade; ++i) d[i] *= 0.5f + 0.5f * std::cos (dsp::kPi * float (i + 1) / float (fade));
        }
        return b;
    }

    /** `full` from rate `from` to the processing rate: windowed sinc (JUCE), low-passed at 0.45 x sr first when
        going down. Done here rather than in JUCE's loader so the energy is measured at the rate that is used. */
    void resampleFull (double from)
    {
        if (from > sr)
            for (int k = 0; k < 4; ++k)
            {
                dsp::Biquad lp;
                lp.setLowpass (from, float (0.45 * sr));
                for (auto& v : full) v = lp.process (v);
            }
        const double ratio = from / sr; // input samples per output sample
        const int latency = juce::roundToInt (juce::WindowedSincInterpolator::getBaseLatency() / ratio);
        const int outLen = std::max (1, juce::roundToInt (double (full.size()) / ratio));
        std::vector<float> in (full);
        in.resize (full.size() + size_t (std::ceil ((outLen + latency + 4) * ratio)) + 8, 0.0f);
        std::vector<float> out (size_t (outLen + latency), 0.0f);
        juce::WindowedSincInterpolator interp;
        interp.process (ratio, in.data(), out.data(), int (out.size()));
        full.assign (out.begin() + latency, out.end());
    }

    void processChunk (float* x, int n) noexcept
    {
        const float preTarget = preDelayTarget();
        const float preRate = float (1.0 / (0.05 * sr));
        for (int i = 0; i < n; ++i)
        {
            if (ctrlLeft-- <= 0)
            {
                lowNow = lowTarget + cutCoeff * (lowNow - lowTarget);
                highNow = highTarget + cutCoeff * (highNow - highTarget);
                designFilters();
                ctrlLeft = kCtrl - 1;
            }
            preDelay += std::clamp ((preTarget - preDelay) * preRate * 8.0f, -0.5f, 0.5f);
            preLine.push (x[i]);
            wet[size_t (i)] = highCut.process (lowCut.process (preLine.readLinear (preDelay)));
        }
        float* chans[] = { wet.data() };
        juce::dsp::AudioBlock<float> block (chans, 1, size_t (n));
        conv.process (juce::dsp::ProcessContextReplacing<float> (block));
        for (int i = 0; i < n; ++i)
        {
            if (mix.isRamping()) updateMixGains (mix.next());
            x[i] = dryGain * x[i] + wetGain * wet[size_t (i)];
        }
    }

    void designFilters() noexcept
    {
        lowCut.setHighpass (sr, lowNow);
        highCut.setLowpass (sr, highNow);
    }
    float preDelayTarget() const noexcept { return float (preDelayMs * 0.001 * sr); }
    void updateMixGains (float m) noexcept
    {
        dryGain = std::sin (0.5f * dsp::kPi * (1.0f - m));
        wetGain = std::sin (0.5f * dsp::kPi * m);
    }

    double sr = 48000.0;
    int maxBlock = 480, ctrlLeft = 0;
    juce::dsp::ProcessSpec spec { 48000.0, 480, 1 };
    juce::dsp::Convolution conv { juce::dsp::Convolution::NonUniform { kHeadSize } };
    std::vector<float> full, wet;
    dsp::DelayLine preLine;
    dsp::Biquad lowCut, highCut;
    dsp::Ramp mix;
    float preDelayMs = 0.0f, preDelay = 0.0f, lowTarget = 80.0f, highTarget = 12000.0f, lowNow = 80.0f, highNow = 12000.0f;
    float cutCoeff = 0.0f, dryGain = 1.0f, wetGain = 0.0f;
    bool pendingFirstLoad = false;
    std::atomic<int> state { none };
    std::atomic<float> wantedPct { 100.0f };
    std::atomic<bool> ready { false };
    int builtPct = 100;                   // worker / message thread only
    juce::AudioBuffer<float> pending;     // worker writes while ! ready, the audio thread takes it while ready
    Worker worker;
};
} // namespace

KOE_REGISTER_EFFECT ("convolution", ConvolutionEffect)
} // namespace koe

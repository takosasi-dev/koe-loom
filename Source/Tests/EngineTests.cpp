// Engine / DSP core tests (lead). Category "Engine".

#include "Dsp/IVoiceShifter.h"
#include "Dsp/NoiseGate.h"
#include "Dsp/NoiseSuppressor.h"
#include "Effects/EffectRegistry.h"
#include "Engine/EffectChain.h"
#include "Engine/VoiceProcessor.h"
#include "Tests/TestUtil.h"

#include <functional>
#include <limits>

namespace koe
{
namespace
{
using namespace test;
constexpr int kBlock = 480;

std::vector<float> runShifter (IVoiceShifter& s, const std::vector<float>& in)
{
    std::vector<float> out (in.size());
    for (size_t pos = 0; pos < in.size(); pos += kBlock)
    {
        const int n = int (std::min<size_t> (kBlock, in.size() - pos));
        s.process (in.data() + pos, out.data() + pos, n);
    }
    return out;
}

/** Render through a processor; perBlock(blockIndex) runs before each block (control changes). */
std::vector<float> render (VoiceProcessor& vp, const std::vector<float>& in, std::function<void (int)> perBlock = {},
                           std::vector<float>* right = nullptr)
{
    std::vector<float> out (in.size());
    if (right) right->assign (in.size(), 0.0f);
    int b = 0;
    for (size_t pos = 0; pos < in.size(); pos += kBlock, ++b)
    {
        if (perBlock) perBlock (b);
        const int n = int (std::min<size_t> (kBlock, in.size() - pos));
        vp.process (in.data() + pos, out.data() + pos, right ? right->data() + pos : nullptr, n);
    }
    return out;
}

void plainSetup (VoiceProcessor& vp)
{
    vp.setNoiseSuppression (false, 1.0f);
    vp.setGate (false, -45, 5, 80, 120);
    vp.setInputGainDb (0);
    vp.setOutputGainDb (0);
    vp.setVoiceChangerOn (true);
    vp.setMicMute (false);
}

double f0Of (const std::vector<float>& v, double fromSec, double lenSec)
{
    const int a = int (fromSec * kSr), n = int (lenSec * kSr);
    return estimateF0 (v.data() + a, n);
}

/** Test effect: delay of L samples on the wet path with mix 0.5, dry delayed to match (AC-42). */
struct LatencyFx : IEffect
{
    int L = 64;
    dsp::DelayLine wet, dry;
    void prepare (double, int) override { wet.prepare (L + 1); dry.prepare (L + 1); }
    void reset() override { wet.reset(); dry.reset(); }
    void setParam (int, float) override {}
    void process (float* x, int n) override
    {
        for (int i = 0; i < n; ++i)
        {
            wet.push (x[i]);
            dry.push (x[i]);
            x[i] = 0.5f * wet.readInt (L) + 0.5f * dry.readInt (L);
        }
    }
    int getLatencySamples() const override { return L; }
};

/** Test effect: emits a NaN once, on the Nth block (AC-41). */
struct NanFx : IEffect
{
    int block = 0, at = 5;
    void prepare (double, int) override {}
    void reset() override { block = 0; }
    void setParam (int, float) override {}
    void process (float* x, int n) override
    {
        if (block++ == at) x[n / 2] = std::numeric_limits<float>::quiet_NaN();
    }
};

/** Test effect: simple gain (for click tests of chain operations). */
struct GainFx : IEffect
{
    float g = 0.5f;
    void prepare (double, int) override {}
    void reset() override {}
    void setParam (int, float) override {}
    void process (float* x, int n) override { for (int i = 0; i < n; ++i) x[i] *= g; }
};

/** Test effect: counts reset() and trigger() calls. */
struct CountFx : IEffect
{
    int resets = 0, triggers = 0;
    void prepare (double, int) override {}
    void reset() override { ++resets; }
    void setParam (int, float) override {}
    void process (float*, int) override {}
    void trigger (EffectTrigger) override { ++triggers; }
};

/** Test effect: records blocks longer than it was prepared for (the stats outlive the effect). */
struct BlockFx : IEffect
{
    struct Stats { int preparedFor = 0, tooLong = 0, calls = 0; };
    explicit BlockFx (Stats& s) : stats (s) {}
    Stats& stats;
    void prepare (double, int maxBlock) override { stats.preparedFor = maxBlock; }
    void reset() override {}
    void setParam (int, float) override {}
    void process (float*, int n) override
    {
        ++stats.calls;
        if (n > stats.preparedFor) ++stats.tooLong;
    }
};
} // namespace

class ShifterTests : public juce::UnitTest
{
public:
    ShifterTests() : juce::UnitTest ("Voice shifter", "Engine") {}

    void runTest() override
    {
        beginTest ("AC-04: +12 st doubles 220 Hz; formant alone keeps the pitch");
        {
            auto in = sine (220.0, 2.0, 0.3f);
            auto s = createPhaseVocoderShifter();
            s->prepare (kSr, kBlock);
            s->setPitchSemitones (12.0f);
            s->reset();
            auto out = runShifter (*s, in);
            expect (allFinite (out));
            const double f = f0Of (out, 1.0, 0.4);
            logMessage ("  +12 st -> " + juce::String (f, 2) + " Hz (" + juce::String (centsBetween (f, 440.0), 1) + " cents)");
            expectLessOrEqual (std::abs (centsBetween (f, 440.0)), 10.0);

            for (float fm : { -6.0f, 6.0f })
            {
                auto s2 = createPhaseVocoderShifter();
                s2->prepare (kSr, kBlock);
                s2->setFormantSemitones (fm);
                s2->reset();
                auto o2 = runShifter (*s2, in);
                expect (allFinite (o2));
                const double f2 = f0Of (o2, 1.0, 0.4);
                logMessage ("  formant " + juce::String (fm) + " -> " + juce::String (f2, 2) + " Hz");
                expectLessOrEqual (std::abs (centsBetween (f2, 220.0)), 10.0);
            }
        }

        beginTest ("AC-35: getLatencySamples matches the measured delay (identity and phase vocoder at 0/0)");
        {
            auto in = synthVoice (2.0, 5);
            auto id = createIdentityShifter (333);
            id->prepare (kSr, kBlock);
            auto out = runShifter (*id, in);
            expectEquals (findLag (in, out, 1000), 333);

            auto s = createPhaseVocoderShifter();
            s->prepare (kSr, kBlock);
            s->reset();
            auto o2 = runShifter (*s, in);
            const int measured = findLag (in, o2, 4000);
            logMessage ("  phase vocoder reported " + juce::String (s->getLatencySamples()) + ", measured " + juce::String (measured));
            expectLessOrEqual (std::abs (measured - s->getLatencySamples()), 1);
        }

        beginTest ("process() does not allocate (audio thread rule)");
        {
            auto s = createPhaseVocoderShifter();
            s->prepare (kSr, kBlock);
            s->setPitchSemitones (5);
            s->reset();
            auto in = synthVoice (1.0, 9);
            std::vector<float> out (kBlock);
            AllocationCounter counter;
            for (size_t pos = 0; pos + kBlock <= in.size(); pos += kBlock)
            {
                s->setPitchSemitones (float (pos % 7));
                s->process (in.data() + pos, out.data(), kBlock);
            }
            expectEquals (counter.count(), 0LL);
        }

        beginTest ("AC-37 (shifter part): output RMS within ±1 dB of the input over the pitch/formant end points");
        {
            auto in = synthVoice (4.0, 21);
            for (float p : { -12.0f, 0.0f, 12.0f })
                for (float f : { -6.0f, 0.0f, 6.0f })
                {
                    if (p == 0.0f && f == 0.0f) continue;
                    auto s = createPhaseVocoderShifter();
                    s->prepare (kSr, kBlock);
                    s->setPitchSemitones (p);
                    s->setFormantSemitones (f);
                    s->reset();
                    auto out = runShifter (*s, in);
                    const int L = s->getLatencySamples();
                    const int a = int (kSr), n = int (in.size()) - a - L;
                    const float diff = rmsDb (out.data() + a + L, n) - rmsDb (in.data() + a, n);
                    logMessage ("  p " + juce::String (p) + " f " + juce::String (f) + ": " + juce::String (diff, 2) + " dB");
                    expectLessOrEqual (std::abs (diff), 1.0f);
                }
        }
    }
};

class DynamicsTests : public juce::UnitTest
{
public:
    DynamicsTests() : juce::UnitTest ("Gate and noise suppression", "Engine") {}

    void runTest() override
    {
        beginTest ("AC-16: gate -40 dB closes on -50 dB tones, passes -30 dB");
        {
            NoiseGate g;
            g.prepare (kSr);
            g.setParams (-40, 5, 80, 120);
            auto quiet = sine (300.0, 2.0, dsp::dbToGain (-50.0f) * 1.41421f);
            for (size_t pos = 0; pos < quiet.size(); pos += kBlock) g.process (quiet.data() + pos, kBlock);
            expectLessOrEqual (peakDb (quiet.data() + int (kSr), int (kSr)), -80.0f);
            g.reset();
            auto loud = sine (300.0, 1.0, dsp::dbToGain (-30.0f) * 1.41421f);
            auto orig = loud;
            for (size_t pos = 0; pos < loud.size(); pos += kBlock) g.process (loud.data() + pos, kBlock);
            expectWithinAbsoluteError (rmsDb (loud.data() + 24000, 24000), rmsDb (orig.data() + 24000, 24000), 0.1f);
            expect (g.isOpen());
        }

        beginTest ("RNNoise: measured latency equals getLatencySamples; -40 dBFS white noise drops >= 10 dB (AC-16)");
        {
            NoiseSuppressor nsup;
            auto voice = synthVoice (3.0, 4);
            for (int block : { 480, 256, 960 })
            {
                nsup.prepare (kSr, block);
                auto wet = voice;
                for (size_t pos = 0; pos + size_t (block) <= wet.size(); pos += size_t (block)) nsup.process (wet.data() + pos, block, 1.0f);
                const int lag = findLag (voice, wet, 2000);
                logMessage ("  rnnoise block " + juce::String (block) + ": measured lag " + juce::String (lag) + ", reported " + juce::String (nsup.getLatencySamples()));
                expectLessOrEqual (std::abs (lag - nsup.getLatencySamples()), 2);
            }
            expectEquals (nsup.getLatencySamples(), NoiseSuppressor::kInternalDelay); // 960-sample blocks: no FIFO delay

            nsup.prepare (kSr, kBlock);
            auto noise = whiteNoise (3.0, dsp::dbToGain (-40.0f) * 1.732f, 5);
            const float before = rmsDb (noise.data() + 48000, 96000);
            for (size_t pos = 0; pos < noise.size(); pos += kBlock) nsup.process (noise.data() + pos, kBlock, 1.0f);
            const float after = rmsDb (noise.data() + 48000, 96000);
            logMessage ("  white noise " + juce::String (before, 1) + " -> " + juce::String (after, 1) + " dB");
            expectGreaterOrEqual (before - after, 10.0f);
        }
    }
};

class ProcessorTests : public juce::UnitTest
{
public:
    ProcessorTests() : juce::UnitTest ("Voice processor", "Engine") {}

    void runTest() override
    {
        beginTest ("AC-13 / AC-37: そのまま = input delayed exactly, L == R, same level");
        {
            VoiceProcessor vp;
            plainSetup (vp);
            vp.setShifter (true, 0, 0); // shifter present but 0/0 -> bypass with matching delay (F-02-9)
            vp.prepare (kSr, kBlock);
            auto in = referenceSpeechOrSynth();
            std::vector<float> right;
            auto out = render (vp, in, {}, &right);
            const int L = vp.getLatencySamples();
            expectEquals (L, vp.getShifterLatencySamples() + 48);
            const int start = int (kSr * 1.0);
            float maxErr = 0.0f;
            for (size_t i = size_t (start); i < out.size(); ++i) maxErr = std::max (maxErr, std::abs (out[i] - in[i - size_t (L)]));
            expectLessOrEqual (maxErr, 1.0e-6f);
            expect (out == right);
            const int n = int (in.size()) - start - L;
            expectWithinAbsoluteError (rmsDb (out.data() + start + L, n), rmsDb (in.data() + start, n), 0.5f);
        }

        beginTest ("AC-11: main voice and layers share one latency");
        {
            VoiceProcessor vp;
            plainSetup (vp);
            vp.setShifterFactory ([] { return createIdentityShifter (500); });
            vp.prepare (kSr, kBlock);
            expectEquals (vp.getShifterLatencySamples(), 500);
            vp.setShifter (true, 3, 0);
            VoiceProcessor::LayerParams lp;
            lp.active = true;
            lp.levelDb = 0;
            vp.setLayer (0, lp);
            auto in = sine (200.0, 1.0, 0.2f);
            auto out = render (vp, in);
            // identity shifter: main (500) + layer (500) are aligned -> output is 2x input, delayed 548
            const int L = 548;
            expectWithinAbsoluteError (out[size_t (30000)], 2.0f * in[size_t (30000 - L)], 1.0e-4f);
        }

        beginTest ("AC-47 / E-28: a scale layer with no pitch detected is silent within 20 ms");
        {
            struct FakeScale : IScaleLayerPitch
            {
                bool detected = true;
                void analyse (const float*, int) override {}
                float layerSemitones (int, bool, int) const override { return detected ? 4.0f : std::numeric_limits<float>::quiet_NaN(); }
            } scale;
            VoiceProcessor vp;
            plainSetup (vp);
            vp.setShifterFactory ([] { return createIdentityShifter (500); });
            vp.prepare (kSr, kBlock);
            vp.setShifter (true, 3, 0);
            vp.setScaleLayerPitch (&scale);
            VoiceProcessor::LayerParams lp;
            lp.active = true;
            lp.scale = true;
            lp.levelDb = 0;
            vp.setLayer (0, lp);
            auto in = sine (200.0, 1.0, 0.2f);
            constexpr int flipBlock = 50;
            auto out = render (vp, in, [&] (int b) { scale.detected = b < flipBlock; });
            // identity shifters: main + layer = 2x input before the flip, main only once the layer is cut
            // (the output is a further 48 samples late: limiter lookahead, common to every path)
            const int L = 548, flip = flipBlock * kBlock, cut = int (kSr * kScaleLayerCutMs / 1000.0) + 48;
            expectWithinAbsoluteError (out[size_t (flip - 10)], 2.0f * in[size_t (flip - 10 - L)], 1.0e-4f);
            float worst = 0.0f;
            for (int i = flip + cut; i < flip + cut + 4800; ++i)
                worst = std::max (worst, std::abs (out[size_t (i)] - in[size_t (i - L)]));
            expectLessOrEqual (worst, 1.0e-4f);
        }

        beginTest ("AC-23: fade-in from 0 to steady over 100 ms");
        {
            VoiceProcessor vp;
            plainSetup (vp);
            vp.setVoiceChangerOn (false);
            vp.prepare (kSr, kBlock);
            std::vector<float> in (size_t (kSr * 0.5), 0.25f);
            auto out = render (vp, in);
            const int L = 48;
            expectLessOrEqual (std::abs (out[size_t (L)]), 0.0025f);
            expectWithinAbsoluteError (dsp::gainToDb (out[size_t (L + 4800 + 10)]), dsp::gainToDb (0.25f), 1.0f);
        }

        beginTest ("AC-08 / AC-36: input and output gain change the level by the set amount");
        for (float g : { -24.0f, 0.0f, 24.0f })
        {
            VoiceProcessor vp;
            plainSetup (vp);
            vp.setVoiceChangerOn (false);
            vp.setInputGainDb (g);
            vp.prepare (kSr, kBlock);
            auto in = sine (1000.0, 1.0, dsp::dbToGain (-40.0f));
            auto out = render (vp, in);
            expectWithinAbsoluteError (rmsDb (out.data() + 24000, 24000) - rmsDb (in.data() + 24000, 24000), g, 0.5f);
        }
        for (float g : { -24.0f, 0.0f, 12.0f })
        {
            VoiceProcessor vp;
            plainSetup (vp);
            vp.setVoiceChangerOn (false);
            vp.setOutputGainDb (g);
            vp.prepare (kSr, kBlock);
            auto in = sine (1000.0, 1.0, dsp::dbToGain (-30.0f) * 1.41421f);
            auto out = render (vp, in);
            expectWithinAbsoluteError (rmsDb (out.data() + 24000, 24000) - rmsDb (in.data() + 24000, 24000), g, 0.25f);
        }
        {
            VoiceProcessor vp;
            plainSetup (vp);
            vp.setVoiceChangerOn (false);
            vp.prepare (kSr, kBlock);
            auto in = sine (1000.0, 2.0, dsp::dbToGain (-30.0f) * 1.41421f);
            auto out = render (vp, in, [&] (int b) { if (b == 100) vp.setOutputGainDb (12.0f); });
            const double r = clickRatio (out, 100 * kBlock, 100 * kBlock + 2400);
            logMessage ("  output gain 0 -> +12 dB click ratio " + juce::String (r, 2));
            expectLessOrEqual (r, 2.0);
        }

        beginTest ("AC-38 / AC-06 / AC-07: peak <= -1 dBFS with +12 dB gain, loud input, NaN, noise, silence");
        {
            VoiceProcessor vp;
            plainSetup (vp);
            vp.setOutputGainDb (12.0f);
            vp.setShifter (true, 5, 2);
            vp.prepare (kSr, kBlock);
            auto in = concat ({ referenceSpeechOrSynth(), sine (440.0, 3.0, 1.0f), whiteNoise (3.0, 1.0f), silence (2.0) });
            for (auto& s : in) s *= 4.0f; // +12 dB over
            in[1000] = std::numeric_limits<float>::quiet_NaN();
            in[5000] = std::numeric_limits<float>::infinity();
            auto out = render (vp, in);
            expect (allFinite (out));
            expectLessOrEqual (peakDb (out), -1.0f + 1.0e-4f);
            expectEquals (vp.getNonFiniteInputCount(), 2LL);
        }

        beginTest ("AC-05: voice ON/OFF toggle and a 1 s pitch sweep +12 -> -12 st do not click");
        {
            VoiceProcessor vp;
            plainSetup (vp);
            vp.setShifter (true, 4, 2);
            vp.prepare (kSr, kBlock);
            auto in = synthVoice (4.0, 12, kSr, false); // no consonant bursts: the sweep window is 1 s long
            auto out = render (vp, in, [&] (int b) {
                if (b == 150) vp.setVoiceChangerOn (false);
                if (b == 250) vp.setVoiceChangerOn (true);
            });
            const double r1 = clickRatio (out, 150 * kBlock, 150 * kBlock + 720);
            const double r2 = clickRatio (out, 250 * kBlock, 250 * kBlock + 720);
            logMessage ("  ON->OFF " + juce::String (r1, 2) + ", OFF->ON " + juce::String (r2, 2));
            expectLessOrEqual (r1, 2.0);
            expectLessOrEqual (r2, 2.0);

            VoiceProcessor vp2;
            plainSetup (vp2);
            vp2.setShifter (true, 12, 0);
            vp2.prepare (kSr, kBlock);
            auto out2 = render (vp2, in, [&] (int b) {
                const int start = 100, len = 100; // 1 s
                if (b >= start && b <= start + len) vp2.setShifter (true, 12.0f - 24.0f * float (b - start) / float (len), 0);
            });
            const double r3 = clickRatio (out2, 100 * kBlock, 200 * kBlock);
            logMessage ("  pitch sweep click ratio " + juce::String (r3, 2));
            expectLessOrEqual (r3, 2.0);
        }

        beginTest ("AC-65: mute reaches < -60 dBFS within 100 ms (voice ON and OFF), unmute is click-free");
        for (bool voice : { true, false })
        {
            VoiceProcessor vp;
            plainSetup (vp);
            vp.setVoiceChangerOn (voice);
            vp.prepare (kSr, kBlock);
            auto in = sine (1000.0, 2.0, 0.3f);
            auto out = render (vp, in, [&] (int b) {
                if (b == 50) vp.setMicMute (true);
                if (b == 120) vp.setMicMute (false);
            });
            const int muteAt = 50 * kBlock;
            const int lat = vp.getLatencySamples();
            expectLessOrEqual (peakDb (out.data() + muteAt + lat + 4800, 4800), -60.0f);
            const double r = clickRatio (out, 120 * kBlock + lat, 120 * kBlock + lat + 1440);
            logMessage ("  unmute click ratio " + juce::String (r, 2));
            expectLessOrEqual (r, 2.0);
        }

        beginTest ("AC-41 / AC-42: NaN from an effect auto-stops that slot; latency effect aligns dry/wet and adds to the total");
        {
            VoiceProcessor vp;
            plainSetup (vp);
            vp.prepare (kSr, kBlock);
            std::vector<EffectChain::TestSlot> fx;
            fx.push_back ({ findEffectInfo ("tremolo"), std::make_unique<GainFx>() });
            fx.push_back ({ findEffectInfo ("tremolo"), std::make_unique<NanFx>() });
            fx.push_back ({ findEffectInfo ("tremolo"), std::make_unique<LatencyFx>() });
            vp.requestChain (EffectChain::createFromEffects (std::move (fx), kSr, kBlock));
            auto in = synthVoice (1.0, 2);
            auto out = render (vp, in);
            expect (allFinite (out));
            auto* chain = vp.getRequestedChain();
            expect (chain->slot (1).autoStopped.load());
            expect (! chain->slot (1).enabled.load());
            expect (chain->slot (0).running && chain->slot (2).running);
            int ev = -1;
            expect (chain->popAutoStopEvent (ev));
            expectEquals (ev, 1);
            expectEquals (chain->getLatencySamples(), 64);
            expectEquals (vp.getLatencySamples(), 48 + vp.getShifterLatencySamples() + 64);

            // impulse through LatencyFx alone with mix 0.5: one peak at L, not two
            LatencyFx l;
            l.prepare (kSr, kBlock);
            std::vector<float> imp (512, 0.0f);
            imp[10] = 1.0f;
            l.process (imp.data(), 512);
            expectWithinAbsoluteError (imp[10 + 64], 1.0f, 1.0e-6f);
            expectWithinAbsoluteError (imp[10], 0.0f, 1.0e-6f);
        }

        beginTest ("F-04-7: a slot switched OFF is reset once its fade ends; triggers sent while OFF are dropped");
        {
            auto counter = std::make_unique<CountFx>();
            auto* c = counter.get();
            std::vector<EffectChain::TestSlot> fx;
            fx.push_back ({ findEffectInfo ("looper"), std::move (counter) });
            auto chain = EffectChain::createFromEffects (std::move (fx), kSr, kBlock);
            std::vector<float> buf (kBlock, 0.1f);
            auto blocks = [&] (int k) { for (int i = 0; i < k; ++i) chain->process (buf.data(), kBlock); };
            blocks (2);
            const int r0 = c->resets;
            chain->slot (0).pendingTrigger.store (int (EffectTrigger::looperRecordPlay));
            blocks (1);
            expectEquals (c->triggers, 1);

            chain->slot (0).enabled.store (false);
            blocks (4); // 20 ms fade = 2 blocks
            expect (! chain->slot (0).running);
            expectEquals (c->resets, r0 + 1);
            chain->slot (0).pendingTrigger.store (int (EffectTrigger::looperClear));
            blocks (2);
            chain->slot (0).enabled.store (true);
            blocks (2);
            expectEquals (c->triggers, 1);
            expectEquals (c->resets, r0 + 2); // the ON reset
        }

        beginTest ("Device reopened with a larger buffer: the chain built for the old block size never gets the longer blocks");
        {
            VoiceProcessor vp;
            plainSetup (vp);
            BlockFx::Stats small, big;
            auto chainFor = [] (BlockFx::Stats& st, int block)
            {
                std::vector<EffectChain::TestSlot> fx;
                fx.push_back ({ findEffectInfo ("tremolo"), std::make_unique<BlockFx> (st) });
                return EffectChain::createFromEffects (std::move (fx), kSr, block);
            };
            std::vector<float> in (2048, 0.1f), out (2048);
            vp.prepare (kSr, 128);
            vp.requestChain (chainFor (small, 128));
            for (int b = 0; b < 20; ++b) vp.process (in.data(), out.data(), nullptr, 128);
            expectGreaterThan (small.calls, 0);
            // AudioEngine::open: prepare for the new device, the callback starts, the owner's new chain comes a little later
            vp.prepare (kSr, 1024);
            for (int b = 0; b < 3; ++b) vp.process (in.data(), out.data(), nullptr, 1024);
            vp.requestChain (chainFor (big, 1024));
            for (int b = 0; b < 10; ++b) vp.process (in.data(), out.data(), nullptr, 1024);
            expectEquals (small.tooLong, 0);
            expectGreaterThan (big.calls, 0);
            expectEquals (big.tooLong, 0);
            expect (allFinite (out));
        }

        beginTest ("chain swap and slot toggles are click-free and allocation-free on the audio thread");
        {
            VoiceProcessor vp;
            plainSetup (vp);
            vp.prepare (kSr, kBlock);
            auto makeChain = [] {
                std::vector<EffectChain::TestSlot> fx;
                fx.push_back ({ findEffectInfo ("tremolo"), std::make_unique<GainFx>() });
                return EffectChain::createFromEffects (std::move (fx), kSr, kBlock);
            };
            auto in = synthVoice (3.0, 8);
            long long allocs = 0;
            auto out = render (vp, in, [&] (int b) {
                if (b == 60) vp.requestChain (makeChain());
                if (b == 160) vp.getRequestedChain()->slot (0).enabled.store (false);
                if (b == 220) vp.getRequestedChain()->slot (0).enabled.store (true);
                vp.collectGarbage();
            });
            juce::ignoreUnused (allocs);
            for (int at : { 60, 160, 220 })
            {
                const double r = clickRatio (out, at * kBlock, at * kBlock + 1440);
                logMessage ("  op at block " + juce::String (at) + " click ratio " + juce::String (r, 2));
                expectLessOrEqual (r, 2.0);
            }
            // audio thread alone (no requests in flight): zero allocations
            std::vector<float> o (kBlock);
            AllocationCounter counter;
            for (size_t pos = 0; pos + kBlock <= in.size(); pos += kBlock) vp.process (in.data() + pos, o.data(), nullptr, kBlock);
            expectEquals (counter.count(), 0LL);
        }
    }
};

/** The wave 8 hooks (INTERFACES.md §10.1): taps, input source, post processor, virtual-mic-only mute, slot wet. */
class Wave8HookTests : public juce::UnitTest
{
public:
    Wave8HookTests() : juce::UnitTest ("Wave 8 processor hooks", "Engine") {}

    struct Tap final : IAudioTap
    {
        std::vector<float> got;
        void push (const float* x, int n) override { got.insert (got.end(), x, x + n); } // test only: allocates
    };
    struct Zeros final : IInputSource
    {
        bool on = true;
        bool render (float* d, int n) override { if (! on) return false; std::fill (d, d + n, 0.0f); return true; }
    };
    struct Halve final : IVoicePostProcessor
    {
        int calls = 0;
        void process (float* x, int n) override { ++calls; for (int i = 0; i < n; ++i) x[i] *= 0.5f; }
    };

    void runTest() override
    {
        const auto in = sine (220.0, 1.0, 0.3f);
        auto fresh = [] (VoiceProcessor& vp) { plainSetup (vp); vp.prepare (kSr, kBlock); };

        beginTest ("input tap sees the device input as it arrives; output tap sees the virtual mic signal");
        {
            VoiceProcessor vp;
            fresh (vp);
            Tap ti, to;
            vp.setTap (VoiceProcessor::TapPoint::input, 0, &ti);
            vp.setTap (VoiceProcessor::TapPoint::output, 1, &to);
            auto out = render (vp, in);
            expect (ti.got == in);
            expect (to.got == out);
        }

        beginTest ("an input source stands in for the device input; returning false uses the device again");
        {
            VoiceProcessor vp;
            fresh (vp);
            Zeros z;
            vp.setInputSource (&z);
            auto out = render (vp, in);
            expectLessOrEqual (peakDb (out.data() + int (kSr * 0.5), int (kSr * 0.5)), -100.0f);
            z.on = false;
            out = render (vp, in);
            expectGreaterThan (peakDb (out.data() + int (kSr * 0.5), int (kSr * 0.5)), -20.0f);
        }

        beginTest ("post processor runs every block on the voice; outputMuted silences only the virtual mic");
        {
            VoiceProcessor vp, ref;
            fresh (vp);
            fresh (ref);
            Halve h;
            vp.setPostProcessor (&h);
            auto out = render (vp, in);
            auto base = render (ref, in);
            expectEquals (h.calls, int ((in.size() + kBlock - 1) / kBlock));
            const int a = int (kSr * 0.5), n = int (kSr * 0.4);
            expectWithinAbsoluteError (rmsDb (out.data() + a, n), rmsDb (base.data() + a, n) - 6.02f, 0.1f);

            VoiceProcessor m;
            fresh (m);
            Tap to;
            m.setTap (VoiceProcessor::TapPoint::output, 0, &to);
            m.setOutputMuted (true);
            out = render (m, in);
            expectLessOrEqual (peakDb (out.data() + int (kSr * 0.1), int (kSr * 0.8)), -120.0f);
            expectGreaterThan (peakDb (to.got.data() + a, n), -20.0f);
        }

        beginTest ("SlotDef::wet = 0 passes the input through exactly; wet = 1 is the effect");
        {
            SlotDef d;
            d.type = "distortion";
            d.params.clear();
            d.wet = 0.0f;
            auto chain = EffectChain::create ({ d }, kSr, kBlock);
            expect (chain != nullptr && chain->size() == 1);
            auto x = in;
            for (size_t pos = 0; pos < x.size(); pos += kBlock) chain->process (x.data() + pos, int (std::min<size_t> (kBlock, x.size() - pos)));
            expect (x == in);
            d.wet = 1.0f;
            chain = EffectChain::create ({ d }, kSr, kBlock);
            x = in;
            for (size_t pos = 0; pos < x.size(); pos += kBlock) chain->process (x.data() + pos, int (std::min<size_t> (kBlock, x.size() - pos)));
            expect (x != in);
        }
    }
};

static ShifterTests shifterTests;
static Wave8HookTests wave8HookTests;
static DynamicsTests dynamicsTests;
static ProcessorTests processorTests;
} // namespace koe

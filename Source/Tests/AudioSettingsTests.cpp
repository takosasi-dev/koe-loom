// Detailed audio settings (S-03 「詳細な設定」, INTERFACES.md §7.3, owner wave4/audio). Category "Engine".

#include "App/AppController.h"
#include "Core/Paths.h"
#include "Dsp/IVoiceShifter.h"
#include "Dsp/PitchDetector.h"
#include "Effects/EffectRegistry.h"
#include "Engine/EffectChain.h"
#include "Engine/ScaleLayerPitch.h"
#include "Engine/Soundboard.h"
#include "Engine/VoiceProcessor.h"
#include "Tests/TestUtil.h"

#include <functional>

namespace koe
{
namespace
{
using namespace test;
constexpr int kBlk = 480;
constexpr int kLimiterDelay = 48;

std::vector<float> runShifterBlocks (IVoiceShifter& s, const std::vector<float>& in)
{
    std::vector<float> out (in.size());
    for (size_t pos = 0; pos < in.size(); pos += kBlk)
        s.process (in.data() + pos, out.data() + pos, int (std::min<size_t> (kBlk, in.size() - pos)));
    return out;
}

/** Renders through a processor; perBlock(b) runs before block b (message-thread changes); allocations inside
    process() are added to *allocs. */
std::vector<float> renderVp (VoiceProcessor& vp, const std::vector<float>& in, std::function<void (int)> perBlock = {},
                             long long* allocs = nullptr)
{
    std::vector<float> out (in.size());
    int b = 0;
    for (size_t pos = 0; pos < in.size(); pos += kBlk, ++b)
    {
        if (perBlock) perBlock (b);
        const int n = int (std::min<size_t> (kBlk, in.size() - pos));
        AllocationCounter ac;
        vp.process (in.data() + pos, out.data() + pos, nullptr, n);
        if (allocs != nullptr) *allocs += ac.count();
    }
    return out;
}

void setupPlain (VoiceProcessor& vp, bool voiceOn)
{
    vp.setNoiseSuppression (false, 1.0f);
    vp.setGate (false, -45, 5, 80, 120);
    vp.setInputGainDb (0);
    vp.setOutputGainDb (0);
    vp.setVoiceChangerOn (voiceOn);
    vp.setMicMute (false);
}

float sineAmpForRmsDb (float db) { return dsp::dbToGain (db) * 1.41421356f; }

struct GainEffect : IEffect
{
    void prepare (double, int) override {}
    void reset() override {}
    void setParam (int, float) override {}
    void process (float* x, int n) override { for (int i = 0; i < n; ++i) x[i] *= 0.5f; }
};

SoundSlotState waitSlot (Soundboard& sb, int slot)
{
    const auto until = juce::Time::getMillisecondCounterHiRes() + 10000.0;
    auto st = sb.getSlotState (slot);
    while (st.status == SoundSlotState::Status::loading && juce::Time::getMillisecondCounterHiRes() < until)
    {
        juce::Thread::sleep (2);
        st = sb.getSlotState (slot);
    }
    return st;
}

struct BoardRender
{
    Soundboard& sb;
    long long allocations = 0;
    std::vector<float> out, mon;
    void run (int n)
    {
        const size_t start = out.size();
        out.resize (start + size_t (n), 0.0f);
        mon.resize (start + size_t (n), 0.0f);
        for (int pos = 0; pos < n; pos += kBlk)
        {
            AllocationCounter ac;
            sb.render (out.data() + start + size_t (pos), mon.data() + start + size_t (pos), std::min (kBlk, n - pos));
            allocations += ac.count();
        }
    }
    void clear() { out.clear(); mon.clear(); }
};
} // namespace

class AudioSettingsConverterTests : public juce::UnitTest
{
public:
    AudioSettingsConverterTests() : juce::UnitTest ("Detailed settings: converter quality", "Engine") {}

    void runTest() override
    {
        const int expectedLatency[] = { 1023, 1535, 2047 };
        for (int q = 0; q < 3; ++q)
        {
            beginTest ("quality " + juce::String (q) + ": AC-04 (±10 cents), level ±1 dB, latency as reported");
            auto in = sine (220.0, 2.0, 0.3f);
            {
                auto s = createConverterShifter (q);
                s->prepare (kSr, kBlk);
                expectEquals (s->getLatencySamples(), expectedLatency[q]);
                s->setPitchSemitones (12.0f);
                s->reset();
                auto out = runShifterBlocks (*s, in);
                const double f = estimateF0 (out.data() + 48000, 19200);
                logMessage ("  q" + juce::String (q) + " +12 st -> " + juce::String (f, 2) + " Hz (" + juce::String (centsBetween (f, 440.0), 1) + " c)");
                expectLessOrEqual (std::abs (centsBetween (f, 440.0)), 10.0);
            }
            for (float fm : { -6.0f, 6.0f })
            {
                auto s = createConverterShifter (q);
                s->prepare (kSr, kBlk);
                s->setFormantSemitones (fm);
                s->reset();
                auto out = runShifterBlocks (*s, in);
                const double f = estimateF0 (out.data() + 48000, 19200);
                expectLessOrEqual (std::abs (centsBetween (f, 220.0)), 10.0);
            }
            for (double hz : { 110.0, 330.0 })
                for (float st : { 5.0f, -7.0f })
                {
                    auto s = createConverterShifter (q);
                    s->prepare (kSr, kBlk);
                    s->setPitchSemitones (st);
                    s->reset();
                    auto out = runShifterBlocks (*s, sine (hz, 1.6, 0.3f));
                    const double c = centsBetween (estimateF0 (out.data() + 38400, 28800), hz * std::pow (2.0, st / 12.0));
                    logMessage ("  q" + juce::String (q) + " " + juce::String (hz, 0) + " Hz " + juce::String (st) + " st: " + juce::String (c, 1) + " c");
                    expectLessOrEqual (std::abs (c), 10.0);
                }

            auto voice = synthVoice (4.0, 21);
            juce::String levels;
            for (float p : { -12.0f, 0.0f, 12.0f })
                for (float fm : { -6.0f, 0.0f, 6.0f })
                {
                    if (p == 0.0f && fm == 0.0f) continue;
                    auto s = createConverterShifter (q);
                    s->prepare (kSr, kBlk);
                    s->setPitchSemitones (p);
                    s->setFormantSemitones (fm);
                    s->reset();
                    auto out = runShifterBlocks (*s, voice);
                    const int L = s->getLatencySamples();
                    const int a = int (kSr), n = int (voice.size()) - a - L;
                    const float diff = rmsDb (out.data() + a + L, n) - rmsDb (voice.data() + a, n);
                    levels << juce::String (diff, 2) << " ";
                    expectLessOrEqual (std::abs (diff), 1.0f);
                }
            logMessage ("  q" + juce::String (q) + " levels (dB): " + levels);

            auto zero = createConverterShifter (q);
            zero->prepare (kSr, kBlk);
            zero->reset();
            const int measured = findLag (voice, runShifterBlocks (*zero, voice), 4000);
            expectLessOrEqual (std::abs (measured - zero->getLatencySamples()), 1);
        }

        beginTest ("quality 1 is today's phase vocoder, bit for bit");
        {
            auto a = createConverterShifter (1);
            auto b = createPhaseVocoderShifter();
            for (auto* s : { a.get(), b.get() })
            {
                s->prepare (kSr, kBlk);
                s->setPitchSemitones (5.0f);
                s->setFormantSemitones (-2.0f);
                s->reset();
            }
            auto in = synthVoice (1.0, 3);
            expect (runShifterBlocks (*a, in) == runShifterBlocks (*b, in));
        }

        beginTest ("switching the quality: reported latency follows at once, the output then aligns, no allocation");
        {
            VoiceProcessor vp;
            setupPlain (vp, true);
            vp.setShifter (true, 0, 0); // bypass with the converter's delay: shows which set is heard
            vp.prepare (kSr, kBlk);
            expectEquals (vp.getConverterQuality(), 1);
            expectEquals (vp.getLatencySamples(), 1535 + kLimiterDelay);
            auto in = synthVoice (2.0, 5);
            long long allocs = 0;
            auto out = renderVp (vp, in, [&] (int b)
            {
                if (b == 20)
                {
                    vp.setConverterQuality (0);
                    expectEquals (vp.getLatencySamples(), 1023 + kLimiterDelay);
                }
            }, &allocs);
            expectEquals (allocs, 0LL);
            const int L = 1023 + kLimiterDelay;
            float maxErr = 0.0f;
            for (size_t i = size_t (kSr * 1.2); i < out.size(); ++i) maxErr = std::max (maxErr, std::abs (out[i] - in[i - size_t (L)]));
            expectLessOrEqual (maxErr, 1.0e-6f);
            vp.collectGarbage();

            vp.setConverterQuality (2);
            expectEquals (vp.getLatencySamples(), 2047 + kLimiterDelay);
            vp.prepare (kSr, kBlk); // a device reopen keeps the quality
            expectEquals (vp.getConverterQuality(), 2);
            expectEquals (vp.getShifterLatencySamples(), 2047);
        }

        beginTest ("switching the quality while the voice is shifted is click-free (0 -> 2 -> 1)");
        {
            VoiceProcessor vp;
            setupPlain (vp, true);
            vp.setShifter (true, 5, 0);
            VoiceProcessor::LayerParams lp;
            lp.active = true;
            lp.pitchSt = -5.0f;
            vp.setLayer (0, lp);
            vp.setConverterQuality (0);
            vp.prepare (kSr, kBlk);
            auto in = synthVoice (6.0, 3, kSr, false);
            long long allocs = 0;
            auto out = renderVp (vp, in, [&] (int b)
            {
                if (b == 200) vp.setConverterQuality (2);
                if (b == 400) vp.setConverterQuality (1);
            }, &allocs);
            expectEquals (allocs, 0LL);
            expect (allFinite (out));
            for (int at : { 200, 400 })
            {
                const double r = clickRatio (out, at * kBlk, at * kBlk + 2047 + 1440 + 1440 + kBlk);
                logMessage ("  switch at block " + juce::String (at) + " click ratio " + juce::String (r, 2));
                expectLessOrEqual (r, 2.0);
            }
            const int L = 1535 + kLimiterDelay;
            const int a = int (kSr * 4.0), n = int (kSr);
            expectWithinAbsoluteError (rmsDb (out.data() + a + L, n), rmsDb (in.data() + a, n) + 1.0f, 2.5f); // main + layer at -6 dB
        }

        beginTest ("switching while the voice changer is OFF takes effect at once");
        {
            VoiceProcessor vp;
            setupPlain (vp, false);
            vp.prepare (kSr, kBlk);
            auto in = synthVoice (0.5, 2);
            vp.setConverterQuality (0);
            renderVp (vp, in);
            vp.setVoiceChangerOn (true);
            vp.setShifter (true, 0, 0);
            auto in2 = synthVoice (1.0, 4);
            auto out = renderVp (vp, in2);
            const int L = 1023 + kLimiterDelay;
            float maxErr = 0.0f;
            for (size_t i = size_t (kSr * 0.5); i < out.size(); ++i) maxErr = std::max (maxErr, std::abs (out[i] - in2[i - size_t (L)]));
            expectLessOrEqual (maxErr, 1.0e-6f);
        }

        beginTest ("AppController: converterQuality and the reported latency (status)");
        {
            auto d = paths::dataDir();
            if (d.getFullPathName().contains ("KoeLoomTests")) d.deleteRecursively();
            d.createDirectory();
            AppController c (false);
            c.startup();
            c.setVoiceChangerOn (true);
            const float before = c.getStatus().latencyMs;
            c.updateSettings ([] (Settings& s) { s.converterQuality = 0; });
            expectEquals (c.getProcessorForTests().getConverterQuality(), 0);
            expectWithinAbsoluteError (before - c.getStatus().latencyMs, float (512.0 * 1000.0 / kSr), 0.01f);
            c.updateSettings ([] (Settings& s) { s.converterQuality = 2; });
            expectWithinAbsoluteError (c.getStatus().latencyMs - before, float (512.0 * 1000.0 / kSr), 0.01f);
            c.updateSettings ([] (Settings& s) { s = Settings {}; });
            c.shutdown();
        }
    }
};

class AudioSettingsInputTests : public juce::UnitTest
{
public:
    AudioSettingsInputTests() : juce::UnitTest ("Detailed settings: input low cut, automatic level, limiter, crossfade", "Engine") {}

    void runTest() override
    {
        beginTest ("high-pass: 50 Hz drops >= 15 dB at 200 Hz, 1 kHz passes; OFF is untouched");
        {
            auto level = [&] (double hz, bool on, float cut)
            {
                VoiceProcessor vp;
                setupPlain (vp, false);
                vp.setHighPass (on, cut);
                vp.prepare (kSr, kBlk);
                auto in = sine (hz, 1.0, sineAmpForRmsDb (-20.0f));
                auto out = renderVp (vp, in);
                if (! on)
                {
                    float maxErr = 0.0f;
                    for (size_t i = 24000; i < out.size(); ++i) maxErr = std::max (maxErr, std::abs (out[i] - in[i - kLimiterDelay]));
                    expectEquals (maxErr, 0.0f);
                }
                return rmsDb (out.data() + 24000, 24000);
            };
            expectWithinAbsoluteError (level (50.0, false, 200.0f), -20.0f, 0.1f);
            expectLessOrEqual (level (50.0, true, 200.0f), -35.0f);
            expectWithinAbsoluteError (level (1000.0, true, 200.0f), -20.0f, 0.5f);
            expectWithinAbsoluteError (level (1000.0, true, 300.0f), -20.0f, 0.6f);
        }

        beginTest ("high-pass: turning it on and moving the cutoff are click-free");
        {
            VoiceProcessor vp;
            setupPlain (vp, false);
            vp.prepare (kSr, kBlk);
            auto in = synthVoice (4.0, 8, kSr, false);
            auto out = renderVp (vp, in, [&] (int b)
            {
                if (b == 100) vp.setHighPass (true, 80.0f);
                if (b == 250) vp.setHighPass (true, 300.0f);
            });
            for (int at : { 100, 250 })
            {
                const double r = clickRatio (out, at * kBlk, at * kBlk + 2400);
                logMessage ("  high-pass op at block " + juce::String (at) + " click ratio " + juce::String (r, 2));
                expectLessOrEqual (r, 2.0);
            }
        }

        beginTest ("automatic level: reaches the target, limited by the max gain, cuts loud input, holds below -50 dBFS");
        {
            auto run = [&] (float inDb, float target, float maxGain, double seconds)
            {
                VoiceProcessor vp;
                setupPlain (vp, false);
                vp.setAgc (true, target, maxGain);
                vp.prepare (kSr, kBlk);
                long long allocs = 0;
                auto out = renderVp (vp, sine (300.0, seconds, sineAmpForRmsDb (inDb)), {}, &allocs);
                expectEquals (allocs, 0LL);
                return rmsDb (out.data() + out.size() - 48000, 48000);
            };
            const float limited = run (-36.0f, -18.0f, 12.0f, 10.0);
            const float full = run (-36.0f, -18.0f, 24.0f, 10.0);
            const float loud = run (-8.0f, -18.0f, 12.0f, 4.0);
            const float quiet = run (-60.0f, -18.0f, 12.0f, 4.0);
            logMessage ("  -36 -> " + juce::String (limited, 2) + " (max 12) / " + juce::String (full, 2) + " (max 24); -8 -> " + juce::String (loud, 2)
                        + "; -60 -> " + juce::String (quiet, 2));
            expectWithinAbsoluteError (limited, -24.0f, 0.5f);
            expectWithinAbsoluteError (full, -18.0f, 0.5f);
            expectWithinAbsoluteError (loud, -18.0f, 0.5f);
            expectWithinAbsoluteError (quiet, -60.0f, 0.3f);
        }

        beginTest ("automatic level: ON and OFF are click-free; OFF returns to unity");
        {
            VoiceProcessor vp;
            setupPlain (vp, false);
            vp.prepare (kSr, kBlk);
            auto in = synthVoice (8.0, 6, kSr, false);
            for (auto& x : in) x *= 0.1f; // about -26 dBFS: the level control has work to do
            auto out = renderVp (vp, in, [&] (int b)
            {
                if (b == 100) vp.setAgc (true, -18.0f, 12.0f);
                if (b == 600) vp.setAgc (false, -18.0f, 12.0f);
            });
            for (int at : { 100, 600 })
            {
                const double r = clickRatio (out, at * kBlk, at * kBlk + 1440);
                logMessage ("  automatic level op at block " + juce::String (at) + " click ratio " + juce::String (r, 2));
                expectLessOrEqual (r, 2.0);
            }
            float maxErr = 0.0f;
            for (size_t i = size_t (650 * kBlk); i < out.size(); ++i) maxErr = std::max (maxErr, std::abs (out[i] - in[i - kLimiterDelay]));
            expectEquals (maxErr, 0.0f);
        }

        beginTest ("limiter: the ceiling holds and glides when changed; a longer release recovers slower");
        {
            VoiceProcessor vp;
            setupPlain (vp, false);
            vp.setLimiter (-6.0f, 60.0f);
            vp.prepare (kSr, kBlk);
            auto loud = sine (1000.0, 1.0, 1.0f);
            auto out = renderVp (vp, loud, [&] (int b) { if (b == 50) vp.setLimiter (-1.0f, 60.0f); });
            expectLessOrEqual (peakDb (out.data() + 4800, 50 * kBlk - 4800), -6.0f + 0.01f);
            expectLessOrEqual (peakDb (out.data() + 50 * kBlk, 24000 - 50 * kBlk), -1.0f + 0.01f);
            expectWithinAbsoluteError (peakDb (out.data() + 36000, 12000), -1.0f, 0.05f);
            expectGreaterThan (clickRatio (out, 50 * kBlk, 50 * kBlk + 960), 0.0); // finite, ran
            auto recovered = [&] (float releaseMs)
            {
                VoiceProcessor v2;
                setupPlain (v2, false);
                v2.prepare (kSr, kBlk);
                v2.setLimiter (-6.0f, releaseMs);
                auto sig = concat ({ sine (1000.0, 0.3, 1.0f), sine (1000.0, 0.5, sineAmpForRmsDb (-20.0f)) });
                auto o = renderVp (v2, sig);
                return rmsDb (o.data() + 14400 + 4800, 960);
            };
            const float fast = recovered (10.0f), slow = recovered (1000.0f);
            logMessage ("  100 ms after the burst: release 10 ms " + juce::String (fast, 2) + " dB, 1000 ms " + juce::String (slow, 2) + " dB");
            expectWithinAbsoluteError (fast, -20.0f, 0.3f);
            expectLessThan (slow, -24.0f);
        }

        beginTest ("presetCrossfadeMs sets the chain swap crossfade");
        {
            auto ratioAfter = [&] (float ms)
            {
                VoiceProcessor vp;
                setupPlain (vp, true);
                vp.setChainCrossfadeMs (ms);
                vp.prepare (kSr, kBlk);
                auto in = sine (200.0, 1.0, 0.3f);
                auto out = renderVp (vp, in, [&] (int b)
                {
                    if (b != 50) return;
                    std::vector<EffectChain::TestSlot> fx;
                    fx.push_back ({ findEffectInfo ("tremolo"), std::make_unique<GainEffect>() });
                    vp.requestChain (EffectChain::createFromEffects (std::move (fx), kSr, kBlk));
                });
                const float ref = rmsDb (out.data() + 40 * kBlk, kBlk);
                return dsp::dbToGain (rmsDb (out.data() + 50 * kBlk + 4800 - 240, kBlk) - ref);
            };
            const float fast = ratioAfter (10.0f), slow = ratioAfter (200.0f);
            logMessage ("  gain 100 ms after the swap: 10 ms fade " + juce::String (fast, 3) + ", 200 ms fade " + juce::String (slow, 3));
            expectWithinAbsoluteError (fast, 0.5f, 0.02f);
            expectWithinAbsoluteError (slow, 0.75f, 0.05f);
        }
    }
};

class AudioSettingsPitchRangeTests : public juce::UnitTest
{
public:
    AudioSettingsPitchRangeTests() : juce::UnitTest ("Detailed settings: pitch detection range", "Engine") {}

    void runTest() override
    {
        beginTest ("scale layers: a voice outside the range is not detected; inside it is");
        {
            auto detect = [&] (double hz)
            {
                ScaleLayerPitch p;
                p.prepare (kSr, kBlk);
                auto in = sine (hz, 0.5, 0.3f);
                AllocationCounter ac;
                for (size_t pos = 0; pos < in.size(); pos += kBlk) p.analyse (in.data() + pos, kBlk);
                expectEquals (ac.count(), 0LL);
                return double (p.getFrequencyHz());
            };
            auto near = [] (double f, double hz) { return f > 0.0 && std::abs (centsBetween (f, hz)) < 20.0; };
            expect (near (detect (150.0), 150.0));
            expect (near (detect (600.0), 600.0));
            dsp::setVoicePitchRange (200.0f, 1000.0f);
            expect (! near (detect (150.0), 150.0), "150 Hz with the range from 200 Hz");
            expect (near (detect (250.0), 250.0));
            dsp::setVoicePitchRange (60.0f, 400.0f);
            expect (! near (detect (600.0), 600.0), "600 Hz with the range up to 400 Hz");
            expect (near (detect (350.0), 350.0));
            dsp::setVoicePitchRange (kPitchMinHz.def, kPitchMaxHz.def);
            expect (near (detect (150.0), 150.0));
        }

        beginTest ("autopitch: outside the range it leaves the voice uncorrected");
        {
            auto run = [&]
            {
                auto fx = createEffect ("autopitch");
                fx->prepare (kSr, kBlk);
                fx->setParam (0, 0.0f);  // key C
                fx->setParam (1, 0.0f);  // chromatic
                fx->setParam (2, 0.0f);  // retune at once
                fx->setParam (3, 1.0f);  // strength
                fx->reset();
                auto x = sine (150.0, 1.5, 0.3f); // 37 cents above D3 (146.83 Hz)
                for (size_t pos = 0; pos < x.size(); pos += kBlk) fx->process (x.data() + pos, kBlk);
                return estimateF0 (x.data() + 48000, 19200);
            };
            const double corrected = run();
            dsp::setVoicePitchRange (200.0f, 1000.0f);
            const double left = run();
            dsp::setVoicePitchRange (kPitchMinHz.def, kPitchMaxHz.def);
            logMessage ("  150 Hz: default range -> " + juce::String (corrected, 2) + " Hz, range from 200 Hz -> " + juce::String (left, 2) + " Hz");
            expectLessOrEqual (std::abs (centsBetween (corrected, 146.83)), 10.0);
            expectLessOrEqual (std::abs (centsBetween (left, 150.0)), 10.0);
        }
    }
};

class AudioSettingsSoundboardTests : public juce::UnitTest
{
public:
    AudioSettingsSoundboardTests() : juce::UnitTest ("Detailed settings: soundboard", "Engine") {}

    void runTest() override
    {
        auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("KoeLoomAudioSettingsTests");
        dir.deleteRecursively();
        dir.createDirectory();
        Soundboard sb;
        BoardRender r { sb };
        const float levels[] = { 0.1f, 0.2f, 0.4f };
        for (int k = 0; k < 3; ++k)
        {
            const auto f = dir.getChildFile ("dc" + juce::String (k) + ".wav");
            expect (writeWav (f, std::vector<float> (48000, levels[k])));
            sb.assignFile (k, f);
        }
        for (int k = 0; k < 3; ++k)
        {
            expect (waitSlot (sb, k).status == SoundSlotState::Status::ready);
            auto d = sb.getSlotDef (k);
            d.retrigger = 2;
            sb.setSlotDef (k, d);
        }

        beginTest ("default fade: starts at full level at once (as before), stops in 5 ms");
        sb.trigger (1);
        r.run (kBlk);
        expectWithinAbsoluteError (r.out[0], levels[1], 1.0e-6f);
        sb.stopAll();
        r.run (kBlk);
        expectWithinAbsoluteError (r.out[size_t (kBlk + 120)], levels[1] * 120.0f / 240.0f, 1.0e-3f);
        expectEquals (r.out.back(), 0.0f);

        beginTest ("soundFadeMs 100: starts fade in and stops fade out over 100 ms; 0: stops at once");
        sb.setFadeMs (100.0f);
        r.clear();
        sb.trigger (1);
        r.run (9600);
        expectWithinAbsoluteError (r.out[2399], levels[1] * 0.5f, 1.0e-3f);
        expectWithinAbsoluteError (r.out[7000], levels[1], 1.0e-6f);
        sb.stopAll();
        r.run (9600);
        expectWithinAbsoluteError (r.out[9600 + 2400], levels[1] * 0.5f, 1.0e-3f);
        expectEquals (r.out[9600 + 4800], 0.0f);
        sb.setFadeMs (0.0f);
        r.clear();
        sb.trigger (2);
        r.run (kBlk);
        expectWithinAbsoluteError (r.out[0], levels[2], 1.0e-6f); // no start fade below 5 ms
        sb.stopAll();
        r.run (kBlk);
        expectEquals (r.out[size_t (kBlk + 1)], 0.0f);
        sb.setFadeMs (kSoundFadeMs.def);

        beginTest ("soundboardMaxVoices 2: the oldest gives way to the third");
        sb.setMaxVoices (2);
        r.clear();
        for (int k = 0; k < 3; ++k)
        {
            sb.trigger (k);
            r.run (kBlk);
        }
        r.run (960);
        expectWithinAbsoluteError (r.out.back(), levels[1] + levels[2], 1.0e-5f);
        expect (! sb.getSlotState (0).playing);
        sb.stopAll();
        r.run (960);
        sb.setMaxVoices (kSoundboardMaxVoices);

        beginTest ("duck attack / release times follow the settings");
        sb.setDuckingDb (-12.0f);
        const float floorGain = dsp::dbToGain (-12.0f);
        for (float attack : { 20.0f, 100.0f })
        {
            sb.setDuckTimes (attack, 1000.0f);
            r.run (48000 * 6); // fully released from the previous round
            r.clear();
            sb.trigger (0);
            r.run (4800);
            const float expected = floorGain + (1.0f - floorGain) * std::exp (-100.0f / attack);
            expectWithinAbsoluteError (sb.voiceDuckGain(), expected, 0.01f);
            r.run (48000 - 4800 + 14400); // the sound ends at 1 s; 300 ms of release
            const float duckAtEnd = floorGain + (1.0f - floorGain) * std::exp (-1000.0f / attack);
            const float released = 1.0f + (duckAtEnd - 1.0f) * std::exp (-0.3f);
            expectWithinAbsoluteError (sb.voiceDuckGain(), released, 0.02f);
        }
        sb.setDuckingDb (0.0f);
        sb.setDuckTimes (kDuckAttackMs.def, kDuckReleaseMs.def);

        beginTest ("monitorIncludeSoundboard OFF keeps sounds off the monitor; ON again ramps back without a jump");
        sb.setMonitorIncludesSounds (false);
        r.clear();
        r.run (kBlk);
        sb.trigger (1);
        r.run (4800);
        float monPeak = 0.0f;
        for (auto x : r.mon) monPeak = std::max (monPeak, std::abs (x));
        expectEquals (monPeak, 0.0f);
        expectGreaterThan (r.out.back(), 0.1f);
        sb.setMonitorIncludesSounds (true);
        r.run (kBlk * 2);
        const size_t at = size_t (kBlk + 4800);
        expectWithinAbsoluteError (r.mon[at + kBlk / 2 - 1], levels[1] * 0.5f, 1.0e-3f);
        expectWithinAbsoluteError (r.mon.back(), r.out.back(), 1.0e-6f);
        sb.stopAll();
        r.run (960);
        expectEquals (r.allocations, 0LL);
        dir.deleteRecursively();
    }
};

static AudioSettingsConverterTests audioSettingsConverterTests;
static AudioSettingsInputTests audioSettingsInputTests;
static AudioSettingsPitchRangeTests audioSettingsPitchRangeTests;
static AudioSettingsSoundboardTests audioSettingsSoundboardTests;
} // namespace koe

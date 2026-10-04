// wave7/ir (INTERFACES.md §9.3): the "convolution" effect 「残響ファイル」, the preset "file" key and the
// AppController file handling. Impulse responses are synthetic files written under KOELOOM_DATA_DIR;
// measured loudness and CPU are on the synthetic voice (合成音声で代用).

#include "App/AppController.h"
#include "Core/Constants.h"
#include "Core/Paths.h"
#include "Dsp/Building.h"
#include "Effects/EffectRegistry.h"
#include "Model/Preset.h"
#include "Tests/EffectTestHarness.h"
#include "Tests/TestUtil.h"
#include "UI/MainComponent.h"
#include "UI/main/Panels.h"

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_dsp/juce_dsp.h>

#include <cmath>

namespace koe
{
namespace
{
using test::kSr;
constexpr int kBlock = 480;

juce::String ja (const char* s) { return juce::String::fromUTF8 (s); }

juce::File fixtureDir()
{
    auto d = paths::dataDir().getChildFile ("conv-fixtures");
    d.createDirectory();
    return d;
}

/** Writes channels (all the same length) as a 24-bit WAV, or as FLAC / AIFF by the file's extension. */
bool writeAudio (const juce::File& file, const std::vector<std::vector<float>>& channels, double sr)
{
    file.deleteFile();
    std::unique_ptr<juce::AudioFormat> format;
    if (file.hasFileExtension ("flac")) format = std::make_unique<juce::FlacAudioFormat>();
    else if (file.hasFileExtension ("aif;aiff")) format = std::make_unique<juce::AiffAudioFormat>();
    else format = std::make_unique<juce::WavAudioFormat>();
    std::unique_ptr<juce::OutputStream> out (file.createOutputStream());
    if (out == nullptr) return false;
    std::unique_ptr<juce::AudioFormatWriter> w (format->createWriterFor (out.get(), sr, unsigned (channels.size()), 24, {}, 0));
    if (w == nullptr) return false;
    out.release();
    juce::AudioBuffer<float> b (int (channels.size()), int (channels[0].size()));
    for (size_t c = 0; c < channels.size(); ++c) b.copyFrom (int (c), 0, channels[c].data(), int (channels[c].size()));
    return w->writeFromAudioSampleBuffer (b, 0, b.getNumSamples());
}

std::vector<float> impulseIr (int n) { std::vector<float> v (size_t (n), 0.0f); v[0] = 0.5f; return v; }

/** White noise under an exponential decay (time constant tau seconds). */
std::vector<float> decayNoise (double seconds, double tau, double sr, uint32_t seed)
{
    auto v = test::whiteNoise (seconds, 0.5f, seed, sr);
    for (size_t i = 0; i < v.size(); ++i) v[i] *= float (std::exp (-double (i) / sr / tau));
    return v;
}

juce::File writeIr (const juce::String& name, const std::vector<std::vector<float>>& ch, double sr)
{
    auto f = fixtureDir().getChildFile (name);
    writeAudio (f, ch, sr);
    return f;
}

const EffectInfo& info() { return *findEffectInfo ("convolution"); }

std::vector<float> params (std::initializer_list<std::pair<const char*, float>> overrides = {})
{
    std::vector<float> p;
    for (auto& s : info().params) p.push_back (s.def);
    for (auto& [id, v] : overrides) p[size_t (info().paramIndex (id))] = info().params[size_t (info().paramIndex (id))].clamp (v);
    return p;
}

/** Like EffectChain::create(): prepare, setAssetPath, setParam for all, reset. */
std::unique_ptr<IEffect> makeConv (const juce::File& ir, const std::vector<float>& p, double sr = kSr)
{
    auto fx = createEffect ("convolution");
    fx->prepare (sr, kBlock);
    fx->setAssetPath (ir == juce::File() ? std::string() : ir.getFullPathName().toStdString());
    for (size_t i = 0; i < p.size(); ++i) fx->setParam (int (i), p[i]);
    fx->reset();
    return fx;
}

void run (IEffect& fx, std::vector<float>& buf)
{
    for (size_t pos = 0; pos < buf.size(); pos += kBlock)
        fx.process (buf.data() + pos, int (std::min<size_t> (kBlock, buf.size() - pos)));
}

double energyFrom (const std::vector<float>& x, size_t start)
{
    double e = 0.0;
    for (size_t i = start; i < x.size(); ++i) e += double (x[i]) * x[i];
    return e;
}

/** Feeds silence in real time-ish blocks so the worker and JUCE's loader can swap the IR in.
    z: a kBlock buffer made by the caller (no allocation in here: it runs under AllocationCounter). */
void settle (IEffect& fx, int milliseconds, std::vector<float>& z)
{
    const auto until = juce::Time::getMillisecondCounterHiRes() + milliseconds;
    while (juce::Time::getMillisecondCounterHiRes() < until)
    {
        std::fill (z.begin(), z.end(), 0.0f);
        fx.process (z.data(), kBlock);
        juce::Thread::sleep (2);
    }
}

std::vector<float> impulseResponseOf (IEffect& fx, double seconds)
{
    std::vector<float> x (size_t (seconds * kSr), 0.0f);
    x[0] = 1.0f;
    run (fx, x);
    return x;
}

void freshDataDir()
{
    auto d = paths::dataDir();
    if (d.getFullPathName().contains ("KoeLoomTests")) d.deleteRecursively();
    d.createDirectory();
}
} // namespace

class ConvolutionTests : public juce::UnitTest
{
public:
    ConvolutionTests() : juce::UnitTest ("Convolution (残響ファイル)", "Effects") {}

    void runTest() override
    {
        freshDataDir();
        testRegistry();
        testNoFile();
        testImpulse();
        testFormatsAndRates();
        testLength();
        testRealtimeSafety();
        testLoudnessAndCpu();
        testPreset();
        testController();
    }

private:
    void testRegistry()
    {
        beginTest ("registry entry: timeSpace, phase 5, five knobs, AC-14 without a file");
        expect (hasEffectFactory ("convolution"));
        expect (info().category == EffectCategory::timeSpace);
        expectEquals (info().phase, 5);
        const char* ids[] = { "mix", "preDelayMs", "lowCutHz", "highCutHz", "lengthPct" };
        expectEquals (int (info().params.size()), 5);
        for (int i = 0; i < 5; ++i) expectEquals (juce::String (info().params[size_t (i)].id), juce::String (ids[i]));
        test::runAc14Checks (*this, "convolution");
    }

    void testNoFile()
    {
        beginTest ("no file / missing / unreadable file: input passes unchanged, getUiState() says so");
        const auto voice = test::synthVoice (1.0);
        auto check = [&] (const juce::File& ir, const std::string& rawPath, int wantState, const juce::String& label)
        {
            auto fx = createEffect ("convolution");
            fx->prepare (kSr, kBlock);
            fx->setAssetPath (rawPath.empty() && ir != juce::File() ? ir.getFullPathName().toStdString() : rawPath);
            const auto p = params ({ { "mix", 1.0f } });
            for (size_t i = 0; i < p.size(); ++i) fx->setParam (int (i), p[i]);
            fx->reset();
            auto out = voice;
            run (*fx, out);
            expect (out == voice, label + ": not passed through");
            expectEquals (fx->getUiState(), wantState, label);
        };
        check ({}, {}, 0, "no file");
        check (fixtureDir().getChildFile ("does-not-exist.wav"), {}, 2, "missing");
        auto junk = fixtureDir().getChildFile ("junk.wav");
        junk.replaceWithText ("this is not audio");
        check (junk, {}, 2, "unreadable");
        auto silent = writeIr ("silent.wav", { std::vector<float> (4800, 0.0f) }, kSr);
        check (silent, {}, 2, "all-zero IR");
    }

    void testImpulse()
    {
        beginTest ("impulse IR, mix 1, filters wide open: the input comes out almost unchanged");
        const auto ir = writeIr ("impulse.wav", { impulseIr (4800) }, kSr);
        auto fx = makeConv (ir, params ({ { "mix", 1.0f }, { "lowCutHz", 20.0f }, { "highCutHz", 20000.0f } }));
        expectEquals (fx->getUiState(), 1);
        expectEquals (fx->getLatencySamples(), 0);
        const auto in = test::sine (1000.0, 1.0, 0.5f);
        auto out = in;
        run (*fx, out);
        double err = 0.0, ref = 0.0;
        for (size_t i = size_t (kSr * 0.1); i < in.size(); ++i)
        {
            const double d = out[i] - in[i];
            err += d * d;
            ref += double (in[i]) * in[i];
        }
        const double errDb = 10.0 * std::log10 (err / ref + 1.0e-30);
        logMessage ("  impulse IR: error vs the input " + juce::String (errDb, 1) + " dB");
        expectLessThan (errDb, -25.0);

        beginTest ("pre-delay: the impulse IR output starts preDelayMs later");
        auto pre = makeConv (ir, params ({ { "mix", 1.0f }, { "preDelayMs", 100.0f }, { "lowCutHz", 20.0f }, { "highCutHz", 20000.0f } }));
        const auto y = impulseResponseOf (*pre, 0.5);
        int onset = 0;
        while (onset < int (y.size()) && std::abs (y[size_t (onset)]) < 1.0e-3f) ++onset;
        expectWithinAbsoluteError (onset, 4800, 4);

        beginTest ("low cut / high cut act on the wet path");
        auto toneGain = [&] (float lowCut, float highCut, double hz)
        {
            auto f = makeConv (ir, params ({ { "mix", 1.0f }, { "lowCutHz", lowCut }, { "highCutHz", highCut } }));
            auto s = test::sine (hz, 1.0, 0.5f);
            run (*f, s);
            return test::rmsDb (s.data() + 24000, 24000) - test::rmsDb (in.data() + 24000, 24000);
        };
        expectLessThan (toneGain (1000.0f, 20000.0f, 100.0), -25.0f, "low cut 1 kHz removes 100 Hz");
        expectLessThan (toneGain (20.0f, 1000.0f, 8000.0), -25.0f, "high cut 1 kHz removes 8 kHz");
    }

    void testFormatsAndRates()
    {
        beginTest ("stereo (left/right averaged), 44.1 kHz, FLAC and AIFF, longer than 10 s (cut)");
        // stereo: left impulse, right inverted impulse -> average = silence -> unusable IR
        {
            auto l = impulseIr (4800), r = impulseIr (4800);
            r[0] = -0.5f;
            auto fx = makeConv (writeIr ("cancel.wav", { l, r }, kSr), params());
            expectEquals (fx->getUiState(), 2, "L = -R averages to nothing");
        }
        // stereo halves: average is the impulse at half size, normalised back to the same gain
        {
            auto l = impulseIr (4800), r = std::vector<float> (4800, 0.0f);
            auto fx = makeConv (writeIr ("stereo.wav", { l, r }, kSr), params ({ { "mix", 1.0f }, { "lowCutHz", 20.0f }, { "highCutHz", 20000.0f } }));
            expectEquals (fx->getUiState(), 1);
            auto s = test::sine (1000.0, 1.0, 0.5f);
            run (*fx, s);
            expectWithinAbsoluteError (test::rmsDb (s.data() + 24000, 24000) - test::rmsDb (test::sine (1000.0, 0.5, 0.5f)), 0.0f, 0.3f);
        }
        for (const char* fname : { "impulse-44k.wav", "impulse.flac", "impulse.aiff" })
        {
            const double rate = juce::String (fname).contains ("44k") ? 44100.0 : kSr;
            auto fx = makeConv (writeIr (fname, { impulseIr (int (rate * 0.1)) }, rate), params ({ { "mix", 1.0f }, { "lowCutHz", 20.0f }, { "highCutHz", 20000.0f } }));
            expectEquals (fx->getUiState(), 1, fname);
            auto s = test::sine (1000.0, 1.0, 0.5f);
            run (*fx, s);
            const float g = test::rmsDb (s.data() + 24000, 24000) - test::rmsDb (test::sine (1000.0, 0.5, 0.5f));
            logMessage (juce::String ("  ") + fname + ": 1 kHz gain " + juce::String (g, 2) + " dB");
            expect (test::allFinite (s));
            expectWithinAbsoluteError (g, 0.0f, 0.5f, fname);
            auto again = makeConv (writeIr (fname, { impulseIr (int (rate * 0.1)) }, rate), params ({ { "mix", 1.0f }, { "lowCutHz", 20.0f }, { "highCutHz", 20000.0f } }));
            const auto y = impulseResponseOf (*again, 0.1);
            int at = 0;
            for (int i = 1; i < int (y.size()); ++i) if (std::abs (y[size_t (i)]) > std::abs (y[size_t (at)])) at = i;
            expectLessOrEqual (at, 2, juce::String (fname) + ": the resampled IR starts at 0");
        }
        // 12 s of slowly decaying noise: only the first kIrMaxSeconds are used
        {
            auto fx = makeConv (writeIr ("long12s.wav", { decayNoise (12.0, 6.0, kSr, 3) }, kSr), params ({ { "mix", 1.0f } }));
            expectEquals (fx->getUiState(), 1);
            const auto y = impulseResponseOf (*fx, 11.0);
            const double before = energyFrom (std::vector<float> (y.begin() + int (9.5 * kSr), y.begin() + int (9.9 * kSr)), 0);
            const double after = energyFrom (y, size_t ((kIrMaxSeconds + 0.05) * kSr));
            expect (test::allFinite (y));
            expectGreaterThan (before, 1.0e-6, "tail still present just before 10 s");
            expectLessThan (after, before * 1.0e-6, "nothing after 10 s");
        }
    }

    void testLength()
    {
        beginTest ("lengthPct keeps the start of the IR (at load and changed while running, without allocation)");
        const auto ir = writeIr ("decay2s.wav", { decayNoise (2.0, 0.29, kSr, 5) }, kSr); // -60 dB at 2 s
        auto tailAfter = [] (const std::vector<float>& y, double sec) { return energyFrom (y, size_t (sec * kSr)); };
        auto full = makeConv (ir, params ({ { "mix", 1.0f } }));
        const auto yFull = impulseResponseOf (*full, 2.5);
        auto quarter = makeConv (ir, params ({ { "mix", 1.0f }, { "lengthPct", 25.0f } }));
        const auto yQuarter = impulseResponseOf (*quarter, 2.5);
        logMessage ("  energy after 0.55 s: 100 % " + juce::String (tailAfter (yFull, 0.55), 6) + ", 25 % " + juce::String (tailAfter (yQuarter, 0.55), 9));
        expectGreaterThan (tailAfter (yFull, 0.55), 1.0e-3);
        expectLessThan (tailAfter (yQuarter, 0.55), tailAfter (yFull, 0.55) * 1.0e-6);

        // live change: 100 % -> 25 % while processing
        auto live = makeConv (ir, params ({ { "mix", 1.0f } }));
        std::vector<float> scratch (size_t (kBlock), 0.0f);
        settle (*live, 50, scratch);
        long long allocs = 0;
        {
            test::AllocationCounter counter;
            live->setParam (4, 25.0f);
            settle (*live, 400, scratch);
            allocs = counter.count();
        }
        expectEquals (int (allocs), 0, "allocations on the audio thread while the IR is swapped");
        live->reset();
        const auto yLive = impulseResponseOf (*live, 2.5);
        expectLessThan (tailAfter (yLive, 0.55), tailAfter (yFull, 0.55) * 1.0e-6, "the shortened IR is in use");
        expect (test::allFinite (yLive));
    }

    void testRealtimeSafety()
    {
        beginTest ("no allocation in process()/setParam()/reset() with an IR, every param at min and max: finite output");
        const auto ir = writeIr ("stereo-room.wav", { decayNoise (1.5, 0.2, 44100.0, 7), decayNoise (1.5, 0.2, 44100.0, 8) }, 44100.0);
        const auto voice = test::synthVoice (2.0);
        auto fx = makeConv (ir, params());
        expectEquals (fx->getUiState(), 1);
        auto buf = voice;
        long long allocs = 0;
        {
            test::AllocationCounter counter;
            for (size_t pos = 0, k = 0; pos + kBlock <= buf.size(); pos += kBlock, ++k)
            {
                const auto& sp = info().params[k % 4]; // lengthPct is covered by testLength
                fx->setParam (int (k % 4), (k / 4) % 2 == 0 ? sp.max : sp.min);
                fx->process (buf.data() + pos, kBlock);
                if (k == 100) fx->reset();
            }
            allocs = counter.count();
        }
        expectEquals (int (allocs), 0);
        expect (test::allFinite (buf));
        for (size_t p = 0; p < info().params.size(); ++p)
            for (const bool hi : { false, true })
            {
                auto pv = params ({ { "mix", 1.0f } });
                pv[p] = hi ? info().params[p].max : info().params[p].min;
                auto f = makeConv (ir, pv);
                auto x = voice;
                run (*f, x);
                expect (test::allFinite (x), juce::String (info().params[p].id) + (hi ? " max" : " min"));
                expectLessThan (test::peakDb (x), 12.0f, juce::String (info().params[p].id));
            }

        beginTest ("mix and cutoff changes over 50 ms do not click (clickRatio <= 2)");
        for (int p : { 0, 1, 2, 3 })
        {
            auto f = makeConv (ir, params());
            auto x = test::synthVoice (2.0, 7, kSr, false);
            const auto& sp = info().params[size_t (p)];
            const int start = int (kSr), steps = 5;
            for (size_t pos = 0, k = 0; pos + kBlock <= x.size(); pos += kBlock, ++k)
            {
                if (k == 0) f->setParam (p, sp.min);
                if (int (pos) >= start && int (pos) < start + steps * kBlock)
                    f->setParam (p, sp.min + (sp.max - sp.min) * float ((int (pos) - start) / kBlock + 1) / float (steps));
                f->process (x.data() + pos, kBlock);
            }
            const double ratio = test::clickRatio (x, start, start + steps * kBlock);
            logMessage (juce::String ("  ") + sp.id + " sweep clickRatio " + juce::String (ratio, 2));
            expectLessOrEqual (ratio, 2.0, sp.id);
        }
    }

    void testLoudnessAndCpu()
    {
        beginTest ("loudness close to the reverb (synthetic voice); CPU per 480-sample block (decides the weight)");
        const auto voice = test::synthVoice (6.0);
        const auto ir = writeIr ("hall-2s.wav", { decayNoise (2.0, 0.29, kSr, 11) }, kSr);
        auto conv = makeConv (ir, params ({ { "mix", 1.0f } }));
        auto wetConv = voice;
        run (*conv, wetConv);
        auto rev = createEffect ("reverb");
        rev->prepare (kSr, kBlock);
        auto* rinfo = findEffectInfo ("reverb");
        for (size_t i = 0; i < rinfo->params.size(); ++i) rev->setParam (int (i), rinfo->params[i].def);
        rev->setParam (rinfo->paramIndex ("type"), 1.0f); // hall
        rev->setParam (rinfo->paramIndex ("mix"), 1.0f);
        rev->reset();
        auto wetRev = voice;
        run (*rev, wetRev);
        const float convDb = test::rmsDb (wetConv), revDb = test::rmsDb (wetRev), dryDb = test::rmsDb (voice);
        logMessage ("  wet only: convolution " + juce::String (convDb - dryDb, 2) + " dB, reverb hall " + juce::String (revDb - dryDb, 2)
                    + " dB (re the dry voice, 合成音声で代用)");
        expectWithinAbsoluteError (convDb - revDb, 0.0f, 3.0f);

        const auto speech = test::synthVoice (10.0);
        for (const double seconds : { 1.0, 3.0, double (kIrMaxSeconds) })
        {
            const auto f = writeIr ("cpu-" + juce::String (int (seconds)) + "s.wav", { decayNoise (seconds, seconds / 7.0, kSr, 13) }, kSr);
            double best = 1.0e9, worstBlock = 0.0;
            for (int r = 0; r < 3; ++r)
            {
                auto fx = makeConv (f, params());
                auto buf = speech;
                const auto t0 = juce::Time::getHighResolutionTicks();
                double worst = 0.0;
                for (size_t pos = 0; pos + kBlock <= buf.size(); pos += kBlock)
                {
                    const auto b0 = juce::Time::getHighResolutionTicks();
                    fx->process (buf.data() + pos, kBlock);
                    worst = std::max (worst, juce::Time::highResolutionTicksToSeconds (juce::Time::getHighResolutionTicks() - b0) * 1.0e6);
                }
                const double sec = juce::Time::highResolutionTicksToSeconds (juce::Time::getHighResolutionTicks() - t0);
                const double us = sec * 1.0e6 / double (buf.size() / kBlock);
                if (us < best) { best = us; worstBlock = worst; }
            }
            logMessage ("  IR " + juce::String (seconds, 0) + " s: " + juce::String (best, 1) + " us/block = " + juce::String (best / 100.0, 2)
                        + " % of 10 ms (slowest block " + juce::String (worstBlock, 0) + " us), weight "
                        + juce::String::fromUTF8 (weightNameJa (info().weight)));
            expectLessThan (best, 1000.0, "far too slow");
        }
    }

    void testPreset()
    {
        beginTest ("preset \"file\": round trip, written only for convolution, bad names dropped and counted");
        Preset p;
        p.id = "user-ir";
        p.name = ja ("残響");
        SlotDef conv { "convolution", true, params(), "ホール #2 (大).wav" };
        SlotDef echo { "echo", true, {}, "x.wav" };
        for (auto& s : findEffectInfo ("echo")->params) echo.params.push_back (s.def);
        p.chain = { conv, echo };
        const auto text = serializePreset (p);
        expect (text.contains ("\"file\""));
        expect (! text.contains ("x.wav"), "file is written for convolution only");
        PresetLoadReport r;
        auto back = parsePreset (text, r);
        expect (back.has_value() && ! r.hasNotices(), r.toJapanese());
        if (back)
        {
            expectEquals (juce::String::fromUTF8 (back->chain[0].file.c_str()), ja ("ホール #2 (大).wav"));
            expect (back->chain[1].file.empty());
        }

        auto load = [] (const juce::String& fileJson, const char* type, int& clamped) -> std::string
        {
            const auto json = "{\"schemaVersion\":1,\"id\":\"user-t\",\"name\":\"t\",\"chain\":[{\"type\":\"" + juce::String (type) + "\",\"file\":" + fileJson + "}]}";
            PresetLoadReport rep;
            auto q = parsePreset (json, rep);
            clamped = rep.valuesClamped;
            return q && ! q->chain.empty() ? q->chain[0].file : std::string ("<none>");
        };
        int clamped = 0;
        for (const char* bad : { "\"../evil.wav\"", "\"a/b.wav\"", "\"a\\\\b.wav\"", "\"..\"", "\"C:x.wav\"", "5", "\" spaced.wav\"" })
        {
            expect (load (bad, "convolution", clamped).empty(), bad);
            expectEquals (clamped, 1, bad);
        }
        expect (load ("\"\"", "convolution", clamped).empty());
        expectEquals (clamped, 0, "empty name = no file, not a correction");
        expect (load ("\"ok.flac\"", "convolution", clamped) == "ok.flac");
        expectEquals (clamped, 0);
        expect (load ("\"../evil.wav\"", "reverb", clamped).empty());
        expectEquals (clamped, 0, "other types ignore the key");
    }

    void testController()
    {
        beginTest ("AppController: setSlotFile copies into ir, reuses identical, renames different, refuses with reasons");
        freshDataDir();
        AppController c (false);
        c.startup();
        juce::String why;
        expect (c.addEffect ("echo", why));
        expect (c.addEffect ("convolution", why), why);
        const int echoSlot = int (c.getChain().size()) - 2, slot = int (c.getChain().size()) - 1;
        const auto outside = paths::dataDir().getChildFile ("picked");
        outside.createDirectory();
        auto a = outside.getChildFile ("room.wav");
        writeAudio (a, { decayNoise (0.5, 0.1, kSr, 21) }, kSr);

        expect (! c.setSlotFile (echoSlot, a, why));
        expect (why.contains (ja ("残響ファイル")), why);

        expect (c.setSlotFile (slot, a, why), why);
        expectEquals (c.getSlotFileName (slot), juce::String ("room.wav"));
        expect (paths::irDir().getChildFile ("room.wav").existsAsFile());
        expect (c.isCurrentPresetModified());
        expect (! c.isSlotFileMissing (slot));
        expectEquals (c.getSlotUiState (slot), 1, "the rebuilt chain loaded the file");

        expect (c.setSlotFile (slot, a, why), why); // identical: reused
        expectEquals (c.getSlotFileName (slot), juce::String ("room.wav"));
        expectEquals (AppController::listIrFiles().size(), 1);

        auto other = outside.getChildFile ("sub").getChildFile ("room.wav");
        other.getParentDirectory().createDirectory();
        writeAudio (other, { decayNoise (0.7, 0.1, kSr, 22) }, kSr);
        expect (c.setSlotFile (slot, other, why), why);
        expectEquals (c.getSlotFileName (slot), juce::String ("room (2).wav"));
        auto dots = outside.getChildFile ("my..ir.wav");
        writeAudio (dots, { decayNoise (0.3, 0.1, kSr, 23) }, kSr);
        expect (c.setSlotFile (slot, dots, why), why);
        expectEquals (c.getSlotFileName (slot), juce::String ("my.ir.wav"));
        expect (AppController::listIrFiles() == juce::StringArray ({ "my.ir.wav", "room (2).wav", "room.wav" }), AppController::listIrFiles().joinIntoString ("|"));

        auto junk = outside.getChildFile ("junk.wav");
        junk.replaceWithText ("not audio");
        expect (! c.setSlotFile (slot, junk, why));
        expect (why.contains (ja ("読めません")), why);
        auto mp3 = outside.getChildFile ("x.mp3");
        mp3.replaceWithText ("x");
        expect (! c.setSlotFile (slot, mp3, why));
        expect (why.contains ("WAV"), why);
        auto tooLong = outside.getChildFile ("long.wav");
        writeAudio (tooLong, { std::vector<float> (size_t (12.0 * 8000.0), 0.1f) }, 8000.0);
        expect (! c.setSlotFile (slot, tooLong, why));
        expect (why.contains (ja ("10 秒")) && why.contains ("12.0"), why);
        expect (! c.setSlotFile (slot, outside.getChildFile ("gone.wav"), why));
        expectEquals (c.getSlotFileName (slot), juce::String ("my.ir.wav"), "a refusal keeps the old file");

        beginTest ("AppController: setSlotFileName, missing files, saved preset keeps only the name");
        c.setSlotFileName (slot, "room.wav");
        expectEquals (c.getSlotFileName (slot), juce::String ("room.wav"));
        c.setSlotFileName (slot, "../room.wav"); // refused silently
        expectEquals (c.getSlotFileName (slot), juce::String ("room.wav"));
        c.setSlotFileName (slot, ja ("ない.wav"));
        expect (c.isSlotFileMissing (slot));
        expectEquals (c.getSlotUiState (slot), 2);
        paths::irDir().getChildFile ("broken.wav").replaceWithText ("x");
        c.setSlotFileName (slot, "broken.wav");
        expect (c.isSlotFileMissing (slot), "unreadable counts as missing");
        c.setSlotFileName (slot, {});
        expect (! c.isSlotFileMissing (slot));
        expectEquals (c.getSlotUiState (slot), 0);
        c.setSlotFileName (slot, "room.wav");
        const auto text = serializePreset (c.getCurrentPreset());
        expect (text.contains ("\"file\": \"room.wav\"") || text.contains ("\"file\":\"room.wav\""), text);
        expect (! text.contains (paths::irDir().getFullPathName().replace ("\\", "\\\\")), "no folder in the preset");

        beginTest ("S-09 of a convolution slot: file row, ir list, missing file in the warning colour, fits 800x560");
        c.updateSettings ([] (Settings& st) { st.setupDone = true; st.tourStep = 7; });
        c.dispatchPendingMessages();
        {
            ui::MainComponent mc (c);
            mc.setSize (ui::Theme::minWidth, ui::Theme::minHeight);
            mc.showSlotDetail (slot);
            auto* panel = dynamic_cast<ui::mainui::SlotDetailPanel*> (ui::mainui::findById (&mc, "panel.slotDetail"));
            expect (panel != nullptr);
            if (panel != nullptr)
            {
                expectEquals (panel->fileStatusText(), ja ("使用中: room.wav"));
                expect (! panel->fileStatusIsWarning());
                auto* list = dynamic_cast<juce::ComboBox*> (ui::mainui::findById (panel, "slotDetail.irList"));
                expect (list != nullptr && list->getText() == "room.wav");
                if (list != nullptr) expectEquals (list->getNumItems(), 1 + AppController::listIrFiles().size());
                for (auto* id : { "slotDetail.irChoose", "slotDetail.irFolder", "slotDetail.irList" })
                {
                    auto* comp = ui::mainui::findById (panel, id);
                    expect (comp != nullptr && comp->isVisible() && ! comp->getBounds().isEmpty(), id);
                }
                const auto pb = mc.getLocalArea (panel, panel->getLocalBounds());
                expect (pb.getBottom() <= mc.getHeight() && pb.getRight() <= mc.getWidth(), "fits 800x560");

                if (list != nullptr) list->setSelectedId (1, juce::sendNotificationSync); // （なし）
                expect (c.getSlotFileName (slot).isEmpty());
                c.setSlotFileName (slot, ja ("ない.wav"));
                c.dispatchPendingMessages();
                expectEquals (panel->fileStatusText(), ja ("ファイルが見つかりません（ない.wav）"));
                expect (panel->fileStatusIsWarning());
            }
        }
    }
};

static ConvolutionTests convolutionTests;
} // namespace koe

// --bench-presets (INTERFACES.md §11.3). Owner: wave9/stream.
// The processor is set up like Tools/Calibrate.cpp's renderPreset (noise suppression off, gate open, output gain 0 dB),
// plus the converter quality; each process() call of one 480-sample block is timed with the high-resolution counter.

#include "Tools/Bench.h"

#include "Core/Constants.h"
#include "Core/Paths.h"
#include "Engine/EffectChain.h"
#include "Engine/VoiceProcessor.h"
#include "Effects/EffectRegistry.h"
#include "Model/PresetLibrary.h"
#include "Tests/TestUtil.h"

#include <algorithm>

namespace koe::tools
{
namespace
{
constexpr int kBlock = 480;
constexpr double kWarmupSeconds = 0.5;

struct Stats { double mean = 0.0, p95 = 0.0, max = 0.0; };

/** One run: times every block of `input` (from `warmup` samples on), microseconds. */
Stats timeOneRun (const Preset& preset, int quality, const std::vector<float>& input, int warmup)
{
    VoiceProcessor vp;
    vp.setConverterQuality (quality);
    vp.setNoiseSuppression (false, 1.0f);
    vp.setGate (false, -80.0f, 5.0f, 80.0f, 120.0f);
    vp.setInputGainDb (0.0f);
    vp.setOutputGainDb (0.0f);
    vp.setVoiceChangerOn (true);
    vp.setShifter (preset.hasShifter, preset.pitchSt, preset.formantSt);
    for (int i = 0; i < kMaxLayers; ++i)
    {
        VoiceProcessor::LayerParams lp;
        if (i < int (preset.layers.size()))
        {
            const auto& l = preset.layers[size_t (i)];
            lp.active = true;
            lp.scale = l.mode == LayerDef::Mode::scale;
            lp.pitchSt = l.pitchSt;
            lp.formantSt = l.formantSt;
            lp.levelDb = l.levelDb;
            lp.key = l.key;
            lp.minor = l.minor;
            lp.degree = l.degree;
        }
        vp.setLayer (i, lp);
    }
    vp.setTrimDb (preset.outputTrimDb);
    vp.prepare (kSampleRate, kBlock);
    std::vector<SlotDef> buildable;
    for (auto& s : preset.chain)
        if (hasEffectFactory (s.type)) buildable.push_back (s);
    vp.requestChain (EffectChain::create (buildable, kSampleRate, kBlock));

    std::vector<float> out (static_cast<size_t> (kBlock));
    std::vector<double> us;
    us.reserve (input.size() / kBlock);
    const double toUs = 1.0e6 / double (juce::Time::getHighResolutionTicksPerSecond());
    for (size_t pos = 0; pos + kBlock <= input.size(); pos += kBlock)
    {
        const auto t0 = juce::Time::getHighResolutionTicks();
        vp.process (input.data() + pos, out.data(), nullptr, kBlock);
        const auto t1 = juce::Time::getHighResolutionTicks();
        if (int (pos) >= warmup) us.push_back (double (t1 - t0) * toUs);
    }
    vp.collectGarbage();

    Stats s;
    if (us.empty()) return s;
    for (auto v : us) s.mean += v;
    s.mean /= double (us.size());
    s.max = *std::max_element (us.begin(), us.end());
    auto p = us.begin() + long (double (us.size() - 1) * 0.95);
    std::nth_element (us.begin(), p, us.end());
    s.p95 = *p;
    return s;
}

double median (std::vector<double> v)
{
    std::sort (v.begin(), v.end());
    return v.empty() ? 0.0 : v[v.size() / 2];
}
} // namespace

std::vector<BenchRow> runBench (int maxPresets, double seconds, int runs)
{
    const auto speech = test::referenceSpeechOrSynth();
    const int warmup = int (kWarmupSeconds * kSampleRate);
    std::vector<float> input (size_t (warmup + int (seconds * kSampleRate)));
    for (size_t i = 0; i < input.size(); ++i) input[i] = speech[i % speech.size()];

    PresetLibrary lib (paths::presetsDir());
    lib.reload();
    std::vector<BenchRow> rows;
    int done = 0;
    for (auto& p : lib.all())
    {
        if (! p.builtin) continue;
        if (maxPresets > 0 && done++ >= maxPresets) break;
        for (int q = 0; q <= 2; ++q)
        {
            std::vector<double> mean, p95, mx;
            for (int r = 0; r < std::max (1, runs); ++r)
            {
                const auto s = timeOneRun (p, q, input, warmup);
                mean.push_back (s.mean);
                p95.push_back (s.p95);
                mx.push_back (s.max);
            }
            BenchRow row;
            row.id = p.id;
            row.quality = q;
            row.meanUs = median (mean);
            row.p95Us = median (p95);
            row.maxUs = median (mx);
            row.realtimePct = row.meanUs / (1.0e6 * kBlock / kSampleRate) * 100.0;
            rows.push_back (row);
        }
    }
    return rows;
}

juce::String benchCsv (const std::vector<BenchRow>& rows)
{
    const auto cpu = juce::SystemStats::getCpuModel().replaceCharacter (',', ' ').trim();
    juce::String csv ("id,quality,meanUs,p95Us,maxUs,realtimePct,cpu\n");
    for (auto& r : rows)
        csv << juce::String (r.id) << "," << r.quality << "," << juce::String (r.meanUs, 1) << "," << juce::String (r.p95Us, 1) << ","
            << juce::String (r.maxUs, 1) << "," << juce::String (r.realtimePct, 2) << "," << cpu << "\n";
    return csv;
}

int benchPresets (const juce::String& csvPath)
{
    const auto rows = runBench (0, 10.0, 3);
    if (rows.empty()) return 1;
    auto file = csvPath.isNotEmpty() ? juce::File::getCurrentWorkingDirectory().getChildFile (csvPath)
                                     : juce::File::getSpecialLocation (juce::File::currentExecutableFile).getSiblingFile ("bench.csv");
    return file.replaceWithText (benchCsv (rows), false, false, "\n") ? 0 : 1;
}
} // namespace koe::tools

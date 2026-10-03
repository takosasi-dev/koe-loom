#include "Tools/Calibrate.h"

#include "Core/Paths.h"
#include "Engine/EffectChain.h"
#include "Engine/VoiceProcessor.h"
#include "Effects/EffectRegistry.h"
#include "Model/PresetLibrary.h"
#include "Tests/TestUtil.h"

#include <cmath>

namespace koe::tools
{
std::vector<float> renderPreset (const Preset& preset, const std::vector<float>& input, int& latencyOut)
{
    constexpr int block = 480;
    VoiceProcessor vp;
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
    vp.prepare (test::kSr, block);
    std::vector<SlotDef> buildable;
    for (auto& s : preset.chain)
        if (hasEffectFactory (s.type)) buildable.push_back (s);
    vp.requestChain (EffectChain::create (buildable, test::kSr, block));

    std::vector<float> out (input.size());
    for (size_t pos = 0; pos < input.size(); pos += block)
    {
        const int n = int (std::min<size_t> (block, input.size() - pos));
        vp.process (input.data() + pos, out.data() + pos, nullptr, n);
    }
    latencyOut = vp.getLatencySamples();
    return out;
}

float measureLevelDb (const std::vector<float>& input, const std::vector<float>& output, int latency)
{
    const int start = int (test::kSr) + latency;
    const int n = int (input.size()) - int (test::kSr); // to the end of the input (no tail)
    if (n <= 0 || start + n > int (output.size())) return -120.0f;
    return test::rmsDb (output.data() + start, n);
}

int calibratePresets (const juce::String& csvPath)
{
    bool real = false;
    const auto speech = test::referenceSpeechOrSynth (&real);
    // pad with silence so the latency-shifted window is fully inside the output
    auto input = speech;
    input.resize (input.size() + size_t (test::kSr), 0.0f);

    PresetLibrary lib (paths::presetsDir());
    lib.reload();
    const auto* asis = lib.find ("natural-asis");
    if (asis == nullptr) return 1;
    int lat = 0;
    auto ref = renderPreset (*asis, input, lat);
    const auto inSpan = std::vector<float> (input.begin(), input.begin() + long (speech.size()));
    const float refDb = measureLevelDb (inSpan, ref, lat);

    juce::String csv ("id,diffDb,suggestedTrimDb,usesReference\n");
    for (auto& p : lib.all())
    {
        if (! p.builtin) continue;
        auto untrimmed = p;
        untrimmed.outputTrimDb = 0.0f;
        int l = 0;
        const auto out = renderPreset (untrimmed, input, l);
        const float diff = measureLevelDb (inSpan, out, l) - refDb;
        const float trim = juce::jlimit (-12.0f, 6.0f, std::round (-diff * 2.0f) / 2.0f); // 0.5 dB steps
        csv << juce::String (p.id) << "," << juce::String (diff, 2) << "," << juce::String (trim, 1) << "," << (real ? "1" : "0") << "\n";
    }
    auto file = csvPath.isNotEmpty() ? juce::File::getCurrentWorkingDirectory().getChildFile (csvPath)
                                     : juce::File::getSpecialLocation (juce::File::currentExecutableFile).getSiblingFile ("calibration.csv");
    return file.replaceWithText (csv, false, false, "\n") ? 0 : 1;
}
} // namespace koe::tools

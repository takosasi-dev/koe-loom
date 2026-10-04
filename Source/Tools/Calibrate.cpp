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
std::vector<float> renderPreset (const Preset& preset, const std::vector<float>& input, int& latencyOut, double sampleRate)
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
    vp.prepare (sampleRate, block);
    std::vector<SlotDef> buildable;
    for (auto& s : preset.chain)
        if (hasEffectFactory (s.type)) buildable.push_back (s);
    vp.requestChain (EffectChain::create (buildable, sampleRate, block));

    std::vector<float> out (input.size());
    for (size_t pos = 0; pos < input.size(); pos += block)
    {
        const int n = int (std::min<size_t> (block, input.size() - pos));
        vp.process (input.data() + pos, out.data() + pos, nullptr, n);
    }
    latencyOut = vp.getLatencySamples();
    return out;
}

float measureLevelDb (const std::vector<float>& input, const std::vector<float>& output, int latency, double sampleRate)
{
    const int start = int (sampleRate) + latency;
    const int n = int (input.size()) - int (sampleRate); // to the end of the input (no tail)
    if (n <= 0 || start + n > int (output.size())) return -120.0f;
    return test::rmsDb (output.data() + start, n);
}

float presetLevelDb (const Preset& preset, const std::vector<float>& speech, double sampleRate)
{
    // pad with silence so the latency-shifted window is fully inside the output
    auto input = speech;
    input.resize (speech.size() + size_t (sampleRate), 0.0f);
    int lat = 0;
    const auto out = renderPreset (preset, input, lat, sampleRate);
    return measureLevelDb (speech, out, lat, sampleRate);
}

float calibratedTrimDb (float shippedTrimDb, float diffDb)
{
    if (std::abs (diffDb) <= 1.0f) return shippedTrimDb; // tools/apply_trims.py --threshold 1.0
    return kTrimDb.clamp (std::round ((shippedTrimDb - diffDb) * 2.0f) / 2.0f);
}

int calibratePresets (const juce::String& csvPath)
{
    bool real = false;
    const auto speech = test::referenceSpeechOrSynth (&real);
    PresetLibrary lib (paths::presetsDir());
    lib.reload();
    const auto* asis = lib.find ("natural-asis");
    if (asis == nullptr) return 1;
    const float refDb = presetLevelDb (*asis, speech);

    juce::String csv ("id,diffDb,suggestedTrimDb,usesReference\n");
    for (auto& p : lib.all())
    {
        if (! p.builtin) continue;
        auto untrimmed = p;
        untrimmed.outputTrimDb = 0.0f;
        const float diff = presetLevelDb (untrimmed, speech) - refDb;
        const float trim = juce::jlimit (-12.0f, 6.0f, std::round (-diff * 2.0f) / 2.0f); // 0.5 dB steps
        csv << juce::String (p.id) << "," << juce::String (diff, 2) << "," << juce::String (trim, 1) << "," << (real ? "1" : "0") << "\n";
    }
    auto file = csvPath.isNotEmpty() ? juce::File::getCurrentWorkingDirectory().getChildFile (csvPath)
                                     : juce::File::getSpecialLocation (juce::File::currentExecutableFile).getSiblingFile ("calibration.csv");
    return file.replaceWithText (csv, false, false, "\n") ? 0 : 1;
}
} // namespace koe::tools

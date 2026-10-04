// プリセットを混ぜる (INTERFACES.md §10.3). Owner: wave8/morph.
// beginMorph builds the blended chain once (every slot either side has); setMorphAmount only writes parameters and
// SlotDef::wet into the running chain, so dragging the slider never rebuilds or crossfades the chain.

#include "App/AppController.h"

#include "Core/Constants.h"
#include "Dsp/Building.h"
#include "Effects/EffectRegistry.h"

#include <algorithm>
#include <map>

namespace koe
{
namespace
{
juce::String u8 (const char* s) { return juce::String::fromUTF8 (s); }

// a * (1 - t) + b * t: exactly a at t = 0 and exactly b at t = 1 (the t = 0 / 1 outputs must match A / B bit for bit)
float mix (float a, float b, float t) { return a * (1.0f - t) + b * t; }

// Copy of AppController.cpp's chainIndexFor (INTERFACES.md §10.2): model slot index -> chain slot index.
int chainIndexFor (const std::vector<SlotDef>& chain, int modelIndex)
{
    if (modelIndex < 0 || modelIndex >= int (chain.size())) return -1;
    if (! hasEffectFactory (chain[size_t (modelIndex)].type)) return -1;
    int idx = 0;
    for (int i = 0; i < modelIndex; ++i)
        if (hasEffectFactory (chain[size_t (i)].type)) ++idx;
    return idx;
}

float wetOf (const SlotDef* s) { return s != nullptr && s->enabled ? std::clamp (s->wet, 0.0f, 1.0f) : 0.0f; } // OFF = wet 0

float paramOf (const SlotDef& s, size_t k, const ParamSpec& spec) { return k < s.params.size() ? s.params[k] : spec.def; }

float gainOf (const Preset& p, size_t i)
{
    if (! p.hasShifter || i >= p.layers.size() || ! p.layers[i].enabled) return 0.0f;
    return dsp::dbToGain (p.layers[i].levelDb);
}
} // namespace

namespace morph
{
std::vector<std::pair<int, int>> pairSlots (const std::vector<SlotDef>& a, const std::vector<SlotDef>& b)
{
    auto key = [] (const SlotDef& s) { return s.type == "convolution" ? s.type + "|" + s.file : s.type; };
    std::map<std::string, std::vector<int>> inB;
    for (int j = 0; j < int (b.size()); ++j) inB[key (b[size_t (j)])].push_back (j);
    std::map<std::string, size_t> seen;
    std::vector<std::pair<int, int>> out;
    for (int i = 0; i < int (a.size()); ++i)
    {
        const auto k = key (a[size_t (i)]);
        const auto& list = inB[k];
        const size_t n = seen[k]++;
        out.push_back ({ i, n < list.size() ? list[n] : -1 });
    }
    int last = -1; // position in out of the pair the previous B slot belongs to
    for (int j = 0; j < int (b.size()); ++j)
    {
        auto it = std::find_if (out.begin(), out.end(), [j] (const auto& p) { return p.second == j; });
        if (it != out.end())
            last = int (it - out.begin());
        else
            out.insert (out.begin() + ++last, { -1, j });
    }
    return out;
}

Preset blend (const MorphData& m, float t)
{
    const auto& a = m.a;
    const auto& b = m.b;
    Preset p = a;
    p.builtin = false;
    p.hasShifter = a.hasShifter || b.hasShifter;
    p.pitchSt = mix (a.hasShifter ? a.pitchSt : 0.0f, b.hasShifter ? b.pitchSt : 0.0f, t);
    p.formantSt = mix (a.hasShifter ? a.formantSt : 0.0f, b.hasShifter ? b.formantSt : 0.0f, t);
    p.outputTrimDb = kTrimDb.clamp (mix (m.trimA, m.trimB, t));

    // voices, paired by number
    p.layers.clear();
    const size_t numLayers = std::max (a.hasShifter ? a.layers.size() : 0, b.hasShifter ? b.layers.size() : 0);
    for (size_t i = 0; i < numLayers; ++i)
    {
        const LayerDef* la = a.hasShifter && i < a.layers.size() ? &a.layers[i] : nullptr;
        const LayerDef* lb = b.hasShifter && i < b.layers.size() ? &b.layers[i] : nullptr;
        LayerDef l = la != nullptr && (lb == nullptr || t < 0.5f) ? *la : *lb; // mode, key, degree, minor switch at 0.5
        if (la != nullptr && lb != nullptr)
        {
            l.pitchSt = mix (la->pitchSt, lb->pitchSt, t);
            l.formantSt = mix (la->formantSt, lb->formantSt, t);
        }
        const float ga = gainOf (a, i), gb = gainOf (b, i);
        if (ga > 0.0f && gb > 0.0f)
        {
            l.levelDb = mix (la->levelDb, lb->levelDb, t);
            l.enabled = true;
        }
        else
        {
            // one side only: fade in linear gain. The engine clamps a voice's level to kLayerLevelDb (-24 dB), so below
            // that it turns OFF (its own 30 ms fade) instead of sitting at -24 dB
            const float g = mix (ga, gb, t);
            l.enabled = g >= dsp::dbToGain (kLayerLevelDb.min);
            if (l.enabled) l.levelDb = kLayerLevelDb.clamp (dsp::gainToDb (g));
        }
        p.layers.push_back (l);
    }

    // chain
    p.chain.clear();
    for (auto [ia, ib] : m.pairs)
    {
        const SlotDef* sa = ia >= 0 ? &a.chain[size_t (ia)] : nullptr;
        const SlotDef* sb = ib >= 0 ? &b.chain[size_t (ib)] : nullptr;
        SlotDef s = sa != nullptr ? *sa : *sb;
        const float wa = wetOf (sa), wb = wetOf (sb);
        if (sa != nullptr && sb != nullptr)
            if (auto* info = findEffectInfo (s.type))
            {
                s.params.resize (info->params.size());
                for (size_t k = 0; k < info->params.size(); ++k)
                {
                    const auto& spec = info->params[k];
                    const float va = paramOf (*sa, k, spec), vb = paramOf (*sb, k, spec);
                    s.params[k] = spec.isChoice() ? (t < 0.5f ? va : vb) : spec.clamp (mix (va, vb, t));
                }
            }
        s.enabled = wa > 0.0f || wb > 0.0f; // independent of t: the chain's structure never changes while blending
        if (s.enabled)
            s.wet = mix (wa, wb, t);
        else if (sa != nullptr && sb != nullptr)
            s.wet = mix (sa->wet, sb->wet, t);
        p.chain.push_back (s);
    }
    return p;
}

juce::String checkBlend (const Preset& p)
{
    const int n = int (p.chain.size());
    if (n > kMaxSlots)
        return u8 ("混ぜるとエフェクトが ") + juce::String (n) + u8 (" 個になり、スロットの上限（10 個）を超えます。");
    int heavy = 0, freeze = 0, looper = 0;
    for (auto& s : p.chain)
    {
        auto* info = findEffectInfo (s.type);
        heavy += s.enabled && info != nullptr && info->weight == EffectWeight::heavy;
        freeze += s.type == "freeze";
        looper += s.type == "looper";
    }
    if (heavy > kMaxHeavyOn)
        return u8 ("混ぜると重いエフェクトが ") + juce::String (heavy) + u8 (" 個 ON になります（同時に 2 つまで）。");
    if (freeze > 1 || looper > 1)
        return u8 ("混ぜるとフリーズかルーパーが 2 つになります（チェーンに 1 つまで）。");
    return {};
}

juce::String blendName (const juce::String& a, const juce::String& b)
{
    const auto sep = u8 (" × ");
    auto x = a.trim(), y = b.trim();
    while (x.length() + y.length() + sep.length() > kPresetNameMaxChars)
    {
        if (x.length() >= y.length())
            x = x.dropLastCharacters (1).trimEnd();
        else
            y = y.dropLastCharacters (1).trimEnd();
    }
    return x + sep + y;
}
} // namespace morph

bool AppController::beginMorph (const std::string& idA, const std::string& idB, juce::String& whyNot)
{
    const auto* a = library->find (idA);
    const auto* b = library->find (idB);
    if (a == nullptr || b == nullptr)
    {
        whyNot = u8 ("プリセットが見つかりません。");
        return false;
    }
    if (hasLooperRecording())
    {
        whyNot = u8 ("ルーパーに録音があるため、混ぜられません。先にルーパーの録音を消してください。"); // E-27
        return false;
    }
    auto trimOf = [this] (const Preset& p) // getEffectiveTrimDb() for a preset that is not the working one
    {
        if (p.builtin)
            if (auto it = settings.calibratedTrimDb.find (juce::String (p.id)); it != settings.calibratedTrimDb.end()) return it->second;
        return p.outputTrimDb;
    };
    MorphData m;
    m.a = *a;
    m.b = *b;
    m.trimA = trimOf (*a);
    m.trimB = trimOf (*b);
    m.pairs = morph::pairSlots (a->chain, b->chain);
    auto p = morph::blend (m, 0.0f);
    if (auto why = morph::checkBlend (p); why.isNotEmpty())
    {
        whyNot = why;
        return false;
    }
    p.name = morph::blendName (a->name, b->name);
    setCompareHold (false); // like a preset load (§9.4)
    current = p;
    currentBaseId = idA;
    settings.currentPresetId = juce::String (idA);
    saveSettingsSoon();
    applyPresetToEngine (true);
    m.builds = chainBuilds;
    m.active = true;
    morph = std::move (m);
    markModified();
    return true;
}

void AppController::setMorphAmount (float amount)
{
    if (! isMorphing()) return;
    morph.amount = std::clamp (amount, 0.0f, 1.0f);
    auto p = morph::blend (morph, morph.amount);
    p.id = current.id;
    p.name = current.name; // "A × B", or what it was saved as
    if (p.chain.size() != current.chain.size()) return; // cannot happen without a rebuild (which ends the blend)
    for (size_t i = 0; i < p.chain.size(); ++i)
        p.chain[i].enabled = current.chain[i].enabled;  // keep the user's ON/OFF switches (they do not rebuild)
    current = p;
    if (auto* chain = processor.getRequestedChain(); chain != nullptr)
        for (int i = 0; i < int (current.chain.size()); ++i)
        {
            const int ci = chainIndexFor (current.chain, i);
            const auto& s = current.chain[size_t (i)];
            auto* info = findEffectInfo (s.type);
            if (ci < 0 || ci >= chain->size() || info == nullptr) continue;
            auto& slot = chain->slot (ci);
            for (size_t k = 0; k < info->params.size() && k < s.params.size() && k < slot.params.size(); ++k) slot.params[k].store (s.params[k]);
            slot.wet.store (s.wet);
        }
    applyPresetToEngine (false); // pitch, formant, voices, trim
    markModified();
}

float AppController::getMorphAmount() const { return morph.amount; }
bool AppController::isMorphing() const { return morph.active && morph.builds == chainBuilds; }
void AppController::endMorph() { morph.active = false; }
bool AppController::getMorphPresets (std::string& idA, std::string& idB, juce::String& nameA, juce::String& nameB) const
{
    if (! isMorphing()) return false;
    idA = morph.a.id;
    idB = morph.b.id;
    nameA = morph.a.name;
    nameB = morph.b.name;
    return true;
}
} // namespace koe

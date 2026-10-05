// 元に戻す / やり直し and A/B 聞き比べ (INTERFACES.md §12.3, owner wave10/edit).
//
// History: noteEdit() (every markModified) pushes the state before the edit; edits closer than kUndoMergeMs merge (a knob
// drag is one step). undo / redo swap the working preset with a stored one; when the chain keeps its shape (slot types, order,
// IR files) the values go into the running effects in place (no rebuild, tails keep ringing), else the chain is rebuilt.
// A step restores everything, the identity (id, name, builtin) included (undoing a blend gives back 「魔王」). A rename or
// 「新しく保存」 changes the identity without a hook: the stored steps follow it (followIdentity), so they keep the new name.
// A/B: playedPreset() returns the library copy of the base preset, so the engine plays the saved voice while the screen keeps
// the edited one. Any edit, save or preset load turns it off.

#include "App/AppController.h"

#include "Effects/EffectRegistry.h"

#include <algorithm>

namespace koe
{
namespace
{
juce::String u8 (const char* s) { return juce::String::fromUTF8 (s); }

/** AppController.cpp's chainIndexFor (file-local there): model slot index -> engine chain slot index, -1 = not built. */
int engineIndex (const std::vector<SlotDef>& chain, int modelIndex)
{
    if (modelIndex < 0 || modelIndex >= int (chain.size()) || ! hasEffectFactory (chain[size_t (modelIndex)].type)) return -1;
    int idx = 0;
    for (int i = 0; i < modelIndex; ++i)
        if (hasEffectFactory (chain[size_t (i)].type)) ++idx;
    return idx;
}

/** Same slots in the same order (type and IR file): the running chain can take the other's values in place. */
bool sameShape (const std::vector<SlotDef>& a, const std::vector<SlotDef>& b)
{
    return std::equal (a.begin(), a.end(), b.begin(), b.end(),
                       [] (const SlotDef& x, const SlotDef& y) { return x.type == y.type && x.file == y.file; });
}

bool sameIdentity (const Preset& a, const Preset& b) { return a.id == b.id && a.name == b.name && a.builtin == b.builtin; }

void copyIdentity (Preset& to, const Preset& from)
{
    to.id = from.id;
    to.name = from.name;
    to.builtin = from.builtin;
}

/** The working preset took the saved preset's identity since the last step without an edit (renamePreset, saveCurrentAsNew):
    relabel the stored steps that carried the old one. (A blend's own identity change comes with its edit and is kept.) */
void followIdentity (EditData& e, const Preset& current, const Preset* base)
{
    if (! e.last.has_value() || sameIdentity (*e.last, current) || base == nullptr || ! sameIdentity (current, *base)) return;
    for (auto* list : { &e.undo, &e.redo })
        for (auto& p : *list)
            if (sameIdentity (p, *e.last)) copyIdentity (p, current);
    copyIdentity (*e.last, current);
}

/** What saving p under base's identity would store (the loader's clean-up: E-30 drops layers without the converter,
    numbers as written), so "back to the saved preset" does not hinge on float noise from the JSON round trip. */
juce::String savedForm (Preset p, const Preset& base)
{
    p.id = base.id;
    p.name = base.name;
    p.builtin = base.builtin;
    PresetLoadReport unused;
    const auto parsed = parsePreset (serializePreset (p), unused);
    return parsed ? serializePreset (*parsed) : juce::String();
}
} // namespace

bool AppController::canUndo() const { return ! edit.undo.empty(); }
bool AppController::canRedo() const { return ! edit.redo.empty(); }

bool AppController::undo (juce::String& whyNot)
{
    auto& e = edit;
    if (e.undo.empty())
    {
        whyNot = u8 ("元に戻せる変更がありません。");
        return false;
    }
    const auto* base = library->find (currentBaseId);
    followIdentity (e, current, base);
    auto target = e.undo.back();
    // during A/B the engine runs the saved copy's chain, so going back to the working preset always rebuilds
    const bool rebuild = e.abOn || ! sameShape (current.chain, target.chain);
    if (rebuild && ! e.abOn && hasLooperRecording())
    {
        whyNot = u8 ("ルーパーに録音があるため、元に戻せません。先にルーパーの録音を消してください。"); // E-27
        return false;
    }
    e.abOn = false;
    e.undo.pop_back();
    e.redo.push_back (current);
    const auto before = current.chain;
    current = std::move (target);
    e.last = current;
    e.lastEditMs = -1.0e9; // the next edit is a step of its own
    if (rebuild) applyPresetToEngine (true);
    else
    {
        if (auto* chain = processor.getRequestedChain(); chain != nullptr)
            for (int i = 0; i < int (current.chain.size()); ++i)
            {
                const auto& s = current.chain[size_t (i)];
                const int ci = engineIndex (current.chain, i);
                const auto* info = findEffectInfo (s.type);
                if (ci < 0 || ci >= chain->size() || info == nullptr) continue;
                auto& slot = chain->slot (ci);
                for (size_t k = 0; k < info->params.size() && k < s.params.size() && k < slot.params.size(); ++k) slot.params[k].store (s.params[k]);
                if (s.enabled != before[size_t (i)].enabled)
                {
                    if (s.enabled) slot.autoStopped.store (false); // like setSlotEnabled: switching on clears 自動停止
                    slot.enabled.store (s.enabled);
                }
                slot.wet.store (s.wet);
                slot.modDepth.store (s.modTarget.empty() ? 0.0f : s.modDepth);
                slot.modIndex.store (s.modTarget.empty() ? EffectChain::kModNone : EffectChain::modIndexFor (*info, s.modTarget));
            }
        applyPresetToEngine (false); // pitch, formant, voices, trim
    }
    modified = base == nullptr || savedForm (current, *base) != savedForm (*base, *base);
    sendChangeMessage(); // not markModified: a step back is not a new step
    return true;
}

bool AppController::redo (juce::String& whyNot)
{
    if (edit.redo.empty())
    {
        whyNot = u8 ("やり直せる変更がありません。");
        return false;
    }
    std::swap (edit.undo, edit.redo); // a redo is an undo with the two lists swapped
    const bool ok = undo (whyNot);
    std::swap (edit.undo, edit.redo);
    if (! ok) whyNot = u8 ("ルーパーに録音があるため、やり直せません。先にルーパーの録音を消してください。"); // the only other refusal (E-27)
    return ok;
}

bool AppController::canAbCompare() const { return modified && library->find (currentBaseId) != nullptr; }

bool AppController::setAbCompare (bool on, juce::String& whyNot)
{
    auto& e = edit;
    if (on == e.abOn) return true;
    if (on)
    {
        const auto* base = library->find (currentBaseId);
        if (! modified || base == nullptr)
        {
            whyNot = modified ? u8 ("元のプリセットが見つからないため、聞き比べできません。")
                              : u8 ("まだ何も変えていないため、聞き比べるものがありません。");
            return false;
        }
        if (hasLooperRecording())
        {
            whyNot = u8 ("ルーパーに録音があるため、A/B を使えません。先にルーパーの録音を消してください。"); // E-27
            return false;
        }
        e.abSaved = *base;
    }
    // Off never refuses: it is how the user gets back, and edits / saves call it with no way to ask.
    e.abOn = on;
    const bool blending = isMorphing();
    applyPresetToEngine (true);
    if (blending) morph.builds = chainBuilds; // A/B only swaps what the engine plays: the blend's controls keep working
    sendChangeMessage();
    return true;
}

bool AppController::isAbCompare() const { return edit.abOn; }

void AppController::noteEdit()
{
    auto& e = edit;
    juce::String unused;
    setAbCompare (false, unused); // touching anything goes back to the edited voice, engine included
    if (e.fresh)
    {
        // startup() set the library copy of the base without a hook; a blend begun first has nothing known to go back to
        e.fresh = false;
        const auto* base = library->find (currentBaseId);
        if (base != nullptr && ! isMorphing()) e.last = *base;
        else e.last.reset();
    }
    followIdentity (e, current, library->find (currentBaseId));
    if (e.last.has_value() && *e.last == current) return; // nothing really changed (a double-click on a knob already at its default)
    const double now = EditData::nowMs();
    if (e.last.has_value() && (e.undo.empty() || now - e.lastEditMs >= kUndoMergeMs))
    {
        e.undo.push_back (std::move (*e.last));
        while (int (e.undo.size()) > kUndoSteps) e.undo.pop_front();
    }
    e.redo.clear();
    e.last = current;
    e.lastEditMs = now;
}

void AppController::editReset()
{
    auto& e = edit;
    e.undo.clear();
    e.redo.clear();
    e.last = current;
    e.fresh = false;
    e.lastEditMs = -1.0e9;
    e.abOn = false; // flag only: loadPreset gives the engine the new preset right after
}

void AppController::editBeforeSave()
{
    juce::String unused;
    setAbCompare (false, unused); // the engine plays what is saved; the history stays
}

const Preset& AppController::playedPreset() const { return edit.abOn ? edit.abSaved : current; }
} // namespace koe

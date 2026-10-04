// 押している間だけのエフェクト and アプリごとの自動切り替え (INTERFACES.md §10.3). Owner: wave8/automation.

#include "App/AppController.h"
#include "Platform/ForegroundApp.h"

namespace koe
{
namespace
{
juce::String u8 (const char* s) { return juce::String::fromUTF8 (s); }

std::array<bool, kMomentarySlots> uiHold {}; // ponytail: one flag set for the one app window; per controller if that changes

juce::String recipeAt (const Settings& s, int i)
{
    const auto id = i < s.momentaryRecipes.size() ? s.momentaryRecipes[i] : juce::String();
    return findMomentaryRecipe (id) != nullptr ? id : juce::String(); // unknown ids (a hand-edited file) count as none
}

/** Builds the chains the settings ask for at the processor's rate / block size and attaches the post processor the
    first time any recipe is set. Cheap when nothing changed (every tick). */
void syncMomentary (AutomationData& a, VoiceProcessor& vp, const Settings& s)
{
    bool need = false;
    for (int i = 0; i < kMomentarySlots; ++i) need = need || recipeAt (s, i).isNotEmpty();
    if (! need && a.fx == nullptr) return; // default settings: nothing in the path at all
    if (a.fx == nullptr) a.fx = std::make_unique<MomentaryFx>();
    const double rate = vp.getSampleRate();
    const int block = vp.getMaxBlockSize();
    const bool reprepared = rate != a.builtRate || block != a.builtBlock;
    for (int i = 0; i < kMomentarySlots; ++i)
    {
        const auto id = recipeAt (s, i);
        if (! reprepared && id == a.built[size_t (i)]) continue;
        const auto* def = findMomentaryRecipe (id);
        a.fx->setChain (i, def != nullptr ? EffectChain::create (momentaryRecipeChain (*def), rate, block) : nullptr, rate,
                        def != nullptr ? def->tailSeconds : 0.0f);
        a.built[size_t (i)] = id;
    }
    a.builtRate = rate;
    a.builtBlock = block;
    if (! a.attached)
    {
        vp.setPostProcessor (a.fx.get());
        a.attached = true;
    }
}

void forgetSwitch (AutomationData& a)
{
    a.switchedTo.clear();
    a.restoreId.clear();
    a.appliedRule = {};
    a.blockedRule = {};
}
} // namespace

void setMomentaryUiHold (int index, bool held)
{
    if (index >= 0 && index < kMomentarySlots) uiHold[size_t (index)] = held;
}

// ============================================================================ 押している間だけのエフェクト
std::vector<AppController::MomentaryRecipe> AppController::getMomentaryRecipes()
{
    std::vector<MomentaryRecipe> r;
    for (auto& d : momentaryRecipeDefs()) r.push_back ({ d.id, u8 (d.nameJa) });
    return r;
}

void AppController::setMomentaryRecipe (int index, const juce::String& recipeId)
{
    if (index < 0 || index >= kMomentarySlots) return;
    const auto id = findMomentaryRecipe (recipeId) != nullptr ? recipeId : juce::String();
    if (id.isEmpty()) setMomentaryHeld (index, false);
    updateSettings ([index, id] (Settings& s)
    {
        while (s.momentaryRecipes.size() <= index) s.momentaryRecipes.add ({});
        s.momentaryRecipes.set (index, id);
    });
    syncMomentary (automation, processor, settings);
}

void AppController::setMomentaryHeld (int index, bool held)
{
    if (index < 0 || index >= kMomentarySlots) return;
    auto& a = automation;
    const auto i = size_t (index);
    if (held)
    {
        if (recipeAt (settings, index).isEmpty()) return; // 型が未設定: nothing to do
        syncMomentary (a, processor, settings);
        a.heldKey[i] = 0;
        for (auto& h : settings.hotkeys)
            if (h.action == "momentary." + juce::String (index + 1)) a.heldKey[i] = h.virtualKey;
    }
    if (a.held[i] == held) return;
    a.held[i] = held;
    if (a.fx != nullptr) a.fx->setHeld (index, held);
    sendChangeMessage();
}

bool AppController::isMomentaryHeld (int index) const
{
    return index >= 0 && index < kMomentarySlots && automation.held[size_t (index)];
}

// ============================================================================ アプリごとの自動切り替え
juce::String AppController::getForegroundProgram() const { return foreground::programInFront(); }

juce::StringArray AppController::listRunningPrograms() { return foreground::visiblePrograms(); }

void AppController::tickAutomation()
{
    auto& a = automation;
    syncMomentary (a, processor, settings);
    if (a.fx != nullptr) a.fx->collectGarbage();
    // release like push-to-talk: RegisterHotKey only reports the press
    for (int i = 0; i < kMomentarySlots; ++i)
    {
        const auto si = size_t (i);
        if (a.held[si] && ! uiHold[si] && (a.heldKey[si] == 0 || ! Hotkeys::isKeyDown (a.heldKey[si]))) setMomentaryHeld (i, false);
    }

    if (! settings.appSwitchOn)
    {
        forgetSwitch (a);
        a.switchTicks = 0;
        return;
    }
    if (++a.switchTicks < 30) return; // once a second
    a.switchTicks = 0;
    const auto exe = getForegroundProgram();
    if (exe.isEmpty() || foreground::isSelf (exe)) return;

    const AppSwitchRule* rule = nullptr;
    for (auto& r : settings.appSwitchRules)
        if (r.exe.equalsIgnoreCase (exe)) { rule = &r; break; }
    const juce::String key = rule != nullptr ? rule->exe.toLowerCase() + "|" + rule->presetId : juce::String();
    if (key == a.appliedRule) return; // acts when the matching rule changes, so a preset picked by hand stays

    if (rule == nullptr)
    {
        // back to the preset from before the switch, unless the user changed or edited it meanwhile
        if (! a.switchedTo.empty() && settings.appSwitchRestore && currentBaseId == a.switchedTo && ! modified && ! hasLooperRecording())
            if (const auto* back = library->find (a.restoreId))
            {
                const auto name = back->name;
                loadPreset (a.restoreId);
                toast (u8 ("元のプリセットに戻しました（") + name + u8 ("）"));
            }
        forgetSwitch (a);
        return;
    }

    const auto target = rule->presetId.toStdString();
    const auto* p = library->find (target);
    if (p == nullptr || target == currentBaseId)
    {
        a.appliedRule = key; // a deleted preset, or already in use
        return;
    }
    juce::String why;
    if (modified) why = u8 ("作業中のプリセットに変更があるため、「") + exe + u8 ("」のプリセットに切り替えませんでした。");
    else if (hasLooperRecording()) why = u8 ("ルーパーに録音があるため、プリセットを自動で切り替えませんでした。"); // E-27
    if (why.isNotEmpty())
    {
        if (a.blockedRule != key) toast (why); // once; retried every second
        a.blockedRule = key;
        return;
    }
    const auto name = p->name;
    if (a.switchedTo.empty() || currentBaseId != a.switchedTo) a.restoreId = currentBaseId;
    loadPreset (target);
    a.switchedTo = target;
    a.appliedRule = key;
    a.blockedRule = {};
    toast (exe + u8 (" → ") + name);
}

void AppController::shutdownAutomation()
{
    auto& a = automation;
    processor.setPostProcessor (nullptr); // fx itself lives until the controller is gone (after the devices close)
    a.attached = false;
    for (int i = 0; i < kMomentarySlots; ++i)
    {
        a.held[size_t (i)] = false;
        uiHold[size_t (i)] = false;
        if (a.fx != nullptr) a.fx->setHeld (i, false);
    }
}
} // namespace koe

#pragma once

#include "Effects/IEffect.h"

#include <string_view>
#include <vector>

namespace koe
{
/** Every effect type: koeloom_effects.md §2 order, then the later waves' types appended. */
const std::vector<EffectInfo>& allEffectInfos();

/** nullptr if the type is not in koeloom_effects.md (an "unknown type", E-22). */
const EffectInfo* findEffectInfo (std::string_view type);

/** nullptr if the type is unknown or no implementation registered a factory. */
std::unique_ptr<IEffect> createEffect (std::string_view type);
bool hasEffectFactory (std::string_view type);
/** F-04-3 (+ wave 10's tapestop, INTERFACES.md §12): types a chain holds at most once. */
inline bool isOnePerChain (std::string_view type) { return type == "freeze" || type == "looper" || type == "tapestop"; }

const char* categoryNameJa (EffectCategory c);
const char* weightNameJa (EffectWeight w); // 軽 / 中 / 重
} // namespace koe

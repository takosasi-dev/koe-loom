#pragma once

#include "Effects/IEffect.h"

#include <string_view>
#include <vector>

namespace koe
{
/** All 30 effect types in koeloom_effects.md §2 order. */
const std::vector<EffectInfo>& allEffectInfos();

/** nullptr if the type is not in koeloom_effects.md (an "unknown type", E-22). */
const EffectInfo* findEffectInfo (std::string_view type);

/** nullptr if the type is unknown or no implementation registered a factory. */
std::unique_ptr<IEffect> createEffect (std::string_view type);
bool hasEffectFactory (std::string_view type);

const char* categoryNameJa (EffectCategory c);
const char* weightNameJa (EffectWeight w); // 軽 / 中 / 重
} // namespace koe

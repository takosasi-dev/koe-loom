#pragma once

#include <juce_core/juce_core.h>

#include <vector>

namespace koe::test
{
/**
    AC-14 standard checks for one effect type, standalone (outside the chain):
    input = reference speech (or synthVoice) 10 s + silence 5 s + white noise -20 dBFS 5 s,
    repeated 1x (or 3x = 60 s when %KOELOOM_FULL_TESTS% is set).
    1. defaults, every numeric param at min and at max (others default), every choice value:
       output is always finite; after the Limiter the peak is <= -1 dBFS.
    2. each numeric param swept min -> max over 50 ms (setParam once per 480-sample block, like the
       chain does) on steady voice: clickRatio <= 2.
    Returns false (and records failures through t) on any problem.
*/
bool runAc14Checks (juce::UnitTest& t, const char* type);

/** Process `in` through a freshly prepared effect with the given params (spec units, size = params of type,
    empty = defaults). Blocks of 480. Returns the output. */
std::vector<float> renderEffect (const char* type, const std::vector<float>& in, const std::vector<float>& params = {});
} // namespace koe::test

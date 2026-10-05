#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace koe::ui
{
/** Stroke icons from the approved mock (24 px viewBox, docs/mockups). */
enum class Icon
{
    logo, mic, power, help, chevronDown, chevronLeft, chevronRight, star, starFilled, check, plus, close,
    headphones, stop, play, playFilled, speaker, folder, warning, search,
    undo, redo // wave10/edit: 元に戻す / やり直し
};

/** Draws the icon fitted into area (keeps aspect). The logo uses `colour` for the first thread and
    `accent` for the second. */
void drawIcon (juce::Graphics& g, Icon icon, juce::Rectangle<float> area, juce::Colour colour,
               float strokeWidth = 2.0f, juce::Colour accent = {});
} // namespace koe::ui

#include "UI/Icons.h"

namespace koe::ui
{
namespace
{
struct Shape { const char* d; bool filled; };

std::vector<Shape> shapesFor (Icon i)
{
    switch (i)
    {
        case Icon::logo:         return { { "M3 7c6 0 9 10 18 10", false }, { "M3 17c6 0 9-10 18-10", false } };
        case Icon::mic:          return { { "M12 2a3 3 0 0 0-3 3v7a3 3 0 0 0 6 0V5a3 3 0 0 0-3-3z", false }, { "M19 10v2a7 7 0 0 1-14 0v-2", false }, { "M12 19v3", false } };
        case Icon::power:        return { { "M12 2v10", false }, { "M18.4 6.6a9 9 0 1 1-12.8 0", false } };
        case Icon::help:         return { { "M22 12a10 10 0 1 1-20 0a10 10 0 1 1 20 0z", false }, { "M9.09 9a3 3 0 0 1 5.83 1c0 2-3 3-3 3", false }, { "M12 17h.01", false } };
        case Icon::chevronDown:  return { { "M6 9l6 6 6-6", false } };
        case Icon::chevronLeft:  return { { "M15 18l-6-6 6-6", false } };
        case Icon::chevronRight: return { { "M9 18l6-6-6-6", false } };
        case Icon::star:         return { { "M12 2l3.09 6.26L22 9.27l-5 4.87 1.18 6.88L12 17.77l-6.18 3.25L7 14.14 2 9.27l6.91-1.01L12 2z", false } };
        case Icon::starFilled:   return { { "M12 2l3.09 6.26L22 9.27l-5 4.87 1.18 6.88L12 17.77l-6.18 3.25L7 14.14 2 9.27l6.91-1.01L12 2z", true } };
        case Icon::check:        return { { "M20 6L9 17l-5-5", false } };
        case Icon::plus:         return { { "M12 5v14", false }, { "M5 12h14", false } };
        case Icon::close:        return { { "M18 6L6 18", false }, { "M6 6l12 12", false } };
        case Icon::headphones:   return { { "M3 18v-6a9 9 0 0 1 18 0v6", false }, { "M21 19a2 2 0 0 1-2 2h-1a2 2 0 0 1-2-2v-3a2 2 0 0 1 2-2h3z", false }, { "M3 19a2 2 0 0 0 2 2h1a2 2 0 0 0 2-2v-3a2 2 0 0 0-2-2H3z", false } };
        case Icon::stop:         return { { "M6 5h12a1 1 0 0 1 1 1v12a1 1 0 0 1-1 1H6a1 1 0 0 1-1-1V6a1 1 0 0 1 1-1z", false } };
        case Icon::play:         return { { "M5 3l14 9-14 9V3z", false } };
        case Icon::playFilled:   return { { "M5 3l14 9-14 9V3z", true } };
        case Icon::speaker:      return { { "M11 5L6 9H2v6h4l5 4V5z", false }, { "M15.54 8.46a5 5 0 0 1 0 7.07", false }, { "M19.07 4.93a10 10 0 0 1 0 14.14", false } };
        case Icon::folder:       return { { "M22 19a2 2 0 0 1-2 2H4a2 2 0 0 1-2-2V5a2 2 0 0 1 2-2h5l2 3h9a2 2 0 0 1 2 2z", false } };
        case Icon::warning:      return { { "M10.29 3.86L1.82 18a2 2 0 0 0 1.71 3h16.94a2 2 0 0 0 1.71-3L13.71 3.86a2 2 0 0 0-3.42 0z", false }, { "M12 9v4", false }, { "M12 17h.01", false } };
        case Icon::search:       return { { "M19 11a8 8 0 1 1-16 0a8 8 0 1 1 16 0z", false }, { "M21 21l-4.35-4.35", false } };
        case Icon::undo:         return { { "M9 14L4 9l5-5", false }, { "M4 9h10.5a5.5 5.5 0 0 1 0 11H11", false } };
        case Icon::redo:         return { { "M15 14l5-5-5-5", false }, { "M20 9H9.5a5.5 5.5 0 0 0 0 11H13", false } };
    }
    return {};
}
} // namespace

void drawIcon (juce::Graphics& g, Icon icon, juce::Rectangle<float> area, juce::Colour colour, float strokeWidth, juce::Colour accent)
{
    const float side = std::min (area.getWidth(), area.getHeight());
    const auto box = area.withSizeKeepingCentre (side, side);
    const auto tf = juce::AffineTransform::scale (side / 24.0f).translated (box.getX(), box.getY());
    const juce::PathStrokeType stroke (strokeWidth * side / 24.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded);
    int index = 0;
    for (auto& s : shapesFor (icon))
    {
        auto p = juce::Drawable::parseSVGPath (s.d);
        p.applyTransform (tf);
        g.setColour (icon == Icon::logo && index == 1 && ! accent.isTransparent() ? accent : colour);
        if (s.filled) g.fillPath (p);
        else g.strokePath (p, stroke);
        ++index;
    }
}
} // namespace koe::ui

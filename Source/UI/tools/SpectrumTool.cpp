// 声の見える化 tool (INTERFACES.md §12.3). Owner: wave10/viz.
// A big SpectrumView (input and output spectra) under a short explanation. The view polls only while this tool is the
// one shown (its timer follows this component's visibility).

#include "UI/main/ToolsView.h"

#include "UI/SpectrumView.h"
#include "UI/Theme.h"
#include "UI/Widgets.h"

namespace koe::ui
{
namespace
{
constexpr int kDescriptionH = Theme::space5 + Theme::space2; // two lines of fontS

class SpectrumTool : public juce::Component
{
public:
    explicit SpectrumTool (AppController& c) : view (c, false)
    {
        view.setComponentID ("spectrum.view");
        addChildComponent (view); // shown (and polling) with the tool
    }

    void visibilityChanged() override { view.setVisible (isVisible()); }

    void resized() override
    {
        auto r = getLocalBounds();
        r.removeFromTop (Theme::space5 + Theme::space2 + kDescriptionH + Theme::space2);
        view.setBounds (r);
    }

    void paint (juce::Graphics& g) override
    {
        const auto& p = Theme::colours();
        auto r = getLocalBounds();
        auto title = r.removeFromTop (Theme::space5);
        g.setColour (p.text);
        g.setFont (Theme::ui (Theme::fontL, true));
        g.drawText (ja ("声の見える化"), title, juce::Justification::centredLeft);
        g.setColour (p.textSub);
        g.setFont (Theme::ui (Theme::fontXS));
        g.drawText (ja ("このページを開いているあいだだけ測ります"), title, juce::Justification::centredRight);
        r.removeFromTop (Theme::space2);
        g.setFont (Theme::ui (Theme::fontS));
        g.drawFittedText (ja ("エフェクトを足したり、つまみを回したりしながら見ると、声のどこが変わったかが分かります。"
                              "左が低い音、右が高い音です。"),
                          r.removeFromTop (kDescriptionH), juce::Justification::topLeft, 2, 1.0f);
    }

private:
    SpectrumView view;
};
} // namespace

std::unique_ptr<juce::Component> makeSpectrumTool (AppController& c, Navigator&) { return std::make_unique<SpectrumTool> (c); }
} // namespace koe::ui

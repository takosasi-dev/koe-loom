#include "UI/main/ToolsView.h"

#include "UI/Theme.h"
#include "UI/Widgets.h"

#include <array>

namespace koe::ui
{
namespace
{
struct ToolInfo { const char* id; const char* name; const char* desc; };
constexpr std::array<ToolInfo, ToolsView::numTools> kTools { {
    { "take", "試し録り", "数秒録って、加工した声をくり返し聞く" },
    { "record", "録音", "加工した声を WAV に保存する" },
    { "pitch", "声の高さ", "いまの声と、加工したあとの高さ" },
    { "morph", "混ぜる", "2 つのプリセットの間を行き来する" },
    { "calibrate", "音量合わせ", "自分の声でプリセットの音量をそろえる" },
    { "miceq", "マイク補正", "マイクのこもりや刺さる高音を、自分の声で測って整える" },
    { "spectrum", "見える化", "加工の前と後の声の周波数を重ねて見る" },
} };
} // namespace

struct ToolsView::Impl
{
    Impl (ToolsView& o, AppController& c, Navigator& nav) : owner (o)
    {
        auto make = [&c, &nav] (int i) -> std::unique_ptr<juce::Component>
        {
            switch (i)
            {
                case 0: return makeTakeTool (c, nav);
                case 1: return makeRecordTool (c, nav);
                case 2: return makePitchTool (c, nav);
                case 3: return makeMorphTool (c, nav);
                case 4: return makeCalibrateTool (c, nav);
                case 5: return makeMicEqTool (c, nav);
                default: return makeSpectrumTool (c, nav);
            }
        };
        for (int i = 0; i < numTools; ++i)
        {
            auto& t = kTools[size_t (i)];
            tabs[size_t (i)] = std::make_unique<PillButton> (ja (t.name), PillButton::Style::tab);
            auto& b = *tabs[size_t (i)];
            b.setRadioGroupId (2);
            b.setClickingTogglesState (false);
            b.setComponentID ("tools.tab." + juce::String (t.id));
            b.setTooltip (ja (t.desc));
            b.onClick = [this, i] { show (Tool (i)); };
            owner.addAndMakeVisible (b);
            pages[size_t (i)] = make (i);
            pages[size_t (i)]->setComponentID ("tools." + juce::String (t.id));
            owner.addChildComponent (*pages[size_t (i)]);
        }
        show (Tool::take);
    }

    void show (Tool t)
    {
        current = t;
        for (int i = 0; i < numTools; ++i)
        {
            tabs[size_t (i)]->setToggleState (i == int (t), juce::dontSendNotification);
            pages[size_t (i)]->setVisible (i == int (t));
        }
    }

    ToolsView& owner;
    std::array<std::unique_ptr<PillButton>, numTools> tabs;
    std::array<std::unique_ptr<juce::Component>, numTools> pages;
    Tool current = Tool::take;
    juce::Rectangle<int> card;
};

ToolsView::ToolsView (AppController& c, Navigator& nav) : impl (std::make_unique<Impl> (*this, c, nav))
{
    setComponentID ("page.tools");
}

ToolsView::~ToolsView() = default;

void ToolsView::showTool (Tool t) { impl->show (t); }
ToolsView::Tool ToolsView::currentTool() const { return impl->current; }

void ToolsView::paint (juce::Graphics& g)
{
    const auto& p = Theme::colours();
    g.setColour (p.surface);
    g.fillRoundedRectangle (impl->card.toFloat(), Theme::radiusL);
    g.setColour (p.border);
    g.drawRoundedRectangle (impl->card.toFloat().reduced (0.5f), Theme::radiusL, 1.0f);
}

void ToolsView::resized()
{
    auto r = getLocalBounds();
    const bool narrow = getWidth() < Theme::narrowWidth;
    auto list = r.removeFromLeft (narrow ? Theme::slotWNarrow - Theme::space5 : Theme::slotW);
    r.removeFromLeft (narrow ? Theme::space2 : Theme::space3);
    const int h = narrow ? Theme::touchMin + Theme::space1 : Theme::buttonH + Theme::space1;
    for (auto& b : impl->tabs)
    {
        b->setBounds (list.removeFromTop (h));
        list.removeFromTop (Theme::space1);
    }
    impl->card = r;
    for (auto& page : impl->pages) page->setBounds (r.reduced (narrow ? Theme::space3 : Theme::space4));
}
} // namespace koe::ui

#pragma once

// ツール page (INTERFACES.md §10.2): the tab next to サウンドボード (案 C: the sidebar button). A list of tools on the
// left, the chosen tool on the right. The page itself is lead's; each tool is made by its owner's factory below.

#include "App/AppController.h"
#include "UI/Navigator.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <memory>

namespace koe::ui
{
class ToolsView : public juce::Component
{
public:
    enum class Tool { take, record, pitch, morph, calibrate, micEq, spectrum };
    static constexpr int numTools = 7;

    ToolsView (AppController& c, Navigator& nav);
    ~ToolsView() override;
    void showTool (Tool t);
    Tool currentTool() const;
    void paint (juce::Graphics& g) override;
    void resized() override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

// One component per tool, created when the page is built (and rebuilt with it on a theme / layout change). Each runs its
// own timer for live values and should stop it while not showing (visibilityChanged). Component IDs: "tools.<id>".
std::unique_ptr<juce::Component> makeTakeTool (AppController&, Navigator&);      // 試し録り   UI/tools/TakeTool.cpp       wave8/capture
std::unique_ptr<juce::Component> makeRecordTool (AppController&, Navigator&);    // 録音       UI/tools/RecordTool.cpp     wave8/capture
std::unique_ptr<juce::Component> makePitchTool (AppController&, Navigator&);     // 声の高さ   UI/tools/PitchTool.cpp      wave8/analysis
std::unique_ptr<juce::Component> makeMorphTool (AppController&, Navigator&);     // 混ぜる     UI/tools/MorphTool.cpp      wave8/morph
std::unique_ptr<juce::Component> makeCalibrateTool (AppController&, Navigator&); // 音量合わせ UI/tools/CalibrateTool.cpp  wave8/analysis
std::unique_ptr<juce::Component> makeMicEqTool (AppController&, Navigator&);     // マイク補正 UI/tools/MicEqTool.cpp      wave9/voice
std::unique_ptr<juce::Component> makeSpectrumTool (AppController&, Navigator&);  // 見える化   UI/tools/SpectrumTool.cpp   wave10/viz
} // namespace koe::ui

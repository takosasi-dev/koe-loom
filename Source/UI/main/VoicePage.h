#pragma once

// The S-01 ボイス page in one of the three looks (S-03 外観 「画面の配置」, Settings::layoutStyle, INTERFACES.md §8):
// 0 = 案 A Studio (VoiceView, the approved mock), 1 = 案 B Paper, 2 = 案 C Mono (docs/mockups/B-S01 / C-S01).
// MainComponent only talks to this interface; the tour finds its targets by component ID (§8.2), so every
// look carries the same IDs.

#include "App/AppController.h"
#include "UI/Navigator.h"

#include <juce_gui_basics/juce_gui_basics.h>

namespace koe::ui::mainui
{
class VoicePage : public juce::Component
{
public:
    /** §8.2.1 layout (MainComponent passes width < Theme::narrowWidth). */
    virtual void setCompact (bool compact) = 0;
    virtual bool isCompact() const = 0;
    /** Model changed (AppController change message). */
    virtual void refresh() = 0;
    /** 30 fps: meters and status (F-08-1). */
    virtual void tick() = 0;
    /** True when the page draws its own ボイチェン ON / マイクミュート / navigation (案 C Mono): MainComponent then
        hides the header bar on this page and gives the page the whole window below the banners (§8.2). */
    virtual bool ownsHeader() const { return false; }
};

std::unique_ptr<VoicePage> makeVoicePage (int layoutStyle, AppController& c, Navigator& nav);
std::unique_ptr<VoicePage> makePaperVoicePage (AppController& c, Navigator& nav); // UI/skins/PaperVoicePage.cpp, owner wave5/paper
std::unique_ptr<VoicePage> makeMonoVoicePage (AppController& c, Navigator& nav);  // UI/skins/MonoVoicePage.cpp, owner wave5/mono
} // namespace koe::ui::mainui

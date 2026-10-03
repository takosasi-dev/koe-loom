#pragma once

// Root component of the main window, owned by the ui-main worker. Contract used by Main.cpp:
// the constructor, Navigator, and renderSnapshots().

#include "App/AppController.h"
#include "UI/Navigator.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <memory>

namespace koe::ui
{
class MainComponent : public juce::Component, public Navigator
{
public:
    explicit MainComponent (AppController& controller);
    ~MainComponent() override;

    void paint (juce::Graphics& g) override;
    void resized() override;

    // Navigator
    void showPage (Page page) override;
    void showSettings (SettingsSection section) override;
    void showPresetBrowser() override;
    void showEffectPicker (int insertAt = -1) override;
    void showSlotDetail (int slot) override;
    void showSetupWizard() override;
    void showHelp (HelpTopic topic) override;
    void startTour (bool resumeFromSaved) override;
    void showOverlay (std::unique_ptr<juce::Component> panel) override;
    void closeOverlay() override;
    void showToast (const juce::String& text) override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

/** KoeLoom.exe --snapshot <dir>: renders every screen offscreen (no window, no audio) at 1120x720 and
    800x560, dark and light, to PNG files for review. Returns the number of files written. */
int renderSnapshots (const juce::File& outputDir);
} // namespace koe::ui

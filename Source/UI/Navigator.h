#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <memory>

namespace koe::ui
{
/** Navigation between screens, implemented by MainComponent (ui-main). Screens receive a reference. */
class Navigator
{
public:
    virtual ~Navigator() = default;

    enum class Page { voice, soundboard, settings, tools }; // tools: INTERFACES.md §10.2
    enum class SettingsSection { devices, environment, hotkeys, startup, appearance, advanced, diagnostics };
    enum class HelpTopic { discordSetup, revertMic, licenses };

    virtual void showPage (Page page) = 0;
    /** Opens S-03 at a section (e.g. "設定で変更" next to the output name -> devices, F-01-11). */
    virtual void showSettings (SettingsSection section) = 0;
    virtual void showPresetBrowser() = 0;                    // S-06 as an overlay
    virtual void showEffectPicker (int insertAt = -1) = 0;   // S-07 as an overlay (-1 = append)
    virtual void showSlotDetail (int slot) = 0;              // S-09 (one at a time, Esc closes)
    virtual void showSetupWizard() = 0;                      // S-04
    virtual void showHelp (HelpTopic topic) = 0;             // [?] menu pages
    virtual void startTour (bool resumeFromSaved) = 0;       // S-08
    /** Shows any component as a centred overlay panel (one at a time); Esc / closeOverlay() closes it. */
    virtual void showOverlay (std::unique_ptr<juce::Component> panel) = 0;
    virtual void closeOverlay() = 0;
    /** Short message at the bottom of the window. */
    virtual void showToast (const juce::String& text) = 0;
};
} // namespace koe::ui

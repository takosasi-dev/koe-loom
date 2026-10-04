#pragma once

// 画面の端に今の声を表示 (INTERFACES.md §11). Owner: wave9/overlay.
// The window (top-most, click-through, non-activating, not on the taskbar) lives in Overlay.cpp; the decision when to show it
// (OverlayTrigger), where (overlayBounds) and what it draws (VoiceOverlayView) are apart from it, so tests and snapshots never
// make a window.

#include "App/AppController.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <memory>
#include <optional>

namespace koe::ui
{
/** The app's on-screen display: a small top-most, click-through, non-activating window in a corner of the screen that shows
    the preset name for Settings::overlaySeconds when it changes (Settings::overlayOn, default OFF). Main.cpp keeps it for the
    app's lifetime (never in tests or snapshots); it watches the controller by itself. May return nullptr. */
std::unique_ptr<juce::Component> makeVoiceOverlay (AppController& c);

/** S-03 「試しに表示」: shows the overlay made by makeVoiceOverlay now, with the current voice, whatever overlayOn and the
    foreground window say. Does nothing when there is none (tests, snapshots). */
void previewVoiceOverlay();

/** What the overlay tells: the working preset's name, ボイチェン ON/OFF and the mic mute. */
struct OverlayState
{
    juce::String name;
    bool voiceOn = true, muted = false;
    bool operator== (const OverlayState&) const = default;
};

OverlayState overlayStateOf (const AppController& c);

/** The small line under the name: 「ボイチェン ON」 / 「ボイチェン OFF」 / 「ミュート中」 (mute wins: nothing reaches the mic). */
juce::String overlayStatusText (const OverlayState& s);

/** When to show (§11.3): only while overlayOn, only when the name / ON-OFF / mute changed since the last call, never while
    starting up and never while KoeLoom's own window is in front. Every call remembers `now`, so a change seen while it
    could not show (OFF, starting up, KoeLoom in front) never pops up later. */
class OverlayTrigger
{
public:
    bool update (const OverlayState& now, bool overlayOn, bool startingUp, bool appInFront);

private:
    std::optional<OverlayState> last;
};

/** The window's place: width x height in the work area's corner (0 top right, 1 bottom right, 2 bottom left, 3 top left;
    others = 0), Theme::space4 from the edges. */
juce::Rectangle<int> overlayBounds (juce::Rectangle<int> workArea, int width, int height, int corner);

/** What the window draws (also drawn alone by --snapshot S11-overlay): rounded, half-see-through surface, the preset name
    (elided when long) and the status line, in Theme::colours(). Not opaque. */
class VoiceOverlayView : public juce::Component
{
public:
    VoiceOverlayView();
    void setState (const OverlayState& s);
    const OverlayState& getState() const noexcept { return state; }
    int preferredWidth() const;
    static int preferredHeight();
    static constexpr int maxWidth = 360, minWidth = 180;
    void paint (juce::Graphics& g) override;

private:
    OverlayState state;
};
} // namespace koe::ui

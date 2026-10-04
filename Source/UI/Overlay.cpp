// 画面の端に今の声を表示 (INTERFACES.md §11.3). Owner: wave9/overlay.

#include "UI/Overlay.h"

#include "UI/Theme.h"
#include "UI/main/Common.h"

#ifndef NOMINMAX
 #define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
 #define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace koe::ui
{
namespace
{
constexpr int kPadX = Theme::space3, kPadY = Theme::space2 + Theme::space1, kBar = Theme::space1;
constexpr int kNameH = 28, kStatusH = 18;
constexpr int kFadeMs = Theme::motionSlow; // §11.3: gone ~300 ms after overlaySeconds
constexpr juce::uint32 kStartupMs = 2000;  // ponytail: fixed grace after launch; key it to "startup finished" if 2 s ever misses

juce::Font nameFont() { return Theme::ui (Theme::fontL, true); }
juce::Font statusFont() { return Theme::ui (Theme::fontXS); }
} // namespace

// =============================================================================================== decision
OverlayState overlayStateOf (const AppController& c)
{
    return { c.getCurrentPreset().name, c.isVoiceChangerOn(), c.isMicMuted() };
}

juce::String overlayStatusText (const OverlayState& s)
{
    if (s.muted) return juce::String::fromUTF8 ("ミュート中");
    return juce::String::fromUTF8 (s.voiceOn ? "ボイチェン ON" : "ボイチェン OFF");
}

bool OverlayTrigger::update (const OverlayState& now, bool overlayOn, bool startingUp, bool appInFront)
{
    const bool changed = last.has_value() && *last != now;
    last = now;
    return changed && overlayOn && ! startingUp && ! appInFront;
}

juce::Rectangle<int> overlayBounds (juce::Rectangle<int> area, int width, int height, int corner)
{
    area = area.reduced (Theme::space4);
    const bool right = corner != 2 && corner != 3;
    const bool bottom = corner == 1 || corner == 2;
    return { right ? area.getRight() - width : area.getX(), bottom ? area.getBottom() - height : area.getY(), width, height };
}

// =============================================================================================== view
VoiceOverlayView::VoiceOverlayView()
{
    setOpaque (false);
    setInterceptsMouseClicks (false, false);
    setSize (minWidth, preferredHeight());
}

void VoiceOverlayView::setState (const OverlayState& s)
{
    state = s;
    repaint();
}

int VoiceOverlayView::preferredHeight() { return kPadY * 2 + kNameH + kStatusH; }

int VoiceOverlayView::preferredWidth() const
{
    const float text = juce::jmax (juce::GlyphArrangement::getStringWidth (nameFont(), state.name),
                                   juce::GlyphArrangement::getStringWidth (statusFont(), overlayStatusText (state)));
    return juce::jlimit (minWidth, maxWidth, int (std::ceil (text)) + kBar + kPadX * 2 + Theme::space1);
}

void VoiceOverlayView::paint (juce::Graphics& g)
{
    const auto& p = Theme::colours();
    auto r = getLocalBounds().toFloat().reduced (0.5f);
    g.setColour (p.surface.withAlpha (0.92f)); // half see-through over the game
    g.fillRoundedRectangle (r, Theme::radiusL);
    g.setColour (p.border);
    g.drawRoundedRectangle (r, Theme::radiusL, 1.0f);

    auto area = getLocalBounds().reduced (kPadX, kPadY);
    // the bar: accent while the voice is changed, sub tone while OFF, danger while muted
    g.setColour (state.muted ? p.danger : state.voiceOn ? p.accent : p.textSub);
    g.fillRoundedRectangle (area.removeFromLeft (kBar).toFloat(), kBar * 0.5f);
    area.removeFromLeft (Theme::space2 + Theme::space1);

    g.setColour (p.text);
    g.setFont (nameFont());
    g.drawText (state.name, area.removeFromTop (kNameH), juce::Justification::centredLeft, true); // … when too long
    g.setColour (state.muted ? p.danger : p.textSub);
    g.setFont (statusFont());
    g.drawText (overlayStatusText (state), area.removeFromTop (kStatusH), juce::Justification::centredLeft, true);
}

// =============================================================================================== window
namespace
{
class VoiceOverlay;
VoiceOverlay* liveOverlay = nullptr; // the one Main.cpp keeps, for 「試しに表示」

/** The desktop window: a transparent holder of one VoiceOverlayView. */
class VoiceOverlay final : public juce::Component, private juce::ChangeListener, private juce::Timer
{
public:
    explicit VoiceOverlay (AppController& ctl) : c (ctl), createdAt (juce::Time::getMillisecondCounter())
    {
        setOpaque (false);
        setInterceptsMouseClicks (false, false);
        addAndMakeVisible (view);
        setVisible (false);
        trigger.update (overlayStateOf (c), false, true, true); // the state at launch is the baseline
        c.addChangeListener (this);
        liveOverlay = this;
    }

    ~VoiceOverlay() override
    {
        if (liveOverlay == this) liveOverlay = nullptr;
        c.removeChangeListener (this);
        stopTimer();
    }

    void show()
    {
        const auto& s = c.getSettings();
        view.setState (overlayStateOf (c));
        const int w = view.preferredWidth(), h = VoiceOverlayView::preferredHeight();
        setBounds (overlayBounds (workAreaOfForegroundWindow(), w, h, s.overlayCorner));
        view.setBounds (getLocalBounds());
        if (! isOnDesktop()) createWindow();
        setAlpha (1.0f);
        setVisible (true); // the peer shows with SW_SHOWNA: no activation, no focus
        if (auto* hwnd = static_cast<HWND> (getWindowHandle()))
            SetWindowPos (hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
        repaint();
        hideAt = juce::Time::getMillisecondCounter() + juce::uint32 (juce::jlimit (kOverlaySeconds.min, kOverlaySeconds.max, s.overlaySeconds) * 1000.0f);
        startTimerHz (30);
    }

private:
    void changeListenerCallback (juce::ChangeBroadcaster*) override
    {
        const bool startingUp = juce::Time::getMillisecondCounter() - createdAt < kStartupMs;
        if (trigger.update (overlayStateOf (c), c.getSettings().overlayOn, startingUp, juce::Process::isForegroundProcess())) show();
    }

    void timerCallback() override
    {
        const auto now = juce::Time::getMillisecondCounter();
        if (now < hideAt) return;
        const float t = float (now - hideAt) / float (kFadeMs);
        if (t >= 1.0f || ! mainui::animate())
        {
            stopTimer();
            setVisible (false);
            return;
        }
        setAlpha (1.0f - t);
    }

    void createWindow()
    {
        setAlwaysOnTop (true);
        addToDesktop (juce::ComponentPeer::windowIsTemporary | juce::ComponentPeer::windowIgnoresMouseClicks);
        // JUCE already makes it a WS_EX_TOOLWINDOW popup (no taskbar) and WS_EX_LAYERED (per-pixel alpha: not opaque).
        // Clicks fall through (WS_EX_TRANSPARENT) and it never takes the focus (WS_EX_NOACTIVATE).
        if (auto* hwnd = static_cast<HWND> (getWindowHandle()))
        {
            const auto ex = GetWindowLongPtrW (hwnd, GWL_EXSTYLE);
            SetWindowLongPtrW (hwnd, GWL_EXSTYLE, ex | WS_EX_TRANSPARENT | WS_EX_LAYERED | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW | WS_EX_TOPMOST);
            SetWindowPos (hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_FRAMECHANGED);
        }
    }

    /** The work area of the screen with the foreground window (a game on the second screen gets it there), else the main one. */
    static juce::Rectangle<int> workAreaOfForegroundWindow()
    {
        const auto& displays = juce::Desktop::getInstance().getDisplays();
        const juce::Displays::Display* d = nullptr;
        if (auto* fg = GetForegroundWindow())
        {
            MONITORINFO mi {};
            mi.cbSize = sizeof (mi);
            if (auto* mon = MonitorFromWindow (fg, MONITOR_DEFAULTTONULL); mon != nullptr && GetMonitorInfoW (mon, &mi))
                d = displays.getDisplayForPoint ({ (mi.rcMonitor.left + mi.rcMonitor.right) / 2, (mi.rcMonitor.top + mi.rcMonitor.bottom) / 2 }, true);
        }
        if (d == nullptr) d = displays.getPrimaryDisplay();
        return d != nullptr ? d->userArea : juce::Rectangle<int> (0, 0, 1280, 720);
    }

    AppController& c;
    VoiceOverlayView view;
    OverlayTrigger trigger;
    const juce::uint32 createdAt;
    juce::uint32 hideAt = 0;
};
} // namespace

std::unique_ptr<juce::Component> makeVoiceOverlay (AppController& c) { return std::make_unique<VoiceOverlay> (c); }

void previewVoiceOverlay()
{
    if (liveOverlay != nullptr) liveOverlay->show();
}
} // namespace koe::ui

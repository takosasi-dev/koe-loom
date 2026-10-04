#pragma once

// Window chrome around the pages: header (tabs, mute, voice on/off, [?]), warning banners (§8.3),
// toasts and the overlay host (one panel at a time, Esc closes).

#include "UI/main/Common.h"

namespace koe::ui::mainui
{
class HeaderBar : public juce::Component
{
public:
    HeaderBar (AppController& c, Navigator& nav);
    void setCompact (bool compact);
    void setPage (Navigator::Page page);
    void refresh();
    void paint (juce::Graphics& g) override;
    void resized() override;

private:
    void showHelpMenu();
    AppController& c;
    Navigator& nav;
    bool compact = false;
    PillButton tabVoice, tabSound, tabTools, tabSettings, mute, voice;
    IconButton help;
};

// ---------------------------------------------------------------------------------------------
/** One banner row: level icon, text, optional action button, × (and "+n" on the last visible row). */
class NoticeRow : public juce::Component, public juce::SettableTooltipClient
{
public:
    NoticeRow (AppController& c, Navigator& nav, const Notice& n);
    void paint (juce::Graphics& g) override;
    void resized() override;
    std::unique_ptr<PillButton> more; // "+n", owned here so it sits inside the row
    const Notice notice;

private:
    AppController& c;
    Navigator& nav;
    std::unique_ptr<PillButton> action;
    std::unique_ptr<SquareIconButton> close;
};

/** Up to 2 banners (danger first), 36 px each; the rest are summarised as "+n" (§8.3, F-01-11). */
class NoticeBar : public juce::Component
{
public:
    static constexpr int maxVisible = 2;
    NoticeBar (AppController& c, Navigator& nav);
    void setNotices (const std::vector<Notice>& notices);
    int preferredHeight() const;
    void resized() override;
    std::function<void()> onHeightChanged;

private:
    AppController& c;
    Navigator& nav;
    std::vector<Notice> current;
    std::vector<std::unique_ptr<NoticeRow>> rows;
};

/** "+n": every active notice in a panel. */
class NoticeListPanel : public PanelBase
{
public:
    NoticeListPanel (AppController& c, Navigator& nav);
    void resized() override;

private:
    std::vector<std::unique_ptr<NoticeRow>> rows;
};

// ---------------------------------------------------------------------------------------------
/** Short messages at the bottom of the window, one at a time for a few seconds. */
class ToastView : public juce::Component, private juce::Timer
{
public:
    ToastView();
    void push (const juce::String& text);
    juce::String currentText() const { return queue.isEmpty() ? juce::String() : queue[0]; }
    int preferredWidth() const;
    void paint (juce::Graphics& g) override;
    std::function<void()> onChanged;

private:
    void timerCallback() override;
    juce::StringArray queue;
};

// ---------------------------------------------------------------------------------------------
/** Dims the window and shows one panel centred (or filling the window for full-screen screens). */
class OverlayHost : public juce::Component, private juce::Timer
{
public:
    OverlayHost();
    void show (std::unique_ptr<juce::Component> panel, bool fullScreen);
    /** The panel is kept alive until the next show()/close() so callers may close from its own callbacks. */
    void close();
    juce::Component* panel() const { return current.get(); }
    void paint (juce::Graphics& g) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent& e) override;
    bool keyPressed (const juce::KeyPress& k) override;
    std::function<void()> onClosed;

private:
    void timerCallback() override;
    std::unique_ptr<juce::Component> current, retired;
    bool fullScreen = false;
    double fadeStart = 0.0;
};
} // namespace koe::ui::mainui

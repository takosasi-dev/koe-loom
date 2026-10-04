#include "UI/main/Shell.h"

namespace koe::ui::mainui
{
namespace
{
const Palette& P() { return Theme::colours(); }

juce::Colour levelColour (NoticeLevel l)
{
    switch (l)
    {
        case NoticeLevel::danger: return P().danger;
        case NoticeLevel::warning: return P().warn;
        case NoticeLevel::info: break;
    }
    return P().textSub;
}

void runNoticeAction (AppController& c, Navigator& nav, const juce::String& id)
{
    if (id == "openSettings.devices") nav.showSettings (Navigator::SettingsSection::devices);
    else if (id == "openSettings.advanced") nav.showSettings (Navigator::SettingsSection::advanced);
    else if (id == "openSetup") nav.showSetupWizard();
    else if (id == "update.apply") c.applyUpdateNow();
    else if (id == "update.openReleases") juce::URL (c.getUpdateState().releaseUrl).launchInDefaultBrowser();
    else c.performAction (id); // controller actions, e.g. "wavStop" / "takeStop" (INTERFACES.md §10)
}
} // namespace

// =============================================================================================== header
HeaderBar::HeaderBar (AppController& ctl, Navigator& n)
    : c (ctl), nav (n),
      tabVoice (ja ("ボイス"), PillButton::Style::tab), tabSound (ja ("サウンドボード"), PillButton::Style::tab),
      tabTools (ja ("ツール"), PillButton::Style::tab), tabSettings (ja ("設定"), PillButton::Style::tab), mute (ja ("マイクミュート"), PillButton::Style::outline, Icon::mic),
      voice (ja ("ボイチェン ON"), PillButton::Style::primary, Icon::power), help (ja ("ヘルプ"), Icon::help, true)
{
    for (auto* b : { &tabVoice, &tabSound, &tabTools, &tabSettings })
    {
        b->setRadioGroupId (1);
        b->setClickingTogglesState (false);
        addAndMakeVisible (*b);
    }
    tabVoice.setComponentID ("header.tab.voice");
    tabSound.setComponentID ("header.tab.soundboard");
    tabTools.setComponentID ("header.tab.tools");
    tabTools.setTooltip (ja ("ツール（試し録り・録音・声の高さ・混ぜる・音量合わせ）"));
    tabSettings.setComponentID ("header.tab.settings");
    help.setComponentID ("header.help");
    tabVoice.onClick = [this] { nav.showPage (Navigator::Page::voice); };
    tabSound.onClick = [this] { nav.showPage (Navigator::Page::soundboard); };
    tabTools.onClick = [this] { nav.showPage (Navigator::Page::tools); };
    tabSettings.onClick = [this] { nav.showPage (Navigator::Page::settings); };

    mute.setPill (true);
    mute.setComponentID ("tour.mute");
    mute.setTitle (ja ("マイクミュート"));
    mute.setTooltip (ja ("マイクミュート（相手に声を送らない）"));
    mute.onClick = [this] { c.setMicMuted (! c.isMicMuted()); };
    addAndMakeVisible (mute);

    voice.setPill (true);
    voice.setFontSize (Theme::fontM);
    voice.setComponentID ("tour.voiceToggle");
    voice.setTooltip (ja ("ボイスチェンジャーの ON/OFF（OFF で元の声）"));
    voice.onClick = [this] { c.setVoiceChangerOn (! c.isVoiceChangerOn()); };
    addAndMakeVisible (voice);

    help.setTooltip (ja ("ヘルプ（ガイドツアーなど）"));
    help.onClick = [this] { showHelpMenu(); };
    addAndMakeVisible (help);
    setComponentID ("main.header");
    refresh();
}

void HeaderBar::setCompact (bool c2)
{
    if (compact == c2) return;
    compact = c2;
    refresh();
    resized();
}

void HeaderBar::setPage (Navigator::Page page)
{
    tabVoice.setToggleState (page == Navigator::Page::voice, juce::dontSendNotification);
    tabSound.setToggleState (page == Navigator::Page::soundboard, juce::dontSendNotification);
    tabTools.setToggleState (page == Navigator::Page::tools, juce::dontSendNotification);
    tabSettings.setToggleState (page == Navigator::Page::settings, juce::dontSendNotification);
}

void HeaderBar::refresh()
{
    // mute: OFF = outline, ON = danger red + "MUTE" (never colour alone, F-08-6)
    const bool muted = c.isMicMuted();
    mute.setStyle (muted ? PillButton::Style::danger : PillButton::Style::outline);
    mute.setButtonText (muted ? juce::String ("MUTE") : (compact ? juce::String() : ja ("マイクミュート")));
    mute.setToggleState (muted, juce::dontSendNotification);
    const bool on = c.isVoiceChangerOn();
    voice.setStyle (on ? PillButton::Style::primary : PillButton::Style::outline);
    voice.setButtonText (on ? ja ("ボイチェン ON") : ja ("ボイチェン OFF"));
    voice.setFontSize (compact ? Theme::fontS : Theme::fontM);
    voice.setToggleState (on, juce::dontSendNotification);
    resized();
    repaint();
}

void HeaderBar::resized()
{
    auto r = getLocalBounds();
    const int logo = compact ? Theme::space4 : Theme::space5 - 2;
    r.removeFromLeft (logo + Theme::space2);
    const int titleW = textWidth (Theme::ui (compact ? Theme::fontM : Theme::fontL, true), "KoeLoom");
    r.removeFromLeft (titleW + (compact ? Theme::space2 : Theme::space4));
    const int tabH = compact ? Theme::touchMin : Theme::buttonH;
    for (auto* b : { &tabVoice, &tabSound, &tabTools, &tabSettings })
    {
        const int w = b->preferredWidth() - (compact ? Theme::space2 : 0);
        b->setBounds (r.removeFromLeft (w).withSizeKeepingCentre (w, tabH));
        r.removeFromLeft (compact ? 2 : Theme::space1);
    }
    const int h = compact ? Theme::controlH : Theme::pillH;
    help.setBounds (r.removeFromRight (h).withSizeKeepingCentre (h, h));
    r.removeFromRight (compact ? Theme::space2 : Theme::space2 + Theme::space1);
    const int vw = voice.preferredWidth() + (compact ? 0 : Theme::space1);
    voice.setBounds (r.removeFromRight (vw).withSizeKeepingCentre (vw, h));
    r.removeFromRight (compact ? Theme::space2 : Theme::space2 + Theme::space1);
    const int mw = mute.getButtonText().isEmpty() ? h : mute.preferredWidth();
    mute.setBounds (r.removeFromRight (mw).withSizeKeepingCentre (mw, h));
}

void HeaderBar::paint (juce::Graphics& g)
{
    const auto& p = P();
    const float logo = float (compact ? Theme::space4 : Theme::space5 - 2);
    drawIcon (g, Icon::logo, { 0.0f, (getHeight() - logo) * 0.5f, logo, logo }, p.text, 2.4f, p.accent);
    const int x = int (logo) + Theme::space2;
    drawText (g, "KoeLoom", { x, 0, 120, getHeight() }, compact ? Theme::fontM : Theme::fontL, p.text, juce::Justification::centredLeft, true);
}

void HeaderBar::showHelpMenu()
{
    juce::PopupMenu m;
    m.addItem (1, ja ("ガイドツアー"));
    m.addItem (2, ja ("Discord の設定手順"));
    m.addItem (3, ja ("元のマイクに戻す方法"));
    m.addItem (4, ja ("ライセンス表示"));
    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (help),
                     [safe = juce::Component::SafePointer<HeaderBar> (this)] (int r)
                     {
                         if (safe == nullptr) return;
                         auto& n = safe->nav;
                         if (r == 1) n.startTour (false);
                         else if (r == 2) n.showHelp (Navigator::HelpTopic::discordSetup);
                         else if (r == 3) n.showHelp (Navigator::HelpTopic::revertMic);
                         else if (r == 4) n.showHelp (Navigator::HelpTopic::licenses);
                     });
}

// =============================================================================================== banners
NoticeRow::NoticeRow (AppController& ctl, Navigator& n, const Notice& no) : notice (no), c (ctl), nav (n)
{
    setTooltip (notice.text);
    if (notice.actionId.isNotEmpty())
    {
        action = std::make_unique<PillButton> (notice.actionLabel.isNotEmpty() ? notice.actionLabel : ja ("開く"), PillButton::Style::outline);
        action->setFontSize (Theme::fontXS);
        action->onClick = [this] { runNoticeAction (c, nav, notice.actionId); };
        addAndMakeVisible (*action);
    }
    if (notice.dismissible)
    {
        close = std::make_unique<SquareIconButton> (ja ("この警告を閉じる"), Icon::close);
        close->onClick = [this] { c.dismissNotice (notice.key); };
        addAndMakeVisible (*close);
    }
}

void NoticeRow::resized()
{
    auto r = getLocalBounds().reduced (Theme::space2, Theme::space1);
    const int s = r.getHeight();
    if (close != nullptr) close->setBounds (r.removeFromRight (s));
    if (more != nullptr)
    {
        r.removeFromRight (Theme::space1);
        const int w = more->preferredWidth();
        more->setBounds (r.removeFromRight (w));
    }
    if (action != nullptr)
    {
        r.removeFromRight (Theme::space1);
        action->setBounds (r.removeFromRight (action->preferredWidth()));
    }
}

void NoticeRow::paint (juce::Graphics& g)
{
    const auto& p = P();
    const auto col = levelColour (notice.level);
    auto r = getLocalBounds().toFloat().reduced (0.75f);
    g.setColour (p.surface);
    g.fillRoundedRectangle (r, Theme::radiusM);
    g.setColour (notice.level == NoticeLevel::info ? p.border : col);
    g.drawRoundedRectangle (r, Theme::radiusM, Theme::borderWidth);
    auto inner = getLocalBounds().reduced (Theme::space3, 0);
    drawIcon (g, Icon::warning, inner.removeFromLeft (18).withSizeKeepingCentre (18, 18).toFloat(), col);
    inner.removeFromLeft (Theme::space2);
    int right = getWidth();
    for (auto* b : { static_cast<juce::Component*> (action.get()), static_cast<juce::Component*> (more.get()), static_cast<juce::Component*> (close.get()) })
        if (b != nullptr) right = juce::jmin (right, b->getX());
    inner.setRight (right - Theme::space2);
    g.setColour (p.text);
    g.setFont (Theme::ui (Theme::fontS));
    g.drawFittedText (notice.text, inner, juce::Justification::centredLeft, 1, 0.85f);
}

NoticeBar::NoticeBar (AppController& ctl, Navigator& n) : c (ctl), nav (n)
{
    setComponentID ("main.notices");
}

int NoticeBar::preferredHeight() const
{
    const int n = juce::jmin (maxVisible, int (current.size()));
    return n == 0 ? 0 : n * Theme::bannerH + (n - 1) * Theme::space1;
}

void NoticeBar::setNotices (const std::vector<Notice>& list)
{
    auto same = [] (const Notice& a, const Notice& b)
    { return a.key == b.key && a.text == b.text && a.level == b.level && a.actionId == b.actionId && a.dismissible == b.dismissible; };
    if (list.size() == current.size() && std::equal (list.begin(), list.end(), current.begin(), same)) return;
    const int oldH = preferredHeight();
    current = list;
    rows.clear();
    for (int i = 0; i < juce::jmin (maxVisible, int (current.size())); ++i)
    {
        auto row = std::make_unique<NoticeRow> (c, nav, current[size_t (i)]);
        row->setComponentID ("notice." + juce::String (i));
        addAndMakeVisible (*row);
        rows.push_back (std::move (row));
    }
    if (int (current.size()) > maxVisible)
    {
        auto& last = rows.back();
        last->more = std::make_unique<PillButton> ("+" + juce::String (int (current.size()) - maxVisible), PillButton::Style::outline);
        last->more->setFontSize (Theme::fontXS);
        last->more->setComponentID ("notice.more");
        last->more->setTooltip (ja ("ほかの警告も見る"));
        last->more->onClick = [this] { nav.showOverlay (std::make_unique<NoticeListPanel> (c, nav)); };
        last->addAndMakeVisible (*last->more);
    }
    resized();
    if (oldH != preferredHeight() && onHeightChanged) onHeightChanged();
}

void NoticeBar::resized()
{
    auto r = getLocalBounds();
    for (auto& row : rows)
    {
        row->setBounds (r.removeFromTop (Theme::bannerH));
        r.removeFromTop (Theme::space1);
    }
}

NoticeListPanel::NoticeListPanel (AppController& c, Navigator& n) : PanelBase (n, ja ("警告の一覧"))
{
    for (auto& no : c.getNotices())
    {
        rows.push_back (std::make_unique<NoticeRow> (c, n, no));
        addAndMakeVisible (*rows.back());
    }
    setSize (720, headerHeight + Theme::space4 + int (rows.size()) * (Theme::bannerH + Theme::space2));
}

void NoticeListPanel::resized()
{
    PanelBase::resized();
    auto r = contentArea().reduced (Theme::space4, 0);
    for (auto& row : rows)
    {
        row->setBounds (r.removeFromTop (Theme::bannerH));
        r.removeFromTop (Theme::space2);
    }
}

// =============================================================================================== toast
ToastView::ToastView()
{
    setInterceptsMouseClicks (false, false);
    setVisible (false);
    setComponentID ("main.toast");
}

void ToastView::push (const juce::String& text)
{
    if (text.isEmpty()) return;
    queue.add (text);
    if (queue.size() == 1)
    {
        setVisible (true);
        startTimer (4000);
        if (onChanged) onChanged();
        repaint();
    }
}

int ToastView::preferredWidth() const
{
    return int (std::ceil (juce::GlyphArrangement::getStringWidth (Theme::ui (Theme::fontS), currentText()))) + Theme::space4 * 2;
}

void ToastView::timerCallback()
{
    if (! queue.isEmpty()) queue.remove (0);
    if (queue.isEmpty())
    {
        stopTimer();
        setVisible (false);
    }
    if (onChanged) onChanged();
    repaint();
}

void ToastView::paint (juce::Graphics& g)
{
    const auto& p = P();
    auto r = getLocalBounds().toFloat().reduced (0.75f);
    g.setColour (p.raised);
    g.fillRoundedRectangle (r, Theme::radiusM);
    g.setColour (p.border);
    g.drawRoundedRectangle (r, Theme::radiusM, 1.0f);
    g.setColour (p.text);
    g.setFont (Theme::ui (Theme::fontS));
    g.drawFittedText (currentText(), getLocalBounds().reduced (Theme::space3, Theme::space1), juce::Justification::centred, 2, 0.9f);
}

// =============================================================================================== overlay
OverlayHost::OverlayHost()
{
    setVisible (false);
    setWantsKeyboardFocus (true);
    setComponentID ("main.overlay");
}

void OverlayHost::show (std::unique_ptr<juce::Component> panel, bool full)
{
    juce::Desktop::getInstance().getAnimator().cancelAnimation (this, false); // a closing fade must not hide the new panel
    retired.reset();
    if (current != nullptr)
    {
        removeChildComponent (current.get());
        retired = std::move (current);
    }
    current = std::move (panel);
    fullScreen = full;
    if (current == nullptr) return close();
    if (current->getWidth() <= 0 || current->getHeight() <= 0) current->setSize (880, 600);
    addAndMakeVisible (*current);
    setVisible (true);
    toFront (false);
    resized();
    if (animate())
    {
        setAlpha (0.0f);
        fadeStart = juce::Time::getMillisecondCounterHiRes();
        startTimerHz (60);
    }
    else
        setAlpha (1.0f);
    current->grabKeyboardFocus();
}

void OverlayHost::close()
{
    if (current == nullptr && ! isVisible()) return;
    stopTimer();
    // F-14-6: a snapshot fades out over motionFast (decelerating); the overlay itself is gone at once
    if (animate() && isShowing() && current != nullptr)
        juce::Desktop::getInstance().getAnimator().animateComponent (this, getBounds(), 0.0f, Theme::motionFast, true, 1.0, 0.0);
    retired.reset();
    if (current != nullptr)
    {
        removeChildComponent (current.get());
        retired = std::move (current);
    }
    setVisible (false);
    if (onClosed) onClosed();
}

void OverlayHost::timerCallback()
{
    const float t = float ((juce::Time::getMillisecondCounterHiRes() - fadeStart) / Theme::motionMid);
    setAlpha (Theme::ease (juce::jlimit (0.0f, 1.0f, t)));
    if (t >= 1.0f) stopTimer();
}

void OverlayHost::resized()
{
    if (current == nullptr) return;
    if (fullScreen)
    {
        current->setBounds (getLocalBounds());
        return;
    }
    const auto area = getLocalBounds().reduced (Theme::space3);
    const int w = juce::jmin (current->getWidth(), area.getWidth());
    const int h = juce::jmin (current->getHeight(), area.getHeight());
    current->setBounds (area.withSizeKeepingCentre (w, h));
}

void OverlayHost::paint (juce::Graphics& g)
{
    g.fillAll (fullScreen ? P().bg : P().overlay);
}

void OverlayHost::mouseDown (const juce::MouseEvent& e)
{
    if (current != nullptr && ! current->getBounds().contains (e.getPosition())) close();
}

bool OverlayHost::keyPressed (const juce::KeyPress& k)
{
    if (k == juce::KeyPress::escapeKey)
    {
        close();
        return true;
    }
    return false;
}
} // namespace koe::ui::mainui

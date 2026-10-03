#include "UI/main/GuideTour.h"

namespace koe::ui::mainui
{
namespace
{
const Palette& P() { return Theme::colours(); }
using Page = Navigator::Page;
using Section = Navigator::SettingsSection;

constexpr int kBubbleW = Theme::space5 * 13 + Theme::space4; // 440 (A-S08)
constexpr int kBubbleH = Theme::space5 * 6 + Theme::space2;  // 200
constexpr float kArrow = 10.0f;

juce::Component* findVisible (juce::Component* root, juce::Component* c, const juce::String& id)
{
    if (c == nullptr || (c != root && ! c->isVisible())) return nullptr;
    if (c->getComponentID() == id && visibleWithin (c, root)) return c;
    for (auto* child : c->getChildren())
        if (auto* f = findVisible (root, child, id)) return f;
    return nullptr;
}

/** E-32: scroll every enclosing Viewport so that c comes into view. */
void scrollIntoView (juce::Component* c)
{
    for (auto* p = c->getParentComponent(); p != nullptr; p = p->getParentComponent())
        if (auto* vp = dynamic_cast<juce::Viewport*> (p))
            if (auto* viewed = vp->getViewedComponent(); viewed != nullptr && (viewed == c || viewed->isParentOf (c)))
            {
                const auto rel = viewed->getLocalArea (c, c->getLocalBounds());
                const auto area = vp->getViewArea();
                int x = area.getX(), y = area.getY();
                if (rel.getRight() > area.getRight()) x = rel.getRight() - area.getWidth() + Theme::space2;
                if (rel.getX() < x) x = rel.getX() - Theme::space2;
                if (rel.getBottom() > area.getBottom()) y = rel.getBottom() - area.getHeight() + Theme::space2;
                if (rel.getY() < y) y = rel.getY() - Theme::space2;
                vp->setViewPosition (juce::jmax (0, x), juce::jmax (0, y));
            }
}

/** c's bounds in root coordinates, clipped by every parent. */
juce::Rectangle<int> visibleArea (juce::Component& root, juce::Component* c)
{
    auto r = root.getLocalArea (c, c->getLocalBounds());
    for (auto* p = c->getParentComponent(); p != nullptr && p != &root; p = p->getParentComponent())
        r = r.getIntersection (root.getLocalArea (p, p->getLocalBounds()));
    return r.getIntersection (root.getLocalBounds());
}
} // namespace

const std::vector<GuideTour::Step>& GuideTour::steps()
{
    static const std::vector<Step> s {
        { { "tour.voiceToggle", "tour.mute" }, Page::voice, Section::devices, {}, "ボイチェンとミュート",
          "ボイチェンの ON/OFF と、相手に声を送らないマイクミュートはここです。" },
        { { "tour.presets", "tour.monitor" }, Page::voice, Section::devices, {}, "プリセットとモニター",
          "プリセットを選び、モニターで自分の声を確かめます。スピーカーで聞くとハウリングします。" },
        { { "tour.pitch", "tour.formant" }, Page::voice, Section::devices, {}, "ピッチとフォルマント",
          "声の高さ（ピッチ）と声質（フォルマント）を、別々に動かせます。" },
        { { "tour.chain" }, Page::voice, Section::devices, {}, "エフェクトチェーン",
          "エフェクトは左から右へ順に処理します。追加・並べ替え・ON/OFF と、重さ（軽・中・重）の表示もここ。" },
        { { "settings.outputGain" }, Page::settings, Section::environment, { "tour.outputMeter" }, "出力の音量",
          "出力の音量は、入力と同じくらいが基準です。調整は設定の「出力ゲイン」で行います。" },
        { { "settings.hotkeys.hint" }, Page::settings, Section::hotkeys, {}, "ホットキー",
          "ゲーム中でも、キーで切り替えられます。最初はどのキーも割り当てていません。" },
        { { "tour.status" }, Page::voice, Section::devices, {}, "負荷と自動停止",
          "CPU と遅延はここに出ます。負荷が高いと、重ねる声や重いエフェクトが自動停止します。" },
    };
    return s;
}

// =============================================================================================== bubble
class GuideTour::Bubble : public juce::Component
{
public:
    explicit Bubble (GuideTour& t)
        : tour (t), next (ja ("次へ"), PillButton::Style::primary), back (ja ("戻る"), PillButton::Style::outline), skip (ja ("スキップ"), true)
    {
        setComponentID ("tour.bubble");
        setFocusContainerType (juce::Component::FocusContainerType::keyboardFocusContainer);
        next.setComponentID ("tour.next");
        back.setComponentID ("tour.back");
        skip.setComponentID ("tour.skip");
        next.onClick = [this] { tour.next(); };
        back.onClick = [this] { tour.back(); };
        skip.onClick = [this] { tour.skip(); };
        for (auto* b : std::initializer_list<juce::Component*> { &skip, &back, &next }) addAndMakeVisible (b);
        setSize (kBubbleW, kBubbleH);
    }

    void setStep (int i)
    {
        index = i;
        const bool last = i == int (steps().size()) - 1;
        next.setButtonText (last ? ja ("完了") : ja ("次へ"));
        back.setEnabled (i > 0);
        resized();
        repaint();
    }

    juce::String counterText() const { return juce::String (index + 1) + " / " + juce::String (int (steps().size())); }

    void resized() override
    {
        auto r = getLocalBounds().reduced (Theme::space4 - 4, Theme::space3);
        r.removeFromBottom (Theme::space3 + Theme::space1); // footnote
        auto row = r.removeFromBottom (Theme::buttonH);
        next.setBounds (row.removeFromRight (juce::jmax (next.preferredWidth(), Theme::space5 * 2 + Theme::space2)));
        row.removeFromRight (Theme::space2);
        back.setBounds (row.removeFromRight (juce::jmax (back.preferredWidth(), Theme::space5 * 2)));
        row.removeFromRight (Theme::space2);
        skip.setBounds (row.removeFromRight (skip.preferredWidth() + Theme::space3));
        dotsArea = row;
    }

    void paint (juce::Graphics& g) override
    {
        const auto& p = P();
        auto b = getLocalBounds().toFloat().reduced (1.0f);
        g.setColour (p.raised);
        g.fillRoundedRectangle (b, Theme::radiusL);
        g.setColour (p.accent);
        g.drawRoundedRectangle (b, Theme::radiusL, 2.0f);
        auto r = getLocalBounds().reduced (Theme::space4 - 4, Theme::space3);
        auto head = r.removeFromTop (Theme::space4);
        const auto counter = counterText();
        drawText (g, counter, head.removeFromLeft (textWidth (Theme::mono (Theme::fontS, true), counter) + Theme::space2), Theme::fontS, p.accent,
                  juce::Justification::centredLeft, true, true);
        drawText (g, ja (steps()[size_t (index)].title), head, Theme::fontM, p.text, juce::Justification::centredLeft, true);
        r.removeFromTop (Theme::space2);
        auto body = r.removeFromTop (r.getHeight() - Theme::buttonH - Theme::space3 - Theme::space1 - Theme::space2);
        g.setColour (p.text);
        g.setFont (Theme::ui (Theme::fontS));
        auto text = ja (steps()[size_t (index)].text);
        if (tour.c.getSettings().layoutStyle == 2) text = text.replace (ja ("左から右へ"), ja ("上から下へ")); // 案 C's rack runs downwards
        g.drawFittedText (text, body, juce::Justification::topLeft, 3, 1.0f);
        // progress dots: the current one is long
        float x = float (dotsArea.getX());
        const float y = float (dotsArea.getCentreY()) - 4.0f;
        for (int i = 0; i < int (steps().size()); ++i)
        {
            const float w = i == index ? 22.0f : 8.0f;
            if (x + w > float (dotsArea.getRight())) break;
            g.setColour (i == index ? p.accent : p.border);
            g.fillRoundedRectangle (x, y, w, 8.0f, 4.0f);
            x += w + 6.0f;
        }
        drawText (g, ja ("ヘルプからいつでもやり直せます"), getLocalBounds().reduced (Theme::space4 - 4, Theme::space2).removeFromBottom (Theme::space3 + 2),
                  Theme::fontXS, p.textSub);
    }

    GuideTour& tour;
    PillButton next, back;
    LinkButton skip;
    int index = 0;
    juce::Rectangle<int> dotsArea;
};

// =============================================================================================== tour
GuideTour::GuideTour (juce::Component& r, AppController& ctl, Navigator& n, int startStep, std::function<void()> done)
    : root (r), c (ctl), nav (n), onFinished (std::move (done)), bubble (std::make_unique<Bubble> (*this)),
      step (juce::jlimit (0, int (steps().size()) - 1, startStep))
{
    setComponentID ("main.tour");
    setWantsKeyboardFocus (true);
    addAndMakeVisible (*bubble);
    root.addMouseListener (this, true);
}

GuideTour::~GuideTour()
{
    root.removeMouseListener (this);
}

void GuideTour::resized()
{
    if (hole.isEmpty() && holeTo.isEmpty()) show (step, +1);
    else relayout();
}

bool GuideTour::locate (const Step& s, juce::Rectangle<int>& out)
{
    auto tryIds = [this, &out] (const juce::StringArray& ids)
    {
        juce::Rectangle<int> u;
        for (auto& id : ids)
            if (auto* comp = findVisible (&root, &root, id))
            {
                scrollIntoView (comp);
                const auto a = visibleArea (root, comp);
                const auto full = root.getLocalArea (comp, comp->getLocalBounds());
                // "visible" = at least half of the part is on screen (E-32)
                if (a.getWidth() * 2 >= full.getWidth() && a.getHeight() * 2 >= full.getHeight() && ! a.isEmpty())
                    u = u.isEmpty() ? a : u.getUnion (a);
            }
        out = u;
        return ! u.isEmpty();
    };
    if (s.page == Page::settings) nav.showSettings (s.section);
    else nav.showPage (s.page);
    if (tryIds (s.ids)) return true;
    if (! s.fallbackIds.isEmpty())
    {
        nav.showPage (Page::voice);
        return tryIds (s.fallbackIds);
    }
    return false;
}

void GuideTour::show (int index, int direction)
{
    const int n = int (steps().size());
    for (int i = index; i >= 0 && i < n; i += direction)
    {
        juce::Rectangle<int> target;
        if (! locate (steps()[size_t (i)], target))
        {
            juce::Logger::writeToLog ("tour: step " + juce::String (i + 1) + " skipped (part not visible)"); // E-32
            continue;
        }
        step = i;
        c.updateSettings ([i] (Settings& s) { s.tourStep = i; }); // F-13-4
        bubble->setStep (i);
        holeFrom = hole.isEmpty() ? target.toFloat() : hole;
        holeTo = target.expanded (Theme::space1).toFloat();
        placeBubble();
        if (animate())
        {
            animStart = juce::Time::getMillisecondCounterHiRes();
            startTimerHz (60);
        }
        else
            hole = holeTo;
        if (bubble->isShowing()) bubble->next.grabKeyboardFocus();
        repaint();
        return;
    }
    if (direction > 0) finish();
    else if (index != step) show (step, +1); // nothing earlier to show: stay
}

void GuideTour::relayout()
{
    if (finished) return;
    juce::Rectangle<int> target;
    if (! locate (steps()[size_t (step)], target)) return;
    holeTo = target.expanded (Theme::space1).toFloat();
    if (! isTimerRunning()) hole = holeTo;
    placeBubble();
    repaint();
}

void GuideTour::placeBubble()
{
    // bubble below / above / right / left of the part, never over the banners (E-31)
    const auto target = holeTo.toNearestInt();
    juce::Rectangle<int> avoid;
    if (auto* notices = findById (&root, "main.notices"); notices != nullptr && notices->isVisible())
        avoid = root.getLocalArea (notices->getParentComponent(), notices->getBounds());
    const auto area = getLocalBounds().reduced (Theme::space2);
    const int gap = Theme::space3 + int (kArrow);
    const int x = juce::jlimit (area.getX(), juce::jmax (area.getX(), area.getRight() - kBubbleW), target.getX() + Theme::space4);
    const int y = juce::jlimit (area.getY(), juce::jmax (area.getY(), area.getBottom() - kBubbleH), target.getY());
    const int belowBanners = avoid.isEmpty() ? area.getY() : avoid.getBottom() + Theme::space2;
    const juce::Rectangle<int> candidates[] = {
        { x, target.getBottom() + gap, kBubbleW, kBubbleH },
        { x, target.getY() - gap - kBubbleH, kBubbleW, kBubbleH },
        { target.getRight() + gap, y, kBubbleW, kBubbleH },
        { target.getX() - gap - kBubbleW, y, kBubbleW, kBubbleH },
        { x, belowBanners, kBubbleW, kBubbleH },                    // small windows: as clear of the part as possible
        { x, area.getBottom() - kBubbleH, kBubbleW, kBubbleH },
    };
    juce::Rectangle<int> chosen;
    int bestOverlap = std::numeric_limits<int>::max();
    for (auto& cand : candidates)
    {
        const auto r = cand.constrainedWithin (area);
        if (r != cand && &cand < candidates + 4) continue; // the first four must fit as they are
        if (r.intersects (avoid)) continue;
        const auto o = r.getIntersection (target);
        const int overlap = o.getWidth() * o.getHeight();
        if (overlap < bestOverlap)
        {
            bestOverlap = overlap;
            chosen = r;
        }
        if (overlap == 0) break;
    }
    if (chosen.isEmpty()) chosen = juce::Rectangle<int> (kBubbleW, kBubbleH).withCentre (area.getCentre()).withBottom (area.getBottom());
    if (animate() && bubble->getBounds() != chosen && ! bubble->getBounds().isEmpty() && isShowing())
        juce::Desktop::getInstance().getAnimator().animateComponent (bubble.get(), chosen, 1.0f, Theme::motionMid, false, 1.0, 0.0);
    else
        bubble->setBounds (chosen);
}

void GuideTour::timerCallback()
{
    const float t = juce::jlimit (0.0f, 1.0f, float ((juce::Time::getMillisecondCounterHiRes() - animStart) / Theme::motionMid));
    const float e = Theme::ease (t);
    auto lerp = [e] (float a, float b) { return a + (b - a) * e; };
    hole = { lerp (holeFrom.getX(), holeTo.getX()), lerp (holeFrom.getY(), holeTo.getY()), lerp (holeFrom.getWidth(), holeTo.getWidth()),
             lerp (holeFrom.getHeight(), holeTo.getHeight()) };
    if (t >= 1.0f) stopTimer();
    repaint();
}

void GuideTour::paint (juce::Graphics& g)
{
    const auto& p = P();
    const auto h = hole.isEmpty() ? holeTo : hole;
    juce::Path dim;
    dim.setUsingNonZeroWinding (false);
    dim.addRectangle (getLocalBounds().toFloat());
    dim.addRoundedRectangle (h, Theme::radiusL);
    g.setColour (p.overlay);
    g.fillPath (dim);
    g.setColour (p.accent);
    g.drawRoundedRectangle (h, Theme::radiusL, 2.0f);

    // arrow from the bubble towards the part
    const auto b = bubble->getBounds().toFloat();
    juce::Path arrow;
    const float ax = juce::jlimit (b.getX() + Theme::radiusL * 2, b.getRight() - Theme::radiusL * 2, h.getX() + Theme::space5);
    if (b.getY() >= h.getBottom())
        arrow.addTriangle (ax - kArrow, b.getY() + 1.0f, ax + kArrow, b.getY() + 1.0f, ax, b.getY() - kArrow);
    else if (b.getBottom() <= h.getY())
        arrow.addTriangle (ax - kArrow, b.getBottom() - 1.0f, ax + kArrow, b.getBottom() - 1.0f, ax, b.getBottom() + kArrow);
    if (! arrow.isEmpty())
    {
        g.setColour (p.raised);
        g.fillPath (arrow);
        g.setColour (p.accent);
        g.strokePath (arrow, juce::PathStrokeType (2.0f));
    }
}

bool GuideTour::hitTest (int x, int y)
{
    // clicks inside the highlighted part go through to it (F-13-3: doing the operation also advances)
    return ! (hole.isEmpty() ? holeTo : hole).contains (float (x), float (y));
}

void GuideTour::mouseUp (const juce::MouseEvent& e)
{
    if (finished || e.eventComponent == this || isParentOf (e.eventComponent)) return;
    const auto pos = e.getEventRelativeTo (this).getPosition().toFloat();
    if (holeTo.contains (pos) && e.mouseWasClicked())
        juce::MessageManager::callAsync ([safe = juce::Component::SafePointer<GuideTour> (this), s = step]
        {
            if (safe != nullptr && ! safe->finished && safe->step == s) safe->next();
        });
}

bool GuideTour::keyPressed (const juce::KeyPress& k)
{
    if (k == juce::KeyPress::escapeKey) { skip(); return true; }
    if (k.isKeyCode (juce::KeyPress::rightKey)) { next(); return true; }
    if (k.isKeyCode (juce::KeyPress::leftKey)) { back(); return true; }
    return false;
}

void GuideTour::next() { show (step + 1, +1); }
void GuideTour::back() { if (step > 0) show (step - 1, -1); }
void GuideTour::skip() { finish(); }

void GuideTour::finish()
{
    if (finished) return;
    finished = true;
    stopTimer();
    c.updateSettings ([] (Settings& s) { s.tourStep = 7; });
    setVisible (false);
    nav.showPage (Page::voice);
    if (onFinished) onFinished();
}
} // namespace koe::ui::mainui

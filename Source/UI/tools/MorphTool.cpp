// 混ぜる tool (INTERFACES.md §10.2 / §10.3). Owner: wave8/morph.
// Pick preset A and B (lists grouped by category), 「混ぜ始める」, then a big A <-> B slider and 「新しく保存」.

#include "UI/main/ToolsView.h"

#include "Core/Constants.h"
#include "UI/Theme.h"
#include "UI/Widgets.h"
#include "UI/main/Common.h"

namespace koe::ui
{
namespace
{
/** Wide A <-> B slider: 8 px track, accent fill up to the thumb, 22 px round thumb. */
class BlendSlider : public juce::Slider
{
public:
    BlendSlider() : juce::Slider (juce::Slider::LinearHorizontal, juce::Slider::NoTextBox)
    {
        setWantsKeyboardFocus (true);
        setRange (0.0, 1.0, 0.01);
        setDoubleClickReturnValue (true, 0.5);
        setTitle (ja ("混ぜ具合"));
        setTooltip (ja ("左いっぱい = A、右いっぱい = B（ダブルクリックで半分ずつ）"));
    }
    void paint (juce::Graphics& g) override
    {
        const auto& p = Theme::colours();
        const float thumb = 22.0f;
        auto r = getLocalBounds().toFloat().reduced (thumb * 0.5f + 1.0f, 0.0f);
        const auto track = r.withSizeKeepingCentre (r.getWidth(), 8.0f);
        g.setColour (isEnabled() ? p.border : p.trackOff);
        g.fillRoundedRectangle (track, 4.0f);
        const float x = r.getX() + float (valueToProportionOfLength (getValue())) * r.getWidth();
        if (isEnabled())
        {
            g.setColour (p.accent);
            g.fillRoundedRectangle (track.withRight (x), 4.0f);
        }
        const auto knob = juce::Rectangle<float> (thumb, thumb).withCentre ({ x, r.getCentreY() });
        g.setColour (isEnabled() ? p.accent : p.trackOff);
        g.fillEllipse (knob);
        g.setColour (p.surface);
        g.drawEllipse (knob.reduced (1.0f), 2.0f);
        if (hasKeyboardFocus (true)) drawFocusRing (g, knob.expanded (3.0f), thumb * 0.5f + 3.0f);
    }
    void focusGained (FocusChangeType) override { repaint(); }
    void focusLost (FocusChangeType) override { repaint(); }
};

class MorphTool : public juce::Component, private juce::Timer
{
public:
    MorphTool (AppController& ctl, Navigator& n)
        : c (ctl), nav (n), start (ja ("混ぜ始める"), PillButton::Style::primary), save (ja ("新しく保存"), PillButton::Style::outline)
    {
        for (auto* box : { &boxA, &boxB })
        {
            addAndMakeVisible (*box);
            box->onChange = [this] { refresh(); };
        }
        boxA.setComponentID ("morph.a");
        boxB.setComponentID ("morph.b");
        boxA.setTitle (ja ("プリセット A"));
        boxB.setTitle (ja ("プリセット B"));
        boxA.setTextWhenNothingSelected (ja ("A を選ぶ"));
        boxB.setTextWhenNothingSelected (ja ("B を選ぶ"));
        start.setComponentID ("morph.start");
        start.setTooltip (ja ("作業中のプリセットを、A と B を混ぜた声にする（最初は A のまま）"));
        start.onClick = [this] { begin(); };
        amount.setComponentID ("morph.amount");
        amount.onValueChange = [this]
        {
            c.setMorphAmount (float (amount.getValue()));
            repaint();
        };
        save.setComponentID ("morph.save");
        save.setTooltip (ja ("混ぜた声を新しいプリセットとして保存する"));
        save.onClick = [this] { askName(); };
        addAndMakeVisible (start);
        addAndMakeVisible (amount);
        addAndMakeVisible (save);
        fillLists();
        // rebuilt mid-blend (theme or layout change): show the blend's A and B again
        std::string idA, idB;
        if (c.getMorphPresets (idA, idB, nameA, nameB))
        {
            select (boxA, idA);
            select (boxB, idB);
        }
        refresh();
        startTimerHz (10);
    }

    void visibilityChanged() override
    {
        if (isVisible())
        {
            fillLists(); // presets saved or removed elsewhere
            refresh();
            startTimerHz (10);
        }
        else
            stopTimer();
    }

    void paint (juce::Graphics& g) override
    {
        const auto& p = Theme::colours();
        drawText (g, ja ("混ぜる"), titleR, Theme::fontL, p.text, juce::Justification::centredLeft, true);
        drawText (g, ja ("2 つのプリセットの間を、スライダーで行き来します。"), descR, Theme::fontS, p.textSub);
        drawText (g, ja ("プリセット A"), labelAR, Theme::fontXS, p.textSub);
        drawText (g, ja ("プリセット B"), labelBR, Theme::fontXS, p.textSub);

        auto names = namesR;
        const auto tone = morphing ? p.text : p.textSub;
        const auto pct = morphing ? juce::String (juce::roundToInt (amount.getValue() * 100.0)) + " %" : juce::String();
        const int pctW = Theme::space5 * 2;
        auto mid = names.withSizeKeepingCentre (pctW, names.getHeight());
        const int side = mid.getX() - names.getX() - Theme::space2;
        drawText (g, (juce::String ("A  ") + label (boxA, nameA)).trimEnd(), names.removeFromLeft (side), Theme::fontS, tone, juce::Justification::centredLeft, true);
        drawText (g, (label (boxB, nameB) + "  B").trimStart(), names.removeFromRight (side), Theme::fontS, tone, juce::Justification::centredRight, true);
        drawText (g, pct, mid, Theme::fontM, tone, juce::Justification::centred, true, true);

        g.setColour (p.textSub);
        g.setFont (Theme::ui (Theme::fontS));
        g.drawFittedText (statusText(), statusR, juce::Justification::topLeft, 2, 1.0f);
    }

    void resized() override
    {
        auto r = getLocalBounds();
        titleR = r.removeFromTop (Theme::space5 - Theme::space1);
        descR = r.removeFromTop (Theme::space4 - Theme::space1);
        r.removeFromTop (Theme::space3);
        auto labels = r.removeFromTop (Theme::space3 + 2);
        auto row = r.removeFromTop (Theme::buttonH);
        start.setBounds (row.removeFromRight (start.preferredWidth()));
        row.removeFromRight (Theme::space2);
        const int boxW = (row.getWidth() - Theme::space2) / 2;
        boxA.setBounds (row.removeFromLeft (boxW));
        boxB.setBounds (row.removeFromRight (boxW));
        labelAR = labels.withX (boxA.getX()).withWidth (boxW);
        labelBR = labels.withX (boxB.getX()).withWidth (boxW);
        r.removeFromTop (Theme::space4);
        namesR = r.removeFromTop (Theme::space4);
        amount.setBounds (r.removeFromTop (Theme::touchMin + Theme::space1));
        r.removeFromTop (Theme::space3);
        statusR = r.removeFromTop (Theme::space5 + Theme::space2); // two lines
        save.setBounds (r.removeFromTop (Theme::buttonH).withWidth (save.preferredWidth()));
    }

private:
    void timerCallback() override
    {
        if (isShowing()) refresh(); // a preset load or an effect added elsewhere ends the blend
    }

    std::string idOf (const juce::ComboBox& box) const
    {
        const int i = box.getSelectedId();
        return i > 0 && i <= int (ids.size()) ? ids[size_t (i - 1)] : std::string();
    }

    void select (juce::ComboBox& box, const std::string& id)
    {
        for (size_t i = 0; i < ids.size(); ++i)
            if (ids[i] == id) box.setSelectedId (int (i) + 1, juce::dontSendNotification);
    }

    void fillLists()
    {
        const auto selA = idOf (boxA), selB = idOf (boxB);
        ids.clear();
        for (auto* box : { &boxA, &boxB }) box->clear (juce::dontSendNotification);
        for (const char* cat : { "natural", "character", "device", "space", "layered", "user" })
        {
            bool heading = false;
            for (auto& p : c.getPresetLibrary().all())
            {
                if (p.category() != cat) continue;
                if (! heading)
                    for (auto* box : { &boxA, &boxB }) box->addSectionHeading (mainui::presetCategoryJa (cat));
                heading = true;
                ids.push_back (p.id);
                for (auto* box : { &boxA, &boxB }) box->addItem (p.name, int (ids.size()));
            }
        }
        select (boxA, selA.empty() ? c.getSettings().currentPresetId.toStdString() : selA);
        select (boxB, selB);
    }

    void refresh()
    {
        const bool now = c.isMorphing();
        if (morphing && ! now) ended = true;
        if (now) ended = false;
        morphing = now;
        amount.setEnabled (now);
        save.setEnabled (now);
        start.setEnabled (! idOf (boxA).empty() && ! idOf (boxB).empty());
        if (now && ! amount.isMouseButtonDown()) amount.setValue (c.getMorphAmount(), juce::dontSendNotification);
        const auto sig = juce::String (int (now)) + juce::String (int (ended)) + juce::String (amount.getValue()) + boxA.getText() + boxB.getText();
        if (sig != shownSig)
        {
            shownSig = sig;
            repaint();
        }
    }

    void begin()
    {
        juce::String why;
        if (! c.beginMorph (idOf (boxA), idOf (boxB), why))
        {
            nav.showToast (why);
            return;
        }
        nameA = boxA.getText();
        nameB = boxB.getText();
        amount.setValue (0.0, juce::dontSendNotification);
        refresh();
    }

    void askName()
    {
        auto panel = std::make_unique<mainui::ConfirmPanel> (nav, ja ("新しく保存"), ja ("プリセットの名前を入れてください（32 文字まで）。"), ja ("保存"), nullptr);
        juce::Component::SafePointer<MorphTool> safe (this);
        panel->addTextField (c.getCurrentPreset().name.substring (0, kPresetNameMaxChars), [this, safe] (const juce::String& name)
        {
            if (name.isEmpty())
            {
                nav.showToast (ja ("名前を入れてください。"));
                return;
            }
            juce::String error;
            nav.showToast (c.saveCurrentAsNew (name, error) ? ja ("「") + name + ja ("」として保存しました。") : error);
            if (safe != nullptr) safe->fillLists();
        });
        nav.showOverlay (std::move (panel));
    }

    juce::String label (const juce::ComboBox& box, const juce::String& started) const
    {
        if (morphing && started.isNotEmpty()) return started; // the names the blend began with, even if the lists changed since
        return box.getSelectedId() > 0 ? box.getText() : juce::String();
    }

    juce::String statusText() const
    {
        if (morphing) return ja ("スライダーで A と B の間を動かせます。気に入ったら「新しく保存」で残せます。");
        if (ended) return ja ("プリセットの切り替えやエフェクトの追加で、混ぜるのを終えました。もう一度「混ぜ始める」を押してください。");
        return ja ("A と B を選んで「混ぜ始める」を押すと、作業中のプリセットが 2 つを混ぜた声になります。");
    }

    AppController& c;
    Navigator& nav;
    juce::ComboBox boxA, boxB;
    PillButton start, save;
    BlendSlider amount;
    std::vector<std::string> ids; // combo item id - 1 -> preset id
    juce::String nameA, nameB, shownSig;
    bool morphing = false, ended = false;
    juce::Rectangle<int> titleR, descR, labelAR, labelBR, namesR, statusR;
};
} // namespace

std::unique_ptr<juce::Component> makeMorphTool (AppController& c, Navigator& nav) { return std::make_unique<MorphTool> (c, nav); }
} // namespace koe::ui

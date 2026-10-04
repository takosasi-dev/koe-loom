#include "UI/MainComponent.h"

#include "Core/Paths.h"
#include "Effects/EffectRegistry.h"
#include "UI/Overlay.h"
#include "UI/Screens.h"
#include "UI/ThemeLibrary.h"
#include "UI/main/GuideTour.h"
#include "UI/main/Panels.h"
#include "UI/main/Shell.h"
#include "UI/main/ToolsView.h"
#include "UI/main/VoicePage.h"
#include "UI/main/VoiceView.h"

#include <juce_audio_formats/juce_audio_formats.h>

#include <cmath>

namespace koe::ui
{
using namespace mainui;

static_assert (Theme::numAccents == kAccentColours && Theme::numTones == kBackgroundTones, "S-03 外観 choices and the settings range");

struct MainComponent::Impl : private juce::ChangeListener, private juce::Timer
{
    Impl (MainComponent& o, AppController& ctl)
        : owner (o), c (ctl), tooltips (&o, 500), header (ctl, o), notices (ctl, o)
    {
    }

    ~Impl() override
    {
        stopTimer();
        c.removeChangeListener (this);
    }

    void start()
    {
        for (auto* comp : std::initializer_list<juce::Component*> { &header, &notices }) owner.addAndMakeVisible (comp);
        if (const auto& s = c.getSettings(); s.accentColour != Theme::accent() || s.backgroundTone != Theme::tone())
        {
            Theme::setVariant (s.accentColour, s.backgroundTone); // the saved 外観 choice, before anything is built
            if (auto* lnf = dynamic_cast<KoeLookAndFeel*> (&juce::LookAndFeel::getDefaultLookAndFeel())) lnf->refreshColours();
        }
        if (const auto& s = c.getSettings(); s.themeId.isNotEmpty() && s.themeId != Theme::themeId())
        {
            // the saved 配色 too, so the pages are built once in it (a broken theme is reported by applyTheme())
            juce::String why;
            if (const auto t = ThemeLibrary::resolve (s.themeId, why))
            {
                Theme::setPalette (s.themeId, t->colours, t->dark);
                if (auto* lnf = dynamic_cast<KoeLookAndFeel*> (&juce::LookAndFeel::getDefaultLookAndFeel())) lnf->refreshColours();
            }
        }
        buildPages();
        owner.addChildComponent (toast);
        owner.addChildComponent (overlay);
        notices.onHeightChanged = [this] { layout(); };
        toast.onChanged = [this] { layoutToast(); };
        overlay.onClosed = [this] { if (tour != nullptr && tour->isVisible()) tour->toFront (true); };
        c.addChangeListener (this);
        startTimerHz (timerHz); // F-08-1; S-03 「メーターの更新」 may change it
    }

    void buildPages()
    {
        voice = makeVoicePage (c.getSettings().layoutStyle, c, owner);
        builtLayout = c.getSettings().layoutStyle;
        sound = std::make_unique<SoundboardView> (c, owner);
        settings = std::make_unique<SettingsView> (c, owner);
        tools = std::make_unique<ToolsView> (c, owner);
        soundBottom = std::make_unique<BottomBar> (c, owner, false);
        voice->setComponentID ("page.voice");
        sound->setComponentID ("page.soundboard");
        settings->setComponentID ("page.settings");
        for (auto* p : pages()) owner.addChildComponent (p);
        owner.addChildComponent (*soundBottom);
        toast.toFront (false);
        overlay.toFront (false);
        if (tour != nullptr) tour->toFront (false);
    }

    std::vector<juce::Component*> pages() const { return { voice.get(), sound.get(), settings.get(), tools.get() }; }

    void layout()
    {
        auto r = owner.getLocalBounds();
        if (r.isEmpty()) return;
        compact = r.getWidth() < Theme::narrowWidth;
        const int pad = compact ? Theme::space2 + Theme::space1 : Theme::space3;
        const int gap = compact ? Theme::space2 : Theme::space3;
        header.setCompact (compact);
        // a voice page with its own header (案 C Mono) gets the window edge to edge, below the banners
        const bool pageHeader = page == Navigator::Page::voice && voice->ownsHeader();
        header.setVisible (! pageHeader);
        auto inner = r.reduced (pad);
        if (! pageHeader)
        {
            header.setBounds (inner.removeFromTop (compact ? Theme::pillH : Theme::headerH));
            inner.removeFromTop (gap);
        }
        const int nh = notices.preferredHeight();
        notices.setBounds (inner.removeFromTop (nh));
        if (nh > 0) inner.removeFromTop (gap);
        voice->setCompact (compact);
        for (auto* p : pages()) p->setBounds (inner);
        if (pageHeader) voice->setBounds (r.withTrimmedTop (nh > 0 ? pad + nh + gap : 0));
        soundBottom->setCompact (compact);
        auto sb = inner;
        soundBottom->setBounds (sb.removeFromBottom (BottomBar::height (compact)));
        sound->setBounds (sb.withTrimmedBottom (gap));
        overlay.setBounds (r);
        if (tour != nullptr && tour->isVisible())
        {
            tour->setBounds (r);
            // the parts moved (banners, resize): follow them, keep clear of banners (E-31). Not while the tour itself is
            // switching pages (案 C's header hides on the voice page): relocating the old step would navigate back to its page
            if (! switchingPage) tour->relayout();
        }
        layoutToast();
    }

    void layoutToast()
    {
        if (! toast.isVisible()) return;
        const auto r = owner.getLocalBounds();
        const int pad = compact ? Theme::space2 + Theme::space1 : Theme::space3;
        const int w = juce::jmin (toast.preferredWidth(), r.getWidth() - pad * 4);
        const int bottomBar = compact ? Theme::touchMin : Theme::buttonH;
        toast.setBounds (r.getCentreX() - w / 2, r.getBottom() - pad - bottomBar - Theme::space2 - Theme::controlH, w, Theme::controlH);
        toast.toFront (false);
        if (overlay.isVisible()) overlay.toFront (false);
        if (tour != nullptr && tour->isVisible()) tour->toFront (false);
    }

    void showPage (Navigator::Page p)
    {
        page = p;
        voice->setVisible (p == Navigator::Page::voice);
        sound->setVisible (p == Navigator::Page::soundboard);
        soundBottom->setVisible (p == Navigator::Page::soundboard);
        settings->setVisible (p == Navigator::Page::settings);
        tools->setVisible (p == Navigator::Page::tools);
        header.setPage (p);
        // the header hides on a voice page that draws its own (案 C Mono); only then is a new layout needed
        // (a tour that switches pages places itself afterwards: layout() does not relocate it meanwhile)
        if (header.isVisible() == (p == Navigator::Page::voice && voice->ownsHeader()))
        {
            const juce::ScopedValueSetter<bool> switching (switchingPage, true);
            layout();
        }
    }

    /** S-03 外観 detailed settings that live outside the settings screen (INTERFACES.md §7.3). */
    void applyScreenSettings()
    {
        const auto& s = c.getSettings();
        auto& p = Theme::prefs();
        p.animations = s.animations;
        p.meterFps = s.meterFps >= 60 ? 60 : 30;
        p.peakHoldMs = s.meterPeakHoldMs;
        p.knobSensitivity = s.knobSensitivity;
        p.knobWheel = s.knobWheel;
        tooltips.setMillisecondsBeforeTipAppears (juce::roundToInt (s.tooltipDelayMs));
        if (timerHz != p.meterFps)
        {
            timerHz = p.meterFps;
            startTimerHz (timerHz);
        }
        const float scale = uiScaleFactor (s);
        if (! juce::approximatelyEqual (juce::Desktop::getInstance().getGlobalScaleFactor(), scale))
            juce::Desktop::getInstance().setGlobalScaleFactor (scale); // window sizes are logical: the minimum scales too
    }

    void refresh()
    {
        applyScreenSettings();
        applyTheme();
        header.refresh();
        notices.setNotices (c.getNotices());
        voice->refresh();
        soundBottom->refresh();
        for (auto& t : c.takeToasts()) toast.push (t);
    }

    /** F-14-2: the theme switch (and the accent / background choice) in S-03 rebuilds every page in the new colours. */
    void applyTheme()
    {
        const auto& s = c.getSettings();
        // 「配色」 (Settings::themeId, INTERFACES.md §8.4): "" = Studio (theme + accent + tone), else a built-in / user palette
        const bool studioSame = s.darkTheme == Theme::isDark() && s.accentColour == Theme::accent() && s.backgroundTone == Theme::tone();
        if (s.themeId == Theme::themeId() && (s.themeId.isNotEmpty() || studioSame) && s.layoutStyle == builtLayout) return;
        Theme::clearPalette();
        Theme::setDark (s.darkTheme);
        Theme::setVariant (s.accentColour, s.backgroundTone);
        if (s.themeId.isNotEmpty())
        {
            juce::String why;
            if (const auto t = ThemeLibrary::resolve (s.themeId, why)) Theme::setPalette (s.themeId, t->colours, t->dark);
            else
            {
                // unknown or broken theme: back to Studio, and say why
                owner.showToast (ja ("配色を読み込めないため Studio に戻しました。") + why);
                c.updateSettings ([] (Settings& st) { st.themeId = {}; });
            }
        }
        if (auto* lnf = dynamic_cast<KoeLookAndFeel*> (&juce::LookAndFeel::getDefaultLookAndFeel())) lnf->refreshColours();
        const auto current = page;
        voice.reset();
        sound.reset();
        settings.reset();
        tools.reset();
        soundBottom.reset();
        buildPages();
        layout();
        showPage (current);
        if (current == Navigator::Page::settings) settings->showSection (Navigator::SettingsSection::appearance);
        owner.sendLookAndFeelChange();
        owner.repaint();
    }

    void startupFlow()
    {
        const auto& s = c.getSettings();
        if (! s.setupDone) owner.showSetupWizard(); // F-10, then the tour (F-13-1)
        else if (s.tourStep >= 0 && s.tourStep < int (GuideTour::steps().size()))
            owner.showOverlay (std::make_unique<ConfirmPanel> (owner, ja ("ガイドツアー"), ja ("前回はガイドツアーの途中で終わりました。続きから再開しますか。"),
                                                               ja ("再開する"), [this] { owner.startTour (true); }, ja ("やめる"),
                                                               [this] { c.updateSettings ([] (Settings& st) { st.tourStep = 7; }); })); // F-13-4
    }

    void changeListenerCallback (juce::ChangeBroadcaster*) override { refresh(); }

    void timerCallback() override
    {
        if (voice->isShowing()) voice->tick(); // nothing to draw while the window sits in the tray
        if (soundBottom->isShowing()) soundBottom->tick (c.getStatus());
        if (++ticks % (timerHz / 3) == 0) // notices raised from the controller's timer come without a change message
        {
            notices.setNotices (c.getNotices());
            for (auto& t : c.takeToasts()) toast.push (t);
        }
    }

    MainComponent& owner;
    AppController& c;
    juce::TooltipWindow tooltips; // F-13-6: 500 ms
    HeaderBar header;
    NoticeBar notices;
    std::unique_ptr<VoicePage> voice;
    int builtLayout = 0;          // S-03 外観 「画面の配置」 the voice page was built with
    std::unique_ptr<SoundboardView> sound;
    std::unique_ptr<SettingsView> settings;
    std::unique_ptr<ToolsView> tools;
    std::unique_ptr<BottomBar> soundBottom; // A-S02 has the same bottom bar as S-01
    ToastView toast;
    OverlayHost overlay;
    std::unique_ptr<GuideTour> tour;
    Navigator::Page page = Navigator::Page::voice;
    bool compact = false;
    bool switchingPage = false; // showPage() lays out again for a page-owned header (see layout())
    int ticks = 0;
    int timerHz = 30;
};

float uiScaleFactor (const Settings& s) { return float (s.uiScalePercent) / 100.0f; }

juce::Point<int> minimumWindowSize (const Settings& s)
{
    return { juce::roundToInt (float (Theme::minWidth) * uiScaleFactor (s)), juce::roundToInt (float (Theme::minHeight) * uiScaleFactor (s)) };
}

MainComponent::MainComponent (AppController& c) : impl (std::make_unique<Impl> (*this, c))
{
    setComponentID ("main");
    setFocusContainerType (FocusContainerType::keyboardFocusContainer);
    impl->start();
    impl->showPage (Page::voice);
    setSize (Theme::defaultWidth, Theme::defaultHeight);
    impl->refresh();
    impl->startupFlow();
}

MainComponent::~MainComponent()
{
    impl->tour.reset();
    impl->overlay.close();
    juce::Desktop::getInstance().getAnimator().cancelAnimation (&impl->overlay, false); // drops the fade's snapshot
}

void MainComponent::paint (juce::Graphics& g) { g.fillAll (Theme::colours().bg); }
void MainComponent::resized() { impl->layout(); }

bool MainComponent::keyPressed (const juce::KeyPress& key)
{
    return impl->page == Page::settings && ! impl->overlay.isVisible() && impl->settings->keyPressed (key);
}

void MainComponent::showPage (Page page) { impl->showPage (page); }

void MainComponent::showSettings (SettingsSection section)
{
    impl->overlay.close();
    impl->showPage (Page::settings);
    impl->settings->showSection (section);
}

void MainComponent::showPresetBrowser() { impl->overlay.show (std::make_unique<PresetBrowser> (impl->c, *this), true); }

void MainComponent::showEffectPicker (int insertAt) { impl->overlay.show (std::make_unique<EffectPicker> (impl->c, *this, insertAt), false); }

void MainComponent::showSlotDetail (int slot)
{
    if (slot < 0 || slot >= int (impl->c.getChain().size())) return;
    impl->overlay.show (std::make_unique<SlotDetailPanel> (impl->c, *this, slot), false); // one at a time (F-04-22)
}

void MainComponent::showSetupWizard()
{
    impl->overlay.show (std::make_unique<SetupWizard> (impl->c, *this,
                                                       [safe = juce::Component::SafePointer<MainComponent> (this)] (bool completed)
                                                       {
                                                           if (safe == nullptr) return;
                                                           safe->impl->c.updateSettings ([] (Settings& s) { s.setupDone = true; });
                                                           // F-13-1: the tour starts right after the setup is completed, the first
                                                           // time only (S-04 reopened from S-03 / [?] does not replay it). あとで設定する
                                                           // closes without the tour; [?] > ガイドツアー starts it any time (F-13-4).
                                                           const bool tour = completed && safe->impl->c.getSettings().tourStep < 0;
                                                           safe->closeOverlay();
                                                           if (tour) safe->startTour (false);
                                                       }),
                        true);
}

void MainComponent::showHelp (HelpTopic topic) { impl->overlay.show (createHelpPanel (topic, impl->c, *this), false); }

void MainComponent::startTour (bool resumeFromSaved)
{
    const int saved = impl->c.getSettings().tourStep;
    const int start = resumeFromSaved && saved >= 0 && saved < int (GuideTour::steps().size()) ? saved : 0;
    impl->overlay.close();
    impl->tour.reset();
    impl->tour = std::make_unique<GuideTour> (*this, impl->c, *this, start, [safe = juce::Component::SafePointer<MainComponent> (this)]
    {
        // the tour calls this from its own button: delete it later
        juce::MessageManager::callAsync ([safe] { if (safe != nullptr && safe->impl->tour != nullptr && ! safe->impl->tour->isVisible()) safe->impl->tour.reset(); });
    });
    addAndMakeVisible (*impl->tour);
    impl->tour->setBounds (getLocalBounds());
    impl->tour->toFront (true);
}

void MainComponent::showOverlay (std::unique_ptr<juce::Component> panel) { impl->overlay.show (std::move (panel), false); }
void MainComponent::closeOverlay() { impl->overlay.close(); }

void MainComponent::showToast (const juce::String& text)
{
    impl->toast.push (text);
    impl->layoutToast();
}

// =============================================================================================== snapshots
namespace
{
/** A short decaying tone as a 48 kHz mono WAV (S-02 snapshot content; never played aloud). */
bool writeSampleWav (const juce::File& file, double seconds, double hz)
{
    constexpr double rate = 48000.0;
    juce::AudioBuffer<float> buf (1, int (seconds * rate));
    for (int i = 0; i < buf.getNumSamples(); ++i)
    {
        const double t = i / rate;
        buf.setSample (0, i, float (0.2 * std::exp (-3.0 * t) * std::sin (juce::MathConstants<double>::twoPi * hz * t)));
    }
    file.deleteFile();
    std::unique_ptr<juce::FileOutputStream> out (file.createOutputStream());
    if (out == nullptr) return false;
    juce::WavAudioFormat wav;
    std::unique_ptr<juce::AudioFormatWriter> writer (wav.createWriterFor (out.get(), rate, 1, 16, {}, 0));
    if (writer == nullptr) return false;
    out.release(); // the writer owns the stream now
    return writer->writeFromAudioSampleBuffer (buf, 0, buf.getNumSamples());
}

/** S-02 as in the mock: eight sounds, Ctrl+1..5, slot 03 playing at about 30 %. Rendered offline (no device). */
void fillSnapshotSoundboard (AppController& c, const juce::File& dir)
{
    struct Sample { const char* name; double seconds, hz; };
    const Sample samples[] = { { "拍手.wav", 2.4, 880.0 },  { "ドラムロール.wav", 3.1, 220.0 }, { "ブザー.wav", 0.8, 330.0 },
                               { "チャイム.wav", 1.6, 1320.0 }, { "笑い声.wav", 2.0, 440.0 },  { "ため息.wav", 1.2, 180.0 },
                               { "驚き.wav", 0.9, 660.0 },    { "効果音 8.wav", 4.5, 550.0 } };
    auto& sb = c.getSoundboard();
    dir.createDirectory();
    for (int i = 0; i < int (std::size (samples)); ++i)
    {
        const auto f = dir.getChildFile (juce::String::fromUTF8 (samples[i].name));
        if (writeSampleWav (f, samples[i].seconds, samples[i].hz)) sb.assignFile (i, f);
    }
    juce::String why;
    for (int i = 1; i <= 5; ++i) c.setHotkey ("sound." + juce::String (i), 2, '0' + i, why); // Ctrl+n
    const auto until = juce::Time::getMillisecondCounter() + 5000;
    auto loading = [&sb]
    {
        for (int i = 0; i < kSoundboardSlots; ++i)
            if (sb.getSlotState (i).status == SoundSlotState::Status::loading) return true;
        return false;
    };
    while (loading() && juce::Time::getMillisecondCounter() < until) juce::Thread::sleep (5);
    sb.trigger (2);
    std::vector<float> out (480), mon (480);
    for (int b = 0; b < 24; ++b) sb.render (out.data(), mon.data(), 480); // 0.24 s of 0.8 s
}

void clearSnapshotSoundboard (AppController& c)
{
    auto& sb = c.getSoundboard();
    sb.stopAll();
    std::vector<float> out (480), mon (480);
    sb.render (out.data(), mon.data(), 480);
    for (int i = 0; i < kSoundboardSlots; ++i) sb.clearSlot (i);
    for (int i = 1; i <= 5; ++i) c.clearHotkey ("sound." + juce::String (i));
}
} // namespace

int renderSnapshots (const juce::File& outputDir)
{
    outputDir.createDirectory();
    if (auto data = paths::dataDir(); data.getFullPathName().contains ("KoeLoomSnapshots")) data.deleteRecursively(); // start clean
    paths::dataDir().createDirectory();

    AppController c (false); // never touches audio devices
    c.startup();
    c.updateSettings ([] (Settings& s)
    {
        s.setupDone = true;
        s.tourStep = 7;
        s.favorites = juce::StringArray { "natural-asis", "character-demon-king", "natural-ikebo", "character-helium", "character-robot", "space-cave" };
    });
    c.loadPreset ("character-demon-king"); // AC-59: 魔王, 4 slots

    const bool wasDark = Theme::isDark();
    const int wasAccent = Theme::accent(), wasTone = Theme::tone();
    const bool wasOff = animationsOff();
    animationsOff() = true;
    KoeLookAndFeel lnf;
    juce::LookAndFeel::setDefaultLookAndFeel (&lnf);
    int written = 0;

    auto shot = [&] (const juce::String& name, int w, int h, const std::function<void (MainComponent&)>& prepare)
    {
        c.updateSettings ([] (Settings& s) { s.tourStep = 7; });
        MainComponent mc (c);
        mc.setSize (w, h);
        if (prepare) prepare (mc);
        if (auto* v = dynamic_cast<VoicePage*> (findById (&mc, "page.voice"))) v->tick();
        const auto image = mc.createComponentSnapshot (mc.getLocalBounds(), true, 1.0f);
        auto file = outputDir.getChildFile (name + ".png");
        file.deleteFile();
        juce::FileOutputStream out (file);
        if (out.openedOk() && juce::PNGImageFormat().writeImageToStream (image, out)) ++written;
    };

    const std::pair<Navigator::SettingsSection, const char*> sections[] = {
        { Navigator::SettingsSection::devices, "devices" },         { Navigator::SettingsSection::environment, "environment" },
        { Navigator::SettingsSection::hotkeys, "hotkeys" },         { Navigator::SettingsSection::startup, "startup" },
        { Navigator::SettingsSection::appearance, "appearance" },   { Navigator::SettingsSection::advanced, "advanced" },
        { Navigator::SettingsSection::diagnostics, "diagnostics" },
    };

    for (const bool dark : { true, false })
    {
        Theme::setDark (dark);
        lnf.refreshColours();
        c.updateSettings ([dark] (Settings& s) { s.darkTheme = dark; });
        const juce::String t = dark ? "-dark" : "-light";
        constexpr int W = Theme::defaultWidth, H = Theme::defaultHeight, w = Theme::minWidth, h = Theme::minHeight;

        shot ("S01-wide" + t, W, H, {});
        // S-03 外観 example for the README: バイオレット + ネイビー (dark), グリーン + ホワイト (light)
        const int va = dark ? 2 : 3, vt = dark ? 2 : 1;
        c.updateSettings ([va, vt] (Settings& s) { s.accentColour = va; s.backgroundTone = vt; });
        shot ("S01-wide-" + juce::String (dark ? "violet-navy" : "green-white") + t, W, H, {});
        c.updateSettings ([] (Settings& s) { s.accentColour = 0; s.backgroundTone = 0; });
        Theme::setVariant (0, 0);
        lnf.refreshColours();
        shot ("S01-min" + t, w, h, {});
        c.setOutputGainDb (8.0f); // a real banner (F-12-4)
        shot ("S01-min-banner" + t, w, h, {});
        c.setOutputGainDb (0.0f);
        // layout stress: two voices (full editors / rows), three banners in the wide layout
        auto twoVoices = [&c] (MainComponent&)
        {
            juce::String why;
            c.addLayer (why);
            c.dispatchPendingMessages();
        };
        shot ("S01-wide-2voices" + t, W, H, twoVoices);
        shot ("S01-min-2voices" + t, w, h, {});
        auto banners = [] (MainComponent& m)
        {
            if (auto* bar = dynamic_cast<NoticeBar*> (findById (&m, "main.notices")))
                bar->setNotices ({ { "s1", NoticeLevel::danger, ja ("（見本）オーディオデバイスが止まりました。戻ると自動で再開します。"), false },
                                   { "s2", NoticeLevel::warning, ja ("（見本）出力先が仮想ケーブルではありません。"), true, ja ("設定で変更"), "openSettings.devices" },
                                   { "s3", NoticeLevel::info, ja ("（見本）3 件目") } });
        };
        shot ("S01-wide-banners" + t, W, H, banners);
        c.loadPreset ("character-demon-king");
        shot ("S02" + t, W, H, [] (MainComponent& m) { m.showPage (Navigator::Page::soundboard); });
        fillSnapshotSoundboard (c, paths::dataDir().getChildFile ("snapshot-sounds"));
        c.updateSettings ([] (Settings& s) { s.soundboardHintShown = true; });
        shot ("S02-sounds" + t, W, H, [] (MainComponent& m) { m.showPage (Navigator::Page::soundboard); });
        c.updateSettings ([] (Settings& s) { s.soundboardHintShown = false; });
        clearSnapshotSoundboard (c);
        // wave 8 (INTERFACES.md §10.2): the ツール page, one shot per tool (+ the narrow window for the first)
        for (int i = 0; i < ToolsView::numTools; ++i)
        {
            static const char* ids[] = { "take", "record", "pitch", "morph", "calibrate", "miceq" };
            auto showTool = [i] (MainComponent& m)
            {
                m.showPage (Navigator::Page::tools);
                if (auto* v = dynamic_cast<ToolsView*> (findById (&m, "page.tools"))) v->showTool (ToolsView::Tool (i));
            };
            shot ("S10-" + juce::String (ids[i]) + t, W, H, showTool);
            if (i == 0) shot ("S10-" + juce::String (ids[i]) + "-min" + t, w, h, showTool);
        }
        // wave9/share: S-06 in the narrow window (the share code buttons must still fit)
        shot ("S06-min" + t, w, h, [] (MainComponent& m) { m.showPresetBrowser(); });
        // wave8/morph: 混ぜる while blending 魔王 x エイリアン at 40 %, driven through the tool's own controls
        auto morphing = [&c] (MainComponent& m)
        {
            m.showPage (Navigator::Page::tools);
            if (auto* v = dynamic_cast<ToolsView*> (findById (&m, "page.tools"))) v->showTool (ToolsView::Tool::morph);
            auto pick = [&m] (const char* id, const char* name)
            {
                if (auto* box = dynamic_cast<juce::ComboBox*> (findById (&m, id)))
                    for (int k = 0; k < box->getNumItems(); ++k)
                        if (box->getItemText (k) == juce::String::fromUTF8 (name)) box->setSelectedItemIndex (k, juce::sendNotificationSync);
            };
            pick ("morph.a", "魔王");
            pick ("morph.b", "エイリアン");
            if (auto* b = dynamic_cast<juce::Button*> (findById (&m, "morph.start")); b != nullptr && b->onClick) b->onClick();
            if (auto* s = dynamic_cast<juce::Slider*> (findById (&m, "morph.amount"))) s->setValue (0.4, juce::sendNotificationSync);
            c.dispatchPendingMessages();
        };
        shot ("S10-morph-active" + t, W, H, morphing);
        shot ("S10-morph-active-min" + t, w, h, morphing);
        c.loadPreset ("character-demon-king");
        // wave8/automation: the S-03 cards 押している間のエフェクト / アプリごとの自動切り替え, found by the search (one set recipe, two rules)
        {
            const auto before = c.getSettings();
            c.updateSettings ([] (Settings& s)
            {
                s.momentaryRecipes = { "yamabiko" };
                s.appSwitchRules = { { "VALORANT.exe", "character-demon-king" }, { "Discord.exe", "natural-asis" } };
            });
            for (auto [q, name] : { std::pair { "押している間", "momentary" }, std::pair { "自動切り替え", "appswitch" } })
                for (const bool narrow : { false, true })
                    shot ("S03-search-" + juce::String (name) + (narrow ? "-min" : "") + t, narrow ? w : W, narrow ? h : H, [q] (MainComponent& m)
                    {
                        m.showSettings (Navigator::SettingsSection::devices);
                        if (auto* v = dynamic_cast<SettingsView*> (findById (&m, "page.settings"))) v->setSearchText (juce::String::fromUTF8 (q));
                    });
            c.updateSettings ([&before] (Settings& s)
            {
                s.momentaryRecipes = before.momentaryRecipes;
                s.appSwitchRules = before.appSwitchRules;
            });
        }
        // wave8/capture: 試し録り with a take (ready / playing) and 録音 (recording / done). No device: the test hook stands in.
        {
            CaptureData::assumeDevicesRunningForTests = true;
            const auto monitorBefore = c.getSettings().monitorDevice;
            c.updateSettings ([] (Settings& s) { s.monitorDevice = "Headphones"; });
            auto& vp = c.getProcessorForTests();
            std::vector<float> in (480), out (480);
            double phase = 0.0;
            auto feed = [&] (double seconds)
            {
                for (int b = 0; b < int (seconds * vp.getSampleRate() / 480.0); ++b)
                {
                    for (auto& x : in) { x = 0.3f * float (std::sin (phase)); phase += 2.0 * juce::MathConstants<double>::pi * 180.0 / vp.getSampleRate(); }
                    vp.process (in.data(), out.data(), nullptr, 480);
                }
            };
            auto showTool = [] (ToolsView::Tool tool)
            {
                return [tool] (MainComponent& m)
                {
                    m.showPage (Navigator::Page::tools);
                    if (auto* v = dynamic_cast<ToolsView*> (findById (&m, "page.tools"))) v->showTool (tool);
                };
            };
            juce::String why;
            c.startTestTake (why);
            feed (4.2);
            c.stopTestTake();
            shot ("S10-take-ready" + t, W, H, showTool (ToolsView::Tool::take));
            c.playTestTake (why);
            feed (1.3);
            shot ("S10-take-playing" + t, W, H, showTool (ToolsView::Tool::take));
            shot ("S10-take-playing-min" + t, w, h, showTool (ToolsView::Tool::take));
            c.clearTestTake();
            c.startWavRecording (why);
            feed (3.0);
            shot ("S10-record-on" + t, W, H, showTool (ToolsView::Tool::record));
            shot ("S10-record-on-min" + t, w, h, showTool (ToolsView::Tool::record));
            c.stopWavRecording();
            shot ("S10-record-done" + t, W, H, showTool (ToolsView::Tool::record));
            c.updateSettings ([monitorBefore] (Settings& s) { s.monitorDevice = monitorBefore; });
            CaptureData::assumeDevicesRunningForTests = false;
        }
        // wave8/analysis: 声の高さ with readings (a gliding tone through 魔王), 音量合わせ while recording, failed and calibrated
        {
            auto showAnalysisTool = [] (MainComponent& m, ToolsView::Tool tool)
            {
                m.showPage (Navigator::Page::tools);
                if (auto* v = dynamic_cast<ToolsView*> (findById (&m, "page.tools"))) v->showTool (tool);
            };
            auto pitchLive = [&c, showAnalysisTool] (MainComponent& m)
            {
                showAnalysisTool (m, ToolsView::Tool::pitch);
                auto* timer = dynamic_cast<juce::Timer*> (findById (&m, "tools.pitch"));
                if (timer == nullptr) return;
                std::vector<float> in (1600), out (1600);
                double phase = 0.0;
                for (int f = 0; f < 300; ++f)
                {
                    const bool pause = (f > 95 && f < 125) || (f > 215 && f < 235);
                    const double hz = 110.0 + 150.0 * (0.5 - 0.5 * std::cos (f * 0.035));
                    for (auto& v : in)
                    {
                        phase += juce::MathConstants<double>::twoPi * hz / c.getProcessorForTests().getSampleRate();
                        v = pause ? 0.0f : float (0.3 * std::sin (phase));
                    }
                    c.getProcessorForTests().process (in.data(), out.data(), nullptr, int (in.size()));
                    timer->timerCallback();
                }
            };
            shot ("S10-pitch-live" + t, W, H, pitchLive);
            shot ("S10-pitch-live-min" + t, w, h, pitchLive);
            AnalysisData::testDevicesRunning = true; // the recording state needs a running input
            auto takeOf = [&c] (float seconds, float amplitude)
            {
                std::vector<float> in (480), out (480);
                for (int k = 0; k < int (seconds * 100); ++k)
                {
                    for (size_t i = 0; i < in.size(); ++i) in[i] = amplitude * float (std::sin (0.03 * double (k * 480 + int (i))));
                    c.getProcessorForTests().process (in.data(), out.data(), nullptr, int (in.size()));
                }
                c.tickForTests();
            };
            auto showCalibrate = [showAnalysisTool] (MainComponent& m) { showAnalysisTool (m, ToolsView::Tool::calibrate); };
            juce::String why;
            c.startCalibration (why);
            takeOf (3.6f, 0.2f);
            shot ("S10-calibrate-recording" + t, W, H, showCalibrate);
            shot ("S10-calibrate-recording-min" + t, w, h, showCalibrate);
            takeOf (7.0f, 0.0f); // 3.6 s of tone is enough voice: measuring starts
            for (int k = 0; k < 3000 && c.getCalibrationState().progress < 0.3f; ++k) juce::Thread::sleep (10);
            shot ("S10-calibrate-analysing-min" + t, w, h, showCalibrate);
            c.cancelCalibration();
            for (int k = 0; k < 3000 && ! c.startCalibration (why); ++k) { juce::Thread::sleep (10); c.tickForTests(); }
            takeOf (10.6f, 0.0f); // silence: fails with the reason
            shot ("S10-calibrate-failed-min" + t, w, h, showCalibrate);
            c.clearCalibration(); // back to idle
            AnalysisData::testDevicesRunning = false;
            c.updateSettings ([] (Settings& s)
            {
                s.calibratedTrimDb = { { "character-demon-king", -2.5f }, { "character-helium", 1.5f }, { "natural-asis", 0.0f } };
                s.calibratedAt = "2026-10-04T18:30:00.000+09:00";
            });
            shot ("S10-calibrate-done-min" + t, w, h, showCalibrate);
            c.clearCalibration();
        }
        // wave9/overlay: S11-overlay = the overlay's content alone (no window) on the page colour: usual, long name, OFF, muted;
        // and the S-03 card 画面の端に今の声を表示 found by the search
        {
            const OverlayState states[] = { { juce::String::fromUTF8 ("魔王"), true, false },
                                            { juce::String::fromUTF8 ("とても長い名前のユーザープリセット・配信用の低い声"), true, false },
                                            { juce::String::fromUTF8 ("ヘリウム"), false, false },
                                            { juce::String::fromUTF8 ("ロボット"), true, true } };
            constexpr int gap = Theme::space4;
            juce::Image sheet (juce::Image::ARGB, VoiceOverlayView::maxWidth + gap * 2, (VoiceOverlayView::preferredHeight() + gap) * 4 + gap, true);
            {
                juce::Graphics g (sheet);
                g.fillAll (Theme::colours().bg);
                int y = gap;
                for (auto& st : states)
                {
                    VoiceOverlayView view;
                    view.setState (st);
                    view.setSize (view.preferredWidth(), VoiceOverlayView::preferredHeight());
                    g.drawImageAt (view.createComponentSnapshot (view.getLocalBounds(), true, 1.0f), gap, y);
                    y += view.getHeight() + gap;
                }
            }
            auto file = outputDir.getChildFile ("S11-overlay" + t + ".png");
            file.deleteFile();
            juce::FileOutputStream out (file);
            if (out.openedOk() && juce::PNGImageFormat().writeImageToStream (sheet, out)) ++written;
            c.updateSettings ([] (Settings& s) { s.overlayOn = true; });
            for (const bool narrow : { false, true })
                shot ("S03-search-overlay" + juce::String (narrow ? "-min" : "") + t, narrow ? w : W, narrow ? h : H, [] (MainComponent& m)
                {
                    m.showSettings (Navigator::SettingsSection::devices);
                    if (auto* v = dynamic_cast<SettingsView*> (findById (&m, "page.settings"))) v->setSearchText (juce::String::fromUTF8 ("画面の端"));
                });
            c.updateSettings ([] (Settings& s) { s.overlayOn = false; });
        }
        // wave9/voice: マイク補正 while recording, and after a measurement (a muffled mic: lows cut, presence lifted)
        {
            MicEqData::testDevicesRunning = true; // the recording state needs a running input
            juce::String why;
            c.startMicEqMeasure (why);
            std::vector<float> in (480), out (480);
            for (int k = 0; k < 360; ++k)
            {
                for (size_t i = 0; i < in.size(); ++i) in[i] = 0.2f * float (std::sin (0.03 * double (k * 480 + int (i))));
                c.getProcessorForTests().process (in.data(), out.data(), nullptr, int (in.size()));
            }
            c.tickForTests();
            auto showRecording = [] (MainComponent& m)
            {
                m.showPage (Navigator::Page::tools);
                if (auto* v = dynamic_cast<ToolsView*> (findById (&m, "page.tools"))) v->showTool (ToolsView::Tool::micEq);
            };
            shot ("S10-miceq-recording-min" + t, w, h, showRecording);
            c.cancelMicEqMeasure();
            MicEqData::testDevicesRunning = false;
            c.updateSettings ([] (Settings& s)
            {
                s.micEqGainsDb = { -2.5f, -2.0f, -1.2f, -0.6f, -0.4f, -0.5f, -0.2f, 0.4f, 1.3f, 2.6f, 3.1f, 1.8f, 0.4f, -1.0f };
                s.micEqAt = "2026-10-04T21:15:00.000+09:00";
                s.micEqOn = true;
            });
            auto showMicEq = [] (MainComponent& m)
            {
                m.showPage (Navigator::Page::tools);
                if (auto* v = dynamic_cast<ToolsView*> (findById (&m, "page.tools"))) v->showTool (ToolsView::Tool::micEq);
            };
            shot ("S10-miceq-done" + t, W, H, showMicEq);
            shot ("S10-miceq-done-min" + t, w, h, showMicEq);
            c.clearMicEq();
        }
        // wave9/stream: the S-03 card 配信用の出力, found by the search, running and with a missing device (test outputs, no device)
        {
            StreamData::outputsForTests = { "Headphones (USB Audio)", "CABLE-A Input (VB-Audio Cable A)" };
            const auto search = [] (MainComponent& m)
            {
                m.showSettings (Navigator::SettingsSection::devices);
                if (auto* v = dynamic_cast<SettingsView*> (findById (&m, "page.settings"))) v->setSearchText (juce::String::fromUTF8 ("配信"));
            };
            c.setStreamDevice ("CABLE-A Input (VB-Audio Cable A)");
            shot ("S03-search-stream" + t, W, H, search);
            shot ("S03-search-stream-min" + t, w, h, search);
            c.setStreamDevice ("USB Speaker (unplugged)");
            shot ("S03-search-stream-lost" + t, W, H, search);
            c.setStreamDevice ({});
            StreamData::outputsForTests.clear();
        }
        for (auto& [section, id] : sections)
            shot ("S03-" + juce::String (id) + t, W, H, [section] (MainComponent& m) { m.showSettings (section); });
        // wave 4: 「詳細な設定」 open, and a search across the sections
        c.updateSettings ([] (Settings& s) { s.settingsShowDetails = true; });
        for (auto section : { Navigator::SettingsSection::environment, Navigator::SettingsSection::startup, Navigator::SettingsSection::appearance })
            shot ("S03-" + juce::String (sections[int (section)].second) + "-details" + t, W, H, [section] (MainComponent& m) { m.showSettings (section); });
        shot ("S03-devices-details-min" + t, w, h, [] (MainComponent& m) { m.showSettings (Navigator::SettingsSection::devices); });        c.updateSettings ([] (Settings& s) { s.settingsShowDetails = false; });
        for (const char* q : { "遅延", "PTT" })
            shot ("S03-search-" + juce::String (q[0] == 'P' ? "ptt" : "latency") + t, W, H, [q] (MainComponent& m)
            {
                m.showSettings (Navigator::SettingsSection::devices);
                if (auto* v = dynamic_cast<SettingsView*> (findById (&m, "page.settings"))) v->setSearchText (juce::String::fromUTF8 (q));
            });
        shot ("S06" + t, W, H, [] (MainComponent& m) { m.showPresetBrowser(); });
        shot ("S07" + t, W, H, [] (MainComponent& m) { m.showEffectPicker (-1); });
        shot ("S07-min" + t, w, h, [] (MainComponent& m) { m.showEffectPicker (-1); });
        shot ("S09" + t, W, H, [] (MainComponent& m) { m.showSlotDetail (0); });
        shot ("S09-min" + t, w, h, [] (MainComponent& m) { m.showSlotDetail (0); });
        // wave9/voice: S-09 with 声の大きさで動かす set (the first numeric knob of slot 0 at +60 %), the meter fed by a voice-level tone
        if (! c.getChain().empty())
            if (const auto* modInfo = findEffectInfo (c.getChain()[0].type))
            {
                std::string modTarget = "wet";
                for (auto& spec : modInfo->params)
                    if (! spec.isChoice()) { modTarget = spec.id; break; }
                c.setSlotMod (0, modTarget, 0.6f);
                auto& vp = c.getProcessorForTests();
                std::vector<float> in (480), out (480);
                for (int b = 0; b < 50; ++b)
                {
                    for (size_t i = 0; i < in.size(); ++i) in[i] = 0.1f * float (std::sin (0.04 * double (b * 480 + int (i))));
                    vp.process (in.data(), out.data(), nullptr, 480);
                }
                auto showMod = [] (MainComponent& m) { m.showSlotDetail (0); }; // the meter reads the level when the panel opens
                shot ("S09-mod" + t, W, H, showMod);
                shot ("S09-mod-min" + t, w, h, showMod);
                c.loadPreset ("character-demon-king");
            }
        shot ("S04" + t, W, H, [] (MainComponent& m) { m.showSetupWizard(); });
        shot ("S04-step2" + t, W, H, [] (MainComponent& m)
        {
            m.showSetupWizard();
            if (auto* next = dynamic_cast<juce::Button*> (findById (&m, "setup.next"))) next->onClick(); // 2 / 3 (the update question)
        });
        shot ("S08-tour4" + t, W, H, [&c] (MainComponent& m)
        {
            c.updateSettings ([] (Settings& s) { s.tourStep = 3; });
            m.startTour (true);
        });
        // wave5/mono: 案 C Mono (layoutStyle 2) in the default palette and in the Mono palette (themeId, wave5/themes)
        for (const char* themeId : { "", "builtin:mono" })
        {
            c.updateSettings ([themeId] (Settings& s) { s.layoutStyle = 2; s.themeId = juce::String (themeId); });
            const juce::String pal = *themeId != 0 ? "-mono" : "";
            shot ("S01-mono-wide" + pal + t, W, H, {});
            shot ("S01-mono-min" + pal + t, w, h, {});
            shot ("S01-mono-min-presets" + pal + t, w, h, [] (MainComponent& m)
            {
                if (auto* b = dynamic_cast<juce::Button*> (findById (&m, "voice.presetSelector")); b != nullptr && b->isVisible()) b->onClick();
            });
        }
        c.updateSettings ([] (Settings& s) { s.layoutStyle = 0; s.themeId = {}; });
    }

    // wave5/paper: S-01 in 案 B Paper (layoutStyle 1), after the default set so those stay as they were
    c.loadPreset ("character-demon-king");
    c.updateSettings ([] (Settings& s) { s.layoutStyle = 1; });
    for (const bool dark : { true, false })
    {
        Theme::setDark (dark);
        lnf.refreshColours();
        c.updateSettings ([dark] (Settings& s) { s.darkTheme = dark; });
        const juce::String t = dark ? "-dark" : "-light";
        shot ("S01-paper-wide" + t, Theme::defaultWidth, Theme::defaultHeight, {});
        shot ("S01-paper-min" + t, Theme::minWidth, Theme::minHeight, {});
    }
    // ... and in its own Paper palette (wave5/themes); Studio's A layout in the Paper / Mono palettes
    c.updateSettings ([] (Settings& s) { s.themeId = "builtin:paper"; });
    shot ("S01-paper-wide-paper", Theme::defaultWidth, Theme::defaultHeight, {});
    shot ("S01-paper-min-paper", Theme::minWidth, Theme::minHeight, {});
    c.updateSettings ([] (Settings& s) { s.layoutStyle = 0; });
    shot ("S01-wide-paper", Theme::defaultWidth, Theme::defaultHeight, {});
    c.updateSettings ([] (Settings& s) { s.themeId = "builtin:mono"; });
    shot ("S01-wide-mono", Theme::defaultWidth, Theme::defaultHeight, {});
    c.updateSettings ([] (Settings& s) { s.layoutStyle = 0; s.themeId = {}; });
    Theme::clearPalette();

    // wave7/ir: S-09 of a "convolution" slot (file row): a file in use, and a file that is missing
    paths::irDir().createDirectory();
    writeSampleWav (paths::irDir().getChildFile (juce::String::fromUTF8 ("ホール.wav")), 1.5, 220.0);
    for (const bool dark : { true, false })
    {
        Theme::setDark (dark);
        lnf.refreshColours();
        c.updateSettings ([dark] (Settings& s) { s.darkTheme = dark; });
        const juce::String t = dark ? "-dark" : "-light";
        c.loadPreset ("character-demon-king");
        juce::String why;
        c.addEffect ("convolution", why);
        const int irSlot = int (c.getChain().size()) - 1;
        c.setSlotFileName (irSlot, juce::String::fromUTF8 ("ホール.wav"));
        shot ("S09-convolution" + t, Theme::defaultWidth, Theme::defaultHeight, [irSlot] (MainComponent& m) { m.showSlotDetail (irSlot); });
        c.setSlotFileName (irSlot, juce::String::fromUTF8 ("消えた残響.wav"));
        shot ("S09-convolution-missing" + t, Theme::minWidth, Theme::minHeight, [irSlot] (MainComponent& m) { m.showSlotDetail (irSlot); });
    }
    c.loadPreset ("character-demon-king");

    juce::LookAndFeel::setDefaultLookAndFeel (nullptr);
    Theme::setDark (wasDark);
    Theme::setVariant (wasAccent, wasTone);
    animationsOff() = wasOff;
    return written;
}
} // namespace koe::ui

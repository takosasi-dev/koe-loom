#include "UI/MainComponent.h"

#include "Core/Paths.h"
#include "UI/Screens.h"
#include "UI/ThemeLibrary.h"
#include "UI/main/GuideTour.h"
#include "UI/main/Panels.h"
#include "UI/main/Shell.h"
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

    std::vector<juce::Component*> pages() const { return { voice.get(), sound.get(), settings.get() }; }

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

    juce::LookAndFeel::setDefaultLookAndFeel (nullptr);
    Theme::setDark (wasDark);
    Theme::setVariant (wasAccent, wasTone);
    animationsOff() = wasOff;
    return written;
}
} // namespace koe::ui

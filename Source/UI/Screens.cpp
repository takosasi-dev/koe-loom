// ui-screens (wave 2). The screens live in UI/screens/: SettingsView (S-03), SoundboardView (S-02),
// PresetBrowser (S-06), SetupWizard (S-04). This file holds the [?] menu pages.
#include "UI/Screens.h"
#include "UI/screens/Common.h"

namespace koe::ui
{
using namespace screens;

namespace
{
// License texts reproduced from third_party/ (binary redistributions must carry them).
const char* const kRnnoiseCopyright = "Copyright (c) 2007-2017, 2024 Jean-Marc Valin\n"
                                      "Copyright (c) 2023 Amazon\n"
                                      "Copyright (c) 2017, Mozilla\n"
                                      "Copyright (c) 2005-2017, Xiph.Org Foundation\n"
                                      "Copyright (c) 2003-2004, Mark Borgerding";

const char* const kBsd3 =
    "Redistribution and use in source and binary forms, with or without modification, are permitted provided that the following conditions are met:\n\n"
    "- Redistributions of source code must retain the above copyright notice, this list of conditions and the following disclaimer.\n\n"
    "- Redistributions in binary form must reproduce the above copyright notice, this list of conditions and the following disclaimer in the "
    "documentation and/or other materials provided with the distribution.\n\n"
    "- Neither the name of the Xiph.Org Foundation nor the names of its contributors may be used to endorse or promote products derived from this "
    "software without specific prior written permission.\n\n"
    "THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED "
    "TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE FOUNDATION OR "
    "CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, "
    "PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF "
    "LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS "
    "SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.";

const char* const kMit =
    "Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated documentation files (the "
    "\"Software\"), to deal in the Software without restriction, including without limitation the rights to use, copy, modify, merge, publish, "
    "distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is furnished to do so, subject to the "
    "following conditions:\n\n"
    "The above copyright notice and this permission notice shall be included in all copies or substantial portions of the Software.\n\n"
    "THE SOFTWARE IS PROVIDED \"AS IS\", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF "
    "MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY "
    "CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE "
    "SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.";

void addSteps (VStack& v, const juce::StringArray& steps)
{
    for (int i = 0; i < steps.size(); ++i)
    {
        auto s = std::make_unique<NumberedStep> (i + 1, steps[i]);
        auto* raw = s.get();
        v.add (std::move (s), [raw] (int w) { return raw->heightForWidth (w); });
    }
}

/** One entry of the license page: name, license, use, copyright lines, a note and (optionally) the full text. */
void addLicense (VStack& v, const juce::String& name, const juce::String& use, const juce::String& license, const juce::String& copyright,
                 const juce::String& note, const juce::String& fullText = {})
{
    auto e = std::make_unique<VStack> (Theme::space1);
    e->addText (name, Theme::fontM, Tone::text, true);
    e->addText (license, Theme::fontS);
    e->addText (use, Theme::fontXS, Tone::sub);
    for (auto* text : { &copyright, &fullText })
        if (text->isNotEmpty())
        {
            auto l = std::make_unique<TextLabel> (*text, Theme::fontXS, text == &copyright ? Tone::text : Tone::sub, false, true);
            auto* raw = l.get();
            e->add (std::move (l), [raw] (int w) { return raw->heightForWidth (w); });
        }
    if (note.isNotEmpty()) e->addText (note, Theme::fontXS, Tone::sub);
    auto* raw = e.get();
    v.add (std::move (e), [raw] (int w) { return raw->heightForWidth (w); });
}

void buildDiscordSetup (VStack& v)
{
    v.addText (ja ("KoeLoom の声を Discord で使うための設定です。出力先（設定の「デバイス」）は「CABLE Input」（Windows によっては「スピーカー (VB-Audio Virtual Cable)」と表示されます）にしておきます。"), Theme::fontS, Tone::sub);
    addSteps (v, discordSteps());
    v.addText (discordRecommended(), Theme::fontS);
    v.addText (ja ("Discord の画面の項目名は、Discord の更新で変わることがあります。"), Theme::fontXS, Tone::sub);
}

void buildRevertMic (VStack& v)
{
    auto& lead = v.addText (revertMicText(), Theme::fontS, Tone::text, true);
    lead.setIcon (Icon::mic);
    v.addText (ja ("KoeLoom を終了すると、仮想マイク（CABLE Output）には声が届かなくなります。Discord やゲーム内ボイス、会議アプリの入力デバイスを、元のマイクに戻してください。"),
               Theme::fontS, Tone::sub);
    addSteps (v, { ja ("Discord を開き、ユーザー設定の「音声・ビデオ」を選びます。"),
                   ja ("「入力デバイス」を、ふだん使っているマイク（または「Default」）に戻します。"),
                   ja ("ほかのアプリでも「CABLE Output」を選んでいた場合は、同じように戻します。") });
    v.addText (ja ("終了するときの確認（トレイの「終了」）は、設定の「起動と常駐」で出す・出さないを選べます。"), Theme::fontXS, Tone::sub);
}

void buildLicenses (VStack& v)
{
    addLicense (v, "KoeLoom", ja ("このソフト"), ja ("GNU Affero General Public License v3.0（AGPL-3.0）"), {},
                ja ("このソフトは AGPL-3.0 で公開しています。全文は、配布物とリポジトリの LICENSE にあります（https://www.gnu.org/licenses/agpl-3.0.html）。"
                        "JUCE を AGPLv3 で使うため、このライセンスを選んでいます。"));
    addLicense (v, "JUCE 8.0.6", ja ("アプリの土台（音声の入出力、画面）"), ja ("AGPLv3 と商用のデュアルライセンス。このソフトは AGPLv3 を選んでいます"),
                "Copyright (c) Raw Material Software Limited",
                ja ("JUCE には、FLAC・Ogg Vorbis（BSD）、zlib・libpng（zlib）、HarfBuzz（Old MIT）、SheenBidi（Apache 2.0）などが含まれます。それぞれのライセンスは、JUCE の LICENSE.md の一覧にあります。"));
    addLicense (v, "RNNoise", ja ("ノイズ抑制"), "BSD-3-Clause", kRnnoiseCopyright, {}, kBsd3);
    addLicense (v, "Signalsmith Stretch", ja ("声の変換（ピッチ）"), "MIT License", "Copyright (c) 2022 Geraint Luff / Signalsmith Audio Ltd.", {}, kMit);
    addLicense (v, "Signalsmith Linear", ja ("Signalsmith Stretch が使う計算ライブラリ"), "MIT License", "Copyright (c) 2025 Signalsmith Audio", {}, kMit);
    v.addText (ja ("画面の文字（Yu Gothic UI、Consolas）は Windows に入っているフォントを使い、このソフトには含めていません。"), Theme::fontXS, Tone::sub);
}
} // namespace

std::unique_ptr<juce::Component> createHelpPanel (Navigator::HelpTopic topic, AppController&, Navigator& nav)
{
    const char* titles[] = { "Discord の設定手順", "元のマイクに戻す方法", "ライセンス表示" };
    const char* ids[] = { "help.discordSetup", "help.revertMic", "help.licenses" };
    const int t = juce::jlimit (0, 2, int (topic));
    auto panel = std::make_unique<OverlayPanel> (ja (titles[t]), [np = &nav] { np->closeOverlay(); });
    panel->setComponentID (ids[t]);
    panel->setSize (Theme::space5 * 20, Theme::space5 * 16);
    auto v = std::make_unique<VStack> (topic == Navigator::HelpTopic::licenses ? Theme::space4 : Theme::space3 - Theme::space1);
    switch (topic)
    {
        case Navigator::HelpTopic::discordSetup: buildDiscordSetup (*v); break;
        case Navigator::HelpTopic::revertMic: buildRevertMic (*v); break;
        case Navigator::HelpTopic::licenses: buildLicenses (*v); break;
    }
    auto* raw = v.get();
    // as tall as the text needs, up to the preferred size (the content scrolls beyond that)
    const int chrome = Theme::space3 * 3 + Theme::controlH;
    const int textW = panel->getWidth() - Theme::space4 * 2 - Theme::space3;
    panel->setSize (panel->getWidth(), juce::jmin (panel->getHeight(), chrome + raw->heightForWidth (textW) + Theme::space3));
    panel->setContent (std::move (v), [raw] (int w) { return raw->heightForWidth (w); });
    return panel;
}
} // namespace koe::ui

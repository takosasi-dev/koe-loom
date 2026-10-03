#include "Effects/EffectRegistry.h"

#include <map>
#include <string>

// Code copy of koeloom_effects.md §2 (list) and §3 (parameter table, the 正本). Keep in sync.

namespace koe
{
namespace
{
using C = EffectCategory;
using W = EffectWeight;

std::vector<EffectInfo> buildInfos()
{
    std::vector<EffectInfo> v;

    v.push_back ({ "compressor", "コンプレッサー", C::dynamics, W::light, 2, "音量の大小差をそろえる",
                   { { "thresholdDb", "しきい値", -60, 0, -24, "dB" },
                     { "ratio", "レシオ", 1, 20, 3, ":1" },
                     { "attackMs", "アタック", 1, 100, 10, "ms" },
                     { "releaseMs", "リリース", 10, 1000, 120, "ms" },
                     { "makeupDb", "メイクアップ", 0, 24, 0, "dB" } } });

    v.push_back ({ "deesser", "ディエッサー", C::dynamics, W::light, 2, "「さ行」の刺さる高音（歯擦音）を抑える",
                   { { "freqHz", "対象の周波数", 3000, 10000, 6500, "Hz" },
                     { "thresholdDb", "しきい値", -60, 0, -30, "dB" },
                     { "reductionDb", "最大の抑制量", 0, 24, 6, "dB" } } });

    v.push_back ({ "enhancer", "エンハンサー", C::dynamics, W::light, 2, "高域に倍音を足し、声を前に出す",
                   { { "freqHz", "処理する帯域の下限", 1000, 8000, 3000, "Hz" },
                     { "amount", "量", 0, 1, 0.4f, "" },
                     { "mix", "ミックス", 0, 1, 0.5f, "" } } });

    v.push_back ({ "eq", "EQ（簡易）", C::eqFilter, W::light, 2, "ハイパス、ローパス、低域・中域・高域の調整",
                   { { "hpfHz", "ハイパス", 20, 1000, 20, "Hz" },
                     { "lpfHz", "ローパス", 1000, 20000, 20000, "Hz" },
                     { "lowDb", "低域（150 Hz シェルフ）", -12, 12, 0, "dB" },
                     { "midDb", "中域", -12, 12, 0, "dB" },
                     { "midHz", "中域の中心", 300, 4000, 1000, "Hz" },
                     { "highDb", "高域（6 kHz シェルフ）", -12, 12, 0, "dB" } } });

    v.push_back ({ "peq", "パラメトリックEQ", C::eqFilter, W::light, 2, "低域・中域2つ・高域を、周波数と幅まで指定して調整する",
                   { { "lowDb", "低域", -12, 12, 0, "dB" },
                     { "lowHz", "低域の周波数", 40, 500, 120, "Hz" },
                     { "mid1Db", "中域1", -12, 12, 0, "dB" },
                     { "mid1Hz", "中域1の周波数", 200, 2000, 500, "Hz" },
                     { "mid1Q", "中域1のQ", 0.3f, 8, 1, "" },
                     { "mid2Db", "中域2", -12, 12, 0, "dB" },
                     { "mid2Hz", "中域2の周波数", 800, 8000, 3000, "Hz" },
                     { "mid2Q", "中域2のQ", 0.3f, 8, 1, "" },
                     { "highDb", "高域", -12, 12, 0, "dB" },
                     { "highHz", "高域の周波数", 2000, 16000, 8000, "Hz" } } });

    v.push_back ({ "autowah", "オートワウ", C::eqFilter, W::light, 2, "声の大きさに合わせてフィルターが開閉する「ワウワウ」",
                   { { "sensitivity", "感度", 0, 1, 0.5f, "" },
                     { "baseHz", "基準の周波数", 200, 1500, 400, "Hz" },
                     { "rangeOct", "動く幅", 0.5f, 4, 2, "oct" },
                     { "q", "Q", 0.5f, 10, 3, "" },
                     { "mix", "ミックス", 0, 1, 0.7f, "" } } });

    v.push_back ({ "filtersweep", "フィルタースイープ", C::eqFilter, W::light, 2, "フィルターを周期的に動かす",
                   { { "mode", "タイプ", 0, 2, 0, "", { { "lowpass", "ローパス" }, { "bandpass", "バンドパス" }, { "highpass", "ハイパス" } } },
                     { "rateHz", "レート", 0.05f, 10, 0.5f, "Hz" },
                     { "baseHz", "基準の周波数", 200, 4000, 800, "Hz" },
                     { "depthOct", "動く幅", 0, 4, 2, "oct" },
                     { "q", "Q", 0.5f, 10, 2, "" },
                     { "mix", "ミックス", 0, 1, 1, "" } } });

    v.push_back ({ "isolator", "アイソレーター", C::eqFilter, W::light, 2, "低域・中域・高域を個別に絞って（最大 -60 dB）消す",
                   { { "lowDb", "低域", -60, 6, 0, "dB" },
                     { "midDb", "中域", -60, 6, 0, "dB" },
                     { "highDb", "高域", -60, 6, 0, "dB" },
                     { "lowMidHz", "低域と中域の境目", 100, 1000, 300, "Hz" },
                     { "midHighHz", "中域と高域の境目", 1000, 8000, 2500, "Hz" } } });

    v.push_back ({ "formantfilter", "母音フィルター", C::eqFilter, W::medium, 2, "「あいうえお」の母音の響きをフィルターで作り、声をうならせる",
                   { { "vowel", "母音の位置（0=あ 1=い 2=う 3=え 4=お）", 0, 4, 0, "" },
                     { "lfoRateHz", "自動で動かす速さ（0 で固定）", 0, 10, 0, "Hz" },
                     { "depth", "動く幅", 0, 1, 0.5f, "" },
                     { "mix", "ミックス", 0, 1, 1, "" } } });

    v.push_back ({ "saturator", "サチュレーター", C::distortion, W::light, 2, "テープや真空管のような、やわらかい飽和",
                   { { "mode", "タイプ", 0, 1, 0, "", { { "tape", "テープ" }, { "tube", "真空管" } } },
                     { "driveDb", "ドライブ", 0, 24, 6, "dB" },
                     { "toneHz", "トーン（ローパス）", 1000, 16000, 8000, "Hz" },
                     { "mix", "ミックス", 0, 1, 1, "" } } });

    v.push_back ({ "distortion", "ディストーション", C::distortion, W::light, 2, "オーバードライブ、ディストーション、ファズの3タイプ",
                   { { "shape", "タイプ", 0, 2, 1, "", { { "overdrive", "オーバードライブ" }, { "distortion", "ディストーション" }, { "fuzz", "ファズ" } } },
                     { "driveDb", "ドライブ", 0, 36, 12, "dB" },
                     { "toneHz", "トーン（ローパス）", 1000, 12000, 6000, "Hz" },
                     { "mix", "ミックス", 0, 1, 0.5f, "" } } });

    v.push_back ({ "bitcrusher", "ビットクラッシャー", C::distortion, W::light, 2, "ビット深度とサンプルレートを落として荒くする",
                   { { "bits", "ビット深度", 4, 16, 16, "bit", {}, true },
                     { "rateHz", "実効サンプルレート", 2000, 48000, 48000, "Hz" },
                     { "mix", "ミックス", 0, 1, 1, "" } } });

    v.push_back ({ "noise", "ノイズ付加", C::distortion, W::light, 2, "ヒスや無線ノイズを混ぜる（音量は -6 dB まで）",
                   { { "kind", "種類", 0, 4, 2, "", { { "white", "ホワイト" }, { "pink", "ピンク" }, { "hiss", "ヒス" }, { "crackle", "クラックル" }, { "static", "無線ノイズ" } } },
                     { "levelDb", "音量", -60, -6, -40, "dB" },
                     { "toneHz", "トーン（ローパス）", 500, 16000, 8000, "Hz" },
                     { "followVoice", "声に連動する度合い", 0, 1, 0, "" } } });

    v.push_back ({ "voicechar", "ボイスキャラクター", C::distortion, W::light, 2, "電話、ラジオ、拡声器、トランシーバーの音を1つで作る",
                   { { "kind", "種類", 0, 3, 0, "", { { "telephone", "電話" }, { "radio", "ラジオ" }, { "megaphone", "拡声器" }, { "walkie", "トランシーバー" } } },
                     { "intensity", "かかり具合", 0, 1, 0.5f, "" },
                     { "mix", "ミックス", 0, 1, 1, "" } } });

    v.push_back ({ "ringmod", "ロボ（リングモジュレーション）", C::distortion, W::light, 2, "金属的でロボットのような声にする",
                   { { "freqHz", "周波数", 10, 2000, 70, "Hz" },
                     { "mix", "ミックス", 0, 1, 0.5f, "" } } });

    v.push_back ({ "modulation", "モジュレーション", C::modulation, W::medium, 2, "コーラス、フランジャー、ビブラートの3タイプ",
                   { { "mode", "タイプ", 0, 2, 0, "", { { "chorus", "コーラス" }, { "flanger", "フランジャー" }, { "vibrato", "ビブラート" } } },
                     { "rateHz", "レート", 0.05f, 10, 1, "Hz" },
                     { "depth", "深さ", 0, 1, 0.5f, "" },
                     { "feedback", "フィードバック（フランジャーのみ）", 0, 0.9f, 0, "" },
                     { "mix", "ミックス", 0, 1, 0.5f, "" } } });

    v.push_back ({ "phaser", "フェイザー", C::modulation, W::medium, 2, "位相をずらして、うねるような音にする",
                   { { "stages", "段数", 2, 12, 4, "段", {}, true },
                     { "rateHz", "レート", 0.05f, 10, 0.5f, "Hz" },
                     { "depth", "深さ", 0, 1, 0.6f, "" },
                     { "feedback", "フィードバック", 0, 0.9f, 0.3f, "" },
                     { "mix", "ミックス", 0, 1, 0.5f, "" } } });

    v.push_back ({ "rotary", "ロータリー", C::modulation, W::medium, 2, "回転スピーカー（ドップラーと音量の揺れ）",
                   { { "rateHz", "回転の速さ", 0.5f, 10, 6, "Hz" },
                     { "depth", "深さ", 0, 1, 0.6f, "" },
                     { "mix", "ミックス", 0, 1, 0.7f, "" } } });

    v.push_back ({ "ensemble", "アンサンブル", C::modulation, W::medium, 2, "揺れる複数の遅延を重ね、大勢で話しているような厚みを出す",
                   { { "voices", "声の数", 2, 8, 4, "声", {}, true },
                     { "rateHz", "レート", 0.1f, 5, 0.8f, "Hz" },
                     { "depth", "深さ", 0, 1, 0.5f, "" },
                     { "mix", "ミックス", 0, 1, 0.5f, "" } } });

    v.push_back ({ "tremolo", "トレモロ", C::modulation, W::light, 2, "音量を周期的に揺らす",
                   { { "rateHz", "レート", 0.5f, 20, 5, "Hz" },
                     { "depth", "深さ", 0, 1, 0.3f, "" } } });

    v.push_back ({ "slicer", "スライサー", C::rhythm, W::light, 2, "テンポに合わせて声を細かく切る。BPM は手動で設定する",
                   { { "bpm", "BPM", 40, 240, 120, "", {}, true },
                     { "pattern", "パターン番号", 0, 7, 0, "", {}, true },
                     { "division", "刻み", 0, 2, 1, "", { { "1/8", "1/8" }, { "1/16", "1/16" }, { "1/32", "1/32" } } },
                     { "depth", "深さ", 0, 1, 1, "" },
                     { "smoothMs", "切れ目のなめらかさ", 0, 50, 5, "ms" } } });

    v.push_back ({ "stutter", "スタッター", C::rhythm, W::medium, 2, "声の一部をテンポに合わせて繰り返す（グリッチ）。乱数は固定シードで再現できる",
                   { { "bpm", "BPM", 40, 240, 120, "", {}, true },
                     { "division", "刻み", 0, 3, 2, "", { { "1/4", "1/4" }, { "1/8", "1/8" }, { "1/16", "1/16" }, { "1/32", "1/32" } } },
                     { "repeatCount", "繰り返す回数", 2, 8, 4, "回", {}, true },
                     { "chance", "拍ごとに繰り返す確率", 0, 1, 0.5f, "" },
                     { "mix", "ミックス", 0, 1, 1, "" } } });

    v.push_back ({ "echo", "エコー", C::timeSpace, W::medium, 2, "デジタル、テープ、リバースの3タイプのディレイ（最大 4 秒）",
                   { { "mode", "タイプ", 0, 2, 0, "", { { "digital", "デジタル" }, { "tape", "テープ" }, { "reverse", "リバース" } } },
                     { "timeMs", "時間", 20, 4000, 300, "ms" },
                     { "feedback", "フィードバック", 0, 0.9f, 0.3f, "" },
                     { "toneHz", "繰り返しのトーン（ローパス）", 1000, 12000, 8000, "Hz" },
                     { "mix", "ミックス", 0, 1, 0.3f, "" } } });

    v.push_back ({ "reverb", "リバーブ", C::timeSpace, W::medium, 2, "ルーム、ホール、プレート、スプリング、アンビエンスの5タイプ",
                   { { "type", "タイプ", 0, 4, 0, "", { { "room", "ルーム" }, { "hall", "ホール" }, { "plate", "プレート" }, { "spring", "スプリング" }, { "ambience", "アンビエンス" } } },
                     { "room", "広さ", 0, 1, 0.5f, "" },
                     { "damp", "高域の減衰", 0, 1, 0.5f, "" },
                     { "preDelayMs", "プリディレイ", 0, 200, 0, "ms" },
                     { "mix", "ミックス", 0, 1, 0.3f, "" } } });

    // ---- Phase 5 ----
    // weights of freeze / granular / whisper (light) and vocoder (medium) follow the measured CPU, not koeloom_effects.md §4
    // (owner decision 2026-10-03, 引き継ぎメモ §4)
    v.push_back ({ "freeze", "フリーズ", C::special, W::light, 5, "直前の音を伸ばし続ける（ホットキーで切替。起動時は OFF）",
                   { { "grainMs", "つなぎの長さ", 20, 500, 120, "ms" },
                     { "mix", "ミックス", 0, 1, 1, "" } } });

    v.push_back ({ "granular", "グラニュラー", C::special, W::light, 5, "声を細かい粒に分けて再構成する",
                   { { "grainMs", "粒の長さ", 10, 200, 60, "ms" },
                     { "density", "密度", 1, 40, 12, "粒/秒" },
                     { "spray", "ばらつき", 0, 1, 0.3f, "" },
                     { "pitchSt", "粒のピッチ", -12, 12, 0, "st" },
                     { "mix", "ミックス", 0, 1, 0.5f, "" } } });

    v.push_back ({ "looper", "ルーパー", C::special, W::light, 5, "声をメモリ内に録音して重ねる。ディスクには保存しない（録音・再生・上書き・消去はボタンとホットキーで操作）",
                   { { "levelDb", "ループの音量", -24, 6, 0, "dB" },
                     { "maxSec", "最大の長さ", 5, 60, 30, "秒", {}, true } } });

    v.push_back ({ "autopitch", "オートピッチ", C::pitchVocoder, W::heavy, 5, "声のピッチを音階に吸着させる（ケロケロ声）",
                   { { "key", "キー", 0, 11, 0, "", { { "C", "C" }, { "C#", "C#" }, { "D", "D" }, { "D#", "D#" }, { "E", "E" }, { "F", "F" },
                                                     { "F#", "F#" }, { "G", "G" }, { "G#", "G#" }, { "A", "A" }, { "A#", "A#" }, { "B", "B" } } },
                     { "scale", "スケール", 0, 2, 0, "", { { "chromatic", "クロマチック" }, { "major", "メジャー" }, { "minor", "マイナー" } } },
                     { "retuneMs", "補正の速さ（0 で瞬時）", 0, 400, 50, "ms" },
                     { "strength", "補正の強さ", 0, 1, 1, "" } } });

    v.push_back ({ "vocoder", "ボコーダー", C::pitchVocoder, W::medium, 5, "声の特徴を別の音（キャリア）にかぶせて、しゃべらせる",
                   { { "character", "キャラクター", 0, 2, 1, "", { { "vintage", "ヴィンテージ" }, { "modern", "モダン" }, { "talkbox", "トークボックス" } } },
                     { "bands", "バンド数", 8, 32, 16, "", {}, true },
                     { "carrier", "キャリアの波形", 0, 2, 0, "", { { "saw", "のこぎり波" }, { "square", "矩形波" }, { "noise", "ノイズ" } } },
                     { "carrierPitch", "キャリアの高さ", 0, 1, 0, "", { { "follow", "声のピッチに追従" }, { "fixed", "固定ピッチ" } } },
                     { "noteSt", "固定の音（MIDI ノート番号）", 36, 84, 48, "", {}, true },
                     { "chord", "和音", 0, 3, 0, "", { { "single", "単音" }, { "major_chord", "メジャー和音" }, { "minor_chord", "マイナー和音" }, { "fifth", "5度（パワーコード）" } } },
                     { "attackMs", "アタック", 1, 100, 10, "ms" },
                     { "releaseMs", "リリース", 10, 500, 80, "ms" },
                     { "dryDb", "元の声の音量", -60, 0, -60, "dB" },
                     { "mix", "ミックス", 0, 1, 1, "" } } });

    v.push_back ({ "whisper", "ささやき", C::pitchVocoder, W::light, 5, "声の特徴をノイズにかぶせ、息のような声にする",
                   { { "bands", "バンド数", 12, 32, 20, "", {}, true },
                     { "brightness", "明るさ", -1, 1, 0, "" },
                     { "mix", "ミックス", 0, 1, 1, "" } } });

    return v;
}

std::map<std::string, EffectFactory, std::less<>>& factories()
{
    static std::map<std::string, EffectFactory, std::less<>> m;
    return m;
}
} // namespace

bool registerEffectFactory (const char* type, EffectFactory factory)
{
    factories()[type] = factory;
    return true;
}

const std::vector<EffectInfo>& allEffectInfos()
{
    static const std::vector<EffectInfo> infos = buildInfos();
    return infos;
}

const EffectInfo* findEffectInfo (std::string_view type)
{
    for (auto& i : allEffectInfos())
        if (type == i.type) return &i;
    return nullptr;
}

bool hasEffectFactory (std::string_view type)
{
    return findEffectInfo (type) != nullptr && factories().find (type) != factories().end();
}

std::unique_ptr<IEffect> createEffect (std::string_view type)
{
    if (findEffectInfo (type) == nullptr) return nullptr;
    auto it = factories().find (type);
    return it == factories().end() ? nullptr : it->second();
}

const char* categoryNameJa (EffectCategory c)
{
    switch (c)
    {
        case C::dynamics: return "ダイナミクス";
        case C::eqFilter: return "EQ・フィルター";
        case C::distortion: return "歪み・ローファイ";
        case C::modulation: return "変調";
        case C::rhythm: return "リズム";
        case C::timeSpace: return "時間・空間";
        case C::special: return "特殊";
        case C::pitchVocoder: return "ピッチ・ボコーダー";
    }
    return "";
}

const char* weightNameJa (EffectWeight w)
{
    switch (w)
    {
        case W::light: return "軽";
        case W::medium: return "中";
        case W::heavy: return "重";
    }
    return "";
}
} // namespace koe

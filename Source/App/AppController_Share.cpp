// プリセットの共有コード and サウンドボードに録音を登録 (INTERFACES.md §11.3). Owner: wave9/share.
// Share code: "KL1:" + URL-safe Base64 (no padding) of the zlib-compressed text "<one-line preset JSON>#<8 hex digits>".
// The hex is FNV-1a of the JSON's UTF-8: JUCE's inflate stream gives out the data before it checks zlib's own checksum
// and never tells a mismatch, so a garbled code would otherwise load as a wrong preset.
// Reading goes the same way as importPreset (parsePreset's E-21..E-30 report, PresetLibrary::saveNew gives a fresh id).
// The processed take: the raw 試し録り through the working preset with tools::renderPreset, offline, into a WAV named like
// the WAV recording (AppController_Capture.cpp).

#include "App/AppController.h"
#include "Core/Paths.h"
#include "Tools/Calibrate.h"

#include <juce_audio_formats/juce_audio_formats.h>

#include <algorithm>

namespace koe
{
namespace
{
juce::String u8 (const char* s) { return juce::String::fromUTF8 (s); }
const char* const kSharePrefix = "KL1:";

bool isCodeChar (juce::juce_wchar ch)
{
    return (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '-' || ch == '_';
}

/** The payload after "KL1:" in text (spaces, line breaks and words around it are fine). Empty when there is none. */
juce::String findPayload (const juce::String& text)
{
    const int at = text.indexOf (kSharePrefix);
    if (at < 0) return {};
    auto p = text.getCharPointer() + (at + 4);
    juce::String out;
    while (! p.isEmpty() && isCodeChar (*p)) out << *p++;
    return out;
}

bool reject (PresetLoadReport& report, const juce::String& why)
{
    report = {};
    report.rejected = true;
    report.rejectReason = why;
    return false;
}

juce::String checksum (const juce::String& json)
{
    juce::uint32 h = 2166136261u;
    for (auto* p = json.toRawUTF8(); *p != 0; ++p) h = (h ^ juce::uint8 (*p)) * 16777619u;
    return juce::String::toHexString (int (h)).paddedLeft ('0', 8);
}

juce::String broken() { return u8 ("共有コードが壊れています（途中で切れているかもしれません）"); }
} // namespace

// ============================================================================ 共有コード
juce::String AppController::makeShareCode (const std::string& presetId, juce::String& whyNot) const
{
    const auto* p = library->find (presetId);
    if (p == nullptr)
    {
        whyNot = u8 ("プリセットが見つかりません。");
        return {};
    }
    auto copy = *p;
    copy.id = "share"; // the reader gives it a new id anyway
    juce::var json;
    if (juce::JSON::parse (serializePreset (copy), json).failed())
    {
        whyNot = u8 ("共有コードを作れませんでした。");
        return {};
    }
    const auto oneLine = juce::JSON::toString (json, true); // a few bytes less before compression
    const auto text = oneLine + "#" + checksum (oneLine);

    juce::MemoryOutputStream packed;
    {
        juce::GZIPCompressorOutputStream zip (packed, 9);
        zip.write (text.toRawUTF8(), text.getNumBytesAsUTF8());
    } // flushed here
    const auto b64 = juce::Base64::toBase64 (packed.getData(), packed.getDataSize());
    return kSharePrefix + b64.replaceCharacter ('+', '-').replaceCharacter ('/', '_').trimCharactersAtEnd ("=");
}

bool AppController::importShareCode (const juce::String& text, std::string& newIdOut, PresetLoadReport& report)
{
    report = {};
    auto payload = findPayload (text);
    if (payload.isEmpty())
        return reject (report, text.contains (kSharePrefix) ? broken() : u8 ("共有コードが見つかりません（KL1: で始まる文字をコピーしてください）"));
    if (payload.length() % 4 == 1) return reject (report, broken());
    payload = payload.replaceCharacter ('-', '+').replaceCharacter ('_', '/');
    while (payload.length() % 4 != 0) payload << "=";

    juce::MemoryOutputStream packed;
    if (! juce::Base64::convertFromBase64 (packed, payload) || packed.getDataSize() == 0) return reject (report, broken());

    // decompress at most kPresetMaxBytes + a little: a huge (or hostile) code never fills the memory
    juce::MemoryInputStream source (packed.getData(), packed.getDataSize(), false);
    juce::GZIPDecompressorInputStream unzip (source);
    juce::MemoryBlock bytes;
    char buffer[4096];
    while (bytes.getSize() <= size_t (kPresetMaxBytes))
    {
        const int n = unzip.read (buffer, int (sizeof (buffer)));
        if (n <= 0) break;
        bytes.append (buffer, size_t (n));
    }
    if (bytes.getSize() > size_t (kPresetMaxBytes)) return reject (report, u8 ("64 KB を超えています"));
    const auto unpacked = juce::String::fromUTF8 (static_cast<const char*> (bytes.getData()), int (bytes.getSize()));
    const int hash = unpacked.lastIndexOfChar ('#');
    const auto json = unpacked.substring (0, juce::jmax (0, hash));
    if (hash < 0 || unpacked.substring (hash + 1) != checksum (json)) return reject (report, broken()); // cut or garbled

    // the same path as importPreset -> PresetLibrary::importFile: parse (E-21..E-30), then a new user preset
    auto p = parsePreset (json, report);
    if (! p) return false;
    juce::String error;
    if (! library->saveNew (*p, p->name, newIdOut, error))
    {
        report.rejected = true;
        report.rejectReason = error;
        return false;
    }
    sendChangeMessage();
    return true;
}

// ============================================================================ 試し録りを加工して保存
juce::File AppController::renderTestTakeToFile (juce::String& whyNot)
{
    const auto* take = capture.take.get();
    if (take == nullptr || take->getLength() == 0)
    {
        whyNot = u8 ("先に試し録りをしてください。");
        return {};
    }
    if (getTestTakeState() == TakeState::recording)
    {
        whyNot = u8 ("試し録りの録音中です。止めてから保存してください。");
        return {};
    }

    // the take is the raw device input (before the input gain): give it the gain the live path would
    const double rate = take->getSampleRate();
    const int length = take->getLength();
    const float gain = juce::Decibels::decibelsToGain (settings.inputGainDb);
    const auto& raw = take->getSamples(); // playback only reads it: safe to copy on this thread
    std::vector<float> input (size_t (length) + size_t (rate), 0.0f); // + 1 s of silence for the latency to come out
    std::transform (raw.begin(), raw.begin() + length, input.begin(), [gain] (float x) { return x * gain; });

    auto preset = current;
    preset.outputTrimDb = getEffectiveTrimDb();
    int latency = 0;
    const auto rendered = tools::renderPreset (preset, input, latency, rate);
    std::vector<float> out (size_t (length), 0.0f); // the latency cut from the head: as long as the take
    for (int i = 0; i < length && size_t (latency + i) < rendered.size(); ++i) out[size_t (i)] = rendered[size_t (latency + i)];

    const auto dir = paths::recordingsDir();
    if (! dir.createDirectory())
    {
        whyNot = u8 ("録音のフォルダを作れませんでした（") + dir.getFullPathName() + u8 ("）。");
        return {};
    }
    const auto name = "KoeLoom " + juce::Time::getCurrentTime().formatted ("%Y-%m-%d %H-%M-%S");
    auto file = dir.getChildFile (name + ".wav");
    for (int i = 2; file.exists(); ++i) file = dir.getChildFile (name + " (" + juce::String (i) + ").wav");

    bool ok = false;
    {
        std::unique_ptr<juce::FileOutputStream> os (file.createOutputStream());
        if (os != nullptr && os->openedOk())
        {
            std::unique_ptr<juce::AudioFormatWriter> w (juce::WavAudioFormat().createWriterFor (os.get(), rate, 1, 24, {}, 0));
            if (w != nullptr)
            {
                os.release(); // the writer owns it now
                const float* channels[] = { out.data() };
                ok = w->writeFromFloatArrays (channels, 1, length);
            }
        }
    } // the file is closed here
    if (! ok)
    {
        file.deleteFile();
        whyNot = file.getBytesFreeOnVolume() < 64 * 1024 * 1024 ? u8 ("ディスクの空きが足りないため、保存できませんでした。")
                                                                 : u8 ("ファイルに書き込めませんでした（") + file.getFullPathName() + u8 ("）。");
        return {};
    }
    capture.lastWav = file; // the 録音 tool shows it as its last file: 「サウンドボードに入れる」 takes it from there
    sendChangeMessage();
    return file;
}
} // namespace koe

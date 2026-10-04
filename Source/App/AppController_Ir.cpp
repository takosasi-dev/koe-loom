// Impulse-response files for the "convolution" effect (INTERFACES.md §9.3, owner wave7/ir).
#include "App/AppController.h"

#include "Core/Constants.h"
#include "Core/Paths.h"

#include <juce_audio_formats/juce_audio_formats.h>

namespace koe
{
namespace
{
juce::String ja (const char* s) { return juce::String::fromUTF8 (s); }

/** Same rule as Preset.cpp (presets drop anything else): a plain file name inside paths::irDir(). */
bool isValidIrFileName (const juce::String& name)
{
    if (name.isEmpty() || name.getNumBytesAsUTF8() > 255 || name.contains ("..") || name.containsAnyOf ("/\\<>:\"|?*")
        || name.trim() != name || name.endsWithChar ('.'))
        return false;
    for (auto p = name.getCharPointer(); ! p.isEmpty(); ++p)
        if (*p < 0x20) return false;
    return true;
}

bool hasIrExtension (const juce::File& f)
{
    return f.hasFileExtension ("wav;flac;aif;aiff");
}

/** "a..b.wav" would be dropped when a preset is loaded: the copy gets a name the preset can keep. */
juce::String copyNameFor (const juce::File& picked)
{
    auto name = picked.getFileName().trim();
    while (name.contains ("..")) name = name.replace ("..", ".");
    return name;
}

bool isConvolutionSlot (const std::vector<SlotDef>& chain, int slot)
{
    return slot >= 0 && slot < int (chain.size()) && chain[size_t (slot)].type == "convolution";
}
} // namespace

bool AppController::setSlotFile (int slot, const juce::File& picked, juce::String& whyNot)
{
    if (! isConvolutionSlot (current.chain, slot))
    {
        whyNot = ja ("残響ファイルを使えるのは「残響ファイル」のスロットだけです。");
        return false;
    }
    if (! picked.existsAsFile())
    {
        whyNot = ja ("ファイルが見つかりません。");
        return false;
    }
    if (! hasIrExtension (picked))
    {
        whyNot = ja ("WAV / FLAC / AIFF のファイルを選んでください。");
        return false;
    }
    {
        juce::AudioFormatManager formats;
        formats.registerBasicFormats();
        std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (picked));
        if (reader == nullptr || reader->sampleRate <= 0.0 || reader->lengthInSamples <= 0 || reader->numChannels == 0)
        {
            whyNot = ja ("音声ファイルとして読めませんでした。");
            return false;
        }
        const double seconds = double (reader->lengthInSamples) / reader->sampleRate;
        if (seconds > double (kIrMaxSeconds) + 1.0e-6)
        {
            whyNot = ja ("残響ファイルは ") + juce::String (juce::roundToInt (kIrMaxSeconds)) + ja (" 秒までです（このファイルは ")
                     + juce::String (seconds, 1) + ja (" 秒）。");
            return false;
        }
    }

    const auto dir = paths::irDir();
    juce::String name;
    if (picked.getParentDirectory() == dir && isValidIrFileName (picked.getFileName()))
    {
        name = picked.getFileName(); // already in the folder
    }
    else
    {
        const auto base = copyNameFor (picked);
        if (! isValidIrFileName (base))
        {
            whyNot = ja ("このファイル名は使えません。名前を変えてから選んでください。");
            return false;
        }
        if (! dir.createDirectory())
        {
            whyNot = ja ("ir フォルダを作れませんでした。");
            return false;
        }
        const auto stem = juce::File::createFileWithoutCheckingPath (base).getFileNameWithoutExtension();
        const auto ext = juce::File::createFileWithoutCheckingPath (base).getFileExtension();
        for (int k = 1; k < 1000 && name.isEmpty(); ++k)
        {
            const auto candidate = k == 1 ? base : stem + " (" + juce::String (k) + ")" + ext;
            const auto target = dir.getChildFile (candidate);
            if (target.existsAsFile())
            {
                if (target.getSize() == picked.getSize() && target.hasIdenticalContentTo (picked)) name = candidate; // reuse
                continue;
            }
            if (! picked.copyFileTo (target))
            {
                whyNot = ja ("ir フォルダにコピーできませんでした。");
                return false;
            }
            name = candidate;
        }
        if (name.isEmpty())
        {
            whyNot = ja ("同じ名前のファイルが多すぎます。ir フォルダを整理してください。");
            return false;
        }
    }

    current.chain[size_t (slot)].file = name.toStdString();
    rebuildChain();
    markModified();
    return true;
}

void AppController::setSlotFileName (int slot, const juce::String& fileName)
{
    if (! isConvolutionSlot (current.chain, slot) || (fileName.isNotEmpty() && ! isValidIrFileName (fileName))) return;
    auto& file = current.chain[size_t (slot)].file;
    if (file == fileName.toStdString()) return;
    file = fileName.toStdString();
    rebuildChain();
    markModified();
}

juce::String AppController::getSlotFileName (int slot) const
{
    if (slot < 0 || slot >= int (current.chain.size())) return {};
    return juce::String::fromUTF8 (current.chain[size_t (slot)].file.c_str());
}

bool AppController::isSlotFileMissing (int slot) const
{
    const auto name = getSlotFileName (slot);
    if (name.isEmpty()) return false;
    return ! paths::irDir().getChildFile (name).existsAsFile() || getSlotUiState (slot) == 2; // 2 = unreadable (Convolution.cpp)
}

juce::StringArray AppController::listIrFiles()
{
    juce::StringArray names;
    for (auto& f : paths::irDir().findChildFiles (juce::File::findFiles, false))
        if (hasIrExtension (f) && isValidIrFileName (f.getFileName())) names.add (f.getFileName());
    names.sortNatural();
    return names;
}
} // namespace koe

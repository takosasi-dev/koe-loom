#pragma once

#include "Core/Constants.h"
#include "Engine/VoiceProcessor.h"

#include <juce_core/juce_core.h>

#include <functional>
#include <memory>

namespace koe
{
struct SoundboardSlotDef
{
    juce::String file;           // absolute path, empty = unassigned
    float volumeDb = 0.0f;       // -24..+6 (F-06-4)
    bool loop = false;           // one-shot / loop
    int retrigger = 0;           // 0 = restart, 1 = ignore, 2 = overlap (F-06-4)
    bool toMonitor = true;       // also play on the monitor (F-06-5)
    bool operator== (const SoundboardSlotDef&) const = default;
};

struct SoundSlotState
{
    enum class Status { empty, loading, ready, error, missing };
    Status status = Status::empty;
    juce::String fileName;       // just the name, for the slot button
    juce::String error;          // Japanese, when status == error (E-13/E-14) or missing (E-12)
    double lengthSeconds = 0.0;
    bool playing = false;
    double positionSeconds = 0.0; // while playing: where the newest voice of the slot is (S-02 progress bar)
};

/**
    Soundboard (F-06): 12 slots of in-memory audio mixed into the virtual mic after the voice chain
    (spec §5.2). Files are decoded on a background thread (WAV / FLAC / Ogg Vorbis; MP3 through Windows
    Media Foundation), limited to 60 s each and 200 MB in total, resampled to the device rate.
    Broken or missing files never crash (E-12..E-14). At most 8 voices; the oldest stops first (F-06-9).
    Also provides the setup wizard's test tone (F-10-3). Ducking lowers the voice while sounds play (F-06-7).
    Message-thread API except render()/voiceDuckGain(), which run on the audio thread.
*/
class Soundboard final : public IAuxSource
{
public:
    Soundboard();
    ~Soundboard() override;

    /** Message thread, when the device (re)opens. */
    void prepare (double sampleRate);

    // ---- persistence (soundboard.json, F-11-1) ----
    void load (const juce::File& soundboardJson);   // missing/corrupt file = empty board
    bool save (const juce::File& soundboardJson) const;

    // ---- slots ----
    void assignFile (int slot, const juce::File& file);  // async; state goes loading -> ready / error
    void clearSlot (int slot);
    SoundboardSlotDef getSlotDef (int slot) const;
    void setSlotDef (int slot, const SoundboardSlotDef& def); // volume / loop / retrigger / monitor (reloads if the file changed)
    SoundSlotState getSlotState (int slot) const;
    long long getTotalBytes() const;

    // ---- playback ----
    void trigger (int slot);
    void stopAll();
    void setDuckingDb (float db);       // 0 = off, down to -24
    void setTestTone (bool on);         // 440 Hz, -18 dBFS, to output only

    /** Called on the message thread whenever a slot's state changes (loaded, failed, started, stopped). */
    std::function<void()> onStateChanged;

    // ---- IAuxSource (audio thread) ----
    void render (float* toOutput, float* toMonitor, int numSamples) override;
    float voiceDuckGain() const override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
} // namespace koe

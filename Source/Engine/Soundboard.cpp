#include "Engine/Soundboard.h"

#include "Dsp/Building.h"

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_events/juce_events.h>

#include <algorithm>
#include <cmath>
#include <new>
#include <vector>

namespace koe
{
namespace
{
juce::String u8 (const char* s) { return juce::String::fromUTF8 (s); }

constexpr int kPoolVoices = 2 * kSoundboardMaxVoices; // 8 sounding + room for the ones fading out
constexpr float kStopFadeMs = 5.0f;
constexpr float kToneFadeMs = 10.0f;
constexpr double kDuckAttackSeconds = 0.02, kDuckReleaseSeconds = 0.3;
constexpr double kTestToneHz = 440.0;
constexpr float kTestToneDb = -18.0f;
constexpr double kMinSourceRate = 1000.0, kMaxSourceRate = 768000.0; // reject absurd headers before allocating
constexpr int kReadChunk = 32768;
constexpr int kCommandCapacity = 64;
constexpr juce::uint32 kCommandMaxAgeMs = 300;
constexpr int kCmdStopAll = -1;            // other commands: slot index to trigger
const char* const kRetriggerIds[] = { "restart", "ignore", "overlap" };

/** Decoded sound: mono at the device rate. Only the message/loader side creates and frees these. */
struct Clip
{
    std::vector<float> samples;
    juce::uint64 id = 0;
};

inline float hermite (float x0, float x1, float x2, float x3, float t) noexcept
{
    const float c1 = 0.5f * (x2 - x0);
    const float c2 = x0 - 2.5f * x1 + 2.0f * x2 - 0.5f * x3;
    const float c3 = 0.5f * (x3 - x0) + 1.5f * (x1 - x2);
    return ((c3 * t + c2) * t + c1) * t + x1;
}

/** src (srcRate) -> out (dstRate, out.size() samples). Cubic Hermite; 8th-order Butterworth low-pass first when going down. */
void resampleInto (std::vector<float>& src, double srcRate, double dstRate, std::vector<float>& out)
{
    if (dstRate < srcRate)
    {
        static constexpr float q[] = { 0.5098f, 0.6013f, 0.9000f, 2.5629f };
        dsp::Biquad lp[4];
        for (int s = 0; s < 4; ++s) lp[s].setLowpass (srcRate, float (0.45 * dstRate), q[s]);
        for (auto& x : src)
            for (auto& b : lp) x = b.process (x);
    }
    const auto last = juce::int64 (src.size()) - 1;
    auto at = [&] (juce::int64 i) { return src[size_t (std::clamp<juce::int64> (i, 0, last))]; };
    const double step = srcRate / dstRate;
    for (size_t j = 0; j < out.size(); ++j)
    {
        const double pos = double (j) * step;
        const auto i = juce::int64 (pos);
        out[j] = hermite (at (i - 1), at (i), at (i + 1), at (i + 2), float (pos - double (i)));
    }
}

juce::String megabytes (long long bytes) { return juce::String (double (bytes) / (1024.0 * 1024.0), 1); }
} // namespace

struct Soundboard::Impl final : private juce::Timer
{
    explicit Impl (Soundboard& o) : owner (o)
    {
        formats.registerBasicFormats(); // WAV, AIFF, FLAC, Ogg Vorbis, Windows Media (MP3/WMA)
        for (int s = 0; s < kSoundboardSlots; ++s) applyAtomics (s);
        startTimerHz (20);
    }

    ~Impl() override
    {
        stopTimer();
        quitting.store (true);
        pool.removeAllJobs (true, 10000);
        for (auto& c : clips) delete c.exchange (nullptr);
        for (auto& r : retired) delete r.clip;
    }

    Soundboard& owner;
    juce::AudioFormatManager formats;   // used by the loader thread only (after construction)
    juce::ThreadPool pool { 1 };
    std::atomic<bool> quitting { false };
    std::atomic<double> rate { kSampleRate };

    // ---- message thread <-> loader thread (never the audio thread) ----
    mutable juce::CriticalSection lock;
    SoundboardSlotDef defs[kSoundboardSlots];
    SoundSlotState states[kSoundboardSlots];
    juce::uint64 requests[kSoundboardSlots] {};   // bumped on every (re)load; a stale job drops its result
    long long bytes[kSoundboardSlots] {};          // memory held or reserved per slot (E-13 total)
    struct Retired { Clip* clip; juce::uint64 renderStartsAtRetire; };
    std::vector<Retired> retired;
    juce::uint64 nextClipId = 0;
    std::atomic<bool> dirty { false };
    juce::uint32 lastMask = 0;                     // message thread

    // ---- read by the audio thread ----
    std::atomic<Clip*> clips[kSoundboardSlots] {};
    std::atomic<float> slotGain[kSoundboardSlots] {};
    std::atomic<bool> slotLoop[kSoundboardSlots] {}, slotMonitor[kSoundboardSlots] {};
    std::atomic<int> slotRetrigger[kSoundboardSlots] {};
    std::atomic<float> duckingDb { 0.0f }, duckOut { 1.0f };
    std::atomic<bool> testTone { false };
    std::atomic<juce::uint32> playingMask { 0 };
    std::atomic<double> slotPosition[kSoundboardSlots] {}; // seconds, newest sounding voice; 0 when silent
    std::atomic<juce::uint64> renderStarts { 0 }, renderEnds { 0 };
    juce::AbstractFifo commandFifo { kCommandCapacity }; // single producer: message thread
    struct Command { int what; juce::uint32 ms; };      // ms: when it was queued
    Command commands[kCommandCapacity] {};

    // ---- audio thread only ----
    struct Voice
    {
        int slot = -1;              // -1 = free
        juce::uint64 clipId = 0;
        juce::int64 pos = 0;
        juce::uint64 order = 0;
        float gain = 1.0f;
        int fadeLeft = -1;          // >= 0: stopping
    };
    Voice voices[kPoolVoices];
    juce::uint64 voiceOrder = 0;
    double tonePhase = 0.0;
    float toneGain = 0.0f, duck = 1.0f;

    // ================================================================ message thread
    void applyAtomics (int s)
    {
        slotGain[s].store (dsp::dbToGain (defs[s].volumeDb));
        slotLoop[s].store (defs[s].loop);
        slotMonitor[s].store (defs[s].toMonitor);
        slotRetrigger[s].store (defs[s].retrigger);
    }

    /** Swap the sound the audio thread sees. The old one is freed once no render can still use it. */
    void publishLocked (int s, Clip* clip, long long clipBytes)
    {
        if (auto* old = clips[s].exchange (clip)) retired.push_back ({ old, renderStarts.load() });
        bytes[s] = clipBytes;
    }

    void collectGarbage()
    {
        std::vector<Clip*> done;
        {
            const juce::ScopedLock sl (lock);
            const auto ends = renderEnds.load();
            const bool idle = renderStarts.load() == ends; // read after ends: no render in progress
            for (size_t i = 0; i < retired.size();)
            {
                // a render that started after the swap compares clip ids and drops voices of the old clip
                if (idle || ends > retired[i].renderStartsAtRetire)
                {
                    done.push_back (retired[i].clip);
                    retired.erase (retired.begin() + long (i));
                }
                else ++i;
            }
        }
        for (auto* c : done) delete c;
    }

    void startLoadLocked (int s)
    {
        const auto id = ++requests[s];
        publishLocked (s, nullptr, 0);
        const auto path = defs[s].file;
        dirty.store (true);
        if (path.isEmpty())
        {
            states[s] = {};
            return;
        }
        states[s] = {};
        states[s].status = SoundSlotState::Status::loading;
        states[s].fileName = juce::File (path).getFileName();
        const double r = rate.load();
        pool.addJob ([this, s, id, path, r] { loadJob (s, id, juce::File (path), r); });
    }

    void pushCommand (int c)
    {
        const Command cmd { c, juce::Time::getMillisecondCounter() };
        int s1, n1, s2, n2;
        commandFifo.prepareToWrite (1, s1, n1, s2, n2);
        if (n1 > 0) commands[s1] = cmd;
        else if (n2 > 0) commands[s2] = cmd;
        commandFifo.finishedWrite (n1 + n2);
    }

    void timerCallback() override
    {
        collectGarbage();
        const auto mask = playingMask.load();
        if (dirty.exchange (false) || mask != lastMask)
        {
            lastMask = mask;
            if (owner.onStateChanged) owner.onStateChanged();
        }
    }

    // ================================================================ loader thread
    bool isStale (int s, juce::uint64 id) const
    {
        const juce::ScopedLock sl (lock);
        return quitting.load() || requests[s] != id;
    }

    void loadJob (int s, juce::uint64 id, const juce::File& file, double dstRate)
    {
        if (isStale (s, id)) return;
        auto status = SoundSlotState::Status::error;
        juce::String error;
        std::unique_ptr<Clip> clip;
        double seconds = 0.0;
        long long clipBytes = 0;
        try
        {
            if (! file.existsAsFile())
            {
                status = SoundSlotState::Status::missing; // E-12
                error = u8 ("ファイルが見つかりません");
            }
            else if (file.getSize() == 0)
                error = u8 ("ファイルが空です（0 バイト）"); // E-14
            else if (std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (file)); reader == nullptr
                     || reader->lengthInSamples <= 0 || reader->numChannels == 0
                     || reader->sampleRate < kMinSourceRate || reader->sampleRate > kMaxSourceRate)
                error = u8 ("読み込めませんでした。ファイルが壊れているか、対応していない形式です（WAV / FLAC / Ogg Vorbis / MP3）"); // E-14
            else
            {
                const auto len = reader->lengthInSamples;
                const double srcRate = reader->sampleRate;
                seconds = double (len) / srcRate;
                // >= 1: a looping voice wraps to sample 0, which must exist
                const auto outLen = std::max<juce::int64> (1, std::llround (double (len) * dstRate / srcRate));
                clipBytes = outLen * juce::int64 (sizeof (float));
                if (len > juce::int64 (std::floor (kSoundboardMaxSeconds * srcRate + 0.5)))
                    error = u8 ("60 秒を超えています（") + juce::String (seconds, 1) + u8 (" 秒）。60 秒以内のファイルを使ってください"); // E-13
                else if (! reserve (s, id, clipBytes, error))
                {
                    if (error.isEmpty()) return; // stale
                }
                else
                    clip = decode (*reader, s, id, srcRate, dstRate, outLen);
                if (clip == nullptr && error.isEmpty()) return; // stale or quitting while decoding
            }
        }
        catch (const std::bad_alloc&)
        {
            clip.reset();
            status = SoundSlotState::Status::error;
            error = u8 ("メモリが足りないため読み込めませんでした");
        }

        const juce::ScopedLock sl (lock);
        if (quitting.load() || requests[s] != id) return;
        if (clip != nullptr)
        {
            clip->id = ++nextClipId;
            publishLocked (s, clip.release(), clipBytes);
            states[s].status = SoundSlotState::Status::ready;
            states[s].lengthSeconds = seconds;
            states[s].error = {};
        }
        else
        {
            bytes[s] = 0;
            states[s].status = status;
            states[s].error = error;
        }
        dirty.store (true);
    }

    /** E-13 total limit: holds the slot's share before decoding. false + error = refused, false + empty = stale. */
    bool reserve (int s, juce::uint64 id, long long need, juce::String& error)
    {
        const juce::ScopedLock sl (lock);
        if (quitting.load() || requests[s] != id) return false;
        long long others = 0;
        for (int i = 0; i < kSoundboardSlots; ++i)
            if (i != s) others += bytes[i];
        if (others + need > kSoundboardMaxTotalBytes)
        {
            error = u8 ("サウンドボード全体の上限 200 MB を超えます（使用中 ") + megabytes (others) + u8 (" MB ＋ このファイル ")
                    + megabytes (need) + u8 (" MB）");
            return false;
        }
        bytes[s] = need;
        return true;
    }

    std::unique_ptr<Clip> decode (juce::AudioFormatReader& reader, int s, juce::uint64 id, double srcRate, double dstRate, juce::int64 outLen)
    {
        const auto len = reader.lengthInSamples;
        const int channels = int (reader.numChannels);
        std::vector<float> mono (size_t (len), 0.0f);
        juce::AudioBuffer<float> chunk (channels, kReadChunk);
        for (juce::int64 pos = 0; pos < len; pos += kReadChunk)
        {
            if (isStale (s, id)) return nullptr;
            const int n = int (std::min<juce::int64> (kReadChunk, len - pos));
            chunk.clear();
            reader.read (chunk.getArrayOfWritePointers(), channels, pos, n); // short/broken data reads as silence
            for (int c = 0; c < channels; ++c)
            {
                const float* x = chunk.getReadPointer (c);
                for (int i = 0; i < n; ++i) mono[size_t (pos + i)] += x[i];
            }
        }
        const float scale = 1.0f / float (channels);
        for (auto& x : mono) x = std::isfinite (x) ? x * scale : 0.0f;

        auto clip = std::make_unique<Clip>();
        if (srcRate == dstRate) clip->samples = std::move (mono);
        else
        {
            clip->samples.assign (size_t (outLen), 0.0f);
            resampleInto (mono, srcRate, dstRate, clip->samples);
        }
        return clip;
    }

    // ================================================================ audio thread
    int countSounding() const noexcept
    {
        int n = 0;
        for (auto& v : voices) if (v.slot >= 0 && v.fadeLeft < 0) ++n;
        return n;
    }

    void startVoice (int s, const Clip& clip, int stopFade) noexcept
    {
        bool sounding = false;
        for (auto& v : voices) sounding = sounding || (v.slot == s && v.fadeLeft < 0);
        const int mode = slotRetrigger[s].load (std::memory_order_relaxed);
        if (sounding && mode == 1) return;                       // ignore
        if (sounding && mode == 0)                               // restart
            for (auto& v : voices) if (v.slot == s && v.fadeLeft < 0) v.fadeLeft = stopFade;

        if (countSounding() >= kSoundboardMaxVoices)             // F-06-9: the oldest stops
        {
            Voice* oldest = nullptr;
            for (auto& v : voices)
                if (v.slot >= 0 && v.fadeLeft < 0 && (oldest == nullptr || v.order < oldest->order)) oldest = &v;
            if (oldest != nullptr) oldest->fadeLeft = stopFade;
        }

        Voice* free = nullptr;
        for (auto& v : voices) if (v.slot < 0) { free = &v; break; }
        if (free == nullptr) // pool full (8 sounding + the rest fading): take the fading one closest to silence
            for (auto& v : voices)
                if (v.fadeLeft >= 0 && (free == nullptr || v.fadeLeft < free->fadeLeft)) free = &v;
        *free = { s, clip.id, 0, ++voiceOrder, slotGain[s].load (std::memory_order_relaxed), -1 };
    }

    void render (float* out, float* mon, int n) noexcept
    {
        renderStarts.fetch_add (1);
        const double sr = rate.load (std::memory_order_relaxed);
        const int stopFade = std::max (1, int (dsp::msToSamples (kStopFadeMs, sr)));

        Clip* cur[kSoundboardSlots];
        for (int s = 0; s < kSoundboardSlots; ++s) cur[s] = clips[s].load();

        // commands from the message thread; ones queued while no device was running are dropped,
        // so opening the device later does not fire a burst of old triggers
        const auto now = juce::Time::getMillisecondCounter();
        int s1, n1, s2, n2;
        commandFifo.prepareToRead (commandFifo.getNumReady(), s1, n1, s2, n2);
        for (int k = 0; k < n1 + n2; ++k)
        {
            const auto& cmd = commands[k < n1 ? s1 + k : s2 + (k - n1)];
            if (now - cmd.ms > kCommandMaxAgeMs) continue;
            const int c = cmd.what;
            if (c == kCmdStopAll)
            {
                for (auto& v : voices) if (v.slot >= 0 && v.fadeLeft < 0) v.fadeLeft = stopFade;
            }
            else if (c >= 0 && c < kSoundboardSlots && cur[c] != nullptr)
                startVoice (c, *cur[c], stopFade);
        }
        commandFifo.finishedRead (n1 + n2);

        // voices whose sound was replaced or cleared stop at once (ids, not pointers: no ABA)
        for (auto& v : voices)
            if (v.slot >= 0 && (cur[v.slot] == nullptr || cur[v.slot]->id != v.clipId)) v.slot = -1;

        for (auto& v : voices)
        {
            if (v.slot < 0) continue;
            const auto& x = cur[v.slot]->samples;
            const auto len = juce::int64 (x.size());
            const bool loop = slotLoop[v.slot].load (std::memory_order_relaxed);
            const bool toMon = slotMonitor[v.slot].load (std::memory_order_relaxed);
            const float g0 = v.gain, target = slotGain[v.slot].load (std::memory_order_relaxed);
            const float dg = (target - g0) / float (n);
            for (int i = 0; i < n; ++i)
            {
                if (v.pos >= len)
                {
                    if (! loop) { v.slot = -1; break; }
                    v.pos = 0;
                }
                float y = x[size_t (v.pos++)] * (g0 + dg * float (i + 1));
                if (v.fadeLeft >= 0)
                {
                    if (v.fadeLeft == 0) { v.slot = -1; break; }
                    y *= float (v.fadeLeft--) / float (stopFade);
                }
                out[i] += y;
                if (toMon) mon[i] += y;
            }
            v.gain = target;
        }

        // test tone (F-10-3): output only, 10 ms fades
        const bool toneOn = testTone.load (std::memory_order_relaxed);
        if (toneOn || toneGain > 0.0f)
        {
            const float amp = dsp::dbToGain (kTestToneDb);
            const float step = 1.0f / std::max (1.0f, dsp::msToSamples (kToneFadeMs, sr));
            const double inc = 2.0 * juce::MathConstants<double>::pi * kTestToneHz / sr;
            for (int i = 0; i < n; ++i)
            {
                toneGain = toneOn ? std::min (1.0f, toneGain + step) : std::max (0.0f, toneGain - step);
                out[i] += amp * toneGain * float (std::sin (tonePhase));
                tonePhase += inc;
                if (tonePhase >= 2.0 * juce::MathConstants<double>::pi) tonePhase -= 2.0 * juce::MathConstants<double>::pi;
            }
        }

        // playing state, positions (newest voice per slot) + ducking (F-06-7): down in ~20 ms while anything sounds, back in ~300 ms
        juce::uint32 mask = 0;
        const Voice* newest[kSoundboardSlots] {};
        for (auto& v : voices)
            if (v.slot >= 0 && v.fadeLeft < 0)
            {
                mask |= 1u << v.slot;
                if (newest[v.slot] == nullptr || v.order > newest[v.slot]->order) newest[v.slot] = &v;
            }
        for (int s = 0; s < kSoundboardSlots; ++s)
            slotPosition[s].store (newest[s] != nullptr ? double (newest[s]->pos) / sr : 0.0, std::memory_order_relaxed);
        playingMask.store (mask);
        const float duckDb = duckingDb.load (std::memory_order_relaxed);
        const float target = mask != 0 && duckDb < 0.0f ? dsp::dbToGain (duckDb) : 1.0f;
        const double tau = target < duck ? kDuckAttackSeconds : kDuckReleaseSeconds;
        duck = target + (duck - target) * float (std::exp (-double (n) / sr / tau));
        duckOut.store (duck, std::memory_order_relaxed);
        renderEnds.fetch_add (1);
    }
};

// ==================================================================== Soundboard
Soundboard::Soundboard() : impl (std::make_unique<Impl> (*this)) {}
Soundboard::~Soundboard() = default;

static bool validSlot (int s) { return s >= 0 && s < kSoundboardSlots; }

static SoundboardSlotDef clampDef (SoundboardSlotDef d)
{
    d.volumeDb = std::isfinite (d.volumeDb) ? kSoundboardVolumeDb.clamp (d.volumeDb) : 0.0f;
    d.retrigger = std::clamp (d.retrigger, 0, 2);
    return d;
}

void Soundboard::prepare (double sampleRate)
{
    if (sampleRate <= 0.0 || sampleRate == impl->rate.load()) return;
    impl->rate.store (sampleRate);
    const juce::ScopedLock sl (impl->lock);
    for (int s = 0; s < kSoundboardSlots; ++s)
        if (impl->defs[s].file.isNotEmpty()) impl->startLoadLocked (s); // decoded for the old rate
}

void Soundboard::load (const juce::File& soundboardJson)
{
    SoundboardSlotDef defs[kSoundboardSlots];
    juce::var root;
    if (soundboardJson.existsAsFile() && juce::JSON::parse (soundboardJson.loadFileAsString(), root).wasOk() && root.isObject())
        if (auto* slots = root["slots"].getArray())
            for (int s = 0; s < std::min (kSoundboardSlots, slots->size()); ++s)
            {
                const auto& o = slots->getReference (s);
                if (! o.isObject()) continue;
                auto& d = defs[s];
                d.file = o["file"].toString();
                d.volumeDb = float (double (o["volumeDb"]));
                d.loop = bool (o["loop"]);
                const auto rt = o["retrigger"].toString();
                for (int i = 0; i < 3; ++i) if (rt == kRetriggerIds[i]) d.retrigger = i;
                d.toMonitor = o.hasProperty ("toMonitor") ? bool (o["toMonitor"]) : true;
                d = clampDef (d);
            }

    const juce::ScopedLock sl (impl->lock);
    for (int s = 0; s < kSoundboardSlots; ++s)
    {
        impl->defs[s] = defs[s];
        impl->applyAtomics (s);
        impl->startLoadLocked (s);
    }
}

bool Soundboard::save (const juce::File& soundboardJson) const
{
    juce::Array<juce::var> slots;
    {
        const juce::ScopedLock sl (impl->lock);
        for (auto& d : impl->defs)
        {
            auto* o = new juce::DynamicObject();
            o->setProperty ("file", d.file);
            o->setProperty ("volumeDb", double (d.volumeDb));
            o->setProperty ("loop", d.loop);
            o->setProperty ("retrigger", kRetriggerIds[std::clamp (d.retrigger, 0, 2)]);
            o->setProperty ("toMonitor", d.toMonitor);
            slots.add (juce::var (o));
        }
    }
    auto* root = new juce::DynamicObject();
    root->setProperty ("version", 1);
    root->setProperty ("slots", slots);
    soundboardJson.getParentDirectory().createDirectory();
    return soundboardJson.replaceWithText (juce::JSON::toString (juce::var (root)), false, false, "\n");
}

void Soundboard::assignFile (int slot, const juce::File& file)
{
    if (! validSlot (slot)) return;
    const juce::ScopedLock sl (impl->lock);
    impl->defs[slot].file = file.getFullPathName();
    impl->startLoadLocked (slot);
}

void Soundboard::clearSlot (int slot)
{
    if (! validSlot (slot)) return;
    const juce::ScopedLock sl (impl->lock);
    impl->defs[slot] = {};
    impl->applyAtomics (slot);
    impl->startLoadLocked (slot); // empty path: drops the sound, state empty
}

SoundboardSlotDef Soundboard::getSlotDef (int slot) const
{
    if (! validSlot (slot)) return {};
    const juce::ScopedLock sl (impl->lock);
    return impl->defs[slot];
}

void Soundboard::setSlotDef (int slot, const SoundboardSlotDef& def)
{
    if (! validSlot (slot)) return;
    const auto d = clampDef (def);
    const juce::ScopedLock sl (impl->lock);
    const bool fileChanged = d.file != impl->defs[slot].file;
    impl->defs[slot] = d;
    impl->applyAtomics (slot);
    if (fileChanged) impl->startLoadLocked (slot);
}

SoundSlotState Soundboard::getSlotState (int slot) const
{
    if (! validSlot (slot)) return {};
    impl->collectGarbage();
    const juce::ScopedLock sl (impl->lock);
    auto st = impl->states[slot];
    st.playing = st.status == SoundSlotState::Status::ready && ((impl->playingMask.load() >> slot) & 1u) != 0;
    st.positionSeconds = st.playing ? impl->slotPosition[slot].load (std::memory_order_relaxed) : 0.0;
    return st;
}

long long Soundboard::getTotalBytes() const
{
    const juce::ScopedLock sl (impl->lock);
    long long total = 0;
    for (auto b : impl->bytes) total += b;
    return total;
}

void Soundboard::trigger (int slot) { if (validSlot (slot)) impl->pushCommand (slot); }
void Soundboard::stopAll() { impl->pushCommand (kCmdStopAll); }
void Soundboard::setDuckingDb (float db) { impl->duckingDb.store (std::isfinite (db) ? kDuckingDb.clamp (db) : 0.0f); }
void Soundboard::setTestTone (bool on) { impl->testTone.store (on); }
void Soundboard::render (float* toOutput, float* toMonitor, int numSamples) { impl->render (toOutput, toMonitor, numSamples); }
float Soundboard::voiceDuckGain() const { return impl->duckOut.load (std::memory_order_relaxed); }
} // namespace koe

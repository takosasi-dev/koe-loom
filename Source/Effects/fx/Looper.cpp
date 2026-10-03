#include "Dsp/Building.h"
#include "Effects/IEffect.h"

#include <juce_audio_basics/juce_audio_basics.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <vector>

// looper (koeloom_effects.md §5.2, F-04-18). Memory only: 60 s are allocated in prepare(), nothing ever
// touches a file. States (getUiState): 0 empty, 1 recording, 2 playing, 3 overdubbing.
//   looperRecordPlay: empty -> recording -> playing -> overdubbing -> playing -> overdubbing ...
//   looperClear: any state -> empty (the loop fades out over 20 ms, the buffer is just forgotten).
//   Recording stops by itself at maxSec (latched when the recording starts: a smaller maxSec only
//   applies to the next recording). The loop length is exactly the number of samples recorded.
// The live voice always passes; the loop is added at levelDb. Overdub adds the voice into the loop
// (write gain ramps over 5 ms so the loop gets no step where an overdub starts or ends). The loop is
// played with 5 ms fades at its two ends (not written into the buffer), so the wrap does not click.
// reset() (slot OFF -> ON, F-04-7) keeps the recording: an unfinished recording / overdub is closed at
// the last sample processed before it and the loop plays again from its start ("再生待ち" -> 再生中).
// Latency 0.

namespace koe
{
namespace
{
class LooperEffect final : public IEffect
{
public:
    enum State { empty = 0, recording = 1, playing = 2, overdubbing = 3 };

    void prepare (double sampleRate, int) override
    {
        sr = sampleRate;
        buf.assign (size_t (sr * 60.0) + 1, 0.0f);
        edge = std::max (1, int (sr * 0.005));
        level.prepare (sr, 30.0f);
        out.prepare (sr, 20.0f);
        dub.prepare (sr, 5.0f);
    }

    void reset() override
    {
        if (state == recording) finishRecording();
        if (state == overdubbing) state = playing;
        dub.snap (0.0f);
        pos = 0;
        level.snap (level.target);
        out.snap (state == empty ? 0.0f : 1.0f);
        uiState.store (state);
    }

    void setParam (int index, float v) override
    {
        if (index == 0) level.setTarget (dsp::dbToGain (v));
        else if (index == 1) maxSec = int (v);
    }

    void trigger (EffectTrigger t) override
    {
        if (t == EffectTrigger::looperClear)
        {
            if (state == recording) len = 0; // nothing to fade out
            state = empty;
            dub.setTarget (0.0f);
            out.setTarget (0.0f);
        }
        else if (t == EffectTrigger::looperRecordPlay)
        {
            switch (state)
            {
                case empty:
                    state = recording;
                    recLen = 0;
                    recMax = std::min (int (buf.size()) - 1, int (sr * maxSec));
                    out.snap (0.0f);
                    break;
                case recording: finishRecording(); break;
                case playing: state = overdubbing; dub.setTarget (1.0f); break;
                case overdubbing: state = playing; dub.setTarget (0.0f); break;
                default: break;
            }
        }
        uiState.store (state);
    }

    int getUiState() const override { return uiState.load(); }

    void process (float* x, int n) override
    {
        juce::ScopedNoDenormals noDenormals;
        for (int i = 0; i < n; ++i)
        {
            const float in = x[i];
            if (state == recording)
            {
                buf[size_t (recLen++)] = in;
                if (recLen >= recMax) { finishRecording(); uiState.store (state); }
                continue;
            }
            const float o = out.next(), g = level.next(), d = dub.next();
            if (len <= 0 || (o <= 0.0f && d <= 0.0f)) continue;

            const int fromEnd = len - 1 - pos;
            const int e = std::min (pos, fromEnd);
            const float w = e >= edgeLen ? 1.0f : (float (e) + 0.5f) / float (edgeLen);
            float& s = buf[size_t (pos)];
            x[i] = in + o * g * w * s;
            if (d > 0.0f) s += d * in;
            if (++pos >= len) pos = 0;
        }
    }

private:
    void finishRecording() noexcept
    {
        len = recLen;
        pos = 0;
        edgeLen = std::max (1, std::min (edge, len / 2));
        state = len > 0 ? playing : empty;
        out.snap (len > 0 ? 1.0f : 0.0f);
    }

    double sr = 48000.0;
    std::vector<float> buf;
    State state = empty;
    int len = 0, recLen = 0, recMax = 0, pos = 0, maxSec = 30, edge = 240, edgeLen = 1;
    dsp::Ramp level, out, dub;
    std::atomic<int> uiState { 0 };
};
} // namespace

KOE_REGISTER_EFFECT ("looper", LooperEffect)
} // namespace koe

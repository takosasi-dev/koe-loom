#include "Dsp/Building.h"
#include "Effects/IEffect.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

// stutter (koeloom_effects.md §3, §5.1, spec F-04-13/14). Sections of one `division` note at `bpm`,
// counted from reset(). At each section start a fixed-seed RNG decides (probability `chance`) whether
// the section stutters: it is cut into `repeatCount` pieces, the first plays live, the others replay
// that first piece. bpm / division / repeatCount / chance are latched per section, so knob moves never
// cut a piece short. Every source switch is a short raised-cosine crossfade (<= 3 ms) from the
// continuation of the old source. reset() clears the history and reseeds -> bit-identical replays.

namespace koe
{
namespace
{
constexpr uint32_t kSeed = 0x5EED1234u;

class StutterEffect final : public IEffect
{
public:
    void prepare (double sampleRate, int) override
    {
        sr = sampleRate;
        // longest section: a quarter note at 40 BPM (1.5 s)
        history.prepare (int (sr * 1.5) + 64);
        mix.prepare (sr, 30.0f);
        maxFade = std::max (1, int (sr * 0.003));
    }

    void reset() override
    {
        history.reset();
        rng.seed (kSeed);
        mix.snap (mix.target);
        t = 0;
        nextStart = 0.0;
        sectionStart = 0;
        triggered = false;
        pieces = 1;
        pieceLen = 1;
        fade = 1;
        tailPos = tailLen = 0;
        tailDelay = 0;
    }

    void setParam (int index, float v) override
    {
        switch (index)
        {
            case 0: bpm = v; break;
            case 1: division = std::clamp (int (v), 0, 3); break;
            case 2: repeatCount = std::clamp (int (v), 2, 8); break;
            case 3: chance = v; break;
            case 4: mix.setTarget (v); break;
            default: break;
        }
    }

    void process (float* x, int n) override
    {
        for (int i = 0; i < n; ++i)
        {
            const float in = x[i];
            history.push (in);
            if (double (t) >= nextStart) startSection();

            const int local = int (t - sectionStart);
            float s = in;
            if (triggered)
            {
                const int k = std::min (pieces - 1, local / pieceLen);
                if (k > 0)
                {
                    const int j = local - k * pieceLen;
                    s = history.readInt (k * pieceLen);
                    if (j < fade)
                        s = crossfade (history.readInt ((k - 1) * pieceLen), s, j, fade);
                }
            }
            if (tailPos < tailLen) // leaving a stuttered section: fade out its last piece's continuation
            {
                s = crossfade (history.readInt (tailDelay), s, tailPos, tailLen);
                ++tailPos;
            }

            const float m = mix.next();
            x[i] = in + m * (s - in);
            ++t;
        }
    }

private:
    static float crossfade (float from, float to, int pos, int len) noexcept
    {
        const float w = 0.5f - 0.5f * std::cos (dsp::kPi * (float (pos) + 0.5f) / float (len));
        return from + w * (to - from);
    }

    void startSection() noexcept
    {
        static constexpr double kDenominator[4] = { 4.0, 8.0, 16.0, 32.0 };
        if (triggered)
        {
            tailDelay = (pieces - 1) * pieceLen; // constant delay = keep playing the last piece
            tailLen = fade;
            tailPos = 0;
        }
        const double len = sr * 60.0 / double (bpm) * 4.0 / kDenominator[division];
        sectionStart = t;
        nextStart += len;
        pieces = repeatCount;
        pieceLen = std::max (1, int (len / double (pieces)));
        fade = std::clamp (pieceLen / 4, 1, maxFade);
        triggered = rng.nextFloat01() < chance;
    }

    double sr = 48000.0, nextStart = 0.0;
    float bpm = 120.0f, chance = 0.5f;
    int division = 2, repeatCount = 4;
    int64_t t = 0, sectionStart = 0;
    bool triggered = false;
    int pieces = 1, pieceLen = 1, fade = 1, maxFade = 144;
    int tailPos = 0, tailLen = 0, tailDelay = 0;
    dsp::DelayLine history;
    dsp::Rng rng;
    dsp::Ramp mix;
};
} // namespace

KOE_REGISTER_EFFECT ("stutter", StutterEffect)
} // namespace koe

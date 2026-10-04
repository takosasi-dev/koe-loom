#include "Dsp/Building.h"
#include "Effects/IEffect.h"

#include <juce_audio_basics/juce_audio_basics.h>

#include <algorithm>
#include <cmath>

// voicechar (koeloom_effects.md §3, §5.1). One chain per kind:
//   HP -> LP (pre-drive) -> presence peak -> compressor -> tanh drive -> HP -> LP x2 (post-drive) -> makeup
// telephone: 350-3400 Hz band, light drive. radio: 220-4800 Hz, asymmetric (tube-like) drive.
// megaphone: 500-4200 Hz, strong 1.6 kHz horn peak, hard drive. walkie: 650-2700 Hz, heavy fast
// compression and drive. `intensity` moves every setting from neutral (0) to the full character (1):
// cutoffs glide exponentially from 20 Hz / 20 kHz, peak/drive/makeup in dB, ratio linearly; below
// intensity 0.05 the chain also fades back to the untouched input, so 0 is exactly the original.
// Changing `kind` crossfades from the old chain to a fresh one (30 ms). mix is linear.
//
// Wave 7 kinds (4..9) run the same chain with their own settings, then a kind-specific stage whose
// amount follows `intensity` (the first four kinds return before it, so their output is unchanged):
//   helmet     short visor reflections (0.9-4.1 ms) + 3.3 ms resonant feedback + breath noise
//   underwater 120-900 Hz, wobbling delay (0.8 Hz + 3.3 Hz pitch sway) + random rising resonances (bubbles)
//   wall       80-700 Hz (4th-order), 170 Hz thump, 11.3 / 16.9 ms feedback taps (the next room ringing)
//   vinyl      150-6000 Hz, 0.55 Hz turntable wow (delay) + crackle ticks and faint surface hiss
//   gasmask    250-2600 Hz, sharp 1.1 kHz canister resonance, 0.7 ms tube feedback + valve hiss in breaths
//   stadium    PA horn band + 160 / 290 / 430 ms dark reflections from the stands (weak feedback)
// Breath / crackle / hiss follow the voice envelope (150 ms release; crackle is gated below about -46 dBFS):
// silence in gives silence out.
// underwater and vinyl delay their wet path by the centre of the wobble (1.5 ms); their dry path is
// delayed by the same amount and getLatencySamples() reports it (F-04-12).

namespace koe
{
namespace
{
struct Character
{
    float hpHz, lpHz, peakHz, peakQ, peakDb, thresholdDb, ratio, driveDb, bias, makeupDb;
};

enum Kind { telephone, radio, megaphone, walkie, helmet, underwater, wall, vinyl, gasmask, stadium, kNumKinds };
constexpr int kFirstNewKind = helmet;
constexpr float kWobbleCentreMs = 1.5f;

// makeupDb was set so the synthetic test voice keeps its RMS within about 1 dB at intensity 1.
constexpr Character kCharacters[kNumKinds] = {
    { 350.0f, 3400.0f, 1800.0f, 1.0f, 4.0f, -26.0f, 3.0f, 6.0f, 0.0f, 8.0f },     // telephone
    { 220.0f, 4800.0f, 2500.0f, 0.8f, 3.0f, -24.0f, 3.0f, 9.0f, 0.25f, 6.5f },    // radio
    { 500.0f, 4200.0f, 1600.0f, 1.4f, 9.0f, -22.0f, 2.0f, 18.0f, 0.1f, 8.3f },    // megaphone
    { 650.0f, 2700.0f, 1400.0f, 1.0f, 5.0f, -34.0f, 8.0f, 14.0f, 0.0f, 20.0f },   // walkie
    { 180.0f, 5500.0f, 2200.0f, 2.0f, 5.0f, -26.0f, 2.5f, 4.0f, 0.0f, 6.8f },     // helmet
    { 120.0f, 900.0f, 450.0f, 1.2f, 4.0f, -28.0f, 2.0f, 2.0f, 0.0f, 6.0f },       // underwater
    { 80.0f, 700.0f, 170.0f, 1.0f, 5.0f, -30.0f, 2.0f, 0.0f, 0.0f, 6.0f },        // wall
    { 150.0f, 6000.0f, 1500.0f, 0.7f, 2.0f, -24.0f, 2.0f, 5.0f, 0.15f, 5.1f },    // vinyl
    { 250.0f, 2600.0f, 1100.0f, 3.0f, 10.0f, -26.0f, 3.0f, 6.0f, 0.0f, 8.2f },    // gasmask
    { 350.0f, 4500.0f, 1800.0f, 1.2f, 7.0f, -24.0f, 3.0f, 12.0f, 0.1f, 6.0f },    // stadium
};

inline bool hasWobble (int kind) noexcept { return kind == underwater || kind == vinyl; }

struct Chain
{
    /** Allocates the delay lines (non-audio thread). */
    void prepare (double sampleRate)
    {
        rate = sampleRate;
        auto ms = [this] (float m) { return std::max (1, int (std::lround (m * 0.001 * rate))); };
        room.prepare (ms (500.0f));
        wob.prepare (ms (4.0f));
        dryLine.prepare (ms (4.0f));
        centre = ms (kWobbleCentreMs);
        tHelmet[0] = ms (0.9f); tHelmet[1] = ms (1.7f); tHelmet[2] = ms (2.6f); tHelmet[3] = ms (4.1f);
        tHelmetFb = ms (3.3f);
        tWall[0] = ms (11.3f); tWall[1] = ms (16.9f);
        tMask = ms (0.7f);
        tStadium[0] = ms (160.0f); tStadium[1] = ms (290.0f); tStadium[2] = ms (430.0f);
        breathBp.setBandpass (rate, 1200.0f, 0.7f);
        hissHp.setHighpass (rate, 3000.0f, 0.7071f);
        crackleHp.setHighpass (rate, 2000.0f, 0.7071f);
        surfaceLp.setCutoff (rate, 5000.0f);
        echoLp.setCutoff (rate, 2500.0f);
        voiceEnv.setTimes (rate, 5.0f, 150.0f);
        lfoA.setRate (rate, 0.8f);
        lfoB.setRate (rate, 3.3f);
        lfoWow.setRate (rate, 0.55f);
        lfoBreath.setRate (rate, 0.25f);
        bubbleLen = ms (45.0f);
    }

    void reset()
    {
        for (auto* f : { &hp1, &hp2, &lpPre, &peak, &lpPost1, &lpPost2 }) f->reset();
        env = 0.0f;
        for (auto* f : { &breathBp, &hissHp, &crackleHp, &bubble }) f->reset();
        bubble.setPeak (rate, 1000.0f, 6.0f, 0.0f);
        surfaceLp.reset();
        echoLp.reset();
        voiceEnv.reset();
        room.reset();
        wob.reset();
        dryLine.reset();
        rng.seed (0x7a11c0deu);
        lfoA.reset (0.0f);
        lfoB.reset (0.3f);
        lfoWow.reset (0.0f);
        lfoBreath.reset (0.75f);
        bubbleLeft = 0;
        bubbleTick = 0;
        lastDry = 0.0f;
    }

    void configure (double sr, int k, float intensity)
    {
        kind = k;
        amt = intensity;
        const auto& c = kCharacters[k];
        const float i = intensity;
        const float hp = 20.0f * std::pow (c.hpHz / 20.0f, i);
        const float lp = 20000.0f * std::pow (c.lpHz / 20000.0f, i);
        hp1.setHighpass (sr, hp, 0.5412f);
        hp2.setHighpass (sr, hp, 1.3066f);
        lpPre.setLowpass (sr, std::min (lp * 1.15f, 20000.0f), 0.7071f);
        lpPost1.setLowpass (sr, lp, 0.5412f); // 4th-order Butterworth after the drive
        lpPost2.setLowpass (sr, lp, 1.3066f);
        peak.setPeak (sr, c.peakHz, c.peakQ, c.peakDb * i);
        threshold = c.thresholdDb;
        slope = 1.0f - 1.0f / (1.0f + (c.ratio - 1.0f) * i);
        drive = dsp::dbToGain (c.driveDb * i);
        bias = c.bias * i;
        biasOut = std::tanh (bias);
        makeup = dsp::dbToGain (c.makeupDb * i);
    }

    float process (float x, float att, float rel) noexcept
    {
        float y = peak.process (lpPre.process (hp1.process (x)));
        const float a = std::abs (y);
        env = a > env ? a + att * (env - a) : a + rel * (env - a);
        const float over = dsp::gainToDb (env) - threshold;
        if (over > 0.0f) y *= dsp::dbToGain (-slope * over);
        y = (std::tanh (drive * y + bias) - biasOut) / drive; // unity small-signal gain
        y = lpPost2.process (lpPost1.process (hp2.process (y))) * makeup; // hp2 also removes the drive's DC
        if (kind < kFirstNewKind)
        {
            lastDry = x;
            return y;
        }
        return processNewKind (x, y);
    }

    float lastDry = 0.0f; // the dry sample aligned with the last output (delayed for the wobble kinds)

private:
    float processNewKind (float x, float y) noexcept
    {
        const float e = voiceEnv.process (x);
        const float gate = std::clamp ((e - 0.005f) * 20.0f, 0.0f, 1.0f); // 0 below about -46 dBFS
        lastDry = x;
        switch (kind)
        {
            case helmet:
            {
                const float early = 0.5f * room.readInt (tHelmet[0] - 1) + 0.35f * room.readInt (tHelmet[1] - 1)
                                    - 0.3f * room.readInt (tHelmet[2] - 1) + 0.2f * room.readInt (tHelmet[3] - 1);
                const float v = y + 0.4f * amt * room.readInt (tHelmetFb - 1);
                room.push (v);
                const float b = 0.5f + 0.5f * lfoBreath.nextSine();
                const float breath = breathBp.process (rng.nextBipolar());
                return v + amt * (0.5f * early + 0.25f * e * b * breath);
            }
            case underwater:
            {
                wob.push (y);
                dryLine.push (x);
                lastDry = dryLine.readInt (centre);
                const float sway = 0.6f * lfoA.nextSine() + 0.25f * lfoB.nextSine(); // ms
                const float out = wob.readCubic (float (centre) + amt * sway * 0.001f * float (rate));
                if (bubbleLeft <= 0 && rng.nextFloat01() < 2.5f / float (rate))
                {
                    bubbleLeft = bubbleLen;
                    bubbleTick = 0;
                }
                if (bubbleLeft > 0)
                {
                    if ((bubbleTick++ & 15) == 0)
                    {
                        const float prog = 1.0f - float (bubbleLeft) / float (bubbleLen);
                        bubble.setPeak (rate, 350.0f * std::pow (4.0f, prog), 6.0f, 12.0f * amt * std::sin (dsp::kPi * prog));
                    }
                    if (--bubbleLeft == 0) bubble.setPeak (rate, 1000.0f, 6.0f, 0.0f);
                }
                return bubble.process (out);
            }
            case vinyl:
            {
                wob.push (y);
                dryLine.push (x);
                lastDry = dryLine.readInt (centre);
                const float out = wob.readCubic (float (centre) + amt * 0.8f * lfoWow.nextSine() * 0.001f * float (rate));
                float tick = 0.0f;
                if (rng.nextFloat01() < 40.0f / float (rate))
                {
                    const float r = rng.nextFloat01();
                    tick = (0.05f + 0.25f * r * r * r) * (rng.nextFloat01() < 0.5f ? -1.0f : 1.0f);
                }
                const float surface = surfaceLp.process (rng.nextBipolar());
                return out + amt * gate * (crackleHp.process (tick) + 0.01f * surface);
            }
            case wall:
            {
                const float fb = 0.5f * (room.readInt (tWall[0] - 1) + room.readInt (tWall[1] - 1));
                const float v = y + 0.5f * amt * fb;
                room.push (v);
                return v;
            }
            case gasmask:
            {
                const float v = y + 0.55f * amt * room.readInt (tMask - 1);
                room.push (v);
                const float s = std::max (0.0f, lfoBreath.nextSine());
                return v + amt * 0.2f * e * s * s * hissHp.process (rng.nextBipolar());
            }
            case stadium:
            {
                const float echo = echoLp.process (0.45f * room.readInt (tStadium[0] - 1) + 0.32f * room.readInt (tStadium[1] - 1)
                                                   + 0.22f * room.readInt (tStadium[2] - 1));
                room.push (y + 0.25f * echo);
                return y + amt * 0.8f * echo;
            }
            default: return y;
        }
    }

    dsp::Biquad hp1, hp2, lpPre, peak, lpPost1, lpPost2;
    float env = 0.0f, threshold = 0.0f, slope = 0.0f, drive = 1.0f, bias = 0.0f, biasOut = 0.0f, makeup = 1.0f;

    int kind = 0;
    double rate = 48000.0;
    float amt = 0.0f;
    dsp::DelayLine room, wob, dryLine;
    int centre = 72, tHelmet[4] {}, tHelmetFb = 1, tWall[2] {}, tMask = 1, tStadium[3] {};
    dsp::Biquad breathBp, hissHp, crackleHp, bubble;
    dsp::OnePoleLowpass surfaceLp, echoLp;
    dsp::EnvelopeFollower voiceEnv;
    dsp::Lfo lfoA, lfoB, lfoWow, lfoBreath;
    dsp::Rng rng;
    int bubbleLeft = 0, bubbleLen = 1, bubbleTick = 0;
};

class VoiceCharEffect final : public IEffect
{
public:
    void prepare (double sampleRate, int) override
    {
        sr = sampleRate;
        intensity.prepare (sr, 30.0f);
        mix.prepare (sr, 30.0f);
        chainFade.prepare (sr, 30.0f);
        att = dsp::onePoleCoeff (2.0f, sr);
        rel = dsp::onePoleCoeff (90.0f, sr);
        chains[0].prepare (sr);
        chains[1].prepare (sr);
        latency = std::max (1, int (std::lround (kWobbleCentreMs * 0.001 * sr)));
    }

    void reset() override
    {
        intensity.snap (intensity.target);
        mix.snap (mix.target);
        chainFade.snap (1.0f);
        cur = 0;
        kindOf[0] = kindOf[1] = kind;
        chains[0].reset();
        chains[1].reset();
        configure();
    }

    void setParam (int index, float v) override
    {
        switch (index)
        {
            case 0:
            {
                const int k = std::clamp (int (v), 0, kNumKinds - 1);
                if (k != kind)
                {
                    kind = k;
                    cur ^= 1;
                    kindOf[cur] = k;
                    chains[cur].reset();
                    chains[cur].configure (sr, k, intensity.value);
                    chainFade.snap (0.0f);
                    chainFade.setTarget (1.0f);
                }
                break;
            }
            case 1: intensity.setTarget (v); break;
            case 2: mix.setTarget (v); break;
            default: break;
        }
    }

    int getLatencySamples() const override { return hasWobble (kind) ? latency : 0; }

    void process (float* x, int n) override
    {
        juce::ScopedNoDenormals noDenormals;
        for (int i = 0; i < n; ++i)
        {
            const float it = intensity.next();
            if ((++sinceConfig & 31) == 0 && it != configured) configure();

            const float in = x[i];
            float wet = chains[cur].process (in, att, rel);
            float dry = chains[cur].lastDry; // == in for the kinds without latency
            if (chainFade.isRamping())
            {
                const float f = chainFade.next();
                wet = f * wet + (1.0f - f) * chains[cur ^ 1].process (in, att, rel);
                const float otherDry = chains[cur ^ 1].lastDry;
                if (otherDry != dry) dry = f * dry + (1.0f - f) * otherDry;
            }
            const float engage = std::min (1.0f, it * 20.0f);
            wet = dry + engage * (wet - dry);
            const float m = mix.next();
            x[i] = dry + m * (wet - dry);
        }
    }

private:
    void configure() noexcept
    {
        configured = intensity.value;
        chains[0].configure (sr, kindOf[0], intensity.value);
        chains[1].configure (sr, kindOf[1], intensity.value);
    }

    double sr = 48000.0;
    Chain chains[2];
    int kind = 0, cur = 0, kindOf[2] { 0, 0 }, sinceConfig = 0, latency = 72;
    float att = 0.0f, rel = 0.0f, configured = -1.0f;
    dsp::Ramp intensity, mix, chainFade;
};
} // namespace

KOE_REGISTER_EFFECT ("voicechar", VoiceCharEffect)
} // namespace koe

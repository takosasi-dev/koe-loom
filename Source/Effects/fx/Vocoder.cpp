#include "Dsp/Building.h"
#include "Dsp/PitchDetector.h"
#include "Effects/IEffect.h"

#include <juce_audio_basics/juce_audio_basics.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <initializer_list>

// vocoder (koeloom_effects.md §5.2, F-04-21). Channel vocoder with an internal carrier only.
//   Bank: `bands` constant-Q band-passes (each two cascaded 2nd-order sections), log spaced over the
//   character's range. Each voice band's envelope (attackMs / releaseMs, scaled by the character)
//   scales the same band of the carrier.
//   Carrier: band-limited saw / square (PolyBLEP) or white noise; chord = up to 3 oscillators
//   (root, +3/+4, +7 semitones; each fades in/out over 20 ms). Pitch: fixed = noteSt; follow = its own
//   MPM detector (spec §5.7) on the slot input, median of the last 3 detections (drops the single-hop
//   octave outliers Phase 0 saw), glide 8 ms. E-28: when the voice is unvoiced the carrier keeps the
//   last note for 50 ms, then fades out over 10 ms; it starts again (no glide) at the next voiced note.
//   The noise carrier has no pitch and always runs. The detector only runs in follow mode.
//   Weights: a saw's band amplitude falls as fc^-0.5 and white noise rises as fc^+0.5, so bands get
//   fc^+0.5 / fc^-0.5 (flat), times the character's tilt.
//   character: vintage = 150 Hz-5 kHz, wide bands (Q x0.55), envelopes x1.5 slower, slightly dark;
//              modern  = 100 Hz-8 kHz, narrow bands, x1, +1.5 dB / oct presence;
//              talkbox = 100 Hz-6 kHz, narrow bands, x0.7 faster, +6 dB bumps at 800 Hz and 1.8 kHz
//                        (the mouth cavity). Coefficient values are our choice ([暫定] in the spec).
// The vocoded signal is brought to the input's loudness (100 ms RMS match, spec §5.8); the match only
// adapts while the carrier runs, so a stopped carrier does not wind the gain up. Then
// wet = vocoded + dryDb * voice, out = voice + mix * (wet - voice). Changing character / bands / carrier
// ducks the output for 5 ms, rebuilds the bank and fades back in. Latency 0 (the detector never delays
// the audio, §5.7).

namespace koe
{
namespace
{
constexpr int kMaxBands = 32;
constexpr int kChunk = 32;
constexpr float kNoiseGain = 8.0f; // a band keeps ~1 % of white noise: +18 dB keeps the level match inside its 30 dB

struct CharacterDef
{
    float lo, hi, qScale, timeScale, tilt;
    bool mouth;
};
constexpr CharacterDef kCharacters[3] = { { 150.0f, 5000.0f, 0.55f, 1.5f, -0.1f, false },
                                          { 100.0f, 8000.0f, 1.0f, 1.0f, 0.25f, false },
                                          { 100.0f, 6000.0f, 1.0f, 0.7f, 0.0f, true } };
constexpr int kChords[4][3] = { { 0, -1, -1 }, { 0, 4, 7 }, { 0, 3, 7 }, { 0, 7, -1 } }; // -1 = unused voice

inline float polyBlep (float t, float dt) noexcept
{
    if (t < dt) { t /= dt; return t + t - t * t - 1.0f; }
    if (t > 1.0f - dt) { t = (t - 1.0f) / dt; return t * t + t + t + 1.0f; }
    return 0.0f;
}

class VocoderEffect final : public IEffect
{
public:
    void prepare (double sampleRate, int) override
    {
        sr = sampleRate;
        detector.prepare (sr);
        hop = detector.getHopSamples();
        holdSamples = int (sr * 0.05);
        noteCoeff = dsp::onePoleCoeff (8.0f, sr);
        duck.prepare (sr, 5.0f);
        mix.prepare (sr, 30.0f);
        dry.prepare (sr, 30.0f);
        carrierGain.prepare (sr, 10.0f);
        for (auto& a : voiceAmp) a.prepare (sr, 20.0f);
        match.prepare (sr, 30.0f);
        rebuild();
    }

    void reset() override
    {
        detector.reset();
        rebuild();
        duck.snap (1.0f);
        mix.snap (mix.target);
        dry.snap (dry.target);
        for (int v = 0; v < 3; ++v) voiceAmp[size_t (v)].snap (kChords[chord][v] >= 0 ? 1.0f : 0.0f);
        phase.fill (0.0f);
        match.reset();
        rng.seed (0x70C0u);
        sinceDetect = 0;
        heldFor = holdSamples + 1;
        recentCount = 0;
        curNote = targetNote = fixedPitch ? noteSt : 48.0f;
        carrierGain.snap (pitchless() ? 1.0f : 0.0f);
        carrierOn = pitchless();
    }

    void setParam (int index, float v) override
    {
        switch (index)
        {
            case 0: wantCharacter = std::clamp (int (v), 0, 2); break;
            case 1: wantBands = std::clamp (int (v), 1, kMaxBands); break;
            case 2: wantCarrier = std::clamp (int (v), 0, 2); break;
            case 3:
                if ((v >= 0.5f) != fixedPitch) detector.reset();
                fixedPitch = v >= 0.5f;
                break;
            case 4: noteSt = v; break;
            case 5:
                chord = std::clamp (int (v), 0, 3);
                for (int k = 0; k < 3; ++k) voiceAmp[size_t (k)].setTarget (kChords[chord][k] >= 0 ? 1.0f : 0.0f);
                break;
            case 6: attackMs = v; updateTimes(); break;
            case 7: releaseMs = v; updateTimes(); break;
            case 8: dry.setTarget (dsp::dbToGain (v)); break;
            case 9: mix.setTarget (v); break;
            default: break;
        }
        if (structureChanged()) duck.setTarget (0.0f);
    }

    void process (float* x, int n) override
    {
        juce::ScopedNoDenormals noDenormals;
        for (int pos = 0; pos < n; pos += kChunk)
        {
            const int len = std::min (kChunk, n - pos);
            float* xs = x + pos;
            updatePitch (xs, len);
            std::array<float, 3> inc {};
            for (int v = 0; v < 3; ++v)
                inc[size_t (v)] = std::min (0.45f, dsp::midiToHz (curNote + float (std::max (0, kChords[chord][v]))) / float (sr));

            for (int i = 0; i < len; ++i)
            {
                if (structureChanged() && duck.value <= 0.0f)
                {
                    rebuild();
                    duck.setTarget (1.0f);
                }
                const float in = xs[i];

                float car = 0.0f;
                for (int v = 0; v < 3; ++v)
                {
                    const float a = voiceAmp[size_t (v)].next();
                    float& t = phase[size_t (v)];
                    const float dt = inc[size_t (v)];
                    if (a > 0.0f)
                    {
                        if (carrier == 0) car += a * (2.0f * t - 1.0f - polyBlep (t, dt));
                        else if (carrier == 1)
                        {
                            const float t2 = t + 0.5f >= 1.0f ? t - 0.5f : t + 0.5f;
                            car += a * 0.577f * ((t < 0.5f ? 1.0f : -1.0f) + polyBlep (t, dt) - polyBlep (t2, dt));
                        }
                    }
                    t += dt;
                    if (t >= 1.0f) t -= 1.0f;
                }
                if (carrier == 2) car = kNoiseGain * rng.nextBipolar();
                car *= carrierGain.next();

                float wet = 0.0f;
                for (int k = 0; k < bands; ++k)
                {
                    const float y = section (section (in, za1[size_t (k)], za2[size_t (k)], k), zb1[size_t (k)], zb2[size_t (k)], k);
                    const float r = std::abs (y);
                    float& e = env[size_t (k)];
                    e = r + (r > e ? att : rel) * (e - r);
                    const float c = section (section (car, zc1[size_t (k)], zc2[size_t (k)], k), zd1[size_t (k)], zd2[size_t (k)], k);
                    wet += c * e * weight[size_t (k)];
                }
                wet = carrierOn ? match.process (in, wet) : wet * match.gain;
                wet = wet * duck.next() + dry.next() * in;
                xs[i] = in + mix.next() * (wet - in);
            }
        }
    }

private:
    using Bank = std::array<float, kMaxBands>;

    bool pitchless() const noexcept { return fixedPitch || carrier == 2; }
    bool structureChanged() const noexcept { return wantBands != bands || wantCharacter != character || wantCarrier != carrier; }

    // band-pass section k (TDF-II; b1 = 0, b2 = -b0)
    float section (float v, float& z1, float& z2, int k) const noexcept
    {
        const float y = b0[size_t (k)] * v + z1;
        z1 = -a1[size_t (k)] * y + z2;
        z2 = -b0[size_t (k)] * v - a2[size_t (k)] * y;
        return y;
    }

    void updatePitch (const float* xs, int len) noexcept
    {
        if (pitchless())
        {
            targetNote = fixedPitch ? noteSt : targetNote;
            carrierOn = true;
        }
        else
        {
            detector.process (xs, len);
            bool fresh = false;
            for (sinceDetect += len; sinceDetect >= hop; sinceDetect -= hop) fresh = true;
            if (detector.isVoiced())
            {
                if (fresh)
                {
                    recent[size_t (recentCount % 3)] = dsp::hzToMidi (detector.getFrequencyHz());
                    ++recentCount;
                    const float a = recent[0], b = recent[1], c = recent[2];
                    targetNote = recentCount < 3 ? recent[size_t ((recentCount - 1) % 3)]
                                                 : std::max (std::min (a, b), std::min (std::max (a, b), c));
                }
                heldFor = 0;
                carrierOn = true;
            }
            else
            {
                recentCount = 0;
                heldFor = std::min (heldFor + len, holdSamples + 1); // capped: hours of silence would overflow the int
                if (heldFor > holdSamples) carrierOn = false;
            }
        }
        if (carrierOn && carrierGain.value <= 0.0f) curNote = targetNote; // a new start does not glide
        else curNote = targetNote + std::pow (noteCoeff, float (len)) * (curNote - targetNote);
        carrierGain.setTarget (carrierOn ? 1.0f : 0.0f);
    }

    void rebuild() noexcept
    {
        bands = wantBands;
        character = wantCharacter;
        carrier = wantCarrier;
        const auto& c = kCharacters[character];
        const float bw = std::log2 (c.hi / c.lo) / float (bands);
        const float q = std::sqrt (std::exp2 (bw)) / (std::exp2 (bw) - 1.0f) * 0.8f * c.qScale;
        const float slope = (carrier == 2 ? -0.5f : 0.5f) + c.tilt;
        for (int k = 0; k < bands; ++k)
        {
            const float fc = c.lo * std::exp2 (bw * (float (k) + 0.5f));
            dsp::Biquad d;
            d.setBandpass (sr, fc, q);
            b0[size_t (k)] = d.b0;
            a1[size_t (k)] = d.a1;
            a2[size_t (k)] = d.a2;
            const float o = std::log2 (fc / 1000.0f);
            float w = std::exp2 (slope * o);
            if (c.mouth)
            {
                const float d1 = std::log2 (fc / 800.0f), d2 = std::log2 (fc / 1800.0f);
                w *= 1.0f + std::exp (-d1 * d1 / 0.245f) + std::exp (-d2 * d2 / 0.245f); // +6 dB peaks, ~0.6 oct wide
            }
            weight[size_t (k)] = w;
        }
        for (auto* z : { &za1, &za2, &zb1, &zb2, &zc1, &zc2, &zd1, &zd2, &env }) z->fill (0.0f);
        updateTimes();
    }

    void updateTimes() noexcept
    {
        const float s = kCharacters[character].timeScale;
        att = dsp::onePoleCoeff (attackMs * s, sr);
        rel = dsp::onePoleCoeff (releaseMs * s, sr);
    }

    double sr = 48000.0;
    dsp::PitchDetector detector;
    int hop = 128, sinceDetect = 0, heldFor = 0, holdSamples = 2400, recentCount = 0;
    std::array<float, 3> recent {}, phase {};
    float noteCoeff = 0.0f, curNote = 48.0f, targetNote = 48.0f;
    bool carrierOn = false, fixedPitch = false;

    int character = 1, wantCharacter = 1, bands = 16, wantBands = 16, carrier = 0, wantCarrier = 0, chord = 0;
    float noteSt = 48.0f, attackMs = 10.0f, releaseMs = 80.0f, att = 0.0f, rel = 0.0f;
    Bank b0 {}, a1 {}, a2 {}, weight {}, env {};
    Bank za1 {}, za2 {}, zb1 {}, zb2 {}, zc1 {}, zc2 {}, zd1 {}, zd2 {};
    std::array<dsp::Ramp, 3> voiceAmp;
    dsp::Ramp duck, mix, dry, carrierGain;
    dsp::RmsMatch match;
    dsp::Rng rng;
};
} // namespace

KOE_REGISTER_EFFECT ("vocoder", VocoderEffect)
} // namespace koe

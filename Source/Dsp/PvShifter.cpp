// Candidate B (spec §5.5): our own phase-vocoder pitch shifter with formant preservation.
//
// Why: Signalsmith Stretch (candidate A) is accurate only with long blocks; at the 20–40 ms blocks the
// latency budget allows, sinusoids come out tens of cents off (DiagTests, phase0/results/02-shifters.md).
// A classic phase vocoder estimates each bin's true frequency from the phase advance between frames,
// so a steady partial is reproduced at exactly alpha * f.
//
// Per frame (Hann, N samples, hop N/4):
//   1. FFT -> magnitude, phase; true frequency per bin from the phase advance.
//   2. Spectral envelope E by cepstral liftering of log|X| (keeps formants, drops harmonics).
//   3. Peaks of |X|; each peak's region (halfway to its neighbours) is moved rigidly by
//      round(alpha * f) - k bins, so every partial keeps its window-lobe shape (Laroche & Dolson 1999).
//      Magnitudes get E(j/beta)/E(k) so the formants stay put (beta = formant factor).
//   4. Phases: a peak advances from the same partial's phase in the previous frame by alpha * f;
//      the bins of its region keep the input's relative phases (identity phase locking).
//   5. IFFT, Hann synthesis window, overlap-add. The Hann window (W samples) may be shorter than the
//      FFT (zero padding): latency = W - 1 samples, hop = W / 4.

#include "Dsp/IVoiceShifter.h"

#include "Dsp/Building.h"
#include "Dsp/LevelMatcher.h"

#include <juce_dsp/juce_dsp.h>

#include <algorithm>
#include <cmath>
#include <vector>

namespace koe
{
namespace
{
constexpr float kTwoPiF = 6.283185307179586f;

inline float princarg (float p) noexcept
{
    p = std::fmod (p + dsp::kPi, kTwoPiF);
    if (p < 0) p += kTwoPiF;
    return p - dsp::kPi;
}

class PhaseVocoderShifter final : public IVoiceShifter
{
public:
    PhaseVocoderShifter (int fftOrder, int windowLength)
        : order (fftOrder), N (1 << fftOrder), W (std::clamp (windowLength / 4 * 4, 64, 1 << fftOrder)), H (W / 4), fft (fftOrder), cepFft (fftOrder) {}

    void prepare (double sr, int maxBlockSize) override
    {
        sampleRate = sr;
        juce::ignoreUnused (maxBlockSize);
        const int bins = N / 2 + 1;
        window.resize (size_t (W));
        for (int i = 0; i < W; ++i) window[size_t (i)] = 0.5f - 0.5f * std::cos (kTwoPiF * float (i) / float (W));
        // Hann analysis * Hann synthesis at 75 % overlap sums to 1.5
        olaGain = 1.0f / 1.5f;
        inRing.assign (size_t (W), 0.0f);
        olaRing.assign (size_t (2 * W), 0.0f);
        frame.assign (size_t (2 * N), 0.0f);
        cep.assign (size_t (2 * N), 0.0f);
        mag.assign (size_t (bins), 0.0f);
        phase.assign (size_t (bins), 0.0f);
        prevPhase.assign (size_t (bins), 0.0f);
        trueBin.assign (size_t (bins), 0.0f);
        env.assign (size_t (bins), 1.0f);
        outMag.assign (size_t (bins), 0.0f);
        outPhase.assign (size_t (bins), 0.0f);
        peaks.assign (size_t (bins), 0);
        curPeakIn.assign (size_t (bins), 0);
        prevPeakIn.assign (size_t (bins), 0);
        curPeakPhase.assign (size_t (bins), 0.0f);
        prevPeakPhase.assign (size_t (bins), 0.0f);
        // lifter: keep quefrencies below ~1.8 ms (resolves formants, removes harmonics of voices up to ~500 Hz)
        lifter = std::max (8, int (sr * 0.0018));
        level.prepare (sr, getLatencySamples());
        reset();
    }

    void reset() override
    {
        std::fill (inRing.begin(), inRing.end(), 0.0f);
        std::fill (olaRing.begin(), olaRing.end(), 0.0f);
        std::fill (prevPhase.begin(), prevPhase.end(), 0.0f);
        std::fill (outPhase.begin(), outPhase.end(), 0.0f);
        prevPeakCount = 0;
        inPos = 0;
        hopCount = 0;
        olaRead = 0;
        curAlpha = targetAlpha();
        curBeta = targetBeta();
        level.reset();
    }

    void setPitchSemitones (float st) override { pitchSt = st; }
    void setFormantSemitones (float st) override { formantSt = st; }
    int getLatencySamples() const override { return W - 1; }

    void process (const float* in, float* out, int n) override
    {
        for (int i = 0; i < n; ++i)
        {
            inRing[size_t (inPos)] = in[i];
            inPos = (inPos + 1) % W;
            if (++hopCount == H)
            {
                hopCount = 0;
                processFrame();
            }
            // olaRead trails the newest frame end by W - 1 samples
            out[i] = olaRing[size_t (olaRead)];
            olaRing[size_t (olaRead)] = 0.0f;
            olaRead = (olaRead + 1) % int (olaRing.size());
        }
        level.process (in, out, n);
    }

private:
    float envAt (float k) const noexcept
    {
        const int bins = N / 2 + 1;
        if (k >= float (bins - 1)) return env[size_t (bins - 1)];
        const int k0 = std::max (0, int (k));
        const float fr = k - float (k0);
        return env[size_t (k0)] + fr * (env[size_t (k0 + 1)] - env[size_t (k0)]);
    }

    float targetAlpha() const noexcept { return std::pow (2.0f, pitchSt / 12.0f); }
    float targetBeta() const noexcept { return std::pow (2.0f, formantSt / 12.0f); }

    void processFrame()
    {
        const int bins = N / 2 + 1;
        // glide parameters a little per frame (F-02-4); the vocoder itself is click-free on changes
        curAlpha += 0.35f * (targetAlpha() - curAlpha);
        curBeta += 0.35f * (targetBeta() - curBeta);
        if (std::abs (curAlpha - targetAlpha()) < 1.0e-4f) curAlpha = targetAlpha();
        if (std::abs (curBeta - targetBeta()) < 1.0e-4f) curBeta = targetBeta();

        // ---- 1. analysis ----
        for (int k = 0; k < W; ++k) frame[size_t (k)] = inRing[size_t ((inPos + k) % W)] * window[size_t (k)];
        std::fill (frame.begin() + W, frame.end(), 0.0f); // zero padding up to the FFT size
        fft.performRealOnlyForwardTransform (frame.data(), true);
        const float expected = kTwoPiF * float (H) / float (N);
        for (int k = 0; k < bins; ++k)
        {
            const float re = frame[size_t (2 * k)], im = frame[size_t (2 * k + 1)];
            mag[size_t (k)] = std::sqrt (re * re + im * im);
            const float ph = std::atan2 (im, re);
            phase[size_t (k)] = ph;
            const float dev = princarg (ph - prevPhase[size_t (k)] - expected * float (k));
            trueBin[size_t (k)] = float (k) + dev / expected;
            prevPhase[size_t (k)] = ph;
        }

        // ---- 2. envelope via cepstrum ----
        for (int k = 0; k < bins; ++k)
        {
            cep[size_t (2 * k)] = std::log (mag[size_t (k)] + 1.0e-7f);
            cep[size_t (2 * k + 1)] = 0.0f;
        }
        cepFft.performRealOnlyInverseTransform (cep.data()); // real cepstrum in cep[0..N)
        for (int q = lifter; q <= N - lifter; ++q) cep[size_t (q)] = 0.0f;
        std::fill (cep.begin() + N, cep.end(), 0.0f);
        cepFft.performRealOnlyForwardTransform (cep.data(), true);
        for (int k = 0; k < bins; ++k) env[size_t (k)] = std::exp (cep[size_t (2 * k)]);

        // ---- 3. find input peaks ----
        float frameMax = 0.0f;
        for (int k = 0; k < bins; ++k) frameMax = std::max (frameMax, mag[size_t (k)]);
        const float floorMag = std::max (frameMax * 1.0e-4f, 1.0e-9f); // -80 dB below the frame peak
        int nPeaks = 0;
        for (int k = 2; k < bins - 2; ++k)
        {
            const float m = mag[size_t (k)];
            if (m > floorMag && m > mag[size_t (k - 1)] && m >= mag[size_t (k + 1)] && m > mag[size_t (k - 2)] && m >= mag[size_t (k + 2)])
                peaks[size_t (nPeaks++)] = k;
        }

        // ---- 4. move each peak region rigidly to alpha * f (Laroche & Dolson): the lobe keeps its
        //         shape, the peak's phase advances from the same partial's phase in the previous frame,
        //         and the region keeps the input's relative phases (identity phase locking) ----
        const float alpha = curAlpha, beta = curBeta;
        const bool identity = alpha == 1.0f;         // formant-only or 0/0: keep the input's phases (exact pitch, PR)
        const float kPh = dsp::kPi * float (W - 1) / float (N); // Hann frame starting at t=0: phase slope per bin of offset
        std::fill (outMag.begin(), outMag.end(), 0.0f);
        int prevIdx = 0;
        for (int i = 0; i < nPeaks; ++i)
        {
            const int p = peaks[size_t (i)];
            const int lo = i == 0 ? 1 : (peaks[size_t (i - 1)] + p) / 2 + 1;
            const int hi = i == nPeaks - 1 ? bins - 2 : (p + peaks[size_t (i + 1)]) / 2;
            const float inBin = trueBin[size_t (p)];
            const float newBin = inBin * alpha;
            const float shift = newBin - inBin; // fractional: the lobe centre lands exactly on alpha * f

            // phase "at the partial's centre" (window phase slope removed)
            const float centreIn = phase[size_t (p)] + kPh * (float (p) - inBin);
            while (prevIdx + 1 < prevPeakCount && std::abs (prevPeakIn[size_t (prevIdx + 1)] - p) <= std::abs (prevPeakIn[size_t (prevIdx)] - p))
                ++prevIdx;
            float centreOut;
            if (identity)
                centreOut = centreIn;
            else if (prevPeakCount > 0 && std::abs (prevPeakIn[size_t (prevIdx)] - p) <= 2)
                centreOut = princarg (prevPeakPhase[size_t (prevIdx)] + expected * newBin); // same partial, advanced by alpha*f
            else
                centreOut = centreIn; // a new partial
            curPeakIn[size_t (i)] = p;
            curPeakPhase[size_t (i)] = centreOut;

            const int jLo = std::max (1, int (std::floor (float (lo) + shift)));
            const int jHi = std::min (bins - 2, int (std::ceil (float (hi) + shift)));
            for (int j = jLo; j <= jHi; ++j)
            {
                const float rs = float (j) - shift;
                if (rs < float (lo) || rs > float (hi)) continue;
                const int r0 = std::min (int (rs), bins - 2);
                const float fr = rs - float (r0);
                const int rn = fr < 0.5f ? r0 : r0 + 1;
                // formant correction E(j/beta)/E(rs), bounded to +-24 dB (a pure tone has no formants)
                const float corr = std::clamp (envAt (float (j) / beta) / std::max (envAt (rs), 1.0e-12f), 1.0f / 16.0f, 16.0f);
                const float m = (mag[size_t (r0)] + fr * (mag[size_t (r0 + 1)] - mag[size_t (r0)])) * corr;
                if (m > outMag[size_t (j)])
                {
                    outMag[size_t (j)] = m;
                    // input phase of this bin relative to the partial's centre, beyond the window slope
                    const float residual = phase[size_t (rn)] + kPh * (float (rn) - inBin) - centreIn;
                    outPhase[size_t (j)] = centreOut - kPh * (float (j) - newBin) + residual;
                }
            }
        }
        std::copy (curPeakIn.begin(), curPeakIn.begin() + nPeaks, prevPeakIn.begin());
        std::copy (curPeakPhase.begin(), curPeakPhase.begin() + nPeaks, prevPeakPhase.begin());
        prevPeakCount = nPeaks;

        // ---- 5. synthesis ----
        for (int j = 0; j < bins; ++j)
        {
            frame[size_t (2 * j)] = outMag[size_t (j)] * std::cos (outPhase[size_t (j)]);
            frame[size_t (2 * j + 1)] = outMag[size_t (j)] * std::sin (outPhase[size_t (j)]);
        }
        frame[1] = 0.0f;
        frame[size_t (2 * (bins - 1) + 1)] = 0.0f;
        fft.performRealOnlyInverseTransform (frame.data());
        // the frame ends at the newest input sample; output trails it by W - 1, so frame sample k lands
        // k samples after the current read position
        const int ringSize = int (olaRing.size());
        for (int k = 0; k < W; ++k)
        {
            const int idx = (olaRead + k) % ringSize;
            olaRing[size_t (idx)] += frame[size_t (k)] * window[size_t (k)] * olaGain;
        }
    }

    int order, N, W, H;
    juce::dsp::FFT fft, cepFft;
    double sampleRate = 48000.0;
    float pitchSt = 0.0f, formantSt = 0.0f, curAlpha = 1.0f, curBeta = 1.0f;
    float olaGain = 1.0f;
    int lifter = 86;
    std::vector<float> window, inRing, olaRing, frame, cep;
    std::vector<float> mag, phase, prevPhase, trueBin, env, outMag, outPhase, curPeakPhase, prevPeakPhase;
    std::vector<int> peaks, curPeakIn, prevPeakIn;
    int prevPeakCount = 0;
    int inPos = 0, hopCount = 0, olaRead = 0;
    dsp::LevelMatcher level;
};
} // namespace

std::unique_ptr<IVoiceShifter> createPhaseVocoderShifter (int fftOrder, int windowLength)
{
    return std::make_unique<PhaseVocoderShifter> (fftOrder, windowLength);
}
} // namespace koe

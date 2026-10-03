// T3: RNNoise integration (spec §5.3, Phase 0 (c)).
//
//   p0_rnnoise [--input file.wav] [--seconds 60] [--tag name] [--normal-priority]   (timing on an MMCSS "Pro Audio" thread by default)
//
// - 48 kHz mono, 480-sample frames. RNNoise takes samples on the int16 scale (+/-32768): see
//   denoise.c (silence test `E < 0.04` on band energies) and README ("RAW 16-bit PCM"), so the bridge
//   scales by 32768 in and 1/32768 out.
// - RnnoiseBridge feeds arbitrary host block sizes through a FIFO. The output FIFO is primed with
//   480 - gcd(block, 480) zeros: the smallest constant delay that never runs dry (after k blocks the
//   frames done cover floor(k*B/480)*480 samples, short of k*B by (k*B mod 480) <= 480 - gcd).
// Writes results/03-rnnoise[-tag].csv and results/03-rnnoise[-tag]-run.txt. Offline only: no audio device.

#include "p0_common.h"

#include <juce_events/juce_events.h>

#include <rnnoise.h>

#include <cstdio>
#include <optional>
#include <numeric>

using namespace p0;

namespace
{
constexpr int kFrame = 480;

class RnnoiseBridge
{
public:
    explicit RnnoiseBridge (int blockSize)
    {
        st = rnnoise_create (nullptr);
        prime = kFrame - std::gcd (blockSize, kFrame);
        ring.assign (size_t (prime + 2 * kFrame + blockSize), 0.0f);
        count = prime;                          // primed with zeros
        writePos = prime;
    }
    ~RnnoiseBridge() { rnnoise_destroy (st); }

    int primeSamples() const { return prime; }

    void process (const float* in, float* out, int n)
    {
        for (int i = 0; i < n; ++i)
        {
            frameIn[inCount++] = in[i] * 32768.0f;
            if (inCount == kFrame)
            {
                const double t0 = nowUs();
                rnnoise_process_frame (st, frameOut, frameIn);
                frameUs = nowUs() - t0;
                ++framesDone;
                for (int k = 0; k < kFrame; ++k)
                {
                    ring[size_t (writePos)] = frameOut[k] * (1.0f / 32768.0f);
                    writePos = (writePos + 1) % int (ring.size());
                }
                count += kFrame;
                inCount = 0;
            }
        }
        for (int i = 0; i < n; ++i)
        {
            if (count <= 0) { out[i] = 0.0f; ++underruns; continue; }
            out[i] = ring[size_t (readPos)];
            readPos = (readPos + 1) % int (ring.size());
            --count;
        }
    }

    double frameUs = 0;
    long long framesDone = 0, underruns = 0;

private:
    DenoiseState* st = nullptr;
    int prime = 0, inCount = 0, count = 0, readPos = 0, writePos = 0;
    float frameIn[kFrame] {}, frameOut[kFrame] {};
    std::vector<float> ring;
};

std::vector<float> runBridge (const std::vector<float>& x, int block, int tail, std::vector<double>* blockPct = nullptr,
                              std::vector<double>* frameUs = nullptr, long long* underruns = nullptr)
{
    RnnoiseBridge br (block);
    std::vector<float> in (x);
    in.resize (x.size() + size_t (tail), 0.0f);
    std::vector<float> out (in.size(), 0.0f);
    const double bufferUs = 1.0e6 * block / kSr;
    for (size_t i = 0; i + size_t (block) <= in.size(); i += size_t (block))
    {
        const long long before = br.framesDone;
        const double t0 = nowUs();
        br.process (in.data() + i, out.data() + i, block);
        const double t1 = nowUs();
        if (blockPct != nullptr) blockPct->push_back (100.0 * (t1 - t0) / bufferUs);
        if (frameUs != nullptr && br.framesDone > before) frameUs->push_back (br.frameUs);
    }
    if (underruns != nullptr) *underruns = br.underruns;
    return out;
}

/** speech with 0.3 s gaps every 1.0 s (10 ms ramps) so noise-only stretches exist. mask: 1 speech, 0 gap. */
std::vector<float> gated (const std::vector<float>& s, std::vector<uint8_t>& mask)
{
    std::vector<float> y (s.size());
    mask.assign (s.size(), 0);
    const int period = int (kSr), on = int (0.7 * kSr), ramp = int (0.01 * kSr);
    for (size_t i = 0; i < s.size(); ++i)
    {
        const int ph = int (i % size_t (period));
        float g = 0.0f;
        if (ph < on)
            g = std::min ({ 1.0f, float (ph) / ramp, float (on - ph) / ramp });
        y[i] = s[i] * g;
        mask[i] = ph < on ? 1 : 0;
    }
    return y;
}

/** RMS (dB) of x[i + offset] over input samples i >= 1 s where mask == want, at least `guard`
    samples away from any mask edge. */
double maskedRmsDb (const std::vector<float>& x, size_t offset, const std::vector<uint8_t>& mask, uint8_t want, int guard)
{
    const size_t len = mask.size();
    std::vector<int> toNext (len);
    int d = guard;
    for (size_t i = len; i-- > 0;)
    {
        d = (i + 1 < len && mask[i + 1] != mask[i]) ? 0 : d + 1;
        toNext[i] = d;
    }
    double s = 0.0;
    size_t n = 0;
    int sinceEdge = guard;
    for (size_t i = 1; i < len; ++i)
    {
        sinceEdge = (mask[i] != mask[i - 1]) ? 0 : sinceEdge + 1;
        if (i < size_t (kSr) || mask[i] != want || sinceEdge < guard || toNext[i] < guard || i + offset >= x.size()) continue;
        s += double (x[i + offset]) * x[i + offset];
        ++n;
    }
    return n ? 10.0 * std::log10 (std::max (1e-20, s / double (n))) : -200.0;
}
} // namespace

int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    Args args (argc, argv);
    std::optional<ScopedProAudio> proAudio;   // time DSP like an audio thread (MMCSS "Pro Audio") unless --normal-priority
    if (! args.has ("--normal-priority")) proAudio.emplace();
    const juce::String priority = ! proAudio ? juce::String ("normal priority") : (proAudio->ok() ? juce::String ("MMCSS Pro Audio") : "MMCSS failed (" + juce::String ((int) proAudio->error) + ")");
    const auto speech = loadSpeech (args);
    const double seconds = args.getDouble ("--seconds", 60.0);
    const juce::String tag = args.has ("--tag") ? "-" + args.get ("--tag") : juce::String();   // keeps runs on other inputs apart
    juce::String csv = "item,condition,value,unit\n";
    juce::String log;
    auto row = [&] (const juce::String& item, const juce::String& cond, double v, const char* unit, int dec = 3)
    {
        csv << item << "," << cond << "," << num (v, dec) << "," << unit << "\n";
        std::printf ("%-28s %-44s %12.*f %s\n", item.toRawUTF8(), cond.toRawUTF8(), dec, v, unit);
    };

    SystemLoad whole;
    whole.start();
    row ("rnnoise_get_frame_size", "", rnnoise_get_frame_size(), "samples", 0);
    row ("rnnoise_get_size", "DenoiseState bytes", rnnoise_get_size(), "bytes", 0);

    // --- latency: block = 480 (no FIFO delay) gives the library's own delay ---------------------
    const int tail = 4800;
    for (int block : { 480, 128, 256, 512 })
    {
        long long under = 0;
        auto y = runBridge (speech.x, block, tail, nullptr, nullptr, &under);
        double envScore = 0, waveScore = 0;
        const int envMs = envelopeLagMs (speech.x, y, 80, &envScore);
        const int lag = waveformLag (speech.x, y, 1200, 1200, &waveScore);
        const int prime = kFrame - std::gcd (block, kFrame);
        const juce::String c = "block " + juce::String (block) + ", speech only";
        row ("fifo_prime", c, prime, "samples", 0);
        row ("latency_wave", c, lag, "samples", 0);
        row ("latency_wave_ms", c, 1000.0 * lag / kSr, "ms");
        row ("latency_wave_score", c, waveScore, "corr", 4);
        row ("latency_env_ms", c, envMs, "ms", 0);
        row ("fifo_underruns", c, double (under), "samples", 0);
        if (block == 480)
            row ("speech_level_change", "speech only, block 480, from 1 s", rmsDb (y.data() + size_t (kSr) + size_t (lag), speech.x.size() - size_t (kSr)) - rmsDb (speech.x.data() + size_t (kSr), speech.x.size() - size_t (kSr)), "dB");
    }

    // --- AC-16: -40 dBFS (RMS) white noise alone -------------------------------------------------
    {
        auto noise = whiteNoiseRms (10.0, -40.0, 11);
        auto y = runBridge (noise, 480, 0);
        const size_t a = size_t (kSr);
        const double in = rmsDb (noise.data() + a, noise.size() - a), out = rmsDb (y.data() + a, noise.size() - a);
        row ("noise_only_in", "white -40 dBFS RMS, 1..10 s", in, "dBFS");
        row ("noise_only_out", "white -40 dBFS RMS, 1..10 s", out, "dBFS");
        row ("noise_only_suppression", "in - out (AC-16 needs >= 10)", in - out, "dB");
        for (int s = 0; s < 10; ++s)
            row ("noise_only_out_per_second", "second " + juce::String (s), rmsDb (y.data() + size_t (s) * a, a), "dBFS");
    }

    // --- speech (gated) + -40 dBFS noise ---------------------------------------------------------
    {
        std::vector<uint8_t> mask;
        auto clean = gated (speech.x, mask);
        auto noise = whiteNoiseRms (double (clean.size()) / kSr, -40.0, 12);
        std::vector<float> mix (clean.size());
        for (size_t i = 0; i < mix.size(); ++i) mix[i] = clean[i] + noise[i];
        auto yMix = runBridge (mix, 480, tail);
        auto yClean = runBridge (clean, 480, tail);
        double sc = 0;
        const int lagCheck = waveformLag (clean, yClean, 1200, 1200, &sc);
        row ("mix_latency_check", "gated speech, block 480", lagCheck, "samples", 0);
        const int guard = int (0.05 * kSr);
        const double gapIn = maskedRmsDb (mix, 0, mask, 0, guard), gapOut = maskedRmsDb (yMix, size_t (lagCheck), mask, 0, guard);
        const double spIn = maskedRmsDb (mix, 0, mask, 1, guard), spOut = maskedRmsDb (yMix, size_t (lagCheck), mask, 1, guard);
        const double cleanIn = maskedRmsDb (clean, 0, mask, 1, guard), cleanOut = maskedRmsDb (yClean, size_t (lagCheck), mask, 1, guard);
        row ("mix_gap_in", "noise during 0.3 s gaps (50 ms guards)", gapIn, "dBFS");
        row ("mix_gap_out", "noise during 0.3 s gaps (50 ms guards)", gapOut, "dBFS");
        row ("mix_gap_suppression", "gap in - gap out", gapIn - gapOut, "dB");
        row ("mix_speech_in", "speech+noise during speech", spIn, "dBFS");
        row ("mix_speech_out", "speech+noise during speech", spOut, "dBFS");
        row ("mix_speech_change", "out - in during speech", spOut - spIn, "dB");
        row ("clean_speech_change", "gated speech without noise, out - in", cleanOut - cleanIn, "dB");
        row ("mix_input_snr", "speech segments vs noise", cleanIn - (-40.0), "dB");
        writeWav (rawDir().getChildFile ("rnnoise_mix_in" + tag + ".wav"), mix);
        writeWav (rawDir().getChildFile ("rnnoise_mix_out" + tag + ".wav"), yMix);
    }

    // --- processing time ---------------------------------------------------------------------------
    {
        auto noise = whiteNoiseRms (double (speech.x.size()) / kSr, -40.0, 13);
        std::vector<float> mix (speech.x.size());
        for (size_t i = 0; i < mix.size(); ++i) mix[i] = speech.x[i] + noise[i];
        std::vector<float> longMix;
        while (double (longMix.size()) < seconds * kSr) longMix.insert (longMix.end(), mix.begin(), mix.end());
        longMix.resize (size_t (seconds * kSr));
        for (int block : { 128, 256, 480 })
        {
            SystemLoad load;
            load.start();
            std::vector<double> pct, fus;
            runBridge (longMix, block, 0, &pct, &fus);
            const auto f = stats (fus), b = stats (pct);
            const juce::String c = "block " + juce::String (block) + ", " + juce::String (seconds, 0) + " s speech+noise";
            row ("frame_us_p50", c, f.p50, "us");
            row ("frame_us_p99", c, f.p99, "us");
            row ("frame_us_max", c, f.max, "us");
            row ("frame_count", c, double (f.n), "frames", 0);
            row ("block_cpu_p50", c, b.p50, "% of buffer");
            row ("block_cpu_p99", c, b.p99, "% of buffer");
            row ("block_cpu_max", c, b.max, "% of buffer");
            row ("system_busy", c, load.busyPercent(), "%", 1);
        }
    }

    log << "# generated by p0_rnnoise " << args.commandLine << "\n# date " << timestamp() << ", input " << speech.source
        << ", Release build, 48 kHz mono, RNNoise v0.2 (third_party, built-in model, generic C + SSE2 path: x86 RTCD files not compiled)\n"
        << "# timing thread: " << priority << "\n# whole-machine busy during the run: " << num (whole.busyPercent(), 1) << " %\n";
    writeText (resultsDir().getChildFile ("03-rnnoise" + tag + ".csv"), csv);
    writeText (resultsDir().getChildFile ("03-rnnoise" + tag + "-run.txt"), log);
    std::printf ("%s", log.toRawUTF8());
    return 0;
}

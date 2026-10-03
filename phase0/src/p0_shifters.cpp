// T2: voice shifter comparison (spec §5.5, Phase 0 (b)). Candidate A = Signalsmith Stretch.
// Candidate C (SoundTouch) is not built: see results/02-shifters.md. Candidate B is out of scope.
//
//   p0_shifters [--input file.wav] [--configs name,name,...] [--cpu-seconds 10] [--no-wav]
//               [--f0-sweep] [--level-scan] [--tag name] [--normal-priority]
//
// Writes results/02-shifters[-tag].csv, 02-ac04[-tag].csv, 02-shifters-run[-tag].txt (conditions),
// optionally 02-f0-sweep[-tag].csv / 02-level-scan[-tag].csv, and listening WAVs to results/raw/.
// CPU is timed on an MMCSS "Pro Audio" thread unless --normal-priority. Offline: no audio device.

#include "p0_common.h"
#include "p0_pitch.h"
#include "p0_shifter.h"

#include <juce_events/juce_events.h>

#include <cstdio>
#include <optional>

using namespace p0;

namespace
{
struct Params { float pitch, formant; bool cpu; };

// spec T2: 0/0, 4/2.5, -4/-2.5, -9/-5 (CPU + listening); +/-12/+/-6 only for the level check (F-02-12 range ends)
const Params kParams[] = { { 0, 0, true }, { 4, 2.5f, true }, { -4, -2.5f, true }, { -9, -5, true }, { 12, 6, false }, { -12, -6, false } };
const int kBuffers[] = { 128, 256, 480 };
constexpr int kInstances = 3;

std::vector<ShifterConfig> allConfigs()
{
    return {
        { "default", 5760, 1440, false },   // presetDefault(1, 48000): 120 ms / 30 ms
        { "cheaper", 4800, 1920, true },    // presetCheaper(1, 48000): 100 ms / 40 ms, split
        { "b960-i120", 960, 120, false },   // ~20 ms blocks
        { "b960-i240", 960, 240, false },
        { "b960-i240-split", 960, 240, true },
        { "b1440-i240", 1440, 240, false }, // 30 ms (current ShifterConfig default)
        { "b1440-i360", 1440, 360, false },
        { "b1440-i480", 1440, 480, false },
        { "b1440-i240-split", 1440, 240, true },
        { "b1920-i240", 1920, 240, false }, // 40 ms
        { "b1920-i480", 1920, 480, false },
        { "b1920-i480-split", 1920, 480, true },
        { "b2880-i480", 2880, 480, false }, // 60 ms
        { "b2880-i720", 2880, 720, false },
    };
}

juce::String paramTag (const Params& p)
{
    auto f = [] (float v) { return juce::String (v, v == std::floor (v) ? 0 : 1); };
    return "p" + f (p.pitch) + "_f" + f (p.formant);
}

/** Run one instance over `in` (+ tail of zeros) in 480-sample blocks. Output has in.size() + tail samples. */
std::vector<float> runOffline (const ShifterConfig& cfg, const Params& p, const std::vector<float>& in, int tail)
{
    SignalsmithShifter s (cfg);
    s.prepare (kSr, 480);
    s.setPitchSemitones (p.pitch);
    s.setFormantSemitones (p.formant);
    s.reset();
    std::vector<float> x (in);
    x.resize (in.size() + size_t (tail), 0.0f);
    std::vector<float> y (x.size(), 0.0f);
    for (size_t i = 0; i < x.size(); i += 480)
    {
        const int n = int (std::min<size_t> (480, x.size() - i));
        s.process (x.data() + i, y.data() + i, n);
    }
    return y;
}

/** CPU of 3 instances processed one after another per block: time / buffer time (%). */
Stats measureCpu (const ShifterConfig& cfg, const Params& p, const std::vector<float>& in, int buffer, double seconds)
{
    std::vector<std::unique_ptr<SignalsmithShifter>> inst;
    for (int i = 0; i < kInstances; ++i)
    {
        inst.push_back (std::make_unique<SignalsmithShifter> (cfg, i + 1));
        inst.back()->prepare (kSr, buffer);
        inst.back()->setPitchSemitones (p.pitch);
        inst.back()->setFormantSemitones (p.formant);
        inst.back()->reset();
    }
    std::vector<float> out (size_t (buffer), 0.0f);
    const double bufferUs = 1.0e6 * buffer / kSr;
    const size_t total = size_t (seconds * kSr), warm = size_t (0.5 * kSr);
    std::vector<double> pct;
    pct.reserve (total / size_t (buffer) + 1);
    for (size_t pos = 0; pos + size_t (buffer) <= total; pos += size_t (buffer))
    {
        const float* src = in.data() + (pos % (in.size() - size_t (buffer)));
        const double t0 = nowUs();
        for (auto& s : inst) s->process (src, out.data(), buffer);
        const double t1 = nowUs();
        if (pos >= warm) pct.push_back (100.0 * (t1 - t0) / bufferUs);
    }
    return stats (std::move (pct));
}

struct F0Result { double medianHz = 0, medianCents = 0, maxAbsCents = 0, zeroCrossHz = 0; int frames = 0, unvoiced = 0; bool finite = true; };

/** Mean frequency from positive-going zero crossings (linear interpolation) over [from, to). */
double zeroCrossingHz (const std::vector<float>& y, size_t from, size_t to)
{
    double first = -1, last = -1;
    int count = 0;
    for (size_t i = from + 1; i < to; ++i)
        if (y[i - 1] < 0.0f && y[i] >= 0.0f)
        {
            const double t = double (i - 1) + double (y[i - 1]) / double (y[i - 1] - y[i]);
            if (first < 0) first = t; else ++count;
            last = t;
        }
    return count > 0 ? kSr * count / (last - first) : 0.0;
}

/** f0 of y over [from, to) with YIN in 4096-sample frames hopping 1024 (sine input only). */
F0Result measureF0 (const std::vector<float>& y, size_t from, size_t to, double expectedHz)
{
    F0Result r;
    for (auto v : y) r.finite = r.finite && std::isfinite (v);
    r.zeroCrossHz = zeroCrossingHz (y, from, to);
    PitchSettings ps;
    ps.fminHz = 60; ps.fmaxHz = 1000; ps.integrationSamples = 2048;
    YinDetector yin;
    yin.prepare (ps);
    std::vector<double> hz, cents;
    for (size_t i = from; i + size_t (yin.bufferSamples()) <= to; i += 1024)
    {
        const double f = yin.detect (y.data() + i);
        ++r.frames;
        if (f <= 0) { ++r.unvoiced; continue; }
        hz.push_back (f);
        cents.push_back (1200.0 * std::log2 (f / expectedHz));
    }
    if (hz.empty()) return r;
    r.medianHz = stats (hz).p50;
    r.medianCents = stats (cents).p50;
    for (double c : cents) r.maxAbsCents = std::max (r.maxAbsCents, std::abs (c));
    return r;
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
    const double cpuSeconds = args.getDouble ("--cpu-seconds", 10.0);
    const bool writeListening = ! args.has ("--no-wav");
    const juce::String tag = args.has ("--tag") ? "-" + args.get ("--tag") : juce::String();   // keeps partial runs apart
    auto out = [&] (const char* base, const char* ext) { return resultsDir().getChildFile (juce::String (base) + tag + ext); };

    auto configs = allConfigs();
    if (args.has ("--configs"))
    {
        auto wanted = juce::StringArray::fromTokens (args.get ("--configs"), ",", "");
        std::vector<ShifterConfig> keep;
        for (auto& c : configs)
            if (wanted.contains (juce::String (c.name))) keep.push_back (c);
        configs = keep;
    }

    std::printf ("input: %s (%zu samples)\n", speech.source.toRawUTF8(), speech.x.size());
    if (writeListening) writeWav (rawDir().getChildFile ("input.wav"), speech.x);

    juce::String csv = "candidate,config,block,interval,split,pitch_st,formant_st,reported_latency_samples,reported_latency_ms,"
                       "env_latency_ms,env_score,wave_latency_samples,wave_latency_ms,wave_score,level_in_db,level_out_db,level_diff_db,"
                       "cpu128_p50,cpu128_p99,cpu128_max,cpu256_p50,cpu256_p99,cpu256_max,cpu480_p50,cpu480_p99,cpu480_max,system_busy_pct\n";
    juce::String ac04 = "config,test,pitch_st,formant_st,expected_hz,yin_median_hz,yin_median_cents,yin_max_abs_cents,zero_cross_hz,zero_cross_cents,"
                        "level_diff_db,frames,unvoiced_frames,finite\n";
    juce::String scan = "config,compensate_pitch,pitch_st,formant_st,env_latency_ms,level_diff_db\n";
    juce::String sweep = "config,wave,compensate_pitch,input_hz,pitch_st,expected_hz,yin_median_hz,yin_cents,zero_cross_cents\n";

    SystemLoad wholeRun;
    wholeRun.start();
    const double cpuStart = processCpuSeconds();

    for (const auto& cfg : configs)
    {
        SignalsmithShifter probe (cfg);
        probe.prepare (kSr, 480);
        const int reported = probe.getLatencySamples();
        const int tail = reported + int (kSr);
        std::printf ("\n[%s] block %d interval %d split %d: reported latency %d samples (in %d + out %d)\n",
                     cfg.name.c_str(), cfg.blockSamples, cfg.intervalSamples, int (cfg.splitComputation),
                     reported, probe.inputLatency(), probe.outputLatency());

        for (const auto& p : kParams)
        {
            auto y = runOffline (cfg, p, speech.x, tail);
            double envScore = 0, waveScore = 0;
            const int envMs = envelopeLagMs (speech.x, y, 300, &envScore);
            int waveLag = -1;
            if (p.pitch == 0 && p.formant == 0)
                waveLag = waveformLag (speech.x, y, envMs * 48, 96, &waveScore);
            const int lag = waveLag >= 0 ? waveLag : envMs * 48;

            // level: from 1 s after the start to the end of the input, latency compensated (presets §5.1)
            const size_t a = size_t (kSr), b = speech.x.size();
            const double levelIn = rmsDb (speech.x.data() + a, b - a);
            const double levelOut = rmsDb (y.data() + a + size_t (lag), b - a);

            juce::String cpuCols;
            SystemLoad load;
            load.start();
            for (int buf : kBuffers)
            {
                if (! p.cpu) { cpuCols << ",,,"; continue; }
                auto st = measureCpu (cfg, p, speech.x, buf, cpuSeconds);
                cpuCols << "," << num (st.p50) << "," << num (st.p99) << "," << num (st.max);
            }
            const double busy = load.busyPercent();

            csv << "A," << cfg.name << "," << cfg.blockSamples << "," << cfg.intervalSamples << "," << int (cfg.splitComputation) << ","
                << p.pitch << "," << p.formant << "," << reported << "," << num (1000.0 * reported / kSr) << ","
                << envMs << "," << num (envScore, 4) << "," << (waveLag >= 0 ? juce::String (waveLag) : juce::String()) << ","
                << (waveLag >= 0 ? num (1000.0 * waveLag / kSr) : juce::String()) << "," << (waveLag >= 0 ? num (waveScore, 4) : juce::String()) << ","
                << num (levelIn) << "," << num (levelOut) << "," << num (levelOut - levelIn) << cpuCols << "," << num (p.cpu ? busy : 0.0, 1) << "\n";
            std::printf ("  %-10s env %3d ms wave %5d smp  level %+7.3f dB%s\n", paramTag (p).toRawUTF8(), envMs, waveLag,
                         levelOut - levelIn, cpuCols.toRawUTF8());

            if (writeListening && p.cpu)
            {
                std::vector<float> trimmed (y.begin() + lag, y.begin() + lag + long (speech.x.size()));
                writeWav (rawDir().getChildFile ("A-" + juce::String (cfg.name) + "_" + paramTag (p) + ".wav"), trimmed);
            }
        }

        // AC-04: 220 Hz sine; +12 st -> 440 Hz +/-10 cents; formant only -6..+6 st -> 220 Hz +/-10 cents
        const auto tone = sine (220.0, 3.0, 0.5f);
        struct T { const char* name; float pitch, formant; double expect; };
        const T tests[] = { { "pitch+12", 12, 0, 440 }, { "pitch-12", -12, 0, 110 }, { "formant-6", 0, -6, 220 }, { "formant-3", 0, -3, 220 },
                            { "formant+3", 0, 3, 220 }, { "formant+6", 0, 6, 220 } };
        for (const auto& t : tests)
        {
            auto y = runOffline (cfg, { t.pitch, t.formant, false }, tone, tail);
            const size_t from = size_t (kSr) + size_t (reported), to = size_t (3.0 * kSr) + size_t (reported);
            auto r = measureF0 (y, from, to, t.expect);
            const double zcCents = r.zeroCrossHz > 0 ? 1200.0 * std::log2 (r.zeroCrossHz / t.expect) : 0.0;
            ac04 << cfg.name << "," << t.name << "," << t.pitch << "," << t.formant << "," << t.expect << "," << num (r.medianHz, 4) << ","
                 << num (r.medianCents) << "," << num (r.maxAbsCents) << "," << num (r.zeroCrossHz, 4) << "," << num (zcCents) << ","
                 << num (rmsDb (y.data() + from, to - from) - rmsDb (tone.data() + size_t (kSr), size_t (2.0 * kSr))) << ","
                 << r.frames << "," << r.unvoiced << "," << (r.finite ? "yes" : "NO") << "\n";
            std::printf ("  AC-04 %-10s median %9.4f Hz (%+8.3f cents, max |%.3f|), zero-crossing %9.4f Hz (%+8.3f cents)\n", t.name, r.medianHz,
                         r.medianCents, r.maxAbsCents, r.zeroCrossHz, zcCents);
        }

        if (args.has ("--f0-sweep"))
        {
            // is the transposition error systematic? 100..400 Hz x pitch +/-4, +/-12 (formant 0); sine and
            // harmonic tone (1st..5th, 1/k), formant compensation on (as used) and off (library without formant work)
            for (bool harmonic : { false, true })
                for (bool comp : { true, false })
                    for (double fin : { 100.0, 150.0, 200.0, 220.0, 250.0, 300.0, 400.0 })
                        for (float pst : { -12.0f, -4.0f, 4.0f, 12.0f })
                        {
                            std::vector<float> s (size_t (3.0 * kSr));
                            for (size_t i = 0; i < s.size(); ++i)
                                for (int k = 1; k <= (harmonic ? 5 : 1); ++k)
                                    s[i] += float (0.4 / k * std::sin (2.0 * juce::MathConstants<double>::pi * k * fin * double (i) / kSr));
                            auto c2 = cfg;
                            c2.compensatePitch = comp;
                            auto y = runOffline (c2, { pst, 0.0f, false }, s, tail);
                            const size_t from = size_t (kSr) + size_t (reported), to = size_t (3.0 * kSr) + size_t (reported);
                            const double expect = fin * std::pow (2.0, pst / 12.0);
                            auto r = measureF0 (y, from, to, expect);
                            sweep << cfg.name << "," << (harmonic ? "harmonic1-5" : "sine") << "," << (comp ? 1 : 0) << "," << fin << "," << pst << ","
                                  << num (expect, 4) << "," << num (r.medianHz, 4) << "," << num (r.medianCents) << ","
                                  << (harmonic ? juce::String() : num (r.zeroCrossHz > 0 ? 1200.0 * std::log2 (r.zeroCrossHz / expect) : 0.0)) << "\n";
                        }
            std::printf ("  f0 sweep done\n");
        }

        if (args.has ("--level-scan"))
        {
            // F-02-12: level over the whole pitch/formant range; compensatePitch false shows the cause
            for (bool comp : { true, false })
                for (float pst : { -12.0f, -9.0f, -6.0f, -4.0f, -2.0f, 0.0f, 2.0f, 4.0f, 6.0f, 9.0f, 12.0f })
                    for (float fst : { -6.0f, -3.0f, 0.0f, 3.0f, 6.0f })
                    {
                        auto c2 = cfg;
                        c2.compensatePitch = comp;
                        auto y = runOffline (c2, { pst, fst, false }, speech.x, tail);
                        const int envMs = envelopeLagMs (speech.x, y, 300);
                        const size_t a = size_t (kSr), b = speech.x.size();
                        const double d = rmsDb (y.data() + a + size_t (envMs * 48), b - a) - rmsDb (speech.x.data() + a, b - a);
                        scan << cfg.name << "," << (comp ? 1 : 0) << "," << pst << "," << fst << "," << envMs << "," << num (d) << "\n";
                    }
            std::printf ("  level scan done\n");
        }
    }
    if (args.has ("--level-scan"))
        writeText (out ("02-level-scan", ".csv"), scan);
    if (args.has ("--f0-sweep"))
        writeText (out ("02-f0-sweep", ".csv"), sweep);

    juce::String header;
    header << "# generated by p0_shifters " << args.commandLine << "\n"
           << "# date " << timestamp() << ", input " << speech.source << ", Release build, 48 kHz mono float32\n"
           << "# CPU: " << kInstances << " instances processed one after another per block, time / buffer time, "
           << cpuSeconds << " s of input per buffer size (first 0.5 s excluded)\n"
           << "# timing thread: " << priority << "\n# whole-machine busy during the run: " << num (wholeRun.busyPercent(), 1) << " %, process CPU "
           << num (processCpuSeconds() - cpuStart, 1) << " s\n";
    writeText (out ("02-shifters", ".csv"), csv);
    writeText (out ("02-ac04", ".csv"), ac04);
    writeText (out ("02-shifters-run", ".txt"), header);
    std::printf ("\n%s", header.toRawUTF8());
    return 0;
}

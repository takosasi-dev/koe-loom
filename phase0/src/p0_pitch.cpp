// T6: pitch detection, YIN vs MPM (spec §5.7, Phase 0 (f)). Offline only.
//
//   p0_pitch [--input file.wav] [--hop 128] [--fmin 60] [--integration 1024] [--tag name] [--normal-priority]
//
// Detectors run once per hop on the newest N samples (N = integration + maxLag = 1024 + 801 at 60 Hz).
// Signals: sines and harmonic tones (fundamental + 2nd..5th harmonics, 1/k amplitudes) at 100..400 Hz:
// 0.5 s silence, 1.5 s tone, then a phase-continuous step +4 st (x1.2599) for 1 s.
//   detection latency = newest sample of the first frame that is within +/-10 cents of the true f0 and
//                       stays within for the next 2 frames, minus the time of the change.
//   steady error      = frames whose whole buffer lies in one steady tone (from 100 ms after the change).
// Speech: synthetic voice (known f0 trajectory) and the reference speech if present (no ground truth:
// voiced ratio and median |frame-to-frame change|).
// Writes results/06-pitch.csv and results/06-pitch-run.txt.

#include "p0_common.h"
#include "p0_pitch.h"

#include <juce_events/juce_events.h>

#include <cstdio>
#include <optional>

using namespace p0;

namespace
{
constexpr double kStepRatio = 1.2599210498948732; // +4 semitones

std::vector<float> testTone (double f0, bool harmonics, double& tOn, double& tStep)
{
    tOn = 0.5; tStep = 2.0;
    const size_t n = size_t (3.0 * kSr);
    std::vector<float> x (n, 0.0f);
    double phase = 0.0;
    for (size_t i = size_t (tOn * kSr); i < n; ++i)
    {
        const double f = double (i) >= tStep * kSr ? f0 * kStepRatio : f0;
        phase += f / kSr;
        phase -= std::floor (phase);
        double s = 0.0;
        for (int k = 1; k <= (harmonics ? 5 : 1); ++k)
            s += std::sin (2.0 * juce::MathConstants<double>::pi * k * phase) / k;
        x[i] = float (0.4 * s);
    }
    return x;
}

struct Frame { size_t end; double hz; };

std::vector<Frame> runDetector (PitchDetectorBase& d, const std::vector<float>& x, int hop, std::vector<double>* us = nullptr)
{
    std::vector<Frame> fr;
    const size_t n = size_t (d.bufferSamples());
    for (size_t end = n; end <= x.size(); end += size_t (hop))
    {
        const double t0 = nowUs();
        const double f = d.detect (x.data() + end - n);
        if (us != nullptr) us->push_back (nowUs() - t0);
        fr.push_back ({ end, f });
    }
    return fr;
}

bool within10 (double f, double truth) { return f > 0 && std::abs (1200.0 * std::log2 (f / truth)) <= 10.0; }

double latencyMs (const std::vector<Frame>& fr, double tChange, double truth)
{
    const size_t change = size_t (tChange * kSr);
    for (size_t i = 0; i + 2 < fr.size(); ++i)
    {
        if (fr[i].end <= change) continue;
        if (within10 (fr[i].hz, truth) && within10 (fr[i + 1].hz, truth) && within10 (fr[i + 2].hz, truth))
            return 1000.0 * double (fr[i].end - change) / kSr;
    }
    return -1.0;
}

struct ErrStats { double meanCents = 0, meanAbs = 0, maxAbs = 0; int frames = 0, unvoiced = 0; };

ErrStats steadyError (const std::vector<Frame>& fr, size_t n, double from, double to, double truth)
{
    ErrStats e;
    for (const auto& f : fr)
    {
        if (double (f.end - n) < from * kSr || double (f.end) > to * kSr) continue;
        ++e.frames;
        if (f.hz <= 0) { ++e.unvoiced; continue; }
        const double c = 1200.0 * std::log2 (f.hz / truth);
        e.meanCents += c;
        e.meanAbs += std::abs (c);
        e.maxAbs = std::max (e.maxAbs, std::abs (c));
    }
    const int voiced = e.frames - e.unvoiced;
    if (voiced > 0) { e.meanCents /= voiced; e.meanAbs /= voiced; }
    if (e.frames > 0 && voiced == 0) e.maxAbs = 1e9;
    return e;
}
} // namespace

int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    Args args (argc, argv);
    std::optional<ScopedProAudio> proAudio;   // time DSP like an audio thread (MMCSS "Pro Audio") unless --normal-priority
    if (! args.has ("--normal-priority")) proAudio.emplace();
    const juce::String priority = ! proAudio ? juce::String ("normal priority") : (proAudio->ok() ? juce::String ("MMCSS Pro Audio") : "MMCSS failed (" + juce::String ((int) proAudio->error) + ")");
    const int hop = args.getInt ("--hop", 128);
    PitchSettings ps;
    ps.fminHz = args.getDouble ("--fmin", ps.fminHz);
    ps.integrationSamples = args.getInt ("--integration", ps.integrationSamples);
    const juce::String tag = args.has ("--tag") ? "-" + args.get ("--tag") : juce::String();
    YinDetector yin;
    MpmDetector mpm;
    yin.prepare (ps);
    mpm.prepare (ps);
    PitchDetectorBase* dets[] = { &yin, &mpm };
    const char* names[] = { "YIN", "MPM" };
    const size_t n = size_t (yin.bufferSamples());

    juce::String csv = "method,signal,f0_hz,onset_latency_ms,step_latency_ms,steady_mean_cents,steady_mean_abs_cents,steady_max_abs_cents,"
                       "steady_frames,steady_unvoiced,step_mean_abs_cents,step_max_abs_cents\n";
    SystemLoad whole;
    whole.start();

    for (int di = 0; di < 2; ++di)
    {
        for (bool harm : { false, true })
            for (double f0 : { 100.0, 150.0, 200.0, 300.0, 400.0 })
            {
                double tOn, tStep;
                auto x = testTone (f0, harm, tOn, tStep);
                auto fr = runDetector (*dets[di], x, hop);
                const double on = latencyMs (fr, tOn, f0), st = latencyMs (fr, tStep, f0 * kStepRatio);
                auto e1 = steadyError (fr, n, tOn + 0.1, tStep, f0);
                auto e2 = steadyError (fr, n, tStep + 0.1, 3.0, f0 * kStepRatio);
                csv << names[di] << "," << (harm ? "harmonic1-5" : "sine") << "," << f0 << "," << num (on) << "," << num (st) << ","
                    << num (e1.meanCents) << "," << num (e1.meanAbs) << "," << num (e1.maxAbs) << "," << e1.frames << "," << e1.unvoiced << ","
                    << num (e2.meanAbs) << "," << num (e2.maxAbs) << "\n";
                std::printf ("%s %-11s %5.0f Hz onset %8.3f ms step %8.3f ms steady mean|c| %7.3f max %7.3f unvoiced %d\n", names[di],
                             harm ? "harmonic1-5" : "sine", f0, on, st, e1.meanAbs, e1.maxAbs, e1.unvoiced);
            }
    }

    // speech: synthetic voice with known f0, and the reference speech if present
    juce::String speechCsv = "method,signal,frames,voiced_ratio,median_abs_frame_change_cents,truth_median_abs_cents,truth_p99_abs_cents,"
                             "truth_within_50c_ratio\n";
    const auto synth = synthVoice (10.0);
    const auto ref = loadSpeech (args);
    const juce::String refName = args.has ("--input") ? juce::File (args.get ("--input")).getFileName() : juce::String ("reference-speech.wav");
    for (int di = 0; di < 2; ++di)
    {
        for (int which = 0; which < (ref.real ? 2 : 1); ++which)
        {
            const auto& x = which == 0 ? synth : ref.x;
            auto fr = runDetector (*dets[di], x, hop);
            int voiced = 0;
            std::vector<double> change, truthErr;
            int within50 = 0;
            double prev = 0;
            for (const auto& f : fr)
            {
                if (f.hz > 0)
                {
                    ++voiced;
                    if (prev > 0) change.push_back (std::abs (1200.0 * std::log2 (f.hz / prev)));
                    if (which == 0)
                    {
                        const double t = (double (f.end) - double (n) / 2.0) / kSr;   // buffer centre
                        const double c = std::abs (1200.0 * std::log2 (f.hz / synthVoiceF0 (t)));
                        truthErr.push_back (c);
                        if (c <= 50) ++within50;
                    }
                }
                prev = f.hz;
            }
            const auto cs = stats (change), ts = stats (truthErr);
            speechCsv << names[di] << "," << (which == 0 ? juce::String::fromUTF8 ("synthVoice (合成音声で代用)") : refName) << "," << fr.size() << ","
                      << num (double (voiced) / double (fr.size()), 4) << "," << num (cs.p50) << ","
                      << (which == 0 ? num (ts.p50) : juce::String()) << "," << (which == 0 ? num (ts.p99) : juce::String()) << ","
                      << (which == 0 ? num (double (within50) / std::max (1, voiced), 4) : juce::String()) << "\n";
            std::printf ("%s %s voiced %.4f median|dc| %.3f truth median %.3f p99 %.3f\n", names[di], which == 0 ? "synth" : "ref",
                         double (voiced) / double (fr.size()), cs.p50, ts.p50, ts.p99);
        }
    }

    // CPU: one detect() per audio block
    juce::String cpuCsv = "method,detect_us_p50,detect_us_p99,detect_us_max,calls,cpu128_p50_pct,cpu128_p99_pct,cpu256_p99_pct,cpu480_p99_pct\n";
    std::vector<float> longSpeech;
    for (int i = 0; i < 3; ++i) longSpeech.insert (longSpeech.end(), synth.begin(), synth.end());
    for (int di = 0; di < 2; ++di)
    {
        std::vector<double> us;
        runDetector (*dets[di], longSpeech, 128, &us);
        const auto s = stats (us);
        auto pct = [&] (double v, int buf) { return 100.0 * v / (1.0e6 * buf / kSr); };
        cpuCsv << names[di] << "," << num (s.p50) << "," << num (s.p99) << "," << num (s.max) << "," << s.n << "," << num (pct (s.p50, 128)) << ","
               << num (pct (s.p99, 128)) << "," << num (pct (s.p99, 256)) << "," << num (pct (s.p99, 480)) << "\n";
        std::printf ("%s detect us p50 %.3f p99 %.3f max %.3f -> p99 %.3f %% of 128\n", names[di], s.p50, s.p99, s.max, pct (s.p99, 128));
    }

    juce::String log;
    log << "# generated by p0_pitch " << args.commandLine << "\n# date " << timestamp() << ", Release build, 48 kHz mono\n"
        << "# range " << ps.fminHz << ".." << ps.fmaxHz << " Hz, integration " << ps.integrationSamples << ", buffer N " << n << " samples ("
        << num (1000.0 * double (n) / kSr) << " ms), hop " << hop << ", FFT-based correlation (juce::dsp::FFT), YIN threshold "
        << ps.yinThreshold << ", MPM k " << ps.mpmK << " clarity " << ps.mpmClarity << "\n"
        << "# speech input: " << ref.source << "\n# timing thread: " << priority << "\n# whole-machine busy during the run: " << num (whole.busyPercent(), 1) << " %\n";
    writeText (resultsDir().getChildFile ("06-pitch" + tag + ".csv"), csv);
    writeText (resultsDir().getChildFile ("06-pitch" + tag + "-speech.csv"), speechCsv);
    writeText (resultsDir().getChildFile ("06-pitch" + tag + "-cpu.csv"), cpuCsv);
    writeText (resultsDir().getChildFile ("06-pitch" + tag + "-run.txt"), log);
    std::printf ("%s", log.toRawUTF8());
    return 0;
}

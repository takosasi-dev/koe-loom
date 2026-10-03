// Ad-hoc diagnostics (category "Diag", not part of the normal run). Writes WAVs to %TEMP%\KoeLoomDiag.

#include "Dsp/IVoiceShifter.h"
#include "Engine/VoiceProcessor.h"
#include "Tests/TestUtil.h"

#include <juce_dsp/juce_dsp.h>
#include <signalsmith-stretch/signalsmith-stretch.h>

namespace koe
{
class DiagTests : public juce::UnitTest
{
public:
    DiagTests() : juce::UnitTest ("Diagnostics", "Diag") {}

    void runTest() override
    {
        using namespace test;
        auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("KoeLoomDiag");
        dir.createDirectory();

        beginTest ("estimator sanity");
        {
            auto s = sine (440.0, 1.0, 0.3f);
            logMessage ("  440 sine -> " + juce::String (estimateF0 (s.data() + 4800, 19200), 3));
        }

        beginTest ("+12 st accuracy per config");
        for (int block : { 960, 1440, 1920, 2880, 5760 })
            for (int interval : { 120, 240, 480 })
            {
                ShifterConfig c;
                c.blockSamples = block;
                c.intervalSamples = interval;
                auto sh = createSignalsmithShifter (c);
                sh->prepare (kSr, 480);
                sh->setPitchSemitones (12);
                sh->reset();
                auto in = sine (220.0, 2.0, 0.3f);
                std::vector<float> out (in.size());
                for (size_t p = 0; p < in.size(); p += 480) sh->process (in.data() + p, out.data() + p, 480);
                const double f = estimateF0 (out.data() + 48000, 19200);
                logMessage ("  block " + juce::String (block) + " int " + juce::String (interval) + ": " + juce::String (f, 2) + " Hz ("
                            + juce::String (centsBetween (f, 440.0), 1) + " c) lat " + juce::String (sh->getLatencySamples()));
            }

        beginTest ("PV +12 on a 200 Hz sine: spectral peaks of the output");
        {
            auto sh = createPhaseVocoderShifter (10);
            sh->prepare (kSr, 480);
            sh->setPitchSemitones (12);
            sh->reset();
            auto in = sine (200.0, 1.6, 0.3f);
            std::vector<float> out (in.size());
            for (size_t p = 0; p < in.size(); p += 480) sh->process (in.data() + p, out.data() + p, 480);
            writeWav (dir.getChildFile ("pv_sine200_p12.wav"), out);
            juce::dsp::FFT f (15);
            std::vector<float> buf (size_t (2 << 15), 0.0f);
            for (int i = 0; i < (1 << 15); ++i)
                buf[size_t (i)] = out[size_t (30000 + i)] * (0.5f - 0.5f * std::cos (6.2831853f * float (i) / float (1 << 15)));
            f.performFrequencyOnlyForwardTransform (buf.data(), true);
            juce::String s;
            for (int pass = 0; pass < 5; ++pass)
            {
                int best = 1;
                for (int k = 1; k < (1 << 14); ++k) if (buf[size_t (k)] > buf[size_t (best)]) best = k;
                s << juce::String (best * kSr / double (1 << 15), 1) << "Hz:" << juce::String (dsp::gainToDb (buf[size_t (best)] / 8192.0f), 1) << "dB ";
                for (int k = std::max (1, best - 20); k < std::min (1 << 14, best + 20); ++k) buf[size_t (k)] = 0;
            }
            logMessage ("  peaks: " + s + " rms " + juce::String (rmsDb (out.data() + 30000, 30000), 1));
        }

        beginTest ("candidate B (phase vocoder): accuracy, level, latency, CPU");
        const std::pair<int, int> pvCfgs[] = { { 10, 1024 }, { 11, 1280 }, { 11, 1536 }, { 11, 2048 } };
        for (auto [order, wlen] : pvCfgs)
        {
            auto createPhaseVocoderShifterW = [wlen = wlen] (int o) { return createPhaseVocoderShifter (o, wlen); };
            double worst = 0, sum = 0; int cnt = 0;
            juce::String detail;
            for (double f : { 110.0, 150.0, 200.0, 260.0, 330.0 })
                for (float st : { 12.0f, 5.0f, -7.0f, -12.0f })
                {
                    auto sh = createPhaseVocoderShifterW (order);
                    sh->prepare (kSr, 480);
                    sh->setPitchSemitones (st);
                    sh->reset();
                    auto in = sine (f, 1.6, 0.3f);
                    std::vector<float> out (in.size());
                    for (size_t p = 0; p < in.size(); p += 480) sh->process (in.data() + p, out.data() + p, 480);
                    const double got = estimateF0 (out.data() + 38400, 28800);
                    const double c = std::abs (centsBetween (got, f * std::pow (2.0, st / 12.0)));
                    worst = std::max (worst, c); sum += c; ++cnt;
                    if (c > 10) detail << juce::String (f, 0) << "/" << st << ":" << juce::String (c, 0) << " ";
                }
            logMessage ("  PV " + juce::String (order) + "/" + juce::String (wlen) + ": worst " + juce::String (worst, 1) + " c, mean " + juce::String (sum / cnt, 1) + " " + detail);

            auto voice = synthVoice (4.0, 21, kSr, false);
            juce::String lv;
            for (float p : { -12.0f, 0.0f, 12.0f })
                for (float fm : { -6.0f, 0.0f, 6.0f })
                {
                    auto sh = createPhaseVocoderShifterW (order);
                    sh->prepare (kSr, 480);
                    sh->setPitchSemitones (p);
                    sh->setFormantSemitones (fm);
                    sh->reset();
                    std::vector<float> out (voice.size());
                    const auto t0 = juce::Time::getHighResolutionTicks();
                    for (size_t q = 0; q < voice.size(); q += 480) sh->process (voice.data() + q, out.data() + q, 480);
                    const double secs = juce::Time::highResolutionTicksToSeconds (juce::Time::getHighResolutionTicks() - t0);
                    const int L = sh->getLatencySamples();
                    const int a = 48000, nn = int (voice.size()) - a - L;
                    lv << "p" << p << "f" << fm << ":" << juce::String (rmsDb (out.data() + a + L, nn) - rmsDb (voice.data() + a, nn), 1) << "dB ";
                    if (p == 12.0f && fm == 0.0f)
                    {
                        logMessage ("  PV " + juce::String (order) + "/" + juce::String (wlen) + " CPU per instance: " + juce::String (100.0 * secs / 4.0, 2) + " % of real time, latency " + juce::String (L));
                        writeWav (dir.getChildFile ("pv" + juce::String (order) + "_p12.wav"), out);
                    }
                    if (p == -12.0f && fm == -6.0f) writeWav (dir.getChildFile ("pv" + juce::String (order) + "_p-12f-6.wav"), out);
                }
            logMessage ("  PV " + juce::String (order) + "/" + juce::String (wlen) + " levels: " + lv);
            auto zero = createPhaseVocoderShifterW (order);
            zero->prepare (kSr, 480);
            zero->reset();
            std::vector<float> out (voice.size());
            for (size_t q = 0; q < voice.size(); q += 480) zero->process (voice.data() + q, out.data() + q, 480);
            logMessage ("  PV " + juce::String (order) + "/" + juce::String (wlen) + " measured lag " + juce::String (findLag (voice, out, 4000)));
        }

        beginTest ("formant compensation on/off and tonality limit vs accuracy (raw Signalsmith)");
        for (int mode = 0; mode < 3; ++mode)
            for (int block : { 1440, 2880 })
            {
                double worst = 0, sum = 0; int cnt = 0;
                for (double f : { 110.0, 150.0, 200.0, 260.0, 330.0 })
                    for (float st : { 12.0f, 5.0f, -7.0f })
                    {
                        signalsmith::stretch::SignalsmithStretch<float> s (1L);
                        s.configure (1, block, 240);
                        s.setTransposeSemitones (st);
                        if (mode == 1) s.setFormantSemitones (0, true);
                        if (mode == 2) s.setFormantSemitones (0, false);
                        auto in = sine (f, 1.6, 0.3f);
                        std::vector<float> out (in.size());
                        for (size_t p = 0; p < in.size(); p += 480)
                        {
                            const float* i1[1] = { in.data() + p };
                            float* o1[1] = { out.data() + p };
                            s.process (i1, 480, o1, 480);
                        }
                        const double got = estimateF0 (out.data() + 38400, 28800);
                        const double c = std::abs (centsBetween (got, f * std::pow (2.0, st / 12.0)));
                        worst = std::max (worst, c); sum += c; ++cnt;
                    }
                logMessage ("  mode " + juce::String (mode) + " (0=no formant call,1=comp,2=nocomp) block " + juce::String (block)
                            + ": worst " + juce::String (worst, 1) + " c, mean " + juce::String (sum / cnt, 1));
            }

        beginTest ("accuracy over input frequencies and shifts");
        {
            const std::pair<int, int> cfgs[] = { { 1440, 120 }, { 1440, 480 }, { 1920, 480 }, { 2400, 240 }, { 2880, 240 }, { 2880, 480 }, { 1440, 360 }, { 1920, 240 } };
            for (auto [block, interval] : cfgs)
            {
                double worst = 0;
                juce::String detail;
                for (double f : { 110.0, 150.0, 200.0, 260.0, 330.0 })
                    for (float st : { 12.0f, 5.0f, -7.0f })
                    {
                        ShifterConfig c;
                        c.blockSamples = block;
                        c.intervalSamples = interval;
                        auto sh = createSignalsmithShifter (c);
                        sh->prepare (kSr, 480);
                        sh->setPitchSemitones (st);
                        sh->reset();
                        auto in = sine (f, 1.6, 0.3f);
                        std::vector<float> out (in.size());
                        for (size_t p = 0; p < in.size(); p += 480) sh->process (in.data() + p, out.data() + p, 480);
                        const double got = estimateF0 (out.data() + 38400, 28800);
                        const double cents = centsBetween (got, f * std::pow (2.0, st / 12.0));
                        worst = std::max (worst, std::abs (cents));
                        if (std::abs (cents) > 10) detail << juce::String (f, 0) << "/" << st << ":" << juce::String (cents, 0) << " ";
                    }
                logMessage ("  block " + juce::String (block) + " int " + juce::String (interval) + ": worst " + juce::String (worst, 1) + " c  " + detail);
            }
        }

        beginTest ("zero-crossing frequency vs YIN on shifted sine");
        for (int block : { 960, 1440, 2880 })
        {
            ShifterConfig c;
            c.blockSamples = block;
            c.intervalSamples = 240;
            auto sh = createSignalsmithShifter (c);
            sh->prepare (kSr, 480);
            sh->setPitchSemitones (12);
            sh->reset();
            auto in = sine (220.0, 3.0, 0.3f);
            std::vector<float> out (in.size());
            for (size_t p = 0; p < in.size(); p += 480) sh->process (in.data() + p, out.data() + p, 480);
            double first = -1, last = -1; int count = 0;
            for (int i = 48000; i < 48000 * 2; ++i)
                if (out[size_t (i - 1)] < 0 && out[size_t (i)] >= 0)
                {
                    const double t = (i - 1) + out[size_t (i - 1)] / (out[size_t (i - 1)] - out[size_t (i)]);
                    if (first < 0) first = t;
                    last = t;
                    ++count;
                }
            const double fz = (count - 1) * kSr / (last - first);
            float env0 = 1e9f, env1 = 0;
            for (int i = 48000; i < 96000; i += 240) { float pk = 0; for (int k = 0; k < 240; ++k) pk = std::max (pk, std::abs (out[size_t (i + k)])); env0 = std::min (env0, pk); env1 = std::max (env1, pk); }
            logMessage ("  block " + juce::String (block) + ": zc " + juce::String (fz, 2) + " Hz, yin " + juce::String (estimateF0 (out.data() + 48000, 19200), 2)
                        + " Hz, AM min/max " + juce::String (env0, 3) + "/" + juce::String (env1, 3));
        }

        beginTest ("raw shifter sweep +12 -> -12: top diffs");
        {
            auto sh = createSignalsmithShifter();
            sh->prepare (kSr, 480);
            sh->setPitchSemitones (12);
            sh->reset();
            auto in = synthVoice (4.0, 12);
            std::vector<float> out (in.size());
            for (int b = 0; b * 480 < int (in.size()); ++b)
            {
                if (b >= 100 && b <= 200) sh->setPitchSemitones (12.0f - 24.0f * float (b - 100) / 100.0f);
                sh->process (in.data() + b * 480, out.data() + b * 480, 480);
            }
            juce::String s;
            for (int b = 90; b < 230; ++b)
            {
                float m = 0;
                for (int i = b * 480; i < b * 480 + 480; ++i) m = std::max (m, std::abs (out[size_t (i)] - out[size_t (i - 1)]));
                if (m > 0.03f) s << b << ":" << juce::String (m, 3) << " ";
            }
            logMessage ("  blocks with diff > 0.03: " + s);
            float mi = 0;
            for (int i = 1; i < int (in.size()); ++i) mi = std::max (mi, std::abs (in[size_t (i)] - in[size_t (i - 1)]));
            logMessage ("  input max diff " + juce::String (mi, 4));
            writeWav (dir.getChildFile ("sweep_raw.wav"), out);
            writeWav (dir.getChildFile ("sweep_in.wav"), in);
        }

        beginTest ("pitch sweep: where is the jump?");
        {
            VoiceProcessor vp;
            vp.setNoiseSuppression (false, 1);
            vp.setGate (false, -45, 5, 80, 120);
            vp.setShifter (true, 12, 0);
            vp.prepare (kSr, 480);
            auto in = synthVoice (4.0, 12);
            std::vector<float> out (in.size());
            for (int b = 0; b * 480 < int (in.size()); ++b)
            {
                if (b >= 100 && b <= 200) vp.setShifter (true, 12.0f - 24.0f * float (b - 100) / 100.0f, 0);
                vp.process (in.data() + b * 480, out.data() + b * 480, nullptr, 480);
            }
            float m = 0; int at = 0;
            for (int i = 100 * 480; i < 200 * 480; ++i)
            {
                const float d = std::abs (out[size_t (i)] - out[size_t (i - 1)]);
                if (d > m) { m = d; at = i; }
            }
            float mb = 0;
            for (int i = 100 * 480 - 5760; i < 100 * 480 - 960; ++i) mb = std::max (mb, std::abs (out[size_t (i)] - out[size_t (i - 1)]));
            logMessage ("  max diff " + juce::String (m, 4) + " at block " + juce::String (at / 480) + " (+" + juce::String (at % 480) + "), before " + juce::String (mb, 4));
            writeWav (dir.getChildFile ("sweep.wav"), out);
        }
    }
};

static DiagTests diagTests;
} // namespace koe

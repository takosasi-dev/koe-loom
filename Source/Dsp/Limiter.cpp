#include "Dsp/Limiter.h"

#include "Dsp/Building.h"

#include <algorithm>
#include <cmath>

namespace koe
{
void Limiter::prepare (double sampleRate, float ceilingDb, float lookaheadMs, float releaseMs)
{
    ceiling = dsp::dbToGain (ceilingDb);
    lookahead = std::max (1, int (sampleRate * lookaheadMs * 0.001));
    releaseCoeff = dsp::onePoleCoeff (releaseMs, sampleRate);
    delay.assign (size_t (lookahead), 0.0f);
    // the min window must also cover the sample leaving the delay line this tick -> one longer
    reqRing.assign (size_t (lookahead + 1), 1.0f);
    avgRing.assign (size_t (lookahead), 1.0f);
    reset();
}

void Limiter::reset()
{
    std::fill (delay.begin(), delay.end(), 0.0f);
    std::fill (reqRing.begin(), reqRing.end(), 1.0f);
    std::fill (avgRing.begin(), avgRing.end(), 1.0f);
    pos = 0;
    reqPos = 0;
    held = 1.0f;
    avgSum = double (lookahead);
}

void Limiter::process (float* x, int n)
{
    bool reduced = false;
    for (int i = 0; i < n; ++i)
    {
        float in = x[i];
        if (! std::isfinite (in)) in = 0.0f;
        const float a = std::abs (in);
        const float req = a > ceiling ? ceiling / a : 1.0f;

        reqRing[size_t (reqPos)] = req;
        reqPos = (reqPos + 1) % int (reqRing.size());
        float minReq = 1.0f;
        for (float r : reqRing) minReq = std::min (minReq, r);

        // instant attack to the window minimum, exponential release
        held = minReq < held ? minReq : minReq + releaseCoeff * (held - minReq);

        avgSum += double (held) - double (avgRing[size_t (pos)]);
        avgRing[size_t (pos)] = held;
        float g = float (avgSum / double (lookahead));
        g = std::min (g, 1.0f);

        const float delayed = delay[size_t (pos)];
        delay[size_t (pos)] = in;
        pos = (pos + 1) % lookahead;

        float y = delayed * g;
        y = std::clamp (y, -ceiling, ceiling); // guard against averaging rounding
        if (g < 0.9886f) reduced = true;      // > 0.1 dB
        x[i] = y;
    }
    // keep the running sum from drifting
    double s = 0.0;
    for (float v : avgRing) s += v;
    avgSum = s;
    if (reduced) active.store (true, std::memory_order_relaxed);
}
} // namespace koe

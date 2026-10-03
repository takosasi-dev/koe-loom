#include "Dsp/Building.h"
#include "Effects/IEffect.h"

#include <array>

// eq (koeloom_effects.md §3): 12 dB/oct Butterworth high-pass and low-pass, RBJ low shelf at 150 Hz
// (corner = half-gain point), peak (Q 0.8) at midHz, high shelf at 6 kHz. Parameters ramp over 25 ms and
// the coefficients are recomputed every 16 samples while anything is moving.

namespace koe
{
namespace
{
class EqEffect final : public IEffect
{
public:
    void prepare (double sampleRate, int) override
    {
        sr = sampleRate;
        for (auto& r : p) r.prepare (sr, kRampMs);
    }

    void reset() override
    {
        for (auto& r : p) r.snap (r.target);
        for (auto* b : { &hp, &lp, &low, &mid, &high }) b->reset();
        design();
        countdown = kCoefInterval;
        pending = false;
    }

    void setParam (int index, float v) override
    {
        if (index >= 0 && index < int (p.size())) p[size_t (index)].setTarget (v);
    }

    void process (float* x, int n) override
    {
        for (int i = 0; i < n; ++i)
        {
            bool moving = false;
            for (auto& r : p) { r.next(); moving = moving || r.isRamping(); }
            if (--countdown <= 0)
            {
                countdown = kCoefInterval;
                if (moving || pending) design();
                pending = moving;
            }
            x[i] = high.process (mid.process (low.process (lp.process (hp.process (x[i])))));
        }
    }

private:
    static constexpr float kRampMs = 25.0f, kMidQ = 0.8f;
    static constexpr int kCoefInterval = 16;
    enum { hpfHz, lpfHz, lowDb, midDb, midHz, highDb };

    void design() noexcept
    {
        hp.setHighpass (sr, p[hpfHz].value);
        lp.setLowpass (sr, p[lpfHz].value);
        low.setLowShelf (sr, 150.0f, p[lowDb].value);
        mid.setPeak (sr, p[midHz].value, kMidQ, p[midDb].value);
        high.setHighShelf (sr, 6000.0f, p[highDb].value);
    }

    double sr = 48000.0;
    std::array<dsp::Ramp, 6> p;
    dsp::Biquad hp, lp, low, mid, high;
    int countdown = 0;
    bool pending = false;
};

KOE_REGISTER_EFFECT ("eq", EqEffect)
} // namespace
} // namespace koe

#include "Dsp/Building.h"
#include "Effects/IEffect.h"

#include <array>

// peq (koeloom_effects.md §3): RBJ low shelf, two peaks with their own Q, high shelf. Parameters ramp
// over 25 ms and the coefficients are recomputed every 16 samples while anything is moving.

namespace koe
{
namespace
{
class PeqEffect final : public IEffect
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
        for (auto* b : { &low, &mid1, &mid2, &high }) b->reset();
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
            x[i] = high.process (mid2.process (mid1.process (low.process (x[i]))));
        }
    }

private:
    static constexpr float kRampMs = 25.0f;
    static constexpr int kCoefInterval = 16;
    enum { lowDb, lowHz, mid1Db, mid1Hz, mid1Q, mid2Db, mid2Hz, mid2Q, highDb, highHz };

    void design() noexcept
    {
        low.setLowShelf (sr, p[lowHz].value, p[lowDb].value);
        mid1.setPeak (sr, p[mid1Hz].value, p[mid1Q].value, p[mid1Db].value);
        mid2.setPeak (sr, p[mid2Hz].value, p[mid2Q].value, p[mid2Db].value);
        high.setHighShelf (sr, p[highHz].value, p[highDb].value);
    }

    double sr = 48000.0;
    std::array<dsp::Ramp, 10> p;
    dsp::Biquad low, mid1, mid2, high;
    int countdown = 0;
    bool pending = false;
};

KOE_REGISTER_EFFECT ("peq", PeqEffect)
} // namespace
} // namespace koe

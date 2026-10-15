#include "EqProcessor.h"

namespace kerf
{

namespace
{
    constexpr double pi = juce::MathConstants<double>::pi;

    EqProcessor::Biquad normalise (double b0, double b1, double b2, double a0, double a1, double a2) noexcept
    {
        return { (float) (b0 / a0), (float) (b1 / a0), (float) (b2 / a0), (float) (a1 / a0), (float) (a2 / a0) };
    }

    double clampFreq (double f, double rate) noexcept { return juce::jlimit (10.0, rate * 0.45, f); }
}

EqProcessor::Biquad EqProcessor::Biquad::lowShelf (double rate, double freq, double gainDb) noexcept
{
    const auto A = std::pow (10.0, gainDb / 40.0);
    const auto w = 2.0 * pi * clampFreq (freq, rate) / rate;
    const auto cw = std::cos (w), sw = std::sin (w);
    const auto alpha = sw / 2.0 * std::sqrt (2.0); // shelf slope S = 1
    const auto sa = 2.0 * std::sqrt (A) * alpha;
    return normalise (A * ((A + 1) - (A - 1) * cw + sa), 2 * A * ((A - 1) - (A + 1) * cw), A * ((A + 1) - (A - 1) * cw - sa),
                      (A + 1) + (A - 1) * cw + sa, -2 * ((A - 1) + (A + 1) * cw), (A + 1) + (A - 1) * cw - sa);
}

EqProcessor::Biquad EqProcessor::Biquad::highShelf (double rate, double freq, double gainDb) noexcept
{
    const auto A = std::pow (10.0, gainDb / 40.0);
    const auto w = 2.0 * pi * clampFreq (freq, rate) / rate;
    const auto cw = std::cos (w), sw = std::sin (w);
    const auto alpha = sw / 2.0 * std::sqrt (2.0);
    const auto sa = 2.0 * std::sqrt (A) * alpha;
    return normalise (A * ((A + 1) + (A - 1) * cw + sa), -2 * A * ((A - 1) + (A + 1) * cw), A * ((A + 1) + (A - 1) * cw - sa),
                      (A + 1) - (A - 1) * cw + sa, 2 * ((A - 1) - (A + 1) * cw), (A + 1) - (A - 1) * cw - sa);
}

EqProcessor::Biquad EqProcessor::Biquad::peak (double rate, double freq, double q, double gainDb) noexcept
{
    const auto A = std::pow (10.0, gainDb / 40.0);
    const auto w = 2.0 * pi * clampFreq (freq, rate) / rate;
    const auto alpha = std::sin (w) / (2.0 * juce::jlimit (0.1, 10.0, q));
    const auto cw = std::cos (w);
    return normalise (1 + alpha * A, -2 * cw, 1 - alpha * A, 1 + alpha / A, -2 * cw, 1 - alpha / A);
}

EqProcessor::Biquad EqProcessor::Biquad::lowCut (double rate, double freq, double q) noexcept
{
    const auto w = 2.0 * pi * clampFreq (freq, rate) / rate;
    const auto cw = std::cos (w), alpha = std::sin (w) / (2.0 * juce::jlimit (0.1, 10.0, q));
    return normalise ((1 + cw) / 2, -(1 + cw), (1 + cw) / 2, 1 + alpha, -2 * cw, 1 - alpha);
}

EqProcessor::Biquad EqProcessor::Biquad::highCut (double rate, double freq, double q) noexcept
{
    const auto w = 2.0 * pi * clampFreq (freq, rate) / rate;
    const auto cw = std::cos (w), alpha = std::sin (w) / (2.0 * juce::jlimit (0.1, 10.0, q));
    return normalise ((1 - cw) / 2, 1 - cw, (1 - cw) / 2, 1 + alpha, -2 * cw, 1 - alpha);
}

EqProcessor::Biquad EqProcessor::Biquad::notch (double rate, double freq, double q) noexcept
{
    const auto w = 2.0 * pi * clampFreq (freq, rate) / rate;
    const auto cw = std::cos (w), alpha = std::sin (w) / (2.0 * juce::jlimit (0.1, 10.0, q));
    return normalise (1, -2 * cw, 1, 1 + alpha, -2 * cw, 1 - alpha);
}

EqProcessor::Biquad EqProcessor::Biquad::forBand (const Band& b, double rate) noexcept
{
    switch (b.type)
    {
        case Type::lowShelf:  return lowShelf (rate, b.freq, b.gainDb);
        case Type::highShelf: return highShelf (rate, b.freq, b.gainDb);
        case Type::lowCut:    return lowCut (rate, b.freq, b.q);
        case Type::highCut:   return highCut (rate, b.freq, b.q);
        case Type::notch:     return notch (rate, b.freq, b.q);
        case Type::bell:      break;
    }
    return peak (rate, b.freq, b.q, b.gainDb);
}

double EqProcessor::Biquad::magnitudeDb (double freq, double rate) const noexcept
{
    const auto w = 2.0 * pi * freq / rate;
    const std::complex<double> z1 = std::polar (1.0, -w), z2 = std::polar (1.0, -2.0 * w);
    const auto num = (double) b0 + (double) b1 * z1 + (double) b2 * z2;
    const auto den = 1.0 + (double) a1 * z1 + (double) a2 * z2;
    return 20.0 * std::log10 (std::max (1.0e-9, std::abs (num / den)));
}

double EqProcessor::responseDb (const Settings& s, double freq, double rate) noexcept
{
    double db = 0.0;
    for (const auto& b : s.bands)
        if (b.on)
            db += Biquad::forBand (b, rate).magnitudeDb (freq, rate);
    return db;
}

void EqProcessor::prepare (double newSampleRate, int)
{
    sampleRate = newSampleRate;
    reset();
}

void EqProcessor::reset()
{
    for (auto& s : state)
        s.fill (0.0f);
    mix.fill (0.0f);
    current = {};
    active = false;
}

void EqProcessor::process (juce::AudioBuffer<float>& buffer, int numSamples, const Settings& target)
{
    // Nothing doing anything and nothing fading out: bypass.
    if (! active && target.isFlat())
        return;

    if (! active)
    {
        // Start from the target (no glide from stale values) with every point faded out.
        current = target;
        mix.fill (0.0f);
        for (auto& s : state)
            s.fill (0.0f);
        active = true;
    }

    const auto numChannels = std::min (2, buffer.getNumChannels());
    constexpr int controlInterval = 32;
    const auto glide = (float) std::min (1.0, controlInterval / (0.03 * sampleRate));

    for (int start = 0; start < numSamples; start += controlInterval)
    {
        const auto n = std::min (controlInterval, numSamples - start);

        for (int b = 0; b < maxBands; ++b)
        {
            auto& c = current.bands[(size_t) b];
            const auto& t = target.bands[(size_t) b];

            // A type change on a sounding point restarts it from silence (fade in).
            if (c.type != t.type)
            {
                c = t;
                mix[(size_t) b] = 0.0f;
                state[(size_t) b * 2].fill (0.0f);
                state[(size_t) b * 2 + 1].fill (0.0f);
            }

            // Gains glide linearly in dB, frequency and Q in log space; on/off fades.
            c.gainDb += (t.gainDb - c.gainDb) * glide;
            c.freq = std::exp (std::log (c.freq) + (std::log (t.freq) - std::log (c.freq)) * glide);
            c.q = std::exp (std::log (c.q) + (std::log (t.q) - std::log (c.q)) * glide);
            c.on = t.on || mix[(size_t) b] > 0.0f;
            mix[(size_t) b] = juce::jlimit (0.0f, 1.0f, mix[(size_t) b] + (t.on ? glide : -glide));

            if (c.on)
                filters[(size_t) b] = Biquad::forBand (c, sampleRate);
        }

        for (int ch = 0; ch < numChannels; ++ch)
        {
            auto* d = buffer.getWritePointer (ch, start);
            for (int b = 0; b < maxBands; ++b)
            {
                const auto m = mix[(size_t) b];
                if (! current.bands[(size_t) b].on)
                    continue;

                const auto& f = filters[(size_t) b];
                auto& z = state[(size_t) b * 2 + (size_t) ch];
                for (int i = 0; i < n; ++i)
                {
                    // Transposed direct form II; the point is blended in by its fade.
                    const auto x = d[i];
                    const auto y = f.b0 * x + z[0];
                    z[0] = f.b1 * x - f.a1 * y + z[1];
                    z[1] = f.b2 * x - f.a2 * y;
                    d[i] = x + (y - x) * m;
                }
            }
        }
    }

    // Points that have faded out stop (with a clean filter state for their next start).
    for (int b = 0; b < maxBands; ++b)
        if (! target.bands[(size_t) b].on && mix[(size_t) b] <= 0.0f)
        {
            current.bands[(size_t) b].on = false;
            state[(size_t) b * 2].fill (0.0f);
            state[(size_t) b * 2 + 1].fill (0.0f);
        }

    // Back to bypass once nothing is audible or fading any more.
    if (target.isFlat() && current.isFlat())
        active = false;
}

} // namespace kerf

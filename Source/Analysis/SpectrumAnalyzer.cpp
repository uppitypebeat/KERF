#include "SpectrumAnalyzer.h"

namespace kerf
{

SpectrumAnalyzer::SpectrumAnalyzer()
{
    fifoData.assign ((size_t) fifo.getTotalSize(), 0.0f);
    history.assign ((size_t) fftSize, 0.0f);
    fftData.assign ((size_t) fftSize * 2, 0.0f);
    bands.fill (-90.0f);
    fineBands.fill (-90.0f);
}

float SpectrumAnalyzer::bandFrequency (int band) noexcept
{
    return 30.0f * std::pow (18000.0f / 30.0f, (float) band / (float) (numBands - 1));
}

float SpectrumAnalyzer::fineBandFrequency (int band) noexcept
{
    return 20.0f * std::pow (1000.0f, (float) band / (float) (numFineBands - 1));
}

void SpectrumAnalyzer::push (const float* left, const float* right, int numSamples) noexcept
{
    const auto scope = fifo.write (std::min (numSamples, fifo.getFreeSpace()));

    auto copy = [&] (int start, int size, int offset)
    {
        for (int i = 0; i < size; ++i)
            fifoData[(size_t) (start + i)] = 0.5f * (left[offset + i] + (right != nullptr ? right[offset + i] : left[offset + i]));
    };

    if (scope.blockSize1 > 0) copy (scope.startIndex1, scope.blockSize1, 0);
    if (scope.blockSize2 > 0) copy (scope.startIndex2, scope.blockSize2, scope.blockSize1);
}

bool SpectrumAnalyzer::update()
{
    const auto available = fifo.getNumReady();
    if (available == 0)
    {
        // Silence: let the display decay.
        for (auto& b : bands)
            b = std::max (-90.0f, b - 1.5f);
        for (auto& b : fineBands)
            b = std::max (-90.0f, b - 1.5f);
        return false;
    }

    // Slide the newest samples into the history window.
    const auto toRead = std::min (available, fftSize);
    const auto skip = available - toRead;
    if (skip > 0)
        fifo.read (skip); // discard what we cannot display

    std::move (history.begin() + toRead, history.end(), history.begin());
    const auto scope = fifo.read (toRead);
    auto* dest = history.data() + (fftSize - toRead);
    std::copy_n (fifoData.data() + scope.startIndex1, scope.blockSize1, dest);
    std::copy_n (fifoData.data() + scope.startIndex2, scope.blockSize2, dest + scope.blockSize1);

    std::fill (fftData.begin(), fftData.end(), 0.0f);
    std::copy (history.begin(), history.end(), fftData.begin());
    window.multiplyWithWindowingTable (fftData.data(), (size_t) fftSize);
    fft.performFrequencyOnlyForwardTransform (fftData.data(), true);

    const auto rate = sampleRate.load();
    const auto binHz = (float) rate / (float) fftSize;

    for (int b = 0; b < numBands; ++b)
    {
        // Each band covers the bins between its neighbours' geometric midpoints.
        const auto f = bandFrequency (b);
        const auto lo = b > 0 ? std::sqrt (f * bandFrequency (b - 1)) : f * 0.9f;
        const auto hi = b < numBands - 1 ? std::sqrt (f * bandFrequency (b + 1)) : f * 1.1f;
        const auto k0 = juce::jlimit (1, fftSize / 2, (int) std::floor (lo / binHz));
        const auto k1 = juce::jlimit (k0, fftSize / 2, (int) std::ceil (hi / binHz));

        float mag = 0.0f;
        for (int k = k0; k <= k1; ++k)
            mag = std::max (mag, fftData[(size_t) k]);

        // Normalise (window/FFT gain) and tilt +3 dB/oct so music reads roughly flat.
        const auto db = juce::Decibels::gainToDecibels (mag * 4.0f / (float) fftSize, -90.0f)
                        + 3.0f * std::log2 (f / 1000.0f);

        // Fast attack, slow release.
        auto& v = bands[(size_t) b];
        v = db > v ? v + 0.6f * (db - v) : v + 0.12f * (db - v);
        v = juce::jlimit (-90.0f, 6.0f, v);
    }

    // Fine bands: below the bin spacing, read the spectrum between bins (linear
    // interpolation of the magnitude); above it, average the power of the covered bins.
    // Peak-picking would make the curve spiky; averaging keeps it detailed but calm.
    const auto lastBin = fftSize / 2;
    for (int b = 0; b < numFineBands; ++b)
    {
        const auto f = fineBandFrequency (b);
        const auto lo = b > 0 ? std::sqrt (f * fineBandFrequency (b - 1)) : f * 0.98f;
        const auto hi = b < numFineBands - 1 ? std::sqrt (f * fineBandFrequency (b + 1)) : f * 1.02f;

        float mag = 0.0f;
        if (hi - lo < binHz * 2.0f)
        {
            const auto pos = juce::jlimit (1.0f, (float) lastBin - 1.0f, f / binHz);
            const auto k = (int) pos;
            const auto t = pos - (float) k;
            mag = fftData[(size_t) k] * (1.0f - t) + fftData[(size_t) k + 1] * t;
        }
        else
        {
            const auto k0 = juce::jlimit (1, lastBin, (int) std::ceil (lo / binHz));
            const auto k1 = juce::jlimit (k0, lastBin, (int) std::floor (hi / binHz));
            double power = 0.0;
            for (int k = k0; k <= k1; ++k)
                power += (double) fftData[(size_t) k] * fftData[(size_t) k];
            mag = (float) std::sqrt (power / (double) (k1 - k0 + 1));
        }

        const auto db = juce::Decibels::gainToDecibels (mag * 4.0f / (float) fftSize, -90.0f)
                        + 3.0f * std::log2 (f / 1000.0f);

        auto& v = fineBands[(size_t) b];
        v = db > v ? v + 0.45f * (db - v) : v + 0.1f * (db - v);
        v = juce::jlimit (-90.0f, 6.0f, v);
    }

    return true;
}

} // namespace kerf

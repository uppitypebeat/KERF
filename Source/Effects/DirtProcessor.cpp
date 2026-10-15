#include "DirtProcessor.h"

namespace kerf
{

void DirtProcessor::prepare (double sampleRate, int maxBlockSize)
{
    oversampling = std::make_unique<juce::dsp::Oversampling<float>> (
        2, 1, juce::dsp::Oversampling<float>::filterHalfBandPolyphaseIIR, true, false);
    oversampling->initProcessing ((size_t) maxBlockSize);
    smoothAmount.reset (sampleRate, 0.05);
    reset();
}

void DirtProcessor::reset()
{
    if (oversampling != nullptr)
        oversampling->reset();
    heldSample.fill (0.0f);
    holdCounter = 0;
}

float DirtProcessor::shape (float x, float a, Mode mode) noexcept
{
    if (mode == Mode::warm)
    {
        const auto k = 1.0f + 9.0f * a * a;
        const auto bias = 0.15f * a;                      // asymmetry -> even harmonics
        const auto y = std::tanh (k * x + bias) - std::tanh (bias);
        return y / std::sqrt (k);
    }

    // FUZZ: hard knee, softer on the negative side.
    const auto k = 1.0f + 40.0f * a * a;
    const auto u = k * x;
    const auto y = u >= 0.0f ? 1.0f - std::exp (-u) : -(1.0f - std::exp (u * 0.7f)) / 0.7f;
    return juce::jlimit (-1.4f, 1.4f, y) / (1.0f + 0.35f * std::sqrt (k - 1.0f));
}

void DirtProcessor::process (juce::AudioBuffer<float>& buffer, int numSamples, const Settings& s)
{
    smoothAmount.setTargetValue (juce::jlimit (0.0f, 1.0f, s.amount));

    if (smoothAmount.getCurrentValue() < 0.0005f && smoothAmount.getTargetValue() < 0.0005f)
    {
        smoothAmount.skip (numSamples);
        return; // fully off: bit-exact bypass
    }

    const auto numChannels = std::min (2, buffer.getNumChannels());

    if (s.mode == Mode::crush)
    {
        for (int i = 0; i < numSamples; ++i)
        {
            const auto a = smoothAmount.getNextValue();
            const auto mix = std::min (1.0f, a * 4.0f);
            const auto bits = 16.0f - 13.0f * a;
            const auto step = std::pow (2.0f, 1.0f - bits);
            const auto hold = 1 + (int) (a * a * 23.0f);

            const bool takeNew = ++holdCounter >= hold;
            if (takeNew)
                holdCounter = 0;

            for (int ch = 0; ch < numChannels; ++ch)
            {
                auto* d = buffer.getWritePointer (ch);
                if (takeNew)
                    heldSample[(size_t) ch] = std::round (d[i] / step) * step;
                d[i] += mix * (heldSample[(size_t) ch] - d[i]);
            }
        }
        return;
    }

    juce::dsp::AudioBlock<float> block (buffer.getArrayOfWritePointers(), (size_t) numChannels, 0, (size_t) numSamples);
    auto up = oversampling->processSamplesUp (block);
    const auto upSamples = (int) up.getNumSamples();
    const auto factor = std::max (1, upSamples / std::max (1, numSamples));

    for (int i = 0; i < upSamples; ++i)
    {
        const auto a = (i % factor == 0) ? smoothAmount.getNextValue() : smoothAmount.getCurrentValue();
        const auto mix = std::min (1.0f, a * 4.0f);
        for (size_t ch = 0; ch < up.getNumChannels(); ++ch)
        {
            auto* d = up.getChannelPointer (ch);
            d[i] += mix * (shape (d[i], a, s.mode) - d[i]);
        }
    }

    oversampling->processSamplesDown (block);
}

} // namespace kerf

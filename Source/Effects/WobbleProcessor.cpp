#include "WobbleProcessor.h"

namespace kerf
{

void WobbleProcessor::prepare (double newSampleRate, int)
{
    sampleRate = newSampleRate;
    lineSize = (int) std::ceil (0.04 * sampleRate) + 4; // 40 ms
    for (auto& l : lines)
        l.assign ((size_t) lineSize, 0.0f);
    smoothAmount.reset (sampleRate, 0.05);
    reset();
}

void WobbleProcessor::reset()
{
    for (auto& l : lines)
        std::fill (l.begin(), l.end(), 0.0f);
    writePos = 0;
    wowPhase = flutterPhase = chorusPhase = 0.0;
    drift = driftTarget = 0.0f;
    driftCounter = 0;
    lowpass.fill (0.0f);
}

float WobbleProcessor::read (const std::vector<float>& line, double delaySamples) const noexcept
{
    auto pos = (double) writePos - delaySamples;
    while (pos < 0.0)
        pos += lineSize;
    const auto i0 = (int) pos;
    const auto i1 = (i0 + 1) % lineSize;
    const auto f = (float) (pos - i0);
    return line[(size_t) i0] + f * (line[(size_t) i1] - line[(size_t) i0]);
}

void WobbleProcessor::process (juce::AudioBuffer<float>& buffer, int numSamples, const Settings& s)
{
    if (lineSize == 0)
        return;

    smoothAmount.setTargetValue (juce::jlimit (0.0f, 1.0f, s.amount));
    const auto numChannels = std::min (2, buffer.getNumChannels());

    if (smoothAmount.getCurrentValue() < 0.0005f && smoothAmount.getTargetValue() < 0.0005f)
    {
        // Keep the delay line filled so switching on later does not start from silence.
        for (int i = 0; i < numSamples; ++i)
        {
            for (int ch = 0; ch < 2; ++ch)
                lines[(size_t) ch][(size_t) writePos] = buffer.getSample (std::min (ch, numChannels - 1), i);
            if (++writePos >= lineSize) writePos = 0;
        }
        smoothAmount.skip (numSamples);
        return;
    }

    const auto ms = sampleRate * 0.001;
    const auto twoPi = juce::MathConstants<double>::twoPi;

    for (int i = 0; i < numSamples; ++i)
    {
        const auto a = smoothAmount.getNextValue();

        for (int ch = 0; ch < 2; ++ch)
            lines[(size_t) ch][(size_t) writePos] = buffer.getSample (std::min (ch, numChannels - 1), i);

        // Slow random drift (new target ~3 times per second, smoothed).
        if (--driftCounter <= 0)
        {
            driftTarget = rng.nextFloat() * 2.0f - 1.0f;
            driftCounter = (int) (sampleRate * 0.33);
        }
        drift += (driftTarget - drift) * (float) (1.0 / (sampleRate * 0.25));

        for (int ch = 0; ch < numChannels; ++ch)
        {
            double delay = 0.0;
            float wet = 0.0f;
            float mix = 0.0f;

            if (s.mode == Mode::tape)
            {
                const auto offset = ch == 1 ? 0.07 : 0.0; // slightly different per side, still mono-compatible
                delay = 8.0 * ms
                      + std::sin (twoPi * (wowPhase + offset)) * 3.5 * ms * a
                      + std::sin (twoPi * (flutterPhase + offset)) * 0.25 * ms * a
                      + drift * 1.5 * ms * a;
                wet = read (lines[(size_t) ch], delay);

                // Worn-tape darkening: one-pole low-pass from ~20 kHz down to ~6 kHz.
                const auto cutoff = 20000.0 - 14000.0 * a;
                const auto coeff = (float) (1.0 - std::exp (-twoPi * cutoff / sampleRate));
                lowpass[(size_t) ch] += coeff * (wet - lowpass[(size_t) ch]);
                wet = lowpass[(size_t) ch];
                mix = std::min (1.0f, a * 6.0f); // fades in, then fully "on tape"
            }
            else
            {
                const auto phase = chorusPhase + (ch == 1 ? 0.5 : 0.0);
                delay = 12.0 * ms + std::sin (twoPi * phase) * 3.0 * ms * (0.3 + 0.7 * a);
                wet = read (lines[(size_t) ch], delay);
                mix = 0.5f * std::min (1.0f, a * 3.0f);
            }

            auto* d = buffer.getWritePointer (ch);
            d[i] = d[i] * (1.0f - mix) + wet * mix;
        }

        if (++writePos >= lineSize)
            writePos = 0;

        wowPhase = std::fmod (wowPhase + 0.6 / sampleRate, 1.0);
        flutterPhase = std::fmod (flutterPhase + 7.0 / sampleRate, 1.0);
        chorusPhase = std::fmod (chorusPhase + 0.9 / sampleRate, 1.0);
    }
}

} // namespace kerf

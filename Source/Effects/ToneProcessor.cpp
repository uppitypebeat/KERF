#include "ToneProcessor.h"

namespace kerf
{

float ToneProcessor::lowPassCutoff (float v) noexcept
{
    const auto amount = juce::jlimit (0.0f, 1.0f, -v);              // 0 when knob >= centre
    return 20000.0f * std::pow (60.0f / 20000.0f, amount);          // exponential sweep
}

float ToneProcessor::highPassCutoff (float v) noexcept
{
    const auto amount = juce::jlimit (0.0f, 1.0f, v);
    return 20.0f * std::pow (8000.0f / 20.0f, amount);
}

void ToneProcessor::prepare (double newSampleRate, int maxBlockSize)
{
    sampleRate = newSampleRate;

    const juce::dsp::ProcessSpec spec { sampleRate, (juce::uint32) maxBlockSize, 2 };
    lowPass.prepare (spec);
    highPass.prepare (spec);
    lowPass.setType (juce::dsp::StateVariableTPTFilterType::lowpass);
    highPass.setType (juce::dsp::StateVariableTPTFilterType::highpass);

    smoothFilter.reset (sampleRate, 0.05);
    smoothResonance.reset (sampleRate, 0.05);

    reset();
}

void ToneProcessor::reset()
{
    lowPass.reset();
    highPass.reset();
    updateFilters (smoothFilter.getTargetValue(), smoothResonance.getTargetValue());
}

void ToneProcessor::updateFilters (float v, float resonance)
{
    const auto maxCutoff = (float) (sampleRate * 0.45);
    // Resonance only on the active side; the idle filter stays flat (Q = 0.707).
    const auto q = 0.707f + juce::jlimit (0.0f, 1.0f, resonance) * 5.5f;

    lowPass.setCutoffFrequency (std::min (lowPassCutoff (v), maxCutoff));
    highPass.setCutoffFrequency (std::min (highPassCutoff (v), maxCutoff));
    lowPass.setResonance (v < -0.01f ? q : 0.707f);
    highPass.setResonance (v > 0.01f ? q : 0.707f);
}

void ToneProcessor::process (juce::AudioBuffer<float>& buffer, int numSamples, const Settings& s)
{
    smoothFilter.setTargetValue (juce::jlimit (-1.0f, 1.0f, s.filter));
    smoothResonance.setTargetValue (juce::jlimit (0.0f, 1.0f, s.resonance));

    const auto numChannels = std::min (2, buffer.getNumChannels());

    // --- Filter (skipped entirely when parked at centre) ------------------------
    const bool filterActive = std::abs (smoothFilter.getCurrentValue()) > 0.005f
                           || std::abs (smoothFilter.getTargetValue()) > 0.005f
                           || smoothFilter.isSmoothing();

    if (filterActive)
    {
        for (int start = 0; start < numSamples; start += controlInterval)
        {
            const auto n = std::min (controlInterval, numSamples - start);
            updateFilters (smoothFilter.skip (n), smoothResonance.skip (n));

            for (int ch = 0; ch < numChannels; ++ch)
            {
                auto* d = buffer.getWritePointer (ch, start);
                for (int i = 0; i < n; ++i)
                    d[i] = highPass.processSample (ch, lowPass.processSample (ch, d[i]));
            }
        }
    }
    else
    {
        smoothResonance.skip (numSamples);
        lowPass.reset();
        highPass.reset();
    }

}

} // namespace kerf

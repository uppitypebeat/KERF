#include "PumpProcessor.h"

namespace kerf
{

float PumpProcessor::duck (double phase, double cycleSeconds) noexcept
{
    phase -= std::floor (phase);
    const auto attack = std::max (0.015, 0.003 / std::max (1.0e-3, cycleSeconds)); // down in >= 3 ms
    const auto release = 0.7;                                                        // back up over 70 %

    if (phase < attack)
        return (float) (phase / attack);
    const auto x = (phase - attack) / release;
    if (x >= 1.0)
        return 0.0f;
    return (float) (1.0 - x * x * (3.0 - 2.0 * x)); // smooth swell
}

float PumpProcessor::gainAt (double ppq, double bpm, const Settings& s) noexcept
{
    const auto beats = cycleBeats (s.mode);
    const auto cycleSeconds = beats * 60.0 / std::max (1.0, bpm);
    return 1.0f - juce::jlimit (0.0f, 1.0f, s.amount) * duck (ppq / beats, cycleSeconds);
}

void PumpProcessor::prepare (double newSampleRate, int)
{
    sampleRate = newSampleRate;
    depth.reset (sampleRate, 0.05);
    reset();
}

void PumpProcessor::reset()
{
    depth.setCurrentAndTargetValue (0.0f);
}

void PumpProcessor::process (juce::AudioBuffer<float>& buffer, int numSamples, const Settings& s, double ppqAtStart, double bpm)
{
    depth.setTargetValue (juce::jlimit (0.0f, 1.0f, s.amount));
    if (! depth.isSmoothing() && depth.getCurrentValue() <= 0.0f)
        return; // off: bit-exact bypass

    const auto beats = cycleBeats (s.mode);
    const auto safeBpm = std::max (1.0, bpm);
    const auto cycleSeconds = beats * 60.0 / safeBpm;
    const auto beatsPerSample = safeBpm / 60.0 / sampleRate;
    const auto channels = buffer.getNumChannels();

    for (int i = 0; i < numSamples; ++i)
    {
        const auto ppq = ppqAtStart + i * beatsPerSample;
        const auto g = 1.0f - depth.getNextValue() * duck (ppq / beats, cycleSeconds);
        for (int ch = 0; ch < channels; ++ch)
            buffer.getWritePointer (ch)[i] *= g;
    }
}

} // namespace kerf

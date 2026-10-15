#pragma once

#include <juce_dsp/juce_dsp.h>

namespace kerf
{

/** TONE: one-knob filter (drive lives in DIRT).

    Filter knob (-1..+1):
        -1 .. 0  low-pass sweeping from 20 kHz (transparent) down to 60 Hz
         0       off (both filters fully open)
         0 .. +1 high-pass sweeping from 20 Hz (transparent) up to 8 kHz
    Low-pass and high-pass are always in series, so crossing the centre never switches
    filter type (no click).
 */
class ToneProcessor
{
public:
    struct Settings
    {
        float filter = 0.0f;
        float resonance = 0.2f;
    };

    void prepare (double sampleRate, int maxBlockSize);
    void reset();
    void process (juce::AudioBuffer<float>& buffer, int numSamples, const Settings& s);

    static float lowPassCutoff (float filterValue) noexcept;
    static float highPassCutoff (float filterValue) noexcept;

private:
    void updateFilters (float filterValue, float resonance);

    double sampleRate = 44100.0;
    juce::dsp::StateVariableTPTFilter<float> lowPass, highPass;
    juce::SmoothedValue<float> smoothFilter, smoothResonance;
    static constexpr int controlInterval = 16; // samples between filter coefficient updates
};

} // namespace kerf

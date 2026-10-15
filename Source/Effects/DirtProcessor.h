#pragma once

#include <juce_dsp/juce_dsp.h>

namespace kerf
{

/** DIRT: one-knob distortion with three characters.

      WARM  - tape-style soft saturation (slightly asymmetric: even harmonics)
      FUZZ  - hard, asymmetric clipping
      CRUSH - bit depth (16 -> 3 bits) and sample-rate reduction (up to 1/24)

    Small amounts blend the effect in (the first quarter of the knob is a wet/dry fade),
    and the output is level-compensated so turning DIRT up adds character, not volume.
    WARM and FUZZ run 2x oversampled to keep aliasing down; CRUSH aliases on purpose. */
class DirtProcessor
{
public:
    enum class Mode { warm = 0, fuzz = 1, crush = 2 };

    struct Settings
    {
        float amount = 0.0f; // 0..1
        Mode mode = Mode::warm;
    };

    void prepare (double sampleRate, int maxBlockSize);
    void reset();
    void process (juce::AudioBuffer<float>& buffer, int numSamples, const Settings& s);

    /** The waveshaper for WARM/FUZZ (exposed for tests). */
    static float shape (float x, float amount, Mode mode) noexcept;

private:
    std::unique_ptr<juce::dsp::Oversampling<float>> oversampling;
    juce::SmoothedValue<float> smoothAmount;
    std::array<float, 2> heldSample {};
    int holdCounter = 0;
};

} // namespace kerf

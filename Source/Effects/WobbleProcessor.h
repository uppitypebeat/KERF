#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <vector>

namespace kerf
{

/** WOBBLE: one-knob pitch movement.

      TAPE   - wow (slow), flutter (fast) and random drift, like a worn tape machine;
               the sound also darkens slightly as the amount rises
      CHORUS - stereo chorus (two modulated copies, opposite phase left/right)

    At 0 the effect is bypassed bit-exactly. The modulated delay is short (<= 25 ms). */
class WobbleProcessor
{
public:
    enum class Mode { tape = 0, chorus = 1 };

    struct Settings
    {
        float amount = 0.0f; // 0..1
        Mode mode = Mode::tape;
    };

    void prepare (double sampleRate, int maxBlockSize);
    void reset();
    void process (juce::AudioBuffer<float>& buffer, int numSamples, const Settings& s);

private:
    float read (const std::vector<float>& line, double delaySamples) const noexcept;

    double sampleRate = 44100.0;
    std::array<std::vector<float>, 2> lines;
    int writePos = 0, lineSize = 0;
    double wowPhase = 0.0, flutterPhase = 0.0, chorusPhase = 0.0;
    float drift = 0.0f, driftTarget = 0.0f;
    int driftCounter = 0;
    juce::Random rng { 0x5eed };
    std::array<float, 2> lowpass {};
    juce::SmoothedValue<float> smoothAmount;
};

} // namespace kerf

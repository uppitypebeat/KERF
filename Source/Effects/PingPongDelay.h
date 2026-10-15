#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <vector>

namespace kerf
{

/** SPACE: a stereo ping-pong delay with a single "amount" macro.

    Signal flow:
        send (stereo) -> mono sum -> left line -> right line -> left line ... (cross feedback)
        each feedback pass: damping low-pass + low-cut + soft saturation (prevents runaway)

    The SPACE amount (0..1):
        - raises the wet level along an equal-power curve,
        - lengthens the tail (effective feedback = feedback x (0.45 + 0.55 x amount)),
        - gently ducks the dry signal (up to -3 dB),
        - and compensates wet level for feedback, so turning SPACE up makes the sound wider
          and more rhythmic rather than simply louder. */
class PingPongDelay
{
public:
    struct Settings
    {
        float amount = 0.0f;        // 0..1 (the SPACE knob)
        double delaySeconds = 0.25;
        float feedback = 0.45f;     // 0..0.95
        float tone = 0.55f;         // 0 = dark, 1 = bright
        float width = 1.0f;         // 0 = centred echoes, 1 = full ping-pong
        bool enabled = true;
    };

    /** The SPACE knob: one control that sets everything so the echoes always fit.
        Time is a dotted 1/8 at the current tempo; feedback, width and darkness grow with
        the amount; the dry signal ducks slightly so the result gets wider, not louder. */
    static Settings fromMacro (float amount, double tempoBpm)
    {
        const auto a = juce::jlimit (0.0f, 1.0f, amount);
        Settings s;
        s.amount = a;
        s.delaySeconds = 0.75 * 60.0 / juce::jlimit (20.0, 400.0, tempoBpm > 0.0 ? tempoBpm : 120.0);
        s.feedback = 0.20f + 0.52f * a;
        s.tone = 0.78f - 0.33f * a;
        s.width = 0.55f + 0.45f * a;
        s.enabled = true;
        return s;
    }

    void prepare (double sampleRate, int maxBlockSize);
    void reset();

    /** Adds the wet signal (from `sendL/R`) into `outL/R` and applies the dry duck to outL/R. */
    void process (const float* sendL, const float* sendR, float* outL, float* outR, int numSamples, const Settings& s);

    static constexpr double maxDelaySeconds = 6.0;

private:
    float readDelay (const std::vector<float>& line, double delaySamples) const noexcept;

    double sampleRate = 44100.0;
    std::vector<float> lineL, lineR;
    int writePos = 0;
    int lineSize = 0;

    juce::SmoothedValue<double> smoothDelay;
    juce::SmoothedValue<float> smoothWet, smoothDry, smoothFeedback, smoothWidth, smoothEnable;

    // Feedback-path filters (one-pole)
    float lpStateL = 0.0f, lpStateR = 0.0f;
    float hpStateL = 0.0f, hpStateR = 0.0f;
    float lpCoeff = 0.5f, hpCoeff = 0.995f;
    float lastTone = -1.0f;
    bool jumpToTargetDelay = true; // first block after prepare/reset: no glide from 0 ms
};

} // namespace kerf

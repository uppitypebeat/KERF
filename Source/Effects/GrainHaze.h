#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <array>
#include <vector>

namespace kerf
{

/** HAZE: a long, soft, slowly moving reverb that only the grains go through.

    The grains are diffused (four all-passes), then circulate in eight delay lines that
    feed each other (a Householder matrix), each line damped and gently modulated, so the
    tail blooms into a cloud instead of ringing. One knob: more HAZE is wetter, longer
    (about 2.5 s up to 12 s) and a little darker. Lows under ~150 Hz stay out of it, so it
    never turns to mud. Real-time safe after prepare(). */
class GrainHaze
{
public:
    void prepare (double sampleRate);
    void reset();

    /** Adds the reverb of `inL/R` (the grains) into `outL/R`. */
    void process (const float* inL, const float* inR, float* outL, float* outR, int numSamples, float amount);

    /** Something still rings (for the UI: the haze shows while its tail lasts). */
    float getLevel() const noexcept { return level; }

private:
    static constexpr int numLines = 8;
    static constexpr int numDiffusers = 4;

    struct Line
    {
        std::vector<float> buffer;
        int write = 0;
        double baseDelay = 0.0;  // samples
        float damp = 0.0f;       // low-pass state
        double lfoPhase = 0.0, lfoStep = 0.0;
    };
    struct Diffuser
    {
        std::vector<float> buffer;
        int write = 0;
    };

    double rate = 48000.0;
    std::array<Line, numLines> lines;
    std::array<Diffuser, numDiffusers> diffusers;
    float highPassL = 0.0f, highPassR = 0.0f;
    float shownAmount = 0.0f;  // smoothed knob
    float level = 0.0f;        // smoothed output level
    int quietSamples = 0;      // silent in and out this long: nothing to compute
};

} // namespace kerf

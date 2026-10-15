#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

namespace kerf
{

/** PUMP: tempo-locked volume ducking, the "sidechain" breathing of dance and hip-hop
    beats, without needing a kick to drive it.

    On every beat (1/4), half beat (1/8) or 16th (1/16) the level dips and swells back.
    The amount sets how deep the dip goes (100 % = silence at the start of each cycle).
    The phase comes from a beat clock (the host's while it plays), so the pump sits on
    the grid. No allocation; at 0 the effect is bypassed bit-exactly. */
class PumpProcessor
{
public:
    enum class Mode { quarter = 0, eighth, sixteenth };

    struct Settings
    {
        float amount = 0.0f; // 0..1
        Mode mode = Mode::quarter;
    };

    /** Length of one pump cycle in beats. */
    static double cycleBeats (Mode m) noexcept { return m == Mode::quarter ? 1.0 : (m == Mode::eighth ? 0.5 : 0.25); }

    /** How far the level is down (0..1 of the amount) at `phase` 0..1 of a cycle that
        lasts `cycleSeconds`: a quick dip (at least 3 ms, so it never clicks) and an
        S-shaped swell back over most of the cycle. Shared with the FX display. */
    static float duck (double phase, double cycleSeconds) noexcept;

    /** The gain (0..1) at a beat position. */
    static float gainAt (double ppq, double bpm, const Settings& s) noexcept;

    void prepare (double sampleRate, int maxBlockSize);
    void reset();

    /** `ppqAtStart`: beat position of the block's first sample. */
    void process (juce::AudioBuffer<float>& buffer, int numSamples, const Settings& s, double ppqAtStart, double bpm);

private:
    double sampleRate = 44100.0;
    juce::SmoothedValue<float> depth; // the amount, glided (no zipper noise when turned)
};

} // namespace kerf

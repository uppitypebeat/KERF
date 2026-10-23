#pragma once

#include <juce_core/juce_core.h>
#include <cmath>
#include "../AudioEngine/SampleVoice.h"

namespace kerf
{

/** The toolbar's metronome: a short click on every beat, higher on the first beat of the bar,
    always at KERF's BPM. Added to the output last, so it is heard but never
    recorded, exported or resampled. Real-time safe.

    - A chop or the sample played from PLAY: a click where the playback passes one of the
      sample's beat lines (the voice notes them, to the sample: `clickAt`). Where playback
      starts does not move the beat.
    - The song or the pattern: `process` with a beat position. */
class Metronome
{
public:
    void prepare (double sampleRate) noexcept { rate = sampleRate; reset(); }
    void reset() noexcept { lastBeat = noBeat; ringing = 0; }

    /** Adds the clicks to `left` / `right`. `ppq`: the beat at the first sample, moving on by
        `beatsPerSample`. While not `running` no new click starts (one ringing out still ends). */
    void process (float* left, float* right, int numSamples, bool running, double ppq, double beatsPerSample, double beatsPerBar) noexcept
    {
        const auto bar = std::max (1, (int) std::round (beatsPerBar > 0.0 ? beatsPerBar : 4.0));
        if (! running)
            lastBeat = noBeat;

        for (int n = 0; n < numSamples; ++n)
        {
            if (running)
            {
                const auto beat = ppq + (double) n * beatsPerSample;
                const auto index = (juce::int64) std::floor (beat + 1.0e-9);
                if (index != lastBeat)
                {
                    // Only right at the start of a beat (starting half way through one: wait for the next).
                    if (beat - (double) index < 2.0 * std::abs (beatsPerSample))
                        start (((index % bar) + bar) % bar == 0);
                    lastBeat = index;
                }
            }
            ring (left, right, n);
        }
    }

    /** Adds the clicks a voice noted this block (offsets within it). */
    void clickAt (float* left, float* right, int numSamples, const VoiceGlobals::BeatClicks& clicks) noexcept
    {
        lastBeat = noBeat;
        int next = 0;
        for (int n = 0; n < numSamples; ++n)
        {
            for (; next < clicks.count && clicks.offset[(size_t) next] <= n; ++next)
                if (clicks.offset[(size_t) next] == n)
                    start (clicks.downbeat[(size_t) next]);
            ring (left, right, n);
        }
    }

private:
    void start (bool downbeat) noexcept
    {
        frequency = downbeat ? 1760.0 : 1320.0;
        level = downbeat ? 0.32f : 0.2f;
        phase = 0.0;
        ringing = (int) (0.06 * rate);
        age = 0;
    }

    void ring (float* left, float* right, int n) noexcept
    {
        if (ringing <= 0)
            return;
        const auto t = (double) age / rate;
        const auto click = level * (float) (std::sin (phase) * std::exp (-t / 0.012) * std::min (1.0, t / 0.0005));
        phase += juce::MathConstants<double>::twoPi * frequency / rate;
        left[n] += click;
        if (right != nullptr) right[n] += click;
        --ringing;
        ++age;
    }

    static constexpr juce::int64 noBeat = std::numeric_limits<juce::int64>::min();
    double rate = 48000.0, phase = 0.0, frequency = 1320.0;
    float level = 0.0f;
    juce::int64 lastBeat = noBeat;
    int ringing = 0, age = 0;
};

} // namespace kerf

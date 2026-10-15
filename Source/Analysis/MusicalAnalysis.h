#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include "TransientDetector.h"

namespace kerf
{

/** Tempo estimate. Never treat it as a fact: show `confidence` in the UI. */
struct TempoEstimate
{
    double bpm = 0.0;            // 0 = unknown
    float confidence = 0.0f;     // 0..1
    int64_t firstBeat = 0;       // frame of the estimated first beat (beat phase)
    bool fromLoopLength = false; // true if the sample length matches a whole number of bars
};

/** Key estimate (key profiles on a harmonic chromagram, plus the bass line). */
struct KeyEstimate
{
    int tonic = -1;              // 0 = C ... 11 = B, -1 = unknown
    bool minor = false;
    float confidence = 0.0f;     // 0..1

    juce::String toString() const;
};

namespace TempoAnalyzer
{
    /** Autocorrelation of the onset detection function (60-200 BPM), with a tempo prior
        centred on 120 BPM, octave-error correction, and a loop-length check: if the
        sample is exactly 1/2/4/8/16 bars long at a plausible tempo, that tempo wins. */
    TempoEstimate estimate (const OnsetAnalysis& onsets, int64_t numFrames, const juce::AudioBuffer<float>* audio = nullptr);
}

namespace KeyAnalyzer
{
    KeyEstimate estimate (const juce::AudioBuffer<float>& audio, double sampleRate,
                          const std::function<bool()>& shouldExit = {});

    juce::String noteName (int pitchClass);
}

} // namespace kerf

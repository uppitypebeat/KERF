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
    bool fromName = false;       // the file name said it
};

/** Key estimate (key profiles on a harmonic chromagram, plus the bass line). */
struct KeyEstimate
{
    int tonic = -1;              // 0 = C ... 11 = B, -1 = unknown
    bool minor = false;
    float confidence = 0.0f;     // 0..1
    bool fromName = false;       // the file name said it

    juce::String toString() const;
};

/** What a sample's file name says about it. Sample packs name their loops
    "..._125_C_Min", "... Gmin (90)", "Bass 128bpm F#m": the tempo and the key, as the
    producer made them. */
struct NameTags
{
    std::vector<double> tempos;  // the BPMs it may mean, the likeliest first (an index can look like one)
    int sure = 0;                // how many of the first are marked as one ("128bpm", "(90)", next to the key); the rest are bare numbers
    int tonic = -1;              // 0 = C ... 11 = B, -1 = none named
    bool minor = false;
};

NameTags readNameTags (const juce::String& fileName);

namespace TempoAnalyzer
{
    /** Autocorrelation of the onset detection function (60-200 BPM), with a tempo prior
        centred on 120 BPM, octave-error correction, and a loop-length check: if the
        sample is exactly 1/2/4/8/16 bars long at a plausible tempo, that tempo wins.
        `named`: what the file name says (readNameTags). A tempo marked as one is the answer
        (of several, the one the rhythm fits best); a bare number only when the rhythm or the
        loop's length agrees (it may be an index). The beat grid is found in the audio. */
    TempoEstimate estimate (const OnsetAnalysis& onsets, int64_t numFrames, const juce::AudioBuffer<float>* audio = nullptr,
                            const NameTags& named = {});
}

namespace KeyAnalyzer
{
    KeyEstimate estimate (const juce::AudioBuffer<float>& audio, double sampleRate,
                          const std::function<bool()>& shouldExit = {});

    juce::String noteName (int pitchClass);
}

} // namespace kerf

#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <vector>

namespace kerf
{

/** Multi-resolution min/max/RMS overview of a sample for fast waveform drawing.

    Built once per sample on a background thread; afterwards immutable and shared with
    the UI. Each level summarises `framesPerBin` source frames per bin (both channels
    combined). The renderer picks the coarsest level that still has at least one bin per
    pixel, and falls back to reading raw samples when zoomed in further. */
class WaveformCache
{
public:
    struct Bin { float min = 0.0f, max = 0.0f, rms = 0.0f; };

    struct Level
    {
        int framesPerBin = 1;
        std::vector<Bin> bins;
    };

    WaveformCache (const juce::AudioBuffer<float>& audio, double sampleRate);

    /** Summary of source frames [startFrame, endFrame), combining bins from the best level. */
    Bin getRange (const juce::AudioBuffer<float>& audio, int64_t startFrame, int64_t endFrame) const;

    double getSampleRate() const noexcept  { return sampleRate; }
    int64_t getNumFrames() const noexcept  { return numFrames; }
    float getPeak() const noexcept         { return peak; }
    const std::vector<Level>& getLevels() const noexcept { return levels; }

private:
    std::vector<Level> levels;
    double sampleRate = 44100.0;
    int64_t numFrames = 0;
    float peak = 0.0f;
};

} // namespace kerf

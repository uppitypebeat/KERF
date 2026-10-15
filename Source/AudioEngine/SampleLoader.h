#pragma once

#include <juce_audio_formats/juce_audio_formats.h>
#include "SampleData.h"

namespace kerf
{

/** Decodes audio files into SampleData. Thread-safe; meant to run on a background thread. */
class SampleLoader
{
public:
    static constexpr double maxLengthSeconds = 600.0; // 10 minutes (~230 MB stereo at 48 kHz)

    struct Result
    {
        SampleData::Ptr sample;
        juce::String error; // human-readable, empty on success
    };

    SampleLoader();

    Result loadFile (const juce::File& file) const;
    Result loadFromMemory (const void* data, size_t size, const juce::String& name, const juce::File& originalFile) const;
    Result fromBuffer (juce::AudioBuffer<float>&& buffer, double sampleRate, const juce::String& name, const juce::File& file) const;

    /** e.g. "*.wav;*.aif;*.aiff;*.flac;*.ogg" */
    juce::String getWildcard() const;
    bool canLoad (const juce::File& f) const;

    /** Encodes a sample as 24-bit FLAC (lossless for anything recorded at <= 24 bit). */
    static juce::MemoryBlock encodeFlac (const SampleData& sample);

private:
    Result readFromReader (std::unique_ptr<juce::AudioFormatReader> reader, const juce::String& name, const juce::File& file) const;

    mutable juce::AudioFormatManager formats; // createReaderFor is non-const but does not modify the registered formats
};

} // namespace kerf

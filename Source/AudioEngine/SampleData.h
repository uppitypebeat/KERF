#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

namespace kerf
{

/** An immutable, loaded sample. Created off the audio thread, shared by reference.

    The audio thread only ever reads this. It is never modified after construction, so
    readers need no locking. Lifetime is managed by ReleasePool so the final release
    (and therefore the deallocation) never happens on the audio thread. */
struct SampleData final : public juce::ReferenceCountedObject
{
    using Ptr = juce::ReferenceCountedObjectPtr<SampleData>;

    SampleData (juce::AudioBuffer<float>&& audioIn, double rate, juce::String displayName, juce::File source)
        : audio (std::move (audioIn)), sampleRate (rate), name (std::move (displayName)), file (std::move (source))
    {
        jassert (audio.getNumChannels() >= 1 && audio.getNumChannels() <= 2);
    }

    int getNumChannels() const noexcept     { return audio.getNumChannels(); }
    int64_t getNumSamples() const noexcept  { return audio.getNumSamples(); }
    double getLengthSeconds() const noexcept { return sampleRate > 0 ? (double) audio.getNumSamples() / sampleRate : 0.0; }

    const juce::AudioBuffer<float> audio;
    const double sampleRate;
    const juce::String name;
    const juce::File file;       // may be juce::File{} for recordings / captures
    juce::String originalFormat; // e.g. "WAV", "FLAC" — informational
    int originalBitDepth = 0;

    JUCE_DECLARE_NON_COPYABLE (SampleData)
};

} // namespace kerf

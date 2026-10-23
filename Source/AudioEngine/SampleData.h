#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <array>

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
        buildOctaves();
    }

    int getNumChannels() const noexcept     { return audio.getNumChannels(); }
    int64_t getNumSamples() const noexcept  { return audio.getNumSamples(); }
    double getLengthSeconds() const noexcept { return sampleRate > 0 ? (double) audio.getNumSamples() / sampleRate : 0.0; }

    /** The audio an octave (level 1), two or three lower in sample rate: band-limited copies
        at half, a quarter and an eighth of the length. Grains pitched up read from these, so
        they do not alias (level 0 is `audio` itself). */
    static constexpr int numOctaves = 3;
    const float* readPointer (int level, int channel) const noexcept
    {
        const auto& b = level <= 0 ? audio : octaves[(size_t) std::min (level, numOctaves) - 1];
        return b.getReadPointer (std::min (channel, b.getNumChannels() - 1));
    }
    int64_t numSamplesAt (int level) const noexcept
    {
        return level <= 0 ? audio.getNumSamples() : octaves[(size_t) std::min (level, numOctaves) - 1].getNumSamples();
    }

    const juce::AudioBuffer<float> audio;
    const double sampleRate;
    const juce::String name;
    const juce::File file;       // may be juce::File{} for recordings / captures
    juce::String originalFormat; // e.g. "WAV", "FLAC" — informational
    int originalBitDepth = 0;

private:
    std::array<juce::AudioBuffer<float>, numOctaves> octaves;

    void buildOctaves()
    {
        // Each level from the one above: a half-band low-pass (windowed sinc, 31 taps; every
        // other tap is zero), keeping every second sample.
        constexpr int half = 15;
        float h[half + 1];
        for (int k = 0; k <= half; ++k)
        {
            const auto x = (double) k * 0.5 * juce::MathConstants<double>::pi;
            const auto sinc = k == 0 ? 1.0 : std::sin (x) / x;
            const auto window = 0.42 + 0.5 * std::cos (juce::MathConstants<double>::pi * k / (half + 1))
                              + 0.08 * std::cos (2.0 * juce::MathConstants<double>::pi * k / (half + 1)); // Blackman
            h[k] = (float) (0.5 * sinc * window);
        }
        const juce::AudioBuffer<float>* from = &audio;
        for (int level = 0; level < numOctaves; ++level)
        {
            const auto inLength = from->getNumSamples();
            const auto outLength = std::max (1, (inLength + 1) / 2);
            auto& to = octaves[(size_t) level];
            to.setSize (from->getNumChannels(), outLength);
            for (int ch = 0; ch < from->getNumChannels(); ++ch)
            {
                const auto* in = from->getReadPointer (ch);
                auto* out = to.getWritePointer (ch);
                auto at = [in, inLength] (int i) { return in[juce::jlimit (0, inLength - 1, i)]; };
                for (int m = 0; m < outLength; ++m)
                {
                    const auto c = 2 * m;
                    auto acc = h[0] * at (c);
                    if (c - half >= 0 && c + half < inLength)
                        for (int k = 1; k <= half; k += 2)
                            acc += h[k] * (in[c - k] + in[c + k]);
                    else
                        for (int k = 1; k <= half; k += 2)
                            acc += h[k] * (at (c - k) + at (c + k));
                    out[m] = acc;
                }
            }
            from = &to;
        }
    }

public:

    JUCE_DECLARE_NON_COPYABLE (SampleData)
};

} // namespace kerf

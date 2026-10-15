#include "TimeStretch.h"

// Third-party header: silence its conversion warnings so they cannot hide ours.
JUCE_BEGIN_IGNORE_WARNINGS_MSVC (4244 4267 4100 4127 4189 4456 4457 4458 4459 4701 4702 4305)
JUCE_BEGIN_IGNORE_WARNINGS_GCC_LIKE ("-Wconversion", "-Wsign-conversion", "-Wfloat-conversion", "-Wshadow",
                                     "-Wunused-parameter", "-Wimplicit-int-float-conversion", "-Wshorten-64-to-32")
#include <signalsmith-stretch/signalsmith-stretch.h>
JUCE_END_IGNORE_WARNINGS_GCC_LIKE
JUCE_END_IGNORE_WARNINGS_MSVC

namespace kerf::TimeStretch
{

SampleData::Ptr render (const SampleData& source, const Request& r, const std::function<bool()>& cancelled)
{
    const auto total = source.getNumSamples();
    const auto start = juce::jlimit<int64_t> (0, total, r.start);
    const auto end = juce::jlimit<int64_t> (start, total, r.end);
    const auto chopLength = end - start;

    if (chopLength < 16 || ! (r.timeRatio > 0.05 && r.timeRatio < 20.0) || ! std::isfinite (r.semitones))
        return nullptr;

    const auto rate = source.sampleRate;
    const auto channels = source.getNumChannels();

    // Context around the chop: real neighbouring audio where it exists.
    const auto context = (int64_t) (0.15 * rate);
    const auto preRoll = std::min (context, start);
    const auto postRoll = std::min (context, total - end);

    signalsmith::stretch::SignalsmithStretch<float> stretch;
    stretch.presetDefault (channels, (float) rate);
    // Tonality limit 8 kHz: keeps high frequencies from sounding "phasey" when transposing.
    stretch.setTransposeSemitones ((float) r.semitones, (float) (8000.0 / rate));

    // Input = pre-roll + chop + post-roll, plus zero padding if still too short.
    const auto minimum = (int64_t) stretch.outputSeekLength ((float) r.timeRatio) * 2 + 64;
    const auto natural = preRoll + chopLength + postRoll;
    const auto zeroPad = std::max<int64_t> (0, minimum - natural);
    const auto inputLength = natural + zeroPad;

    juce::AudioBuffer<float> input (channels, (int) inputLength);
    input.clear();
    for (int ch = 0; ch < channels; ++ch)
        input.copyFrom (ch, 0, source.audio, ch, (int) (start - preRoll), (int) natural);

    if (cancelled && cancelled())
        return nullptr;

    const auto outputLength = std::max<int64_t> (1, (int64_t) std::llround ((double) inputLength / r.timeRatio));
    juce::AudioBuffer<float> output (channels, (int) outputLength);
    output.clear();

    std::vector<float*> inPtrs, outPtrs;
    for (int ch = 0; ch < channels; ++ch)
    {
        inPtrs.push_back (input.getWritePointer (ch));
        outPtrs.push_back (output.getWritePointer (ch));
    }

    if (! stretch.exact (inPtrs.data(), (int) inputLength, outPtrs.data(), (int) outputLength))
        return nullptr;

    if (cancelled && cancelled())
        return nullptr;

    // Crop to the chop.
    const auto cropStart = (int) std::llround ((double) preRoll / r.timeRatio);
    const auto cropLength = (int) juce::jlimit<int64_t> (1, outputLength - cropStart, std::llround ((double) chopLength / r.timeRatio));

    juce::AudioBuffer<float> chop (channels, cropLength);
    for (int ch = 0; ch < channels; ++ch)
    {
        chop.copyFrom (ch, 0, output, ch, cropStart, cropLength);

        // Safety: the stretcher should never produce NaN/Inf, but never trust it blindly.
        auto* d = chop.getWritePointer (ch);
        for (int i = 0; i < cropLength; ++i)
            if (! std::isfinite (d[i]))
                d[i] = 0.0f;
    }

    return new SampleData (std::move (chop), rate, source.name + " [stretched]", {});
}

} // namespace kerf::TimeStretch

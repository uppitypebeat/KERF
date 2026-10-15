#include "SampleLoader.h"

namespace kerf
{

SampleLoader::SampleLoader()
{
    formats.registerBasicFormats(); // WAV, AIFF, FLAC, Ogg Vorbis (+ platform codecs where available)
}

juce::String SampleLoader::getWildcard() const
{
    return formats.getWildcardForAllFormats();
}

bool SampleLoader::canLoad (const juce::File& f) const
{
    return f.existsAsFile() && formats.findFormatForFileExtension (f.getFileExtension()) != nullptr;
}

SampleLoader::Result SampleLoader::loadFile (const juce::File& file) const
{
    if (! file.existsAsFile())
        return { nullptr, "File not found: " + file.getFullPathName() };

    if (file.getSize() == 0)
        return { nullptr, "The file is empty." };

    if (formats.findFormatForFileExtension (file.getFileExtension()) == nullptr)
        return { nullptr, "Unsupported file type (" + file.getFileExtension() + "). Use WAV, AIFF, FLAC or OGG." };

    std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (file));
    if (reader == nullptr)
        return { nullptr, "Could not read \"" + file.getFileName() + "\". The file may be damaged." };

    return readFromReader (std::move (reader), file.getFileNameWithoutExtension(), file);
}

SampleLoader::Result SampleLoader::loadFromMemory (const void* data, size_t size, const juce::String& name, const juce::File& originalFile) const
{
    if (data == nullptr || size == 0)
        return { nullptr, "No audio data." };

    auto stream = std::make_unique<juce::MemoryInputStream> (data, size, true);
    std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (std::move (stream)));

    if (reader == nullptr)
        return { nullptr, "Stored audio could not be decoded." };

    return readFromReader (std::move (reader), name, originalFile);
}

SampleLoader::Result SampleLoader::readFromReader (std::unique_ptr<juce::AudioFormatReader> reader, const juce::String& name, const juce::File& file) const
{
    const auto rate = reader->sampleRate;
    const auto length = reader->lengthInSamples;
    const auto channels = (int) reader->numChannels;

    if (! (rate >= 8000.0 && rate <= 768000.0))
        return { nullptr, "Unsupported sample rate (" + juce::String (rate) + " Hz)." };

    if (length <= 0 || channels <= 0)
        return { nullptr, "The file contains no audio." };

    if ((double) length / rate > maxLengthSeconds)
        return { nullptr, "The file is too long (" + juce::String ((double) length / rate / 60.0, 1)
                          + " min). The maximum is " + juce::String ((int) (maxLengthSeconds / 60.0)) + " minutes." };

    const auto outChannels = std::min (channels, 2);
    juce::AudioBuffer<float> buffer;

    try
    {
        buffer.setSize (outChannels, (int) length);
    }
    catch (const std::bad_alloc&)
    {
        return { nullptr, "Not enough memory to load this file." };
    }

    if (channels <= 2)
    {
        if (! reader->read (&buffer, 0, (int) length, 0, true, outChannels > 1))
            return { nullptr, "Reading the audio data failed." };
    }
    else
    {
        // More than two channels: fold odd channels left, even channels right.
        juce::AudioBuffer<float> all (channels, (int) length);
        if (! reader->read (&all, 0, (int) length, 0, true, true))
            return { nullptr, "Reading the audio data failed." };

        buffer.clear();
        for (int ch = 0; ch < channels; ++ch)
            buffer.addFrom (ch % 2, 0, all, ch, 0, (int) length, 2.0f / (float) channels);
    }

    // Replace any NaN/Inf from damaged files with silence.
    for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
    {
        auto* d = buffer.getWritePointer (ch);
        for (int i = 0; i < buffer.getNumSamples(); ++i)
            if (! std::isfinite (d[i]))
                d[i] = 0.0f;
    }

    auto result = fromBuffer (std::move (buffer), rate, name, file);
    if (result.sample != nullptr)
    {
        result.sample->originalFormat = reader->getFormatName();
        result.sample->originalBitDepth = (int) reader->bitsPerSample;
    }
    return result;
}

SampleLoader::Result SampleLoader::fromBuffer (juce::AudioBuffer<float>&& buffer, double sampleRate, const juce::String& name, const juce::File& file) const
{
    if (buffer.getNumSamples() == 0 || buffer.getNumChannels() == 0)
        return { nullptr, "The audio is empty." };

    if (buffer.getNumChannels() > 2)
        buffer.setSize (2, buffer.getNumSamples(), true);

    return { new SampleData (std::move (buffer), sampleRate, name.isEmpty() ? juce::String ("Untitled") : name, file), {} };
}

juce::MemoryBlock SampleLoader::encodeFlac (const SampleData& sample)
{
    juce::MemoryBlock block;
    juce::FlacAudioFormat flac;

    std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::MemoryOutputStream> (block, false);
    auto writer = flac.createWriterFor (stream, juce::AudioFormatWriterOptions{}
                                                    .withSampleRate (sample.sampleRate)
                                                    .withNumChannels (sample.getNumChannels())
                                                    .withBitsPerSample (24)
                                                    .withQualityOptionIndex (5));
    if (writer == nullptr)
    {
        // Unusual sample rate FLAC cannot store: fall back to 32-bit float WAV.
        juce::WavAudioFormat wav;
        writer = wav.createWriterFor (stream, juce::AudioFormatWriterOptions{}
                                                  .withSampleRate (sample.sampleRate)
                                                  .withNumChannels (sample.getNumChannels())
                                                  .withBitsPerSample (32)
                                                  .withSampleFormat (juce::AudioFormatWriterOptions::SampleFormat::floatingPoint));
        if (writer == nullptr)
            return {};
    }

    writer->writeFromAudioSampleBuffer (sample.audio, 0, sample.audio.getNumSamples());
    writer.reset(); // flushes into `block`
    return block;
}

} // namespace kerf

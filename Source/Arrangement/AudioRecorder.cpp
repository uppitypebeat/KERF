#include "AudioRecorder.h"

namespace kerf
{

bool AudioRecorder::arm (const Request& r, double sampleRate)
{
    if (state.load() != State::idle)
        return false;

    request = r;
    rate = sampleRate > 0.0 ? sampleRate : 44100.0;
    const auto maxFrames = (int) std::ceil (juce::jlimit (1.0, 600.0, r.maxSeconds) * rate);

    try
    {
        buffer.setSize (2, maxFrames, false, true, false);
    }
    catch (const std::bad_alloc&)
    {
        return false;
    }

    buffer.clear();
    writePos.store (0);
    targetFrames.store (0);
    level.store (0.0f);
    stopRequested.store (false);
    cancelRequested.store (false);
    lastSeenAudioBlocks = audioBlocks.load();
    stalledPolls = 0;

    state.store (State::armed); // publishes the buffer to the audio thread
    return true;
}

std::optional<juce::AudioBuffer<float>> AudioRecorder::takeFinished()
{
    if (state.load() != State::finished)
        return std::nullopt;

    const auto frames = (int) writePos.load();
    std::optional<juce::AudioBuffer<float>> result;

    if (frames > 0)
    {
        juce::AudioBuffer<float> take (2, frames);
        for (int ch = 0; ch < 2; ++ch)
            take.copyFrom (ch, 0, buffer, ch, 0, frames);
        result = std::move (take);
    }

    buffer.setSize (0, 0);
    state.store (State::idle);
    return result;
}

void AudioRecorder::forceSettleIfAudioStalled()
{
    const auto blocks = audioBlocks.load();
    if (blocks != lastSeenAudioBlocks)
    {
        lastSeenAudioBlocks = blocks;
        stalledPolls = 0;
        return;
    }

    if (++stalledPolls < 3 || ! isActive())
        return;

    // The audio thread is not running: complete the user's request here.
    if (cancelRequested.exchange (false))
    {
        buffer.setSize (0, 0);
        state.store (State::idle);
    }
    else if (stopRequested.exchange (false))
    {
        state.store (writePos.load() > 0 ? State::finished : State::idle);
    }
}

void AudioRecorder::finishRecording() noexcept
{
    state.store (writePos.load() > 0 ? State::finished : State::idle);
}

void AudioRecorder::process (Source source, const float* const* channels, int numChannels, int numSamples,
                             const TransportInfo& transport, double effectiveBpm)
{
    if (source == Source::input)
        audioBlocks.fetch_add (1, std::memory_order_relaxed);

    auto s = state.load();
    if (s == State::idle || s == State::finished || source != request.source)
        return;

    if (cancelRequested.exchange (false))
    {
        writePos.store (0);
        state.store (State::idle); // message thread frees the buffer on the next arm
        return;
    }

    if (stopRequested.exchange (false))
    {
        finishRecording();
        return;
    }

    int startOffset = 0;

    if (s == State::armed)
    {
        const auto bpm = effectiveBpm > 0.0 ? effectiveBpm : 120.0;
        if (request.lengthBeats > 0.0)
            targetFrames.store ((int64_t) std::llround (request.lengthBeats * 60.0 / bpm * rate));

        s = (request.startOnBar && transport.isPlaying) ? State::waitingForBar : State::recording;
        state.store (s);
    }

    if (s == State::waitingForBar)
    {
        if (! transport.isPlaying || transport.bpm <= 0.0)
            return;

        const auto beatsPerBar = transport.beatsPerBar > 0.0 ? transport.beatsPerBar : 4.0;
        const auto samplesPerBeat = 60.0 / transport.bpm * rate;
        const auto nextBar = std::ceil (transport.ppqAtBlockStart / beatsPerBar - 1.0e-9) * beatsPerBar;
        // Small tolerance: beat positions accumulate rounding error, and a bar line at
        // 479.99999 samples really is sample 480 (the next block).
        const auto offset = std::ceil ((nextBar - transport.ppqAtBlockStart) * samplesPerBeat - 1.0e-4);

        if (offset >= numSamples)
            return;

        startOffset = juce::jlimit (0, numSamples - 1, (int) offset);
        // Re-evaluate the target with the host tempo now that it is known precisely.
        if (request.lengthBeats > 0.0)
            targetFrames.store ((int64_t) std::llround (request.lengthBeats * samplesPerBeat));
        state.store (State::recording);
    }

    // Recording
    const auto capacity = (int64_t) buffer.getNumSamples();
    const auto target = targetFrames.load();
    const auto limit = target > 0 ? std::min (target, capacity) : capacity;
    auto pos = writePos.load();
    const auto toWrite = (int) std::min<int64_t> (numSamples - startOffset, limit - pos);

    float peak = 0.0f;
    if (toWrite > 0)
    {
        for (int ch = 0; ch < 2; ++ch)
        {
            // No input channels (bus disabled by the host): record silence.
            const float* src = (numChannels > 0 && channels != nullptr) ? channels[std::min (ch, numChannels - 1)] : nullptr;
            if (src == nullptr)
            {
                buffer.clear (ch, (int) pos, toWrite);
                continue;
            }
            src += startOffset;
            buffer.copyFrom (ch, (int) pos, src, toWrite);
            peak = std::max (peak, juce::FloatVectorOperations::findMaximum (src, toWrite));
            peak = std::max (peak, -juce::FloatVectorOperations::findMinimum (src, toWrite));
        }
        pos += toWrite;
        writePos.store (pos);
    }

    level.store (std::max (peak, level.load() * 0.85f));

    if (pos >= limit)
        finishRecording();
}

} // namespace kerf

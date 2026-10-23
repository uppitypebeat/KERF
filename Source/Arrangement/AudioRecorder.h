#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <atomic>
#include <optional>
#include "../AudioEngine/SamplerEngine.h"

namespace kerf
{

/** Records audio into a pre-allocated buffer: KERF's input bus (audio routed from the
    DAW).

    Thread protocol:
      - The message thread allocates the buffer and arms (only from `idle`).
      - Only the audio thread moves armed -> waiting -> recording -> finished, and
        handles stop/cancel requests at the start of its next block.
      - The message thread reads the buffer only in `finished`, then returns to idle.
    So the buffer is never touched by both threads at once, and the audio thread never
    allocates or frees memory. */
class AudioRecorder
{
public:
    enum class Source { input = 0, output = 1 };
    enum class State : int { idle = 0, armed, waitingForBar, recording, finished };

    struct Request
    {
        Source source = Source::input;
        double lengthBeats = 0.0;   // 0 = free (until stopped or the maximum length)
        double maxSeconds = 120.0;
        bool startOnBar = true;     // when the host is playing, start on the next bar line
    };

    /** Message thread. Returns false if busy or out of memory. */
    bool arm (const Request& request, double sampleRate);
    void requestStop()   { stopRequested.store (true); }
    void requestCancel() { cancelRequested.store (true); }

    /** Message thread: if a take is finished, hands it over and returns to idle. */
    std::optional<juce::AudioBuffer<float>> takeFinished();

    /** Message thread: used when the host has stopped calling processBlock, so pending
        stop/cancel requests can still complete. */
    void forceSettleIfAudioStalled();

    State getState() const noexcept { return state.load(); }
    Source getSource() const noexcept { return request.source; }
    bool isActive() const noexcept { const auto s = getState(); return s != State::idle && s != State::finished; }
    double getRecordedSeconds() const noexcept { return (double) writePos.load() / std::max (1.0, rate); }
    double getTargetSeconds() const noexcept { return targetFrames.load() > 0 ? (double) targetFrames.load() / std::max (1.0, rate) : 0.0; }
    float getLevel() const noexcept { return level.load(); }

    /** Audio thread. `channels` are `numChannels` pointers already offset to this block. */
    void process (Source source, const float* const* channels, int numChannels, int numSamples, const TransportInfo& transport,
                  double effectiveBpm);

private:
    void finishRecording() noexcept;

    juce::AudioBuffer<float> buffer;
    Request request;
    double rate = 44100.0;

    std::atomic<State> state { State::idle };
    std::atomic<bool> stopRequested { false }, cancelRequested { false };
    std::atomic<int64_t> writePos { 0 }, targetFrames { 0 };
    std::atomic<float> level { 0.0f };
    std::atomic<uint32_t> audioBlocks { 0 };
    uint32_t lastSeenAudioBlocks = 0;
    int stalledPolls = 0;
};

} // namespace kerf

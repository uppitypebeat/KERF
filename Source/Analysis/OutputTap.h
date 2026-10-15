#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <vector>

namespace kerf
{

/** Lock-free stereo copy of KERF's output for the visualisers (oscilloscope, vectorscope).

    Audio thread: push() writes what fits and never blocks (if the UI falls behind,
    samples are dropped). UI thread: pull() moves everything new into a rolling history
    of the most recent frames. */
class OutputTap
{
public:
    static constexpr int historySize = 4096;

    OutputTap()
    {
        left.assign ((size_t) fifo.getTotalSize(), 0.0f);
        right.assign ((size_t) fifo.getTotalSize(), 0.0f);
        historyL.assign ((size_t) historySize, 0.0f);
        historyR.assign ((size_t) historySize, 0.0f);
    }

    /** Audio thread. */
    void push (const float* l, const float* r, int numSamples) noexcept
    {
        const auto scope = fifo.write (std::min (numSamples, fifo.getFreeSpace()));
        auto copy = [&] (int start, int size, int offset)
        {
            for (int i = 0; i < size; ++i)
            {
                left[(size_t) (start + i)] = l[offset + i];
                right[(size_t) (start + i)] = r[offset + i];
            }
        };
        if (scope.blockSize1 > 0) copy (scope.startIndex1, scope.blockSize1, 0);
        if (scope.blockSize2 > 0) copy (scope.startIndex2, scope.blockSize2, scope.blockSize1);
    }

    /** UI thread: appends new frames to the history. Returns how many arrived. */
    int pull()
    {
        const auto available = fifo.getNumReady();
        const auto scope = fifo.read (available);
        auto append = [this] (int start, int size)
        {
            for (int i = 0; i < size; ++i)
            {
                historyL[(size_t) writePos] = left[(size_t) (start + i)];
                historyR[(size_t) writePos] = right[(size_t) (start + i)];
                writePos = (writePos + 1) % historySize;
            }
        };
        append (scope.startIndex1, scope.blockSize1);
        append (scope.startIndex2, scope.blockSize2);
        return available;
    }

    /** UI thread: frame `age` samples ago (0 = newest). */
    float getLeft (int age) const noexcept  { return historyL[(size_t) index (age)]; }
    float getRight (int age) const noexcept { return historyR[(size_t) index (age)]; }

private:
    int index (int age) const noexcept { return ((writePos - 1 - age) % historySize + historySize) % historySize; }

    juce::AbstractFifo fifo { 16384 };
    std::vector<float> left, right, historyL, historyR;
    int writePos = 0;
};

} // namespace kerf

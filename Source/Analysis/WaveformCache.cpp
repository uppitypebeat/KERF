#include "WaveformCache.h"

namespace kerf
{

WaveformCache::WaveformCache (const juce::AudioBuffer<float>& audio, double rate)
    : sampleRate (rate), numFrames (audio.getNumSamples())
{
    const int numChannels = audio.getNumChannels();
    if (numFrames == 0 || numChannels == 0)
        return;

    // Level 0 straight from the audio; coarser levels from the level below (x8 each).
    Level base;
    base.framesPerBin = 32;
    base.bins.resize ((size_t) ((numFrames + base.framesPerBin - 1) / base.framesPerBin));

    for (size_t b = 0; b < base.bins.size(); ++b)
    {
        const auto start = (int64_t) b * base.framesPerBin;
        const auto end = std::min<int64_t> (numFrames, start + base.framesPerBin);
        float mn = std::numeric_limits<float>::max(), mx = std::numeric_limits<float>::lowest();
        double sumSq = 0.0;

        for (int ch = 0; ch < numChannels; ++ch)
        {
            const auto* d = audio.getReadPointer (ch);
            for (auto i = start; i < end; ++i)
            {
                mn = std::min (mn, d[i]);
                mx = std::max (mx, d[i]);
                sumSq += (double) d[i] * d[i];
            }
        }

        base.bins[b] = { mn, mx, (float) std::sqrt (sumSq / (double) ((end - start) * numChannels)) };
        peak = std::max ({ peak, std::abs (mn), std::abs (mx) });
    }

    levels.push_back (std::move (base));

    while (levels.back().bins.size() > 64)
    {
        const auto& prev = levels.back();
        Level next;
        next.framesPerBin = prev.framesPerBin * 8;
        next.bins.resize ((prev.bins.size() + 7) / 8);

        for (size_t b = 0; b < next.bins.size(); ++b)
        {
            Bin out { std::numeric_limits<float>::max(), std::numeric_limits<float>::lowest(), 0.0f };
            double sumSq = 0.0;
            int count = 0;

            for (size_t k = b * 8; k < std::min (prev.bins.size(), b * 8 + 8); ++k)
            {
                out.min = std::min (out.min, prev.bins[k].min);
                out.max = std::max (out.max, prev.bins[k].max);
                sumSq += (double) prev.bins[k].rms * prev.bins[k].rms;
                ++count;
            }

            out.rms = (float) std::sqrt (sumSq / std::max (1, count));
            next.bins[b] = out;
        }

        levels.push_back (std::move (next));
    }
}

WaveformCache::Bin WaveformCache::getRange (const juce::AudioBuffer<float>& audio, int64_t startFrame, int64_t endFrame) const
{
    startFrame = juce::jlimit<int64_t> (0, numFrames, startFrame);
    endFrame = juce::jlimit<int64_t> (0, numFrames, endFrame);

    if (endFrame <= startFrame)
    {
        // Sub-sample range (deep zoom): return the single sample value.
        if (startFrame < numFrames && audio.getNumChannels() > 0)
        {
            const auto v = audio.getSample (0, (int) startFrame);
            return { v, v, std::abs (v) };
        }
        return {};
    }

    const auto span = endFrame - startFrame;

    // Pick the coarsest level whose bins are no bigger than the range.
    const Level* level = nullptr;
    for (const auto& l : levels)
        if (l.framesPerBin <= span)
            level = &l;

    Bin out { std::numeric_limits<float>::max(), std::numeric_limits<float>::lowest(), 0.0f };

    if (level == nullptr)
    {
        // Zoomed in past level 0: read raw samples.
        double sumSq = 0.0;
        int count = 0;
        for (int ch = 0; ch < audio.getNumChannels(); ++ch)
        {
            const auto* d = audio.getReadPointer (ch);
            for (auto i = startFrame; i < endFrame; ++i)
            {
                out.min = std::min (out.min, d[i]);
                out.max = std::max (out.max, d[i]);
                sumSq += (double) d[i] * d[i];
                ++count;
            }
        }
        out.rms = (float) std::sqrt (sumSq / std::max (1, count));
        return out;
    }

    const auto firstBin = (size_t) (startFrame / level->framesPerBin);
    const auto lastBin = std::min (level->bins.size(), (size_t) ((endFrame + level->framesPerBin - 1) / level->framesPerBin));
    double sumSq = 0.0;
    int count = 0;

    for (auto b = firstBin; b < lastBin; ++b)
    {
        out.min = std::min (out.min, level->bins[b].min);
        out.max = std::max (out.max, level->bins[b].max);
        sumSq += (double) level->bins[b].rms * level->bins[b].rms;
        ++count;
    }

    out.rms = (float) std::sqrt (sumSq / std::max (1, count));
    if (count == 0)
        out = {};
    return out;
}

} // namespace kerf

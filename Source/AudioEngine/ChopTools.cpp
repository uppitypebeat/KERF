#include "ChopTools.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <random>

namespace kerf::ChopTools
{

namespace
{
    std::vector<int64_t> withRegionEnds (std::vector<int64_t> inner, int64_t regionStart, int64_t regionEnd)
    {
        std::vector<int64_t> out { regionStart };
        std::sort (inner.begin(), inner.end());
        for (auto p : inner)
            if (p > regionStart && p < regionEnd)
                out.push_back (p);
        out.push_back (regionEnd);
        return out;
    }
}

std::vector<int64_t> byTransients (const OnsetAnalysis& onsets, int64_t regionStart, int64_t regionEnd,
                                   float sensitivity, double minDistanceSeconds, int maxChops)
{
    auto picked = TransientDetector::selectOnsets (onsets, sensitivity, minDistanceSeconds, maxChops - 1, regionStart, regionEnd);

    // Remove an onset right at the region start (the start is always a boundary).
    const auto minGap = (int64_t) (minDistanceSeconds * onsets.sampleRate);
    picked.erase (std::remove_if (picked.begin(), picked.end(), [&] (int64_t p) { return p - regionStart < std::max<int64_t> (minGap, 32); }),
                  picked.end());

    return withRegionEnds (picked, regionStart, regionEnd);
}

std::vector<int64_t> byBeats (double bpm, double sampleRate, int64_t regionStart, int64_t regionEnd, double beatsPerChop,
                              double origin)
{
    if (bpm <= 0.0 || sampleRate <= 0.0 || beatsPerChop <= 0.0)
        return { regionStart, regionEnd };

    // The tempo's grid from the start of the sample (the beat lines in the waveform): every
    // cut exactly where a beat (or a bar) of that tempo falls, nothing guessed from the audio.
    const auto step = 60.0 / bpm * beatsPerChop * sampleRate;
    std::vector<int64_t> inner;
    for (auto k = std::ceil (((double) regionStart - origin) / step); origin + k * step < (double) regionEnd && inner.size() < 256; k += 1.0)
        if (const auto at = (int64_t) std::llround (origin + k * step); at > regionStart)
            inner.push_back (at);

    // Avoid a tiny first sliver - unless SHIFT put the grid there on purpose (what comes
    // before its first line is a chop of its own, the rest falls on the lines).
    if (! inner.empty() && origin == 0.0 && inner.front() - regionStart < (int64_t) (step * 0.25))
        inner.erase (inner.begin());

    return withRegionEnds (inner, regionStart, regionEnd);
}

std::vector<int64_t> random (int64_t regionStart, int64_t regionEnd, int count, uint32_t seed,
                             float minLengthFraction, const OnsetAnalysis* snap, double sampleRate)
{
    count = juce::jlimit (1, 128, count);
    const auto length = regionEnd - regionStart;
    if (length <= 0 || count == 1)
        return { regionStart, regionEnd };

    const auto minLen = (int64_t) ((double) length / count * juce::jlimit (0.0f, 0.95f, minLengthFraction));

    // Distribute the free space randomly: each chop gets minLen plus a random share.
    std::mt19937 rng (seed);
    std::uniform_real_distribution<double> dist (0.05, 1.0);

    std::vector<double> weights ((size_t) count);
    double total = 0.0;
    for (auto& w : weights) { w = dist (rng); total += w; }

    const auto freeSpace = (double) (length - minLen * count);
    std::vector<int64_t> inner;
    double pos = (double) regionStart;
    for (int i = 0; i < count - 1; ++i)
    {
        pos += (double) minLen + freeSpace * weights[(size_t) i] / total;
        inner.push_back ((int64_t) pos);
    }

    if (snap != nullptr && ! snap->onsets.empty())
    {
        const auto window = (int64_t) (0.04 * sampleRate);
        for (auto& p : inner)
        {
            int64_t best = p, bestDist = window + 1;
            for (const auto& o : snap->onsets)
                if (auto d = std::abs (o.position - p); d < bestDist)
                {
                    bestDist = d;
                    best = o.position;
                }
            p = best;
        }
        std::sort (inner.begin(), inner.end());
        inner.erase (std::unique (inner.begin(), inner.end()), inner.end());
    }

    return withRegionEnds (inner, regionStart, regionEnd);
}

std::vector<int64_t> RuleBasedSuggester::suggest (const juce::AudioBuffer<float>& audio, double sampleRate,
                                                  const OnsetAnalysis& onsets, const TempoEstimate& tempo,
                                                  int64_t regionStart, int64_t regionEnd, int targetCount)
{
    targetCount = juce::jlimit (2, 64, targetCount);
    const auto length = regionEnd - regionStart;
    if (length <= 0 || onsets.onsets.empty())
        return { regionStart, regionEnd };

    // Short-time energy (10 ms windows) for silence/contrast scoring.
    const auto mono = TransientDetector::mixToMono (audio);
    const auto win = std::max<int64_t> (64, (int64_t) (0.01 * sampleRate));
    auto energy = [&] (int64_t from, int64_t to)
    {
        from = juce::jlimit<int64_t> (0, (int64_t) mono.size(), from);
        to = juce::jlimit<int64_t> (0, (int64_t) mono.size(), to);
        double s = 0.0;
        for (auto i = from; i < to; ++i)
            s += (double) mono[(size_t) i] * mono[(size_t) i];
        return to > from ? std::sqrt (s / (double) (to - from)) : 0.0;
    };

    const auto beatPeriod = tempo.bpm > 0.0 ? 60.0 / tempo.bpm * sampleRate : 0.0;

    struct Scored { int64_t pos; double score; };
    std::vector<Scored> scored;

    for (const auto& o : onsets.onsets)
    {
        if (o.position <= regionStart + win || o.position >= regionEnd - win)
            continue;

        double score = o.strength;

        // Beat alignment: up to +60 % on the beat, +25 % on the off-beat (8th).
        if (beatPeriod > 0.0 && tempo.confidence > 0.2f)
        {
            const auto phase = std::fmod ((double) (o.position - tempo.firstBeat) / beatPeriod + 1000.0, 1.0);
            const auto distBeat = std::min (phase, 1.0 - phase);
            const auto distHalf = std::abs (phase - 0.5);
            score *= 1.0 + 0.6 * std::max (0.0, 1.0 - distBeat * 10.0) * tempo.confidence
                         + 0.25 * std::max (0.0, 1.0 - distHalf * 10.0) * tempo.confidence;
        }

        // Quiet before / loud after -> likely phrase start.
        const auto before = energy (o.position - 8 * win, o.position - win / 2);
        const auto after = energy (o.position, o.position + 8 * win);
        const auto contrast = after > 1.0e-6 ? juce::jlimit (0.0, 1.0, 1.0 - before / after) : 0.0;
        score *= 1.0 + 0.8 * contrast;

        scored.push_back ({ o.position, score });
    }

    std::sort (scored.begin(), scored.end(), [] (const Scored& a, const Scored& b) { return a.score > b.score; });

    // Musically sized chops: at least a fraction of an equal division apart.
    const auto minSpacing = (int64_t) ((double) length / targetCount * 0.45);

    std::vector<int64_t> chosen;
    for (const auto& s : scored)
    {
        if ((int) chosen.size() >= targetCount - 1)
            break;

        if (s.pos - regionStart < minSpacing || regionEnd - s.pos < minSpacing / 2)
            continue;

        const bool tooClose = std::any_of (chosen.begin(), chosen.end(), [&] (int64_t c) { return std::abs (c - s.pos) < minSpacing; });
        if (! tooClose)
            chosen.push_back (s.pos);
    }

    return withRegionEnds (chosen, regionStart, regionEnd);
}

} // namespace kerf::ChopTools

#include "ChopTools.h"

#include <algorithm>
#include <array>
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
            if (p > regionStart && p < regionEnd && p > out.back())
                out.push_back (p);
        out.push_back (regionEnd);
        return out;
    }

    /** The first onset at or after `position`. */
    std::vector<Onset>::const_iterator onsetFrom (const OnsetAnalysis& analysis, int64_t position)
    {
        return std::lower_bound (analysis.onsets.begin(), analysis.onsets.end(), position,
                                 [] (const Onset& o, int64_t p) { return o.position < p; });
    }

    /** `chops` cuts at random in [start, end), on hits where there are any. */
    std::vector<int64_t> randomCuts (int64_t start, int64_t end, int chops, std::mt19937& rng, const OnsetAnalysis* analysis)
    {
        const auto length = end - start;
        if (chops <= 1 || length < 2 * (int64_t) chops)
            return { start, end };

        auto minLength = (int64_t) ((double) length / chops * 0.35);
        std::vector<int64_t> picked;
        auto farEnough = [&] (int64_t p)
        {
            if (p - start < minLength || end - p < minLength)
                return false;
            return std::none_of (picked.begin(), picked.end(), [&] (int64_t q) { return std::abs (q - p) < minLength; });
        };

        // Hits, the strong ones more likely.
        if (analysis != nullptr)
        {
            struct Candidate { int64_t position; double weight; };
            std::vector<Candidate> candidates;
            for (auto it = onsetFrom (*analysis, start + 1); it != analysis->onsets.end() && it->position < end; ++it)
                if (it->strength >= 0.05f)
                    candidates.push_back ({ it->position, 0.15 + (double) it->strength });

            while ((int) picked.size() < chops - 1 && ! candidates.empty())
            {
                double total = 0.0;
                for (const auto& c : candidates)
                    total += c.weight;
                auto r = std::uniform_real_distribution<double> (0.0, total) (rng);
                size_t i = 0;
                while (i + 1 < candidates.size() && r >= candidates[i].weight)
                    r -= candidates[i++].weight;
                if (farEnough (candidates[i].position))
                    picked.push_back (candidates[i].position);
                candidates.erase (candidates.begin() + (ptrdiff_t) i);
            }
        }

        // Not enough hits: random positions (onto a hit within 40 ms if there is one).
        std::uniform_real_distribution<double> where ((double) start, (double) end);
        for (int tries = 0; (int) picked.size() < chops - 1 && tries < 400; ++tries)
        {
            if (tries == 200)
                minLength /= 2; // crowded: closer cuts rather than fewer chops
            auto p = (int64_t) where (rng);
            if (analysis != nullptr)
            {
                const auto window = (int64_t) (0.04 * analysis->sampleRate);
                auto bestDistance = window + 1;
                for (auto it = onsetFrom (*analysis, p - window); it != analysis->onsets.end() && it->position <= p + window; ++it)
                    if (const auto d = std::abs (it->position - p); d < bestDistance)
                    {
                        bestDistance = d;
                        p = it->position;
                    }
            }
            if (farEnough (p))
                picked.push_back (p);
        }

        return withRegionEnds (picked, start, end);
    }
}

std::vector<int64_t> byTransients (const OnsetAnalysis& onsets, int64_t regionStart, int64_t regionEnd,
                                   float sensitivity, double minDistanceSeconds, int maxCount)
{
    auto picked = TransientDetector::selectOnsets (onsets, sensitivity, minDistanceSeconds, maxCount - 1, regionStart, regionEnd);

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

std::vector<ChopSpan> spansBetween (const std::vector<int64_t>& cuts)
{
    std::vector<ChopSpan> spans;
    for (size_t i = 0; i + 1 < cuts.size(); ++i)
        if (cuts[i + 1] > cuts[i])
            spans.push_back ({ cuts[i], cuts[i + 1] });
    return spans;
}

std::vector<ChopSpan> leaveOutSilence (const std::vector<int64_t>& cuts, const OnsetAnalysis& analysis, bool splitAtSilence)
{
    if (cuts.size() < 2)
        return {};

    const auto minGap = (int64_t) (minGapSeconds * analysis.sampleRate);
    const auto endGap = (int64_t) (0.03 * analysis.sampleRate);       // silence at a chop's end: left out from this long
    const auto scrapLength = (int64_t) (0.025 * analysis.sampleRate);
    const auto& silences = analysis.silences;

    // Each chop's sound: the pieces of it between the silences that are left out.
    struct Piece { int64_t start, end; bool afterSilence, beforeSilence; };
    std::vector<std::vector<Piece>> chops (cuts.size() - 1);
    auto first = silences.begin();
    for (size_t i = 0; i + 1 < cuts.size(); ++i)
    {
        const auto a = cuts[i], b = cuts[i + 1];
        if (b <= a)
            continue;
        while (first != silences.end() && first->end <= a)
            ++first;

        Piece piece { a, b, false, false };
        for (auto s = first; s != silences.end() && s->start < b; ++s)
        {
            const auto lo = std::max (s->start, a), hi = std::min (s->end, b);
            if (hi <= lo)
                continue;
            if (lo <= piece.start)
            {
                piece.start = hi; // starts in silence: where the sound does
                piece.afterSilence = true;
            }
            else if (hi >= b)
            {
                if (hi - lo >= endGap) // ends in silence: with its sound's tail
                {
                    piece.end = lo;
                    piece.beforeSilence = true;
                }
            }
            else if (splitAtSilence && hi - lo >= minGap)
            {
                chops[i].push_back ({ piece.start, lo, piece.afterSilence, true });
                piece = { hi, b, true, false };
            }
        }
        if (piece.end > piece.start)
            chops[i].push_back (piece);
    }

    // Scraps: a bit of sound cut off from its sound by a cut and by silence.
    for (size_t i = 0; i < chops.size(); ++i)
    {
        auto& pieces = chops[i];
        for (size_t k = 0; k < pieces.size();)
        {
            const auto p = pieces[k];
            if (p.end - p.start >= scrapLength || ! (p.afterSilence || p.beforeSilence))
            {
                ++k;
                continue;
            }

            const auto a = cuts[i], b = cuts[i + 1];
            if (p.start == a && ! p.afterSilence && i > 0 && ! chops[i - 1].empty() && chops[i - 1].back().end == a)
            {
                chops[i - 1].back().end = p.end; // the tail of the chop before
                chops[i - 1].back().beforeSilence = p.beforeSilence;
            }
            else if (p.end == b && ! p.beforeSilence && i + 1 < chops.size() && ! chops[i + 1].empty() && chops[i + 1].front().start == b)
            {
                chops[i + 1].front().start = p.start; // the start of the next chop's sound
                chops[i + 1].front().afterSilence = p.afterSilence;
            }
            pieces.erase (pieces.begin() + (ptrdiff_t) k); // (alone between silences: away)
        }
    }

    std::vector<ChopSpan> spans;
    for (const auto& pieces : chops)
        for (const auto& p : pieces)
            if (p.end - p.start >= ChopLimits::minLength)
                spans.push_back ({ p.start, p.end });
    return spans;
}

std::vector<int64_t> snapToHits (std::vector<int64_t> cuts, const OnsetAnalysis& analysis, int64_t window)
{
    if (cuts.size() < 3 || window <= 0 || analysis.onsets.empty())
        return cuts;

    constexpr float minStrength = 0.12f;
    double drift = 0.0; // how far the music sits from the grid lately (strong hits only)
    for (size_t i = 1; i + 1 < cuts.size(); ++i)
    {
        const auto centre = cuts[i] + (int64_t) drift;
        const Onset* best = nullptr;
        double bestScore = 0.0;
        for (auto it = onsetFrom (analysis, centre - window); it != analysis.onsets.end() && it->position <= centre + window; ++it)
        {
            if (it->strength < minStrength)
                continue;
            const auto d = (double) std::abs (it->position - centre) / (double) window;
            if (const auto score = (double) it->strength * (1.0 - d * d); score > bestScore)
            {
                bestScore = score;
                best = &*it;
            }
        }

        if (best != nullptr && best->position > cuts[i - 1] && best->position < cuts[i + 1])
        {
            if (best->strength >= 0.3f)
                drift = juce::jlimit (-0.5 * (double) window, 0.5 * (double) window, 0.5 * drift + 0.5 * (double) (best->position - cuts[i]));
            cuts[i] = best->position;
        }
    }

    // In order, none closer than a chop can be.
    std::vector<int64_t> out { cuts.front() };
    for (size_t i = 1; i + 1 < cuts.size(); ++i)
        if (cuts[i] - out.back() >= ChopLimits::minLength && cuts.back() - cuts[i] >= ChopLimits::minLength)
            out.push_back (cuts[i]);
    out.push_back (cuts.back());
    return out;
}

std::vector<ChopSpan> soundsIn (const OnsetAnalysis& analysis, int64_t regionStart, int64_t regionEnd)
{
    const auto minGap = (int64_t) (minGapSeconds * analysis.sampleRate);
    std::vector<ChopSpan> sounds;
    auto from = regionStart;
    for (const auto& s : analysis.silences)
    {
        if (s.end <= from)
            continue;
        if (s.start >= regionEnd)
            break;
        const auto lo = std::max (s.start, from), hi = std::min (s.end, regionEnd);
        if (lo <= from)
        {
            from = hi; // the region starts in silence
        }
        else if (hi >= regionEnd)
        {
            sounds.push_back ({ from, lo }); // ends in it
            from = regionEnd;
        }
        else if (hi - lo >= minGap)
        {
            sounds.push_back ({ from, lo });
            from = hi;
        }
    }
    if (from < regionEnd)
        sounds.push_back ({ from, regionEnd });

    sounds.erase (std::remove_if (sounds.begin(), sounds.end(), [] (const ChopSpan& s) { return s.length() < ChopLimits::minLength; }),
                  sounds.end());
    return sounds;
}

std::vector<Share> shareOut (const std::vector<ChopSpan>& sounds, int count)
{
    std::vector<Share> shares;
    if (sounds.empty() || count < 1)
        return shares;

    if (count >= (int) sounds.size())
    {
        // By length, at least one each (largest remainder).
        double total = 0.0;
        for (const auto& s : sounds)
            total += (double) s.length();
        std::vector<double> ideal;
        int given = 0;
        for (const auto& s : sounds)
        {
            ideal.push_back ((double) count * (double) s.length() / total);
            shares.push_back ({ s, std::max (1, (int) std::floor (ideal.back())) });
            given += shares.back().chops;
        }
        while (given != count)
        {
            const auto more = given < count;
            size_t best = shares.size();
            double bestOwed = 0.0;
            for (size_t i = 0; i < shares.size(); ++i)
            {
                const auto owed = ideal[i] - (double) shares[i].chops;
                if (! more && shares[i].chops <= 1)
                    continue;
                if (best == shares.size() || (more ? owed > bestOwed : owed < bestOwed))
                {
                    best = i;
                    bestOwed = owed;
                }
            }
            if (best == shares.size())
                break;
            shares[best].chops += more ? 1 : -1;
            given += more ? 1 : -1;
        }
        return shares;
    }

    // Fewer chops than sounds: the two sounds with the shortest silence between them share
    // a chop, again and again.
    for (const auto& s : sounds)
        shares.push_back ({ s, 1 });
    while ((int) shares.size() > count)
    {
        size_t best = 0;
        auto bestGap = std::numeric_limits<int64_t>::max();
        for (size_t i = 0; i + 1 < shares.size(); ++i)
            if (const auto gap = shares[i + 1].span.start - shares[i].span.end; gap < bestGap)
            {
                bestGap = gap;
                best = i;
            }
        shares[best].span.end = shares[best + 1].span.end;
        shares.erase (shares.begin() + (ptrdiff_t) best + 1);
    }
    return shares;
}

std::vector<ChopSpan> equal (const OnsetAnalysis* analysis, int64_t regionStart, int64_t regionEnd, int count)
{
    if (analysis == nullptr)
        return spansBetween (equalSlices (regionStart, regionEnd, count));

    std::vector<ChopSpan> spans;
    for (const auto& share : shareOut (soundsIn (*analysis, regionStart, regionEnd), count))
    {
        auto cuts = equalSlices (share.span.start, share.span.end, share.chops);
        const auto part = share.span.length() / std::max (1, share.chops);
        cuts = snapToHits (cuts, *analysis, std::min ((int64_t) (0.025 * analysis->sampleRate), part / 10));
        const auto more = leaveOutSilence (cuts, *analysis, false);
        spans.insert (spans.end(), more.begin(), more.end());
    }
    return spans;
}

std::vector<ChopSpan> random (const OnsetAnalysis* analysis, int64_t regionStart, int64_t regionEnd, int count, uint32_t seed)
{
    count = juce::jlimit (1, 128, count);
    std::mt19937 rng (seed);
    if (analysis == nullptr)
        return spansBetween (randomCuts (regionStart, regionEnd, count, rng, nullptr));

    std::vector<ChopSpan> spans;
    for (const auto& share : shareOut (soundsIn (*analysis, regionStart, regionEnd), count))
    {
        const auto more = leaveOutSilence (randomCuts (share.span.start, share.span.end, share.chops, rng, analysis), *analysis, false);
        spans.insert (spans.end(), more.begin(), more.end());
    }
    return spans;
}

std::vector<ChopSpan> RuleBasedSuggester::suggest (const juce::AudioBuffer<float>& audio, double sampleRate,
                                                   const OnsetAnalysis& onsets, const TempoEstimate& tempo,
                                                   int64_t regionStart, int64_t regionEnd, int targetCount)
{
    targetCount = juce::jlimit (2, 128, targetCount);
    if (regionEnd - regionStart <= 0)
        return {};

    // Every sound after a silence starts a chop; with fewer chops than sounds, the closest
    // sounds share one.
    const auto sounds = soundsIn (onsets, regionStart, regionEnd);
    if (sounds.empty())
        return {};
    if (targetCount <= (int) sounds.size())
    {
        std::vector<ChopSpan> spans;
        for (const auto& share : shareOut (sounds, targetCount))
        {
            const auto more = leaveOutSilence ({ share.span.start, share.span.end }, onsets, false);
            spans.insert (spans.end(), more.begin(), more.end());
        }
        return spans;
    }

    // Short-time energy (10 ms windows) for silence/contrast scoring, on the whole signal
    // and on its lows (under ~150 Hz: kicks, bass notes).
    const auto mono = TransientDetector::mixToMono (audio);
    std::vector<float> low (mono.size());
    {
        const auto k = (float) (1.0 - std::exp (-juce::MathConstants<double>::twoPi * 150.0 / sampleRate));
        float y = 0.0f;
        for (size_t i = 0; i < mono.size(); ++i)
            low[i] = (y += k * (mono[i] - y));
    }
    const auto win = std::max<int64_t> (64, (int64_t) (0.01 * sampleRate));
    auto energy = [] (const std::vector<float>& x, int64_t from, int64_t to)
    {
        from = juce::jlimit<int64_t> (0, (int64_t) x.size(), from);
        to = juce::jlimit<int64_t> (0, (int64_t) x.size(), to);
        double s = 0.0;
        for (auto i = from; i < to; ++i)
            s += (double) x[(size_t) i] * x[(size_t) i];
        return to > from ? std::sqrt (s / (double) (to - from)) : 0.0;
    };

    const auto beatPeriod = tempo.bpm > 0.0 ? 60.0 / tempo.bpm * sampleRate : 0.0;
    const auto onGrid = beatPeriod > 0.0 && tempo.confidence > 0.2f;
    auto beatPhase = [&] (int64_t position) { return std::fmod ((double) (position - tempo.firstBeat) / beatPeriod + 1000.0, 1.0); };
    auto beatIndex = [&] (int64_t position) { return (int) (std::llround ((double) (position - tempo.firstBeat) / beatPeriod) % 4 + 4) % 4; };

    struct Scored { int64_t pos; double score; size_t sound; };
    std::vector<Scored> scored;
    std::vector<double> lowAfter;
    double loudestLow = 1.0e-9;

    for (const auto& o : onsets.onsets)
    {
        if (o.position <= regionStart + win || o.position >= regionEnd - win)
            continue;
        const auto in = std::find_if (sounds.begin(), sounds.end(), [&] (const ChopSpan& s) { return o.position > s.start + win && o.position < s.end - win; });
        if (in == sounds.end())
            continue;

        double score = o.strength;

        // Beat alignment: up to +60 % on the beat, +25 % on the off-beat (8th).
        if (onGrid)
        {
            const auto phase = beatPhase (o.position);
            const auto distBeat = std::min (phase, 1.0 - phase);
            const auto distHalf = std::abs (phase - 0.5);
            score *= 1.0 + 0.6 * std::max (0.0, 1.0 - distBeat * 10.0) * tempo.confidence
                         + 0.25 * std::max (0.0, 1.0 - distHalf * 10.0) * tempo.confidence;
        }

        // Quiet before / loud after -> likely phrase start.
        const auto before = energy (mono, o.position - 8 * win, o.position - win / 2);
        const auto after = energy (mono, o.position, o.position + 8 * win);
        const auto contrast = after > 1.0e-6 ? juce::jlimit (0.0, 1.0, 1.0 - before / after) : 0.0;
        score *= 1.0 + 0.8 * contrast;

        // A short pause just before (shorter than a silence that splits): a fresh start.
        if (std::any_of (onsets.silences.begin(), onsets.silences.end(), [&] (const Silence& s) { return s.end <= o.position && o.position - s.end < win; }))
            score *= 1.5;

        // The lows coming in (a kick, a bass note): weighed below, against the loudest.
        const auto lowBefore = energy (low, o.position - 8 * win, o.position - win / 2);
        const auto la = energy (low, o.position, o.position + 8 * win);
        lowAfter.push_back (la > 1.0e-9 ? la * juce::jlimit (0.0, 1.0, 1.0 - lowBefore / la) : 0.0);
        loudestLow = std::max (loudestLow, lowAfter.back());

        scored.push_back ({ o.position, score, (size_t) std::distance (sounds.begin(), in) });
    }

    for (size_t i = 0; i < scored.size(); ++i)
        scored[i].score *= 1.0 + 0.6 * lowAfter[i] / loudestLow;

    // The bar's first beat: of the four beats, the one the strongest (and lowest) hits fall on.
    if (onGrid && ! scored.empty())
    {
        std::array<double, 4> weight {};
        for (size_t i = 0; i < scored.size(); ++i)
        {
            const auto phase = beatPhase (scored[i].pos);
            if (std::min (phase, 1.0 - phase) < 0.1)
                weight[(size_t) beatIndex (scored[i].pos)] += scored[i].score * (1.0 + lowAfter[i] / loudestLow);
        }
        const auto downbeat = (int) std::distance (weight.begin(), std::max_element (weight.begin(), weight.end()));
        for (auto& s : scored)
        {
            const auto phase = beatPhase (s.pos);
            if (std::min (phase, 1.0 - phase) >= 0.1)
                continue;
            const auto index = (beatIndex (s.pos) - downbeat + 4) % 4;
            s.score *= 1.0 + (index == 0 ? 0.5 : (index == 2 ? 0.2 : 0.0)) * tempo.confidence;
        }
    }

    std::sort (scored.begin(), scored.end(), [] (const Scored& a, const Scored& b) { return a.score > b.score; });

    // Musically sized chops: at least a fraction of an equal division apart (closer only if
    // there are not enough hits that far apart).
    int64_t audible = 0;
    for (const auto& s : sounds)
        audible += s.length();
    const auto wanted = targetCount - (int) sounds.size();
    std::vector<std::vector<int64_t>> chosen (sounds.size());
    int found = 0;
    for (const auto spacingFactor : { 0.45, 0.22, 0.1 })
    {
        const auto minSpacing = (int64_t) ((double) audible / targetCount * spacingFactor);
        for (const auto& s : scored)
        {
            if (found >= wanted)
                break;
            const auto& sound = sounds[s.sound];
            if (s.pos - sound.start < minSpacing || sound.end - s.pos < minSpacing / 2)
                continue;
            auto& here = chosen[s.sound];
            if (std::any_of (here.begin(), here.end(), [&] (int64_t c) { return std::abs (c - s.pos) < minSpacing; }))
                continue;
            here.push_back (s.pos);
            ++found;
        }
    }

    std::vector<ChopSpan> spans;
    for (size_t i = 0; i < sounds.size(); ++i)
    {
        const auto more = leaveOutSilence (withRegionEnds (chosen[i], sounds[i].start, sounds[i].end), onsets, false);
        spans.insert (spans.end(), more.begin(), more.end());
    }
    return spans;
}

} // namespace kerf::ChopTools

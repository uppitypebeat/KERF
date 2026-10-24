#include "DiceInternal.h"

#include <algorithm>
#include <map>
#include <set>

namespace kerf::PatternDice::detail
{

namespace
{
    // What a pattern must be (named thresholds, so they can be tuned in one place).
    constexpr float maxChopShare = 0.6f;        // no chop more than this share of the notes (with 4+ chops to choose from)
    constexpr float minDensityOfMode = 0.3f;    // at least this much of the mode's density ...
    constexpr float maxDensityOfMode = 2.0f;    // ... and at most this much (onsets per step)
    constexpr float maxSimilarity = 0.45f;      // to any of the last rolls
    constexpr int maxIdenticalBarsInARow = 2;   // repetition is fine - then something must change

    juce::int64 barPrint (const std::vector<PatternEvent>& events, int bar)
    {
        juce::uint64 h = 1469598103u;
        for (const auto& e : events)
            if (e.step / PatternData::stepsPerBar == bar)
                for (const auto v : { e.step % PatternData::stepsPerBar, e.length, e.chopId, e.pitch, e.repeats })
                    h = h * 1000003u + (juce::uint64) (juce::int64) v;
        return (juce::int64) h;
    }
}

float similarity (const std::vector<PatternEvent>& a, const std::vector<PatternEvent>& b)
{
    // Shared (step, chop) pairs over all pairs (Jaccard).
    std::set<std::pair<int, int>> sa, sb;
    for (const auto& e : a) sa.insert ({ e.step, e.chopId });
    for (const auto& e : b) sb.insert ({ e.step, e.chopId });
    if (sa.empty() && sb.empty())
        return 1.0f;
    int shared = 0;
    for (const auto& p : sa)
        shared += sb.count (p) > 0 ? 1 : 0;
    return (float) shared / (float) (sa.size() + sb.size() - (size_t) shared);
}

Evaluation evaluate (const std::vector<PatternEvent>& events, const std::vector<ChopTraits>& chops, const Options& options, int modeIndex)
{
    Evaluation ev;
    auto fail = [&ev] (const char* name) { ++ev.failures; ev.failed.add (name); };
    const auto& mode = modes()[(size_t) juce::jlimit (0, (int) modes().size() - 1, modeIndex)];
    const auto bars = juce::jlimit (1, 8, options.bars);
    const auto steps = bars * PatternData::stepsPerBar;

    if (events.empty())
    {
        fail ("empty");
        return ev;
    }

    // 1. Not too dense, not too empty (for this mode).
    const auto density = (float) events.size() / (float) steps;
    if (density < mode.density * minDensityOfMode)
        fail ("too sparse");
    if (density > std::min (0.95f, mode.density * maxDensityOfMode))
        fail ("too dense");

    // 2. One chop everywhere? (Only when there is a choice.)
    std::map<int, int> count;
    for (const auto& e : events)
        ++count[e.chopId];
    int most = 0;
    for (const auto& [id, n] : count)
        most = std::max (most, n);
    const auto share = (float) most / (float) events.size();
    if (chops.size() >= 4 && events.size() >= 8 && share > maxChopShare)
        fail ("one chop overused");

    // 3. Repetition with change: not the same bar over and over, and the end not the start.
    std::vector<juce::int64> prints;
    for (int b = 0; b < bars; ++b)
        prints.push_back (barPrint (events, b));
    int run = 1, longest = 1;
    for (size_t b = 1; b < prints.size(); ++b)
    {
        run = prints[b] == prints[b - 1] ? run + 1 : 1;
        longest = std::max (longest, run);
    }
    if (bars >= 4 && longest > maxIdenticalBarsInARow + (mode.variation < 0.3f ? 1 : 0))
        fail ("too repetitive");
    if (bars >= 2 && prints.front() == prints.back())
        fail ("no development");

    // 4. A motif: some three chops in a row come back.
    if (events.size() >= 8)
    {
        std::map<std::tuple<int, int, int>, int> grams;
        bool motif = false;
        for (size_t i = 0; i + 2 < events.size() && ! motif; ++i)
            motif = ++grams[{ events[i].chopId, events[i + 1].chopId, events[i + 2].chopId }] >= 2;
        if (! motif)
            fail ("no motif");
    }

    // 5. A rhythm, not a metronome: more than one distance between notes.
    if (events.size() >= 6)
    {
        std::set<int> gaps;
        for (size_t i = 0; i + 1 < events.size(); ++i)
            gaps.insert (events[i + 1].step - events[i].step);
        if (gaps.size() < 2)
            fail ("uniform rhythm");
    }

    // 6. New: not (nearly) one of the last rolls.
    float closest = 0.0f;
    if (options.recent != nullptr)
        for (const auto& old : *options.recent)
            closest = std::max (closest, similarity (events, old));
    if (closest > maxSimilarity)
        fail ("like a recent roll");

    // For ties: varied bars, a few different chops, no chop dominating, new.
    std::set<juce::int64> distinct (prints.begin(), prints.end());
    ev.score = (float) distinct.size() / (float) bars + std::min (1.0f, (float) count.size() / 4.0f) - share - closest;
    return ev;
}

} // namespace kerf::PatternDice::detail

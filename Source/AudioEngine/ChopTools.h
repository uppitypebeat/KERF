#pragma once

#include <vector>
#include "ChopGrid.h"
#include "ChopTypes.h"
#include "../Analysis/TransientDetector.h"
#include "../Analysis/MusicalAnalysis.h"

namespace kerf
{

/** Automatic chop generators. Pure functions: easy to test.

    The cut generators return boundary positions (including region start and end). With the
    sample's analysis, every mode's cuts then become chops that leave the silence out
    (leaveOutSilence): no chop starts with silence, none ends in a long one, and where the
    sample is silent for a while there is no chop at all. */
namespace ChopTools
{
    /** Silence at least this long inside a chop is cut out of it (shorter pauses are part of the sound). */
    constexpr double minGapSeconds = 0.1;

    std::vector<int64_t> byTransients (const OnsetAnalysis& onsets, int64_t regionStart, int64_t regionEnd,
                                       float sensitivity, double minDistanceSeconds, int maxCount = 64);

    /** Chops on the tempo's beat grid (the beat lines in the waveform: from `origin`, the
        sample's start unless SHIFT moved it): one chop every `beatsPerChop` beats, cut exactly
        on the grid. */
    std::vector<int64_t> byBeats (double bpm, double sampleRate, int64_t regionStart, int64_t regionEnd, double beatsPerChop,
                                  double origin = 0.0);

    /** Contiguous chops between the cuts (nothing left out). */
    std::vector<ChopSpan> spansBetween (const std::vector<int64_t>& cuts);

    /** The cuts as chops, the silence left out:
        - a chop that starts in silence starts where the sound does,
        - one that ends in silence (30 ms or more) ends with its sound's tail,
        - all silent: no chop,
        - `splitAtSilence`: a silence of at least minGapSeconds inside a chop splits it (the
          sound after it is a chop of its own); else it stays (the count modes: fewer chops
          than sounds share them),
        - a scrap of sound under 25 ms, cut off from its sound by a cut and by silence, goes
          back to its sound (the tail of the chop before, the start of the next one) or,
          alone between silences, away. */
    std::vector<ChopSpan> leaveOutSilence (const std::vector<int64_t>& cuts, const OnsetAnalysis& analysis, bool splitAtSilence);

    /** Each cut between the first and the last moved onto the hit near it (within `window`
        frames, a clear hit: a strong one and a near one win), so the chop starts with its hit
        instead of a sliver before it or half its attack. The window follows the music when
        it drifts from the grid. */
    std::vector<int64_t> snapToHits (std::vector<int64_t> cuts, const OnsetAnalysis& analysis, int64_t window);

    /** The sound in [regionStart, regionEnd): the stretches between the silences of at least
        minGapSeconds (and without the silence at the region's ends). */
    std::vector<ChopSpan> soundsIn (const OnsetAnalysis& analysis, int64_t regionStart, int64_t regionEnd);

    /** How `count` chops go over the sounds: with at least one per sound, each sound gets
        chops by its length (no chop across a silence); with fewer chops than sounds, the
        sounds closest together share a chop. */
    struct Share { ChopSpan span; int chops = 1; };
    std::vector<Share> shareOut (const std::vector<ChopSpan>& sounds, int count);

    /** EQUAL: `count` equal chops. With the analysis: over the sound only (shareOut), each
        cut onto a hit within a hair of it. */
    std::vector<ChopSpan> equal (const OnsetAnalysis* analysis, int64_t regionStart, int64_t regionEnd, int count);

    /** RANDOM: `count` chops with random cuts, no chop shorter than a third of an equal one.
        With the analysis: over the sound only, and the cuts land on hits (strong ones more
        likely), random positions only where there are not enough hits. */
    std::vector<ChopSpan> random (const OnsetAnalysis* analysis, int64_t regionStart, int64_t regionEnd, int count, uint32_t seed);

    /** Interface for chop-suggestion engines. The built-in one is rule-based; a future ML
        model can implement the same interface without touching the rest of the code. */
    class ChopSuggester
    {
    public:
        virtual ~ChopSuggester() = default;
        virtual juce::String getName() const = 0;
        virtual std::vector<ChopSpan> suggest (const juce::AudioBuffer<float>& audio, double sampleRate,
                                               const OnsetAnalysis& onsets, const TempoEstimate& tempo,
                                               int64_t regionStart, int64_t regionEnd, int targetCount) = 0;
    };

    /** SMART: rule-based suggestions. Every sound after a silence starts a chop (with fewer
        chops than sounds, the sounds closest together share one). The other cuts are the
        best hits, scored by
        - transient strength,
        - alignment to the estimated beat grid, the bar's first beat most,
        - silence/low energy just before it (phrase boundaries),
        - energy contrast between before and after, in the lows most (kicks, bass notes),
        keeping a spacing that grows with the region length, so chops come out as musically
        sized phrases rather than every single hit. Offline, deterministic, no network. */
    class RuleBasedSuggester final : public ChopSuggester
    {
    public:
        juce::String getName() const override { return "Smart (rule-based)"; }
        std::vector<ChopSpan> suggest (const juce::AudioBuffer<float>& audio, double sampleRate,
                                       const OnsetAnalysis& onsets, const TempoEstimate& tempo,
                                       int64_t regionStart, int64_t regionEnd, int targetCount) override;
    };
}

} // namespace kerf

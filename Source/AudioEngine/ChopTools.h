#pragma once

#include <vector>
#include "ChopGrid.h"
#include "../Analysis/TransientDetector.h"
#include "../Analysis/MusicalAnalysis.h"

namespace kerf
{

/** Automatic chop generators. Each returns boundary positions (including region start and
    end) to pass to ChopModel::replaceWithBoundaries(). Pure functions: easy to test. */
namespace ChopTools
{
    std::vector<int64_t> byTransients (const OnsetAnalysis& onsets, int64_t regionStart, int64_t regionEnd,
                                       float sensitivity, double minDistanceSeconds, int maxChops = 64);

    /** Chops on the tempo's beat grid (the beat lines in the waveform: from `origin`, the
        sample's start unless SHIFT moved it): one chop every `beatsPerChop` beats, cut exactly
        on the grid. */
    std::vector<int64_t> byBeats (double bpm, double sampleRate, int64_t regionStart, int64_t regionEnd, double beatsPerChop,
                                  double origin = 0.0);

    /** `count` chops with random boundaries.
        - minLengthFraction: no chop shorter than this fraction of an equal slice (0..1)
        - snapToOnsets: move each boundary to the nearest onset within 40 ms if one exists */
    std::vector<int64_t> random (int64_t regionStart, int64_t regionEnd, int count, uint32_t seed,
                                 float minLengthFraction, const OnsetAnalysis* snapToOnsets, double sampleRate);

    /** Interface for chop-suggestion engines. The built-in one is rule-based; a future ML
        model can implement the same interface without touching the rest of the code. */
    class ChopSuggester
    {
    public:
        virtual ~ChopSuggester() = default;
        virtual juce::String getName() const = 0;
        virtual std::vector<int64_t> suggest (const juce::AudioBuffer<float>& audio, double sampleRate,
                                              const OnsetAnalysis& onsets, const TempoEstimate& tempo,
                                              int64_t regionStart, int64_t regionEnd, int targetCount) = 0;
    };

    /** SMART: rule-based suggestions. Scores each onset candidate by
        - transient strength,
        - alignment to the estimated beat grid,
        - silence/low energy just before it (phrase boundaries),
        - energy contrast between before and after,
        then keeps the best `targetCount` with a spacing that grows with the region length,
        so chops come out as musically sized phrases rather than every single hit. Offline,
        deterministic, no network. */
    class RuleBasedSuggester final : public ChopSuggester
    {
    public:
        juce::String getName() const override { return "Smart (rule-based)"; }
        std::vector<int64_t> suggest (const juce::AudioBuffer<float>& audio, double sampleRate,
                                      const OnsetAnalysis& onsets, const TempoEstimate& tempo,
                                      int64_t regionStart, int64_t regionEnd, int targetCount) override;
    };
}

} // namespace kerf

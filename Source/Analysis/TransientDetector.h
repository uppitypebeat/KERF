#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <functional>
#include <vector>

namespace kerf
{

struct Onset
{
    int64_t position = 0; // frame where the attack starts
    float strength = 0.0f; // 0..1 relative to the strongest onset in the sample
};

/** A silent stretch of the sample, [start, end) in frames. */
struct Silence
{
    int64_t start = 0, end = 0;
    int64_t length() const noexcept { return end - start; }
};

/** Onset detection function + candidate onsets for a whole sample.
    Computed once on a background thread. Changing sensitivity in the UI only re-filters
    the candidates (selectOnsets), which is instant. */
struct OnsetAnalysis
{
    std::vector<Onset> onsets;      // all candidates, sorted by position
    std::vector<float> odf;         // onset detection function (one value per hop)
    std::vector<Silence> silences;  // where the sample is silent (findSilences), sorted
    int hopSize = 512;
    double sampleRate = 44100.0;

    double odfFrameRate() const noexcept { return sampleRate / hopSize; }
};

namespace TransientDetector
{
    /** Spectral-flux onset detection with adaptive threshold and attack refinement.
        `shouldExit` lets a background job be cancelled. */
    OnsetAnalysis analyse (const juce::AudioBuffer<float>& audio, double sampleRate,
                           const std::function<bool()>& shouldExit = {});

    /** Picks onsets for chopping.
        sensitivity 0..1: higher finds more (weaker) transients.
        minDistanceSeconds: closer onsets are dropped (the stronger one wins).
        Region limits the search to [regionStart, regionEnd). */
    std::vector<int64_t> selectOnsets (const OnsetAnalysis& analysis, float sensitivity,
                                       double minDistanceSeconds, int maxCount,
                                       int64_t regionStart, int64_t regionEnd);

    /** Where the sample is silent: at least 10 ms far below its loud parts (50 dB, or just
        above its noise floor when it has one), so quiet that nothing is heard there. The
        sounds' edges stay out of it: their attack starts 1.5 ms after a silence, their tail
        fades on for 10 ms into it. A pop shorter than 12 ms in silence is silence. */
    std::vector<Silence> findSilences (const juce::AudioBuffer<float>& audio, double sampleRate);

    /** Mono mixdown helper. */
    std::vector<float> mixToMono (const juce::AudioBuffer<float>& audio);
}

} // namespace kerf

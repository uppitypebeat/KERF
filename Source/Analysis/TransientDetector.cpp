#include "TransientDetector.h"

#include <juce_dsp/juce_dsp.h>
#include <algorithm>
#include <numeric>

namespace kerf::TransientDetector
{

std::vector<float> mixToMono (const juce::AudioBuffer<float>& audio)
{
    const auto n = audio.getNumSamples();
    const auto chans = audio.getNumChannels();
    std::vector<float> mono ((size_t) n, 0.0f);

    for (int ch = 0; ch < chans; ++ch)
    {
        const auto* d = audio.getReadPointer (ch);
        for (int i = 0; i < n; ++i)
            mono[(size_t) i] += d[i] / (float) chans;
    }
    return mono;
}

namespace
{
    //==========================================================================
    // Onsets for chopping: SuperFlux (Boeck & Widmer, DAFx 2013). A log-magnitude spectrum on
    // a log-spaced filterbank (lows count as much as highs), compared with a maximum-filtered
    // spectrum a little earlier (vibrato and slides do not count as new notes), positive
    // differences summed. Peaks above a local average are onsets.

    constexpr double framesPerSecond = 200.0;
    constexpr int bandsPerOctave = 24;
    constexpr int lag = 1;                       // compare with the frame 5 ms earlier
    constexpr float localFloor = 0.2f;           // a neighbourhood counts if its hits reach 20 % of the loudest
    constexpr float localWeight = 0.75f;

    struct Band { int first, last; std::vector<float> weights; float tilt = 1.0f; }; // tilt: the band's weight in the sum

    /** Triangular filters, `bandsPerOctave` per octave from 30 Hz up; bands that would fall on
        the same FFT bin are merged (the lowest octaves have few bins). */
    std::vector<Band> makeFilterbank (int fftSize, double sampleRate)
    {
        const auto binHz = sampleRate / fftSize;
        const auto top = std::min (16000.0, sampleRate * 0.45);
        std::vector<int> centres;
        for (auto f = 30.0; f < top; f *= std::pow (2.0, 1.0 / bandsPerOctave))
        {
            const auto bin = (int) std::round (f / binHz);
            if (centres.empty() || bin > centres.back())
                centres.push_back (bin);
        }

        std::vector<Band> bands;
        for (size_t i = 1; i + 1 < centres.size(); ++i)
        {
            Band b;
            b.first = centres[i - 1];
            b.last = centres[i + 1];
            for (int k = b.first; k <= b.last; ++k)
            {
                const auto w = k <= centres[i] ? (float) (k - b.first + 1) / (float) (centres[i] - b.first + 1)
                                               : (float) (b.last - k + 1) / (float) (b.last - centres[i] + 1);
                b.weights.push_back (w);
            }
            const auto sum = std::accumulate (b.weights.begin(), b.weights.end(), 0.0f);
            for (auto& w : b.weights)
                w /= sum;
            b.tilt = (float) (centres[i] * binHz / 1000.0); // per Hz, like linear bins: hats over a snare tail still count
            bands.push_back (std::move (b));
        }
        return bands;
    }

    /** Where the attack begins, near a rough onset: on a short energy envelope of the
        pre-emphasised signal (attacks are bright), the steepest rise in the window, then back
        to where the level was still near the floor before it, then to a zero crossing just
        before (a chop starting there does not click). */
    int64_t refineAttack (const std::vector<float>& mono, int64_t rough, double sampleRate)
    {
        const auto n = (int64_t) mono.size();
        const auto step = std::max<int64_t> (8, (int64_t) (sampleRate * 0.0005));   // 0.5 ms
        const auto width = std::max<int64_t> (16, (int64_t) (sampleRate * 0.002));  // 2 ms
        const auto lo = std::max<int64_t> (1, rough - (int64_t) (sampleRate * 0.03));
        const auto hi = std::min<int64_t> (n - width - 1, rough + (int64_t) (sampleRate * 0.03));
        if (hi <= lo + 2 * step)
            return juce::jlimit<int64_t> (0, n - 1, rough);

        std::vector<float> env;
        for (auto p = lo; p < hi; p += step)
        {
            double e = 0.0;
            for (auto i = p; i < p + width; ++i)
            {
                const auto bright = mono[(size_t) i] - 0.9f * mono[(size_t) i - 1];
                e += (double) bright * bright;
            }
            env.push_back ((float) (10.0 * std::log10 (e / (double) width + 1.0e-12)));
        }

        // Steepest rise over ~2 ms, nearer the rough position winning a close call.
        const auto span = (int) (width / step);
        const auto roughIndex = (double) (rough - lo) / (double) step;
        int best = -1;
        float bestScore = -1.0e9f;
        for (int i = 0; i + span < (int) env.size(); ++i)
        {
            const auto rise = env[(size_t) (i + span)] - env[(size_t) i];
            const auto score = rise - 0.05f * (float) std::abs ((double) i - roughIndex);
            if (score > bestScore) { bestScore = score; best = i; }
        }
        if (best < 0)
            return juce::jlimit<int64_t> (0, n - 1, rough);

        // Back to the start: the last moment before the rise that was still near the floor
        // of the 12 ms before it (the floor, not a ripple: a bass wave is longer than the
        // envelope window).
        const auto back = std::max (1, (int) (0.012 * sampleRate / (double) step));
        const auto from = std::max (0, best - back);
        const auto floorLevel = *std::min_element (env.begin() + from, env.begin() + best + span);
        auto i = best;
        while (i > from && env[(size_t) i] > floorLevel + 1.5f)
            --i;
        auto pos = lo + (int64_t) i * step + width / 2;

        // Nearest zero crossing at or before pos (within 2 ms).
        const auto zcLimit = std::max<int64_t> (1, pos - (int64_t) (sampleRate * 0.002));
        for (auto k = std::min (pos, n - 1); k > zcLimit; --k)
            if ((mono[(size_t) k - 1] <= 0.0f && mono[(size_t) k] > 0.0f) || (mono[(size_t) k - 1] >= 0.0f && mono[(size_t) k] < 0.0f))
                return k;
        return pos;
    }

    std::vector<Onset> detectOnsets (const std::vector<float>& mono, double sampleRate, const std::function<bool()>& shouldExit, bool& cancelled)
    {
        const auto n = (int64_t) mono.size();
        const int fftOrder = sampleRate > 64000.0 ? 11 : 10;  // ~23 ms
        const int fftSize = 1 << fftOrder;
        const auto hop = std::max (1, (int) std::round (sampleRate / framesPerSecond));
        const auto numFrames = (int) (n / hop) + 1;
        if (n < fftSize / 4 || numFrames < 4)
            return {};

        const auto bands = makeFilterbank (fftSize, sampleRate);
        const auto numBands = bands.size();
        juce::dsp::FFT fft (fftOrder);
        juce::dsp::WindowingFunction<float> window ((size_t) fftSize, juce::dsp::WindowingFunction<float>::hann, false);
        std::vector<float> frame ((size_t) fftSize * 2);
        std::vector<std::vector<float>> spec ((size_t) numFrames, std::vector<float> (numBands, 0.0f));

        // Frames centred on f * hop (the signal padded with silence at both ends).
        for (int f = 0; f < numFrames; ++f)
        {
            if (shouldExit && (f & 255) == 0 && shouldExit()) { cancelled = true; return {}; }
            std::fill (frame.begin(), frame.end(), 0.0f);
            const auto start = (int64_t) f * hop - fftSize / 2;
            for (int i = 0; i < fftSize; ++i)
                if (const auto s = start + i; s >= 0 && s < n)
                    frame[(size_t) i] = mono[(size_t) s];
            window.multiplyWithWindowingTable (frame.data(), (size_t) fftSize);
            fft.performFrequencyOnlyForwardTransform (frame.data(), true);

            auto& row = spec[(size_t) f];
            for (size_t b = 0; b < numBands; ++b)
            {
                float v = 0.0f;
                for (int k = bands[b].first; k <= bands[b].last; ++k)
                    v += bands[b].weights[(size_t) (k - bands[b].first)] * frame[(size_t) k];
                row[b] = std::log10 (1.0f + 100.0f * v); // strong compression: quiet hits count too
            }
        }

        // SuperFlux detection function.
        std::vector<float> odf ((size_t) numFrames, 0.0f);
        for (int f = lag; f < numFrames; ++f)
        {
            const auto& now = spec[(size_t) f];
            const auto& before = spec[(size_t) (f - lag)];
            float sum = 0.0f;
            for (size_t b = 0; b < numBands; ++b)
            {
                const auto reference = std::max ({ before[b], b > 0 ? before[b - 1] : 0.0f, b + 1 < numBands ? before[b + 1] : 0.0f });
                sum += bands[b].tilt * std::max (0.0f, now[b] - reference);
            }
            odf[(size_t) f] = sum;
        }

        // Peaks: the maximum within +-30 ms, above the median of the 100 ms before and 70 ms
        // after (the floor between hits: a quiet 16th next to loud ones still stands out);
        // the excess over it is the onset's strength.
        const int preMax = 6, postMax = 6, preAvg = 20, postAvg = 14;
        std::vector<Onset> found;
        std::vector<float> scratch;
        for (int f = lag; f < numFrames; ++f)
        {
            const auto v = odf[(size_t) f];
            if (v <= 0.0f)
                continue;
            bool isPeak = true;
            for (int k = std::max (0, f - preMax); k <= std::min (numFrames - 1, f + postMax) && isPeak; ++k)
                if (odf[(size_t) k] > v || (odf[(size_t) k] == v && k < f))
                    isPeak = false;
            if (! isPeak)
                continue;

            scratch.clear();
            for (int k = std::max (0, f - preAvg); k <= std::min (numFrames - 1, f + postAvg); ++k)
                scratch.push_back (odf[(size_t) k]);
            std::nth_element (scratch.begin(), scratch.begin() + (ptrdiff_t) scratch.size() / 2, scratch.end());
            const auto excess = v - scratch[scratch.size() / 2];
            if (excess > 0.0f)
            {
                // The compressed difference peaks somewhere while the attack crosses the window:
                // the attack itself is searched for around the frame centre.
                const auto rough = (int64_t) f * hop;
                found.push_back ({ refineAttack (mono, std::max<int64_t> (0, rough), sampleRate), excess });
            }
        }

        // Strength 0..1: against the strongest hits overall (half the loudest, or the 95th
        // percentile if higher: one big crash does not make every other hit look weak), or
        // against the strongest hits within 1.5 s (a quiet passage keeps its hits) when
        // that neighbourhood has real hits in it.
        std::sort (found.begin(), found.end(), [] (const Onset& a, const Onset& b) { return a.position < b.position; });
        if (! found.empty())
        {
            std::vector<float> s;
            for (const auto& o : found) s.push_back (o.strength);
            std::sort (s.begin(), s.end());
            const auto loudest = s.back();
            const auto reference = std::max ({ 1.0e-6f, 0.5f * loudest, s[(size_t) ((double) (s.size() - 1) * 0.95)] });
            const auto radius = (int64_t) (1.5 * sampleRate);
            std::vector<float> strength (found.size());
            for (size_t i = 0; i < found.size(); ++i)
            {
                float local = 0.0f;
                for (size_t k = 0; k < found.size(); ++k)
                    if (std::abs (found[k].position - found[i].position) <= radius)
                        local = std::max (local, found[k].strength);
                const auto global = found[i].strength / reference;
                const auto relative = local >= localFloor * loudest ? found[i].strength / local : 0.0f;
                strength[i] = std::min (1.0f, std::max (global, localWeight * relative));
            }
            for (size_t i = 0; i < found.size(); ++i)
                found[i].strength = strength[i];
        }
        return found;
    }
}

OnsetAnalysis analyse (const juce::AudioBuffer<float>& audio, double sampleRate, const std::function<bool()>& shouldExit)
{
    OnsetAnalysis result;
    result.sampleRate = sampleRate;

    const auto mono = mixToMono (audio);
    const auto n = (int64_t) mono.size();

    // The tempo detector's function: linear-bin spectral flux, ~21 ms window at 48 kHz
    // (scales with the sample rate). Tempo detection is tuned on it; chopping uses its own
    // detector below.
    const int fftOrder = sampleRate > 64000.0 ? 11 : 10;
    const int fftSize = 1 << fftOrder;
    const int hop = fftSize / 4;
    result.hopSize = hop;

    if (n < fftSize)
        return result;

    juce::dsp::FFT fft (fftOrder);
    juce::dsp::WindowingFunction<float> window ((size_t) fftSize, juce::dsp::WindowingFunction<float>::hann, false);

    std::vector<float> frame ((size_t) fftSize * 2);
    std::vector<float> prevMag ((size_t) fftSize / 2 + 1, 0.0f);

    const auto numFrames = (int) ((n - fftSize) / hop) + 1;
    result.odf.assign ((size_t) numFrames, 0.0f);

    for (int f = 0; f < numFrames; ++f)
    {
        if (shouldExit && (f & 255) == 0 && shouldExit())
            return {};

        std::fill (frame.begin(), frame.end(), 0.0f);
        std::copy_n (mono.begin() + (ptrdiff_t) f * hop, fftSize, frame.begin());
        window.multiplyWithWindowingTable (frame.data(), (size_t) fftSize);
        fft.performFrequencyOnlyForwardTransform (frame.data(), true);

        float flux = 0.0f;
        for (int k = 1; k <= fftSize / 2; ++k)
        {
            const auto mag = std::log1p (100.0f * frame[(size_t) k]); // log compression
            flux += std::max (0.0f, mag - prevMag[(size_t) k]);       // half-wave rectified
            prevMag[(size_t) k] = mag;
        }
        result.odf[(size_t) f] = flux;
    }

    // Normalise.
    const auto maxOdf = *std::max_element (result.odf.begin(), result.odf.end());
    if (maxOdf <= 0.0f)
        return result;
    for (auto& v : result.odf)
        v /= maxOdf;

    bool cancelled = false;
    result.onsets = detectOnsets (mono, sampleRate, shouldExit, cancelled);
    if (cancelled)
        return {};
    return result;
}

std::vector<int64_t> selectOnsets (const OnsetAnalysis& analysis, float sensitivity, double minDistanceSeconds,
                                   int maxCount, int64_t regionStart, int64_t regionEnd)
{
    // sensitivity 0 -> only the strongest hits (strength 0.6); 1 -> nearly everything (0.01),
    // geometric in between (50 % = 0.077, the best setting on the test corpus).
    const auto threshold = 0.6f * std::pow (0.01f / 0.6f, juce::jlimit (0.0f, 1.0f, sensitivity));
    const auto minDistance = (int64_t) (std::max (0.0, minDistanceSeconds) * analysis.sampleRate);

    std::vector<Onset> picked;
    for (const auto& o : analysis.onsets)
        if (o.position >= regionStart && o.position < regionEnd && o.strength >= threshold)
            picked.push_back (o);

    // Enforce minimum distance: strongest first.
    std::sort (picked.begin(), picked.end(), [] (const Onset& a, const Onset& b) { return a.strength > b.strength; });

    std::vector<Onset> kept;
    for (const auto& o : picked)
    {
        if ((int) kept.size() >= maxCount)
            break;

        const bool tooClose = std::any_of (kept.begin(), kept.end(), [&] (const Onset& k)
                                           { return std::abs (k.position - o.position) < minDistance; });
        if (! tooClose)
            kept.push_back (o);
    }

    std::vector<int64_t> positions;
    for (const auto& k : kept)
        positions.push_back (k.position);
    std::sort (positions.begin(), positions.end());
    return positions;
}

} // namespace kerf::TransientDetector

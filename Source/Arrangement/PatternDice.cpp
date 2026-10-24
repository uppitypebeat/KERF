#include "PatternDice.h"
#include "DiceInternal.h"

#include <algorithm>
#include <cmath>
#include <array>
#include <iterator>
#include <map>
#include <numeric>

namespace kerf::PatternDice
{

namespace
{
    // Its pitch: a normalised autocorrelation (NSDF, after McLeod) of a stretch after the
    // attack, at about 12 kHz. Only a clear periodicity counts (noise, clicks: none).
    float hearPitch (const float* left, const float* right, int64_t from, int64_t available, double rate)
    {
        const auto decimate = std::max (1, (int) std::round (rate / 12000.0));
        const auto slowRate = rate / decimate;
        const auto n = (int) std::min<int64_t> (1024, available / decimate);
        if (n < 256)
            return 0.0f;

        std::vector<float> y ((size_t) n);
        for (int i = 0; i < n; ++i)
        {
            float sum = 0.0f;
            for (int j = 0; j < decimate; ++j)
            {
                const auto at = from + (int64_t) i * decimate + j;
                sum += left[at] + right[at];
            }
            y[(size_t) i] = sum / (float) (2 * decimate);
        }
        const auto mean = std::accumulate (y.begin(), y.end(), 0.0f) / (float) n;
        for (auto& v : y)
            v -= mean;

        const auto minLag = std::max (2, (int) (slowRate / 1000.0));
        const auto maxLag = std::min (n / 2, (int) (slowRate / 40.0));
        if (maxLag <= minLag + 2)
            return 0.0f;
        std::vector<float> nsdf ((size_t) maxLag + 2, 0.0f);
        float top = 0.0f;
        for (int lag = minLag - 1; lag <= maxLag + 1; ++lag)
        {
            double acf = 0.0, m = 0.0;
            for (int i = 0; i + lag < n; ++i)
            {
                acf += (double) y[(size_t) i] * y[(size_t) (i + lag)];
                m += (double) y[(size_t) i] * y[(size_t) i] + (double) y[(size_t) (i + lag)] * y[(size_t) (i + lag)];
            }
            nsdf[(size_t) lag] = m > 1.0e-12 ? (float) (2.0 * acf / m) : 0.0f;
            if (lag >= minLag && lag <= maxLag)
                top = std::max (top, nsdf[(size_t) lag]);
        }
        if (top < 0.6f)
            return 0.0f;

        // The first peak nearly as high as the highest (not a multiple of the period).
        for (int lag = minLag; lag <= maxLag; ++lag)
        {
            const auto a = nsdf[(size_t) lag - 1], b = nsdf[(size_t) lag], c = nsdf[(size_t) lag + 1];
            if (b >= 0.9f * top && b >= a && b >= c)
            {
                const auto curve = a - 2.0f * b + c;
                const auto shift = std::abs (curve) > 1.0e-9f ? 0.5f * (a - c) / curve : 0.0f;
                return (float) (slowRate / ((float) lag + juce::jlimit (-0.5f, 0.5f, shift)));
            }
        }
        return 0.0f;
    }
}

ChopTraits describe (const SampleData& sample, const ChopParams& chop)
{
    ChopTraits t;
    t.id = chop.id;
    t.seconds = (double) chop.length() / std::max (1.0, sample.sampleRate);
    t.start = (double) chop.start / std::max (1.0, sample.sampleRate);

    const auto rate = std::max (1.0, sample.sampleRate);
    const auto start = juce::jlimit<int64_t> (0, std::max<int64_t> (0, sample.getNumSamples() - 1), chop.start);
    const auto frames = (int) std::min<int64_t> (chop.length(), std::min<int64_t> ((int64_t) (0.3 * rate), sample.getNumSamples() - start));
    if (frames < 16)
        return t;

    const auto* left = sample.audio.getReadPointer (0);
    const auto* right = sample.audio.getReadPointer (sample.getNumChannels() > 1 ? 1 : 0);

    // Energy: all of it, below ~150 Hz (a one-pole low-pass), and of the change from sample
    // to sample (high frequencies). Loudness in short windows for the attack and the tail.
    const auto lowCoeff = (float) std::exp (-juce::MathConstants<double>::twoPi * 150.0 / rate);
    const auto window = std::max (1, (int) (0.01 * rate));
    double total = 0.0, low = 0.0, high = 0.0;
    float lowState = 0.0f, previous = 0.0f;
    std::vector<double> windows ((size_t) (frames / window + 1), 0.0);
    for (int i = 0; i < frames; ++i)
    {
        const auto x = 0.5f * (left[start + i] + right[start + i]);
        lowState = lowCoeff * lowState + (1.0f - lowCoeff) * x;
        const auto d = x - previous;
        previous = x;
        total += (double) x * x;
        low += (double) lowState * lowState;
        high += (double) d * d;
        windows[(size_t) (i / window)] += (double) x * x;
    }
    if (total < 1.0e-9)
        return t;

    t.low = (float) juce::jlimit (0.0, 1.0, low / total * 1.6);
    // The change from sample to sample against the level: about 2 for plain noise (a snare's
    // rattle), 3 or more for noise with the lows gone (a hi-hat), near 0 for low tones.
    t.bright = (float) juce::jlimit (0.0, 1.0, high / total / 3.5);
    t.level = (float) juce::jlimit (0.0, 1.0, std::sqrt (total / frames) * 3.0);

    // Punch: the loudest of the first 30 ms against the average after 60 ms.
    const auto first = std::min<size_t> (3, windows.size());
    const auto attack = *std::max_element (windows.begin(), windows.begin() + (std::ptrdiff_t) first);
    double tail = 0.0;
    int tailCount = 0;
    for (size_t w = 6; w < windows.size(); ++w) { tail += windows[w]; ++tailCount; }
    tail = tailCount > 0 ? tail / tailCount : attack;
    t.punch = (float) juce::jlimit (0.0, 1.0, (attack / std::max (1.0e-12, tail) - 1.0) / 8.0);
    t.sustain = (float) juce::jlimit (0.0, 1.0, std::sqrt (tail / std::max (1.0e-12, attack)) * std::min (1.0, t.seconds / 0.4));

    // Its note, from 30 ms in (past the click of the attack).
    const auto skip = (int64_t) (0.03 * rate);
    const auto available = std::min<int64_t> (chop.length(), sample.getNumSamples() - start) - skip;
    if (available > 0)
        t.f0 = hearPitch (left, right, start + skip, available, rate);
    if (t.f0 > 0.0f)
        t.pitchClass = (((int) std::lround (12.0 * std::log2 (t.f0 / 440.0) + 69.0)) % 12 + 12) % 12;
    return t;
}


Roles assignRoles (const std::vector<ChopTraits>& chops)
{
    using namespace detail;
    Roles r;
    if (chops.empty())
        return r;

    auto contains = [] (const std::vector<int>& ids, int id) { return std::find (ids.begin(), ids.end(), id) != ids.end(); };
    auto best = [&] (auto&& score, const std::vector<int>& taken)
    {
        int id = 0;
        float top = -1.0e9f;
        for (const auto& c : chops)
            if (! contains (taken, c.id))
                if (const auto s = score (c); s > top) { top = s; id = c.id; }
        return id != 0 ? id : chops.front().id;
    };
    auto traits = [&chops] (int id) { return *std::find_if (chops.begin(), chops.end(), [id] (const ChopTraits& c) { return c.id == id; }); };

    r.kick = best (kickScore, {});
    r.snare = best (snareScore, { r.kick });
    r.hat = best (hatScore, { r.kick, r.snare });
    r.perc = best ([] (const ChopTraits& c) { return c.punch - c.sustain; }, { r.kick, r.snare, r.hat });

    // Seconds for variety: only a chop nearly as good as the first (a ghost kick, a clap
    // next to the snare, a second hat), and an open hat: bright and ringing longer.
    auto second = [&] (auto&& score, int firstId, float margin, auto&& fits, const std::vector<int>& taken)
    {
        if (chops.size() < 5)
            return 0;
        const auto id = best (score, taken);
        const auto c = traits (id);
        return ! contains (taken, id) && fits (c) && score (c) >= score (traits (firstId)) - margin ? id : 0;
    };
    r.kick2 = second (kickScore, r.kick, 0.35f, [] (const ChopTraits& c) { return c.low > 0.25f; }, { r.kick, r.snare, r.hat });
    r.snare2 = second (snareScore, r.snare, 0.3f, [] (const ChopTraits& c) { return c.punch > 0.2f; }, { r.kick, r.snare, r.hat, r.kick2 });
    r.hat2 = second (hatScore, r.hat, 0.3f, [] (const ChopTraits& c) { return c.bright > 0.35f; }, { r.kick, r.snare, r.hat, r.kick2, r.snare2 });
    {
        const auto hat = traits (r.hat);
        float top = hat.sustain + 0.1f;
        for (const auto& c : chops)
            if (c.id != r.hat && c.id != r.kick && c.id != r.snare && c.bright > 0.35f && c.low < 0.3f && c.sustain > top)
            {
                top = c.sustain;
                r.openHat = c.id;
            }
    }

    // Tonal chops for melodies: long and sustaining first.
    std::vector<ChopTraits> sorted (chops);
    std::sort (sorted.begin(), sorted.end(), [] (const ChopTraits& a, const ChopTraits& b) { return tonalScore (a) > tonalScore (b); });
    for (size_t i = 0; i < sorted.size() && r.melodic.size() < 3; ++i)
        r.melodic.push_back (sorted[i].id);
    return r;
}

std::vector<ChopTraits> TraitCache::get (const SampleData& sample, const std::vector<ChopParams>& chops)
{
    // Each chop is listened to once; a new sample (or a chop moved) is measured anew.
    if (&sample != sampleSeen)
    {
        known.clear();
        sampleSeen = &sample;
    }
    std::vector<ChopTraits> out;
    for (const auto& c : chops)
    {
        const auto key = std::make_tuple (c.id, c.start, c.end);
        auto it = known.find (key);
        if (it == known.end())
            it = known.emplace (key, describe (sample, c)).first;
        out.push_back (it->second);
    }
    return out;
}

juce::StringArray styleNames()
{
    juce::StringArray names;
    for (const auto& s : detail::styles())
        names.add (s.name);
    return names;
}

juce::StringArray modeNames()
{
    juce::StringArray names;
    for (const auto& m : detail::modes())
        names.add (m.name);
    return names;
}

juce::int64 fingerprint (const std::vector<PatternEvent>& events)
{
    // What you see in the grid (velocities aside): two rolls with the same print look alike.
    juce::uint64 h = (juce::uint64) events.size();
    for (const auto& e : events)
        for (const auto v : { e.step, e.length, e.chopId, e.pitch, e.repeats, e.random, (int) std::lround (e.chance * 20.0f), e.slide ? 1 : 0 })
            h = h * 1000003u + (juce::uint64) (juce::int64) v;
    return (juce::int64) h;
}

Result roll (const std::vector<ChopTraits>& chops, const Options& options)
{
    // Make a candidate, check it; a failed one is made again from the next seed. The best of
    // a few is kept (a bounded number: pressing the die must feel instant).
    constexpr int maxAttempts = 12;
    constexpr juce::int64 seedStep = 0x9e3779b97f4a7c15LL; // the next try: a seed far from this one
    Result result;
    if (chops.empty() || options.bars < 1)
        return result;

    const auto modeIndex = juce::jlimit (0, (int) detail::modes().size() - 1, options.mode);
    detail::Evaluation best;
    best.failures = 1 << 20;
    for (int attempt = 0; attempt < maxAttempts; ++attempt)
    {
        detail::GenerationContext context (options.seed + seedStep * attempt);
        auto events = detail::generate (chops, options, context);
        const auto ev = detail::evaluate (events, chops, options, modeIndex);
        if (ev.failures < best.failures || (ev.failures == best.failures && ev.score > best.score))
        {
            best = ev;
            result.events = std::move (events);
            result.style = context.style;
            result.name = context.backbone || context.style == detail::chopUp ? juce::String (detail::styles()[(size_t) context.style].name) : juce::String ("MOTIFS");
            result.seed = context.seed;
            result.inKey = context.inKey;
            result.attempts = attempt + 1;

            // What this roll is, in a few words: the form, the curve, the tools, the ending.
            static const char* toolNames[] = { "pitch", "ratchets", "ghost notes", "held notes", "slides", "scattered chops" };
            juce::StringArray words;
            if (context.form.length() > 1) // (one part: no form to speak of)
                words.add (context.form);
            if (context.densityShape.isNotEmpty())
                words.add (context.densityShape);
            juce::StringArray tools;
            for (int t = 0; t < detail::numTools; ++t)
                if (context.tools[(size_t) t])
                    tools.add (toolNames[t]);
            if (! tools.isEmpty())
                words.add (tools.joinIntoString (" + "));
            if (context.ending.isNotEmpty())
                words.add (context.ending);
            result.idea = words.joinIntoString (", ");
        }
        if (ev.failures == 0)
            break;
    }
    return result;
}


std::vector<PatternEvent> withLocks (const std::vector<PatternEvent>& rolled, const std::vector<PatternEvent>& kept,
                                     const std::vector<int>& lockedSteps)
{
    // Every step taken by a lock: the locked steps, and the steps their notes hold on to.
    std::vector<int> taken (lockedSteps.begin(), lockedSteps.end());
    for (const auto& e : kept)
        for (int k = 0; k < std::max (1, e.length); ++k)
            taken.push_back (e.step + k);
    std::sort (taken.begin(), taken.end());
    auto isTaken = [&taken] (int s) { return std::binary_search (taken.begin(), taken.end(), s); };

    std::vector<PatternEvent> out (kept.begin(), kept.end());
    for (auto e : rolled)
    {
        if (isTaken (e.step))
            continue;
        for (int k = 1; k < e.length; ++k)
            if (isTaken (e.step + k)) { e.length = k; break; }
        out.push_back (e);
    }
    std::sort (out.begin(), out.end(), [] (const PatternEvent& a, const PatternEvent& b) { return a.step < b.step; });
    return out;
}
} // namespace kerf::PatternDice

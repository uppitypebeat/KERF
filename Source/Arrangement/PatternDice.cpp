#include "PatternDice.h"

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


namespace
{
    float kickScore (const ChopTraits& c)  { return 1.6f * c.low + 0.6f * c.punch - 1.2f * c.bright - 0.3f * (float) std::min (1.0, c.seconds); }
    // A snare is bright, but not as bright as a hat: its score peaks half way up.
    float snareScore (const ChopTraits& c) { return c.punch + 3.0f * c.bright * (1.0f - c.bright) - 0.8f * c.low; }
    float hatScore (const ChopTraits& c)   { return 1.5f * c.bright - c.low - 0.6f * c.sustain - 0.4f * (float) std::min (1.0, c.seconds); }
    float bassScore (const ChopTraits& c)  { return 1.2f * c.low + c.sustain - c.bright; }
    float tonalScore (const ChopTraits& c) { return c.sustain + 0.3f * (float) std::min (2.0, c.seconds) - 0.4f * c.punch; }

    bool contains (const std::vector<int>& ids, int id) { return std::find (ids.begin(), ids.end(), id) != ids.end(); }
}

Roles assignRoles (const std::vector<ChopTraits>& chops)
{
    Roles r;
    if (chops.empty())
        return r;

    auto best = [&chops] (auto&& score, const std::vector<int>& taken)
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

namespace
{
    // One bar of a style, 16 steps:
    //   K kick   k ghost kick   S snare   s ghost snare   r snare roll
    //   H hat    h soft hat     R hat roll   T hat triplets (3 hits; "T-": over an 8th)
    //   O open hat (held)   P percussion   L log drum / bass (tuned, in the key)
    //   X any chop (a neighbour each pass)   M a note of the tonal chops
    //   C the hook (put in by the die: a chop that is no drum, on a few offbeats)
    //   - the note before held on (merged)   . rest
    struct Style
    {
        const char* name;
        std::vector<const char*> bars;  // ideas for a bar (the first is A, another can be B)
        std::vector<const char*> fills; // phrase endings (the last four steps)
        bool eightOhEight = false;      // kicks are tuned and slide (trap, drill)
        int rollRepeats = 2;            // hits in a roll step
        const char* fillAlphabet = "rRKS"; // made-up fills take these
    };

    enum StyleIndex { boomBap, trap, drill, house, drumAndBass, breakStutter, jerseyClub, amapiano, flip, chopUp, numStyles };

    const std::vector<Style>& styles()
    {
        static const std::vector<Style> all {
            { "BOOM BAP",  { "K.h.S..hk.K.S.h.", "K..hS.hkK.k.S..h", "K.hkS.h.h.K.S.hs", "K.h.S.hkh.K.S..k", "K.O-S.hkK.K.S.O-", "K..kS..hK.K.S.hh", "K.h.S.k.h.KkS..h" },
                           { "SRSR", "k.SS", "s.rr", "KSKS", "S.Ss" }, false, 2, "rSKsk" },
            { "TRAP",      { "K.H.HKRHS.H.KHR.", "K-.HH.RKS.HHK.RH", "K.HRH.KHS.RHK.HH", "K--.H.RHS.HKH.RH", "K.T-H.KHS.T-K.HR", "K.HHT-K.S.HHT-KH" },
                           { "RRRR", "rrRR", "S.rr", "RHRR", "T-T-", "RRT-" }, true, 4, "RrTK" },
            { "DRILL",     { "K..H.HK.S.HK.HH.", "K.H..HKKS.H..HRK", "K..HH.K.S..HK.RH", "K.HH..K.S.H.KH.R", "K.T-.HK.S.T-K.H.", "K..HT-KKS.H.T-RK" },
                           { "RrRr", "S.RR", "KrrR", "rRRR", "T-rr" }, true, 3, "RrTK" },
            { "HOUSE",     { "K.H.K.H.K.H.K.HS", "K.O-K.HSK.O-K.H.", "K.H.KSH.K.H.KSHR", "K.HhK.HSK.HhK.HR", "K.O-KSO-K.O-KSO-", "KhH.KhHSKhH.KhHh" },
                           { "SRSR", "K.rr", "SSRR", "KRKR", "KKrr" }, false, 2, "rRSK" },
            { "DRUM N BASS", { "K.H.S.HKH.K.S.HR", "K.H.S.H.HHK.S.H.", "K.h.S.hKk.K.S.hR", "K.H.S..KH.KHS.HR", "K.HKS.H.HKK.S.HH", "K.H.S.h.k.K.S.hS" },
                           { "SRrr", "rrrr", "KSrr", "S.RR", "SSrr" }, false, 3, "rSRK" },
            { "BREAK STUTTER", { "K.S.KkS.K.SXK.r.", "KK.SX.SKK.SX.rSR", "K.SXK.rSK.SXKKrX", "KX.SKXS.K.S.XrSR", "K.SrK.S.KXS.KrSR" },
                           { "rrrr", "XXrr", "KrKr", "RRrr" }, false, 3, "rXKS" },
            { "JERSEY CLUB", { "K..K..K.K.S.K.X.", "K..K..K.S.K..KX.", "KX.K..K.K..SK.K.", "K..K.XK.S.K..K.S", "K..K..K.SKS.K.XS" },
                           { "KKKK", "rKrK", "KrrX", "SKSK" }, false, 3, "KrXS" },
            { "AMAPIANO",  { "KhhLKhL-KhLLKhL-", "Kh.LKhLhKL-hKhLL", "KhLhK.L-KhhLK.L-", "KhhLKh.LKLhLKhL-" },
                           { "LLLL", "rLrL", "L-rr", "LhLL" }, false, 2, "LrLh" },
            { "FLIP",      { "M--.M-.M---M.M-.", "M---M-M.M--.M-..", "K.M-.M--K.M-.MM-", "M-.M-.M-M---M.M.", "M.MM-.M-M.MM-.M-" },
                           { "MMMM", "M.MM", "MRMR", "MMRR" }, false, 2, "MMR." },
            { "CHOP UP",   {}, {}, false, 3, "" }, // the slices themselves, re-arranged in time (see chopUpBars)
        };
        return all;
    }

    int pickStyle (double bpm, bool drums, bool tonal, bool slices, int avoid, juce::Random& rng)
    {
        // How likely each style is at this tempo
        // (boom bap, trap, drill, house, dnb, stutter, jersey club, amapiano, flip, chop up).
        std::array<float, numStyles> w {};
        if (bpm <= 0.0)        w = { 3, 2, 1, 2, 1, 2, 1, 1, 2, 2 };
        else if (bpm < 105.0)  w = { 5, 2, 1, 0, 0, 2, 0, 1, 3, 3 };
        else if (bpm < 135.0)  w = { 1, 2, 1, 4, 0, 2, 1, 3, 3, 3 };
        else if (bpm < 162.0)  w = { 0, 5, 4, 1, 1, 2, 4, 0, 2, 2 };
        else                   w = { 0, 2, 1, 0, 6, 2, 1, 0, 1, 2 };
        if (! drums)
            for (int i = boomBap; i <= amapiano; ++i) w[(size_t) i] *= 0.15f; // no drum hits in the sample: mostly a flip
        if (! tonal)
            w[flip] *= 0.2f;
        w[chopUp] *= slices ? 1.5f : 0.2f; // long slices of a loop: re-arranging them is the classic
        if (avoid >= 0 && avoid < numStyles)
        {
            const auto others = std::accumulate (w.begin(), w.end(), 0.0f) - w[(size_t) avoid];
            if (others > 0.0f)
                w[(size_t) avoid] = 0.0f;
        }
        const auto sum = std::accumulate (w.begin(), w.end(), 0.0f);
        auto x = rng.nextFloat() * sum;
        for (size_t i = 0; i < w.size(); ++i)
            if ((x -= w[i]) < 0.0f)
                return (int) i;
        return boomBap;
    }

    // Changes a bar idea (the first bar a little, later ones more): ghost notes come and go,
    // a hat becomes a roll or triplets, an offbeat gets a hit, a kick moves a step, a
    // backbeat turns into a ghost. The one stays.
    std::string vary (std::string bar, juce::Random& rng, float amount)
    {
        for (size_t s = 1; s < bar.size(); ++s)
        {
            if (rng.nextFloat() > amount)
                continue;
            auto& c = bar[s];
            const auto free = [&bar] (size_t at) { return at < bar.size() && bar[at] == '.' && (at + 1 >= bar.size() || bar[at + 1] != '-'); };
            switch (c)
            {
                case 'h': case 'k': case 's': c = '.'; break;
                case 'H':
                    if (rng.nextFloat() < 0.3f && free (s + 1)) { c = 'T'; bar[s + 1] = '-'; ++s; }
                    else c = rng.nextBool() ? 'R' : 'h';
                    break;
                case 'K':
                    if (s % 4 != 0 && free (s + 1) && (s + 1 < bar.size() && bar[s + 1] == '.')) std::swap (c, bar[s + 1]), ++s;
                    break;
                case 'S': if (s % 4 != 0) c = 's'; break;
                case '.': if (bar[s - 1] != '-' && s % 2 == 1) c = "hkXsH"[rng.nextInt (5)]; break;
                case 'M': if (s % 4 != 0) c = '-'; break;
                default: break;
            }
        }
        return bar;
    }

    // The notes a chop may take to stay in the key: every shift (-12..+12) that lands its own
    // pitch on the scale. Without its pitch or the key: the scale counted from the chop itself.
    struct Notes
    {
        std::vector<int> shifts; // ascending
        int home = 0;            // index of the shift to start from
    };

    Notes notesFor (int pitchClass, int tonic, bool minor, bool toTonic)
    {
        static const int minorScale[] = { 0, 3, 5, 7, 10 }, majorScale[] = { 0, 2, 4, 7, 9 };
        const auto* scale = minor ? minorScale : majorScale;
        const auto known = pitchClass >= 0 && tonic >= 0;
        const auto own = known ? pitchClass : 0, key = known ? tonic : 0;

        Notes n;
        for (int shift = -PatternEvent::maxPitch; shift <= PatternEvent::maxPitch; ++shift)
        {
            const auto degree = ((own + shift - key) % 12 + 12) % 12;
            if (std::find (scale, scale + 5, degree) != scale + 5)
                n.shifts.push_back (shift);
        }
        // Start at its own pitch (or the scale note nearest it); an 808 at the key's root.
        const auto target = toTonic && known ? ((key - own) % 12 + 18) % 12 - 6 : 0;
        for (size_t i = 0; i < n.shifts.size(); ++i)
            if (std::abs (n.shifts[i] - target) < std::abs (n.shifts[(size_t) n.home] - target))
                n.home = (int) i;
        return n;
    }

    int noteAt (const Notes& n, int degree)
    {
        return n.shifts[(size_t) juce::jlimit (0, (int) n.shifts.size() - 1, n.home + degree)];
    }

    // CHOP UP: the chops in their order, each as long as it lasts at the tempo - then cut up:
    // two swap places, one stutters, the line jumps; more so with every bar, the last bar
    // ending in a ratchet run.
    std::vector<PatternEvent> chopUpBars (const std::vector<ChopTraits>& chops, int bars, double bpm, juce::Random& rng)
    {
        std::vector<PatternEvent> events;
        const auto stepSeconds = bpm > 0.0 ? 60.0 / bpm / 4.0 : 0.15;
        auto lengthOf = [&] (size_t i) { return juce::jlimit (1, 8, (int) std::lround (chops[i].seconds / stepSeconds)); };
        const auto count = chops.size();
        // Where it starts: the top, or (now and then) somewhere in the loop.
        const size_t top = rng.nextFloat() < 0.6f ? 0 : (size_t) rng.nextInt ((int) count);
        size_t next = top;
        int pending = -1; // the second of a swapped pair
        const auto wildness = 0.7f + 0.6f * rng.nextFloat();
        for (int bar = 0; bar < bars; ++bar)
        {
            const auto chaos = wildness * (bars > 1 ? 0.12f + 0.45f * (float) bar / (float) (bars - 1) : 0.3f);
            const auto lastBar = bar == bars - 1 && (bars > 1 || rng.nextBool());
            const auto end = lastBar ? 12 + 2 * rng.nextInt (2) : 16;
            if (bar % 4 == 0)
                next = top; // every phrase starts from the same place, like the original
            for (int s = 0; s < end;)
            {
                PatternEvent e;
                e.step = bar * 16 + s;
                e.velocity = s % 4 == 0 ? 0.95f : 0.8f;
                const auto x = rng.nextFloat();
                size_t i = next % count;
                if (pending >= 0)
                {
                    i = (size_t) pending;
                    pending = -1;
                }
                else if (s > 0 && ! events.empty() && x < chaos * 0.3f)
                {
                    // Stutter: the chop before again, short and ratcheted.
                    e.chopId = events.back().chopId;
                    e.repeats = 2 + rng.nextInt (2);
                    e.length = 1;
                }
                else if (x < chaos * 0.6f && count > 1)
                {
                    i = (next + 1) % count; // swapped: the next one first
                    pending = (int) (next % count);
                    next += 2;
                }
                else if (x < chaos * 0.8f)
                {
                    i = (size_t) rng.nextInt ((int) count); // a jump
                    next = i + 1;
                }
                else
                    ++next;

                if (e.chopId == 0)
                {
                    e.chopId = chops[i].id;
                    e.length = std::min (lengthOf (i), end - s);
                    // Long ones cut short now and then (a chop as a stab).
                    if (e.length > 2 && rng.nextFloat() < chaos * 0.5f)
                        e.length = 2;
                }
                e.length = std::max (1, std::min (e.length, end - s));
                s += e.length;
                events.push_back (e);
            }
            if (lastBar) // the run: the last chop ratcheting, faster at the end
            {
                const auto id = events.empty() ? chops.front().id : events.back().chopId;
                for (int s = end; s < 16; ++s)
                {
                    PatternEvent e;
                    e.step = bar * 16 + s;
                    e.chopId = rng.nextFloat() < 0.75f ? id : chops[(size_t) rng.nextInt ((int) count)].id;
                    e.velocity = 0.7f + 0.05f * (float) (s - end);
                    e.repeats = s >= 14 ? 4 : 2 + rng.nextInt (2);
                    events.push_back (e);
                }
            }
        }
        return events;
    }
}

juce::StringArray styleNames()
{
    juce::StringArray names;
    for (const auto& s : styles())
        names.add (s.name);
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

Result roll (const std::vector<ChopTraits>& chops, const Options& options, juce::Random& rng)
{
    Result result;
    const auto bars = options.bars;
    auto& events = result.events;
    if (chops.empty() || bars < 1)
        return result;

    auto traitsOf = [&chops] (int id) { return *std::find_if (chops.begin(), chops.end(), [id] (const ChopTraits& c) { return c.id == id; }); };
    const auto punchiest = std::max_element (chops.begin(), chops.end(), [] (const ChopTraits& a, const ChopTraits& b) { return a.punch < b.punch; });
    const auto drums = punchiest->punch > 0.15f && chops.size() >= 2;
    const auto tonal = std::any_of (chops.begin(), chops.end(), [] (const ChopTraits& c) { return c.sustain > 0.25f; });
    std::vector<double> lengths;
    for (const auto& c : chops)
        lengths.push_back (c.seconds);
    std::nth_element (lengths.begin(), lengths.begin() + (std::ptrdiff_t) lengths.size() / 2, lengths.end());
    const auto eighth = options.bpm > 0.0 ? 30.0 / options.bpm : 0.25;
    const auto slices = chops.size() >= 3 && lengths[lengths.size() / 2] >= eighth;

    result.style = options.style >= 0 && options.style < numStyles ? options.style
                 : pickStyle (options.bpm, drums, tonal, slices, options.avoidStyle, rng);
    const auto& style = styles()[(size_t) result.style];
    result.name = style.name;

    if (result.style == chopUp)
    {
        events = chopUpBars (chops, bars, options.bpm, rng);
        if (events.size() > (size_t) PatternData::maxEvents)
            events.resize ((size_t) PatternData::maxEvents);
        return result;
    }

    // Who plays: with enough chops, any of the ones nearly as good as the best for a role
    // (not always the same kick, snare and hat).
    auto roles = assignRoles (chops);
    if (chops.size() >= 5)
    {
        auto near = [&] (float (*score) (const ChopTraits&), int current, const std::vector<int>& taken, float margin)
        {
            float top = -1.0e9f;
            for (const auto& c : chops)
                if (! contains (taken, c.id))
                    top = std::max (top, score (c));
            std::vector<int> pool;
            for (const auto& c : chops)
                if (! contains (taken, c.id) && score (c) >= top - margin)
                    pool.push_back (c.id);
            return pool.empty() ? current : pool[(size_t) rng.nextInt ((int) pool.size())];
        };
        auto swapIn = [] (int& role, int& second, int pick) { if (pick != role) { if (second == pick || second == 0) second = role; role = pick; } };
        swapIn (roles.kick, roles.kick2, near (kickScore, roles.kick, { roles.snare, roles.hat }, 0.3f));
        swapIn (roles.snare, roles.snare2, near (snareScore, roles.snare, { roles.kick, roles.hat, roles.kick2 }, 0.3f));
        swapIn (roles.hat, roles.hat2, near (hatScore, roles.hat, { roles.kick, roles.snare, roles.kick2, roles.snare2 }, 0.3f));
        if (roles.openHat == roles.hat)
            roles.openHat = 0;
    }
    // Tonal chops: a few of the most tonal, in a new order each roll.
    std::vector<int> melodic;
    {
        std::vector<ChopTraits> sorted (chops);
        std::sort (sorted.begin(), sorted.end(), [] (const ChopTraits& a, const ChopTraits& b) { return tonalScore (a) > tonalScore (b); });
        // Ones with a clear pitch first: a melody from a tone, not from a noise.
        const auto pitched = std::any_of (sorted.begin(), sorted.end(), [] (const ChopTraits& c) { return c.sustain > 0.2f && c.pitchClass >= 0; });
        for (size_t i = 0; i < sorted.size() && melodic.size() < 5; ++i)
            if (sorted[i].sustain > 0.2f && (sorted[i].pitchClass >= 0 || ! pitched))
                melodic.push_back (sorted[i].id);
        if (melodic.empty())
            melodic.push_back (sorted.front().id);
        for (size_t i = melodic.size(); i > 1; --i)
            std::swap (melodic[i - 1], melodic[(size_t) rng.nextInt ((int) i)]);
        melodic.resize (std::min<size_t> (melodic.size(), (size_t) (1 + rng.nextInt (3))));
    }
    // The bass / log drum: low and long, not the kick when another one will do.
    int bass = roles.kick;
    {
        float top = -1.0e9f;
        for (const auto& c : chops)
            if (c.id != roles.snare && c.id != roles.hat && (c.id != roles.kick || chops.size() < 5))
                if (const auto s = bassScore (c); s > top && c.low > 0.2f) { top = s; bass = c.id; }
    }

    // The bars: A, B now and then, each a variation (the first a little, later ones more:
    // the energy grows), fills where phrases end - the style's or made up.
    const std::string a = style.bars[(size_t) rng.nextInt ((int) style.bars.size())];
    const std::string b = style.bars[(size_t) rng.nextInt ((int) style.bars.size())];
    auto makeFill = [&]
    {
        if (rng.nextFloat() < 0.6f)
            return std::string (style.fills[(size_t) rng.nextInt ((int) style.fills.size())]);
        std::string fill;
        const std::string alphabet (style.fillAlphabet);
        for (int k = 0; k < 4; ++k)
            fill += alphabet[(size_t) rng.nextInt ((int) alphabet.size())];
        return fill;
    };
    std::vector<std::string> sheet;
    for (int bar = 0; bar < bars; ++bar)
    {
        auto idea = (bars >= 4 && bar % 4 == 2) || (bars == 2 && bar == 1 && rng.nextFloat() < 0.4f) ? b : a;
        idea = vary (idea, rng, bar == 0 ? 0.14f : 0.1f + 0.2f * (float) bar / (float) (bars - 1));
        const auto fill = makeFill();
        if (bar == bars - 1 && (bars > 1 || rng.nextFloat() < 0.5f))
            idea.replace (12, 4, fill);
        else if (bars >= 8 && bar % 4 == 3) // the end of the first phrase: half a fill
            idea.replace (14, 2, fill.substr (2));
        if (idea.size() > 1 && idea[1] == '-' && idea[0] != 'K' && idea[0] != 'M')
            idea[1] = '.'; // nothing to hold on the one
        sheet.push_back (idea);
    }

    // The hook: a chop that is none of the drums, on the same few offbeats - every bar, or
    // as a call and its answer (every other bar, a step higher).
    int hook = 0;
    if (result.style != flip && rng.nextFloat() < 0.8f)
    {
        std::vector<int> pool;
        for (const auto& c : chops)
            if (c.id != roles.kick && c.id != roles.snare && c.id != roles.hat && c.id != roles.hat2 && c.id != bass)
                pool.push_back (c.id);
        if (! pool.empty())
            hook = pool[(size_t) rng.nextInt ((int) pool.size())];
    }
    int hookAnswer = 0;
    if (hook != 0)
    {
        static const std::vector<std::vector<int>> spots { { 3 }, { 6 }, { 7 }, { 10 }, { 14 }, { 3, 11 }, { 6, 14 }, { 7, 15 }, { 2, 10 }, { 3, 6 }, { 10, 13 }, { 11, 14 }, { 6, 7 } };
        const auto& at = spots[(size_t) rng.nextInt ((int) spots.size())];
        const auto every = rng.nextInt (3); // 0 every bar, 1 the odd bars, 2 the even ones
        hookAnswer = std::array<int, 4> { 1, -1, 2, 0 }[(size_t) rng.nextInt (4)];
        for (int bar = 0; bar < bars; ++bar)
            if (every == 0 || bars == 1 || bar % 2 == every - 1)
                for (const auto p : at)
                {
                    auto& c = sheet[(size_t) bar][(size_t) p];
                    if (c == '.' || c == 'h' || c == 'H' || c == 'k')
                        c = 'C';
                }
    }

    // Tuned notes stay in the key: each tonal chop gets the shifts that put its own pitch on
    // the scale; an 808 or a log drum starts on the key's root.
    const auto tonic = options.tonic;
    std::map<int, Notes> notes;
    auto notesOf = [&] (int id, bool toTonic) -> const Notes&
    {
        const auto key = toTonic ? -id : id;
        if (auto it = notes.find (key); it != notes.end())
            return it->second;
        const auto pc = traitsOf (id).pitchClass;
        result.inKey = result.inKey || (pc >= 0 && tonic >= 0);
        return notes[key] = notesFor (pc, tonic, options.minor, toTonic);
    };

    int degree = 0;
    auto nextNote = [&] (int id, int bar, bool fill)
    {
        degree = juce::jlimit (-3, 7, degree + (fill ? 1 : rng.nextInt (5) - 2));
        return noteAt (notesOf (id, false), degree + (bar % 2 == 1 && ! fill ? 1 : 0)); // the answer a step higher
    };
    static const int eightOhEight[] = { 0, 0, 0, -1, 1, -2, 2, 3 }; // scale steps from the root
    static const int logDrum[] = { 0, 0, 1, 2, -1, 3, 0, -2 };
    auto either = [&rng] (int second, float chance, int first) { return second != 0 && rng.nextFloat() < chance ? second : first; };

    // Hats with a pulse of their own: every 2nd, 3rd or 4th one louder (or all even), and
    // in trap and drill the rolls now and then climbing.
    const auto accentEvery = std::array<int, 4> { 0, 2, 3, 4 }[(size_t) rng.nextInt (4)];
    const auto climbingRolls = style.eightOhEight && rng.nextBool();
    int hatCount = 0, run = 0;

    for (int bar = 0; bar < bars; ++bar)
    {
        const auto& line = sheet[(size_t) bar];
        for (int s = 0; s < 16; ++s)
        {
            const auto c = line[(size_t) s];
            const auto step = bar * 16 + s;
            const auto inFill = s >= 12 && bar == bars - 1;
            if (c == '-' && ! events.empty() && events.back().step + events.back().length == step)
            {
                ++events.back().length; // held: merged into one long note
                continue;
            }
            if (c == '.' || c == '-')
                continue;

            PatternEvent e;
            e.step = step;
            const auto onBeat = s % 4 == 0;
            const auto rolling = c == 'R' || c == 'T';
            run = rolling && ! events.empty() && events.back().step + events.back().length == step && events.back().chopId == roles.hat ? run + 1 : 0;
            switch (c)
            {
                case 'K': e.chopId = roles.kick;  e.velocity = onBeat ? 1.0f : 0.9f; break;
                case 'k': e.chopId = either (roles.kick2, 0.6f, roles.kick); e.velocity = 0.6f; e.chance = 0.8f; break;
                case 'S': e.chopId = bar > 0 ? either (roles.snare2, 0.25f, roles.snare) : roles.snare; e.velocity = 0.95f; break;
                case 's': e.chopId = either (roles.snare2, 0.5f, roles.snare); e.velocity = 0.45f; e.chance = 0.7f; break;
                case 'r': e.chopId = roles.snare; e.velocity = 0.75f; e.repeats = style.rollRepeats; break;
                case 'H': e.chopId = roles.hat;   e.velocity = onBeat ? 0.8f : 0.65f; break;
                case 'h': e.chopId = either (roles.hat2, 0.5f, roles.hat); e.velocity = 0.5f; e.chance = 0.85f; break;
                case 'R': e.chopId = roles.hat;   e.velocity = 0.7f; e.repeats = juce::jlimit (2, PatternEvent::maxRepeats, style.rollRepeats - rng.nextInt (2)); break;
                case 'T': e.chopId = roles.hat;   e.velocity = 0.7f; e.repeats = 3; break;
                case 'O': e.chopId = roles.openHat != 0 ? roles.openHat : roles.hat; e.velocity = 0.75f; break;
                case 'P': e.chopId = roles.perc;  e.velocity = 0.7f; break;
                case 'X': e.chopId = chops[(size_t) rng.nextInt ((int) chops.size())].id; e.velocity = 0.7f; e.random = 1; e.chance = 0.8f; break;
                case 'C': e.chopId = hook; e.velocity = 0.8f; e.pitch = noteAt (notesOf (hook, false), bar % 2 == 1 ? hookAnswer : 0); break;
                case 'L':
                    e.chopId = bass;
                    e.velocity = onBeat ? 0.95f : 0.85f;
                    e.pitch = noteAt (notesOf (bass, true), logDrum[rng.nextInt ((int) std::size (logDrum))]);
                    break;
                case 'M':
                    e.chopId = melodic[(size_t) (rng.nextFloat() < 0.6f ? 0 : rng.nextInt ((int) melodic.size()))];
                    e.velocity = onBeat ? 0.95f : 0.8f;
                    e.pitch = nextNote (e.chopId, bar, inFill);
                    break;
                default: continue;
            }
            if (c == 'H' && accentEvery > 0)
                e.velocity += hatCount++ % accentEvery == 0 ? 0.15f : -0.1f;
            if (rolling && climbingRolls && run > 0)
                e.pitch = noteAt (notesOf (roles.hat, false), run);
            // A little life in the levels (accents stay on top).
            e.velocity = juce::jlimit (0.05f, 1.0f, e.velocity + (rng.nextFloat() - 0.5f) * 0.1f);

            // Trap, drill: the kick is an 808 - tuned per hit (in the key), sliding into the next.
            if (style.eightOhEight && c == 'K')
                e.pitch = noteAt (notesOf (e.chopId, true), eightOhEight[rng.nextInt ((int) std::size (eightOhEight))]);
            events.push_back (e);
        }
    }

    // Slides: from a tuned note into the next one close behind it (an 808, a log drum, a melody).
    for (size_t i = 0; i + 1 < events.size(); ++i)
    {
        auto& e = events[i];
        const auto& next = events[i + 1];
        const auto close = next.step - e.step <= 4 && next.chopId == e.chopId && next.pitch != e.pitch;
        const auto chance = style.eightOhEight || result.style == amapiano ? 0.6f : 0.45f;
        if (close && (e.pitch != 0 || next.pitch != 0) && rng.nextFloat() < chance)
            e.slide = true;
    }

    if (events.size() > (size_t) PatternData::maxEvents)
        events.resize ((size_t) PatternData::maxEvents);
    return result;
}

} // namespace kerf::PatternDice

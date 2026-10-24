#include "DiceInternal.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <numeric>

namespace kerf::PatternDice::detail
{

//==============================================================================
// Genre bars

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
        { "CHOP UP",   {}, {}, false, 3, "" }, // the slices themselves, re-arranged in time (chopUpBars)
    };
    return all;
}

bool isDrumStyle (int style)
{
    return style >= boomBap && style <= amapiano;
}

int pickStyle (double bpm, bool drums, bool tonal, bool slices, bool placed, int avoid, juce::Random& rng)
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
    w[chopUp] *= placed ? 3.0f : (slices ? 1.5f : 0.2f); // a chopped loop: its own groove, flipped, is the classic
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

std::string varyBar (std::string bar, juce::Random& rng, float amount)
{
    // Ghost notes come and go, a hat becomes a roll or triplets, an offbeat gets a hit, a
    // kick moves a step, a backbeat turns into a ghost. The one stays.
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
                if (s % 4 != 0 && free (s + 1)) { std::swap (c, bar[s + 1]); ++s; }
                break;
            case 'S': if (s % 4 != 0) c = 's'; break;
            case '.': if (bar[s - 1] != '-' && s % 2 == 1) c = "hkXsH"[rng.nextInt (5)]; break;
            case 'M': if (s % 4 != 0) c = '-'; break;
            default: break;
        }
    }
    return bar;
}

static std::vector<PatternEvent> slicesInOrder (const std::vector<ChopTraits>& chops, int bars, double bpm, juce::Random& rng)
{
    // The chops in their order, each as long as it lasts at the tempo - then cut up: two swap
    // places, one stutters, the line jumps; more so with every bar, the last bar ending in a
    // ratchet run.
    std::vector<PatternEvent> events;
    const auto stepSeconds = bpm > 0.0 ? 60.0 / bpm / 4.0 : 0.15;
    auto lengthOf = [&] (size_t i) { return juce::jlimit (1, 8, (int) std::lround (chops[i].seconds / stepSeconds)); };
    const auto count = chops.size();
    const size_t top = rng.nextFloat() < 0.6f ? 0 : (size_t) rng.nextInt ((int) count);
    size_t next = top;
    int pending = -1;
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
                e.chopId = events.back().chopId; // a stutter: the chop before again, short and ratcheted
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
                if (e.length > 2 && rng.nextFloat() < chaos * 0.5f)
                    e.length = 2; // a long one as a stab
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

std::vector<std::pair<size_t, int>> placesInLoop (const std::vector<ChopTraits>& chops, double bpm, double originSeconds)
{
    std::vector<std::pair<size_t, int>> placed;
    if (bpm <= 0.0 || chops.size() < 2)
        return placed;
    const auto stepSeconds = 60.0 / bpm / 4.0;
    for (size_t i = 0; i < chops.size(); ++i)
        if (const auto step = (int) std::lround ((chops[i].start - originSeconds) / stepSeconds); step >= 0)
            placed.push_back ({ i, step });
    // By step; on a shared step the louder one.
    std::stable_sort (placed.begin(), placed.end(), [&chops] (const auto& a, const auto& b)
                      { return a.second < b.second || (a.second == b.second && chops[a.first].level > chops[b.first].level); });
    placed.erase (std::unique (placed.begin(), placed.end(), [] (const auto& a, const auto& b) { return a.second == b.second; }), placed.end());
    if (placed.size() < 2)
        placed.clear();
    return placed;
}

std::vector<PatternEvent> chopUpBars (const std::vector<ChopTraits>& chops, int bars, double bpm, double originSeconds, juce::Random& rng)
{
    const auto placed = placesInLoop (chops, bpm, originSeconds);
    if (placed.size() < 3)
        return slicesInOrder (chops, bars, bpm, rng);

    const auto stepSeconds = 60.0 / bpm / 4.0;
    constexpr int barSteps = 16;
    auto ownSteps = [&] (size_t i) { return juce::jlimit (1, 8, (int) std::lround (chops[i].seconds / stepSeconds)); };

    // The loop: how many bars it spans; which of them this pattern starts from.
    int lastEnd = 0;
    for (const auto& [i, step] : placed)
        lastEnd = std::max (lastEnd, step + ownSteps (i));
    const auto loopBars = std::max (1, std::max ((int) std::lround (lastEnd / (double) barSteps), placed.back().second / barSteps + 1));
    const auto offset = loopBars > bars && rng.nextFloat() < 0.5f ? rng.nextInt (loopBars - bars + 1) : 0;

    // A bar as (step, chop index); the source bar's own hits.
    using Hit = std::pair<int, size_t>;
    auto sourceBar = [&] (int sourceIndex)
    {
        std::vector<Hit> hits;
        for (const auto& [i, step] : placed)
            if (step / barSteps == sourceIndex % loopBars)
                hits.push_back ({ step % barSteps, i });
        return hits;
    };
    auto similar = [&chops] (size_t a, size_t b)
    {
        // Two chops that could stand in for each other: both hits, or both tones.
        return (chops[a].sustain > 0.35f) == (chops[b].sustain > 0.35f);
    };

    std::vector<PatternEvent> events;
    const auto wildness = 0.7f + 0.6f * rng.nextFloat();
    for (int bar = 0; bar < bars; ++bar)
    {
        auto hits = sourceBar (offset + bar);
        std::map<int, int> repeatsAt; // step -> ratchet

        // The first bar mostly as it is (the groove first heard), more flipped with every bar.
        const auto chaos = wildness * (bars > 1 ? 0.12f + 0.5f * (float) bar / (float) (bars - 1) : 0.35f);
        auto ops = (int) std::floor (chaos * 4.0f + rng.nextFloat());
        enum Op { swap, stutter, repeatBeat, reverseBeat, drop, borrow, numOps };
        while (ops-- > 0 && ! hits.empty())
        {
            const auto op = (Op) [&]
            {
                const float w[] = { 0.32f, 0.2f, 0.15f, 0.1f, 0.1f, loopBars > 1 ? 0.13f : 0.03f };
                auto x = rng.nextFloat() * 1.0f;
                for (int k = 0; k < numOps; ++k)
                    if ((x -= w[k]) < 0.0f)
                        return k;
                return 0;
            }();
            auto inBeat = [&hits] (int beat)
            {
                std::vector<size_t> at;
                for (size_t h = 0; h < hits.size(); ++h)
                    if (hits[h].first / 4 == beat)
                        at.push_back (h);
                return at;
            };
            switch (op)
            {
                case swap:
                {
                    // Two hits trade chops (alike ones first: a kick for a kick).
                    const auto a = (size_t) rng.nextInt ((int) hits.size());
                    std::vector<size_t> partners;
                    for (size_t h = 0; h < hits.size(); ++h)
                        if (h != a && hits[h].second != hits[a].second && similar (hits[h].second, hits[a].second))
                            partners.push_back (h);
                    if (partners.empty())
                        for (size_t h = 0; h < hits.size(); ++h)
                            if (h != a && hits[h].second != hits[a].second)
                                partners.push_back (h);
                    if (! partners.empty())
                        std::swap (hits[a].second, hits[partners[(size_t) rng.nextInt ((int) partners.size())]].second);
                    break;
                }
                case stutter:
                {
                    // A hit ratchets, or rings out as a run of 16ths into the gap after it.
                    const auto h = (size_t) rng.nextInt ((int) hits.size());
                    if (rng.nextBool())
                        repeatsAt[hits[h].first] = 2 + rng.nextInt (2);
                    else
                        for (int s = hits[h].first + 1, n = 0; s < barSteps && n < 3; ++s, ++n)
                        {
                            if (std::any_of (hits.begin(), hits.end(), [s] (const Hit& x) { return x.first == s; }))
                                break;
                            hits.push_back ({ s, hits[h].second });
                        }
                    break;
                }
                case repeatBeat:
                {
                    // One beat played again in place of the next (the classic sampler repeat).
                    const auto beat = rng.nextInt (3);
                    const auto from = inBeat (beat);
                    if (from.empty())
                        break;
                    std::vector<Hit> copy;
                    for (const auto h : from)
                        copy.push_back ({ hits[h].first + 4, hits[h].second });
                    hits.erase (std::remove_if (hits.begin(), hits.end(), [beat] (const Hit& x) { return x.first / 4 == beat + 1; }), hits.end());
                    hits.insert (hits.end(), copy.begin(), copy.end());
                    break;
                }
                case reverseBeat:
                {
                    const auto at = inBeat (rng.nextInt (4));
                    for (size_t k = 0; k < at.size() / 2; ++k)
                        std::swap (hits[at[k]].second, hits[at[at.size() - 1 - k]].second);
                    break;
                }
                case drop:
                    if (hits.size() >= 4)
                    {
                        const auto h = 1 + (size_t) rng.nextInt ((int) hits.size() - 1);
                        if (hits[h].first != 0)
                            hits.erase (hits.begin() + (std::ptrdiff_t) h);
                    }
                    break;
                case borrow:
                {
                    // A beat from another bar of the loop (or another beat of this one).
                    const auto beat = 1 + rng.nextInt (3);
                    const auto other = sourceBar (offset + bar + 1 + rng.nextInt (std::max (1, loopBars)));
                    const auto fromBeat = loopBars > 1 ? beat : rng.nextInt (4);
                    hits.erase (std::remove_if (hits.begin(), hits.end(), [beat] (const Hit& x) { return x.first / 4 == beat; }), hits.end());
                    for (const auto& x : other)
                        if (x.first / 4 == fromBeat)
                            hits.push_back ({ beat * 4 + x.first % 4, x.second });
                    break;
                }
                case numOps: break;
            }
            std::sort (hits.begin(), hits.end());
            hits.erase (std::unique (hits.begin(), hits.end(), [] (const Hit& a, const Hit& b) { return a.first == b.first; }), hits.end());
        }

        // The last bar: a run to finish (now and then).
        const auto lastBar = bar == bars - 1 && (bars > 1 || rng.nextBool()) && rng.nextFloat() < 0.65f;
        const auto end = lastBar ? 12 + 2 * rng.nextInt (2) : barSteps;
        hits.erase (std::remove_if (hits.begin(), hits.end(), [end] (const Hit& x) { return x.first >= end; }), hits.end());
        if (hits.empty() || hits.front().first != 0)
            hits.insert (hits.begin(), { 0, hits.empty() ? placed.front().first : hits.front().second }); // always something on the one

        for (size_t h = 0; h < hits.size(); ++h)
        {
            const auto [step, index] = hits[h];
            const auto next = h + 1 < hits.size() ? hits[h + 1].first : end;
            PatternEvent e;
            e.step = bar * barSteps + step;
            e.chopId = chops[index].id;
            e.length = std::max (1, std::min (next - step, ownSteps (index)));
            e.velocity = step % 4 == 0 ? 0.95f : (step % 2 == 0 ? 0.85f : 0.78f);
            if (const auto r = repeatsAt.find (step); r != repeatsAt.end())
            {
                e.repeats = juce::jlimit (1, PatternEvent::maxRepeats, r->second);
                e.length = std::min (e.length, 2);
            }
            events.push_back (e);
        }
        if (lastBar) // the run: the last hit ratcheting, faster at the end
        {
            const auto id = events.back().chopId;
            for (int s = end; s < barSteps; ++s)
            {
                PatternEvent e;
                e.step = bar * barSteps + s;
                e.chopId = rng.nextFloat() < 0.75f ? id : chops[placed[(size_t) rng.nextInt ((int) placed.size())].first].id;
                e.velocity = 0.7f + 0.05f * (float) (s - end);
                e.repeats = s >= 14 ? 4 : 2 + rng.nextInt (2);
                events.push_back (e);
            }
        }
    }
    return events;
}

//==============================================================================
// Notes in the key

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

//==============================================================================
// Roles

float kickScore (const ChopTraits& c)  { return 1.6f * c.low + 0.6f * c.punch - 1.2f * c.bright - 0.3f * (float) std::min (1.0, c.seconds); }
// A snare is bright, but not as bright as a hat: its score peaks half way up.
float snareScore (const ChopTraits& c) { return c.punch + 3.0f * c.bright * (1.0f - c.bright) - 0.8f * c.low; }
float hatScore (const ChopTraits& c)   { return 1.5f * c.bright - c.low - 0.6f * c.sustain - 0.4f * (float) std::min (1.0, c.seconds); }
float bassScore (const ChopTraits& c)  { return 1.2f * c.low + c.sustain - c.bright; }
float tonalScore (const ChopTraits& c) { return c.sustain + 0.3f * (float) std::min (2.0, c.seconds) - 0.4f * c.punch; }

float roleFit (const ChopTraits& c, Role role)
{
    const auto shortness = (float) juce::jlimit (0.0, 1.0, 1.0 - c.seconds / 0.6);
    const auto longness = (float) juce::jlimit (0.0, 1.0, c.seconds / 0.8);
    const auto pitched = c.pitchClass >= 0 ? 1.0f : 0.0f;
    switch (role)
    {
        case Role::percussive: return 0.15f + c.punch * (1.0f - 0.6f * c.sustain) + 0.3f * shortness;
        case Role::groove:     return 0.3f + 0.5f * c.punch + 0.3f * (1.0f - std::abs (c.bright - 0.45f)) + 0.2f * c.level;
        case Role::impact:     return 0.1f + 0.6f * c.punch + 0.5f * c.low + 0.4f * c.level;
        case Role::sustained:  return 0.1f + c.sustain + 0.5f * longness - 0.3f * c.punch;
        case Role::melodic:    return 0.1f + 0.7f * pitched + 0.5f * c.sustain + 0.2f * longness;
        case Role::texture:    return 0.1f + 0.6f * c.sustain + 0.4f * c.bright - 0.4f * c.punch + 0.2f * longness;
        case Role::fill:       return 0.15f + 0.6f * c.bright + 0.4f * c.punch + 0.3f * shortness - 0.3f * c.sustain;
        case Role::numRoles:   break;
    }
    return 0.5f;
}

//==============================================================================
// Modes: named characters, only weights.

const std::vector<ModeProfile>& modes()
{
    //                    density swing variation template silence transition experimental      tools: pitch ratchet ghost hold slide scatter   min max
    static const std::vector<ModeProfile> all {
        { "BALANCED",   0.42f, 0.25f, 0.35f, 0.70f, 0.30f, 0.60f, 0.10f, { 0.55f, 0.55f, 0.60f, 0.50f, 0.35f, 0.20f }, 2, 3 },
        { "GROOVE",     0.55f, 0.15f, 0.25f, 0.95f, 0.20f, 0.50f, 0.05f, { 0.30f, 0.70f, 0.90f, 0.30f, 0.25f, 0.20f }, 2, 3 },
        { "CHOPPY",     0.72f, 0.20f, 0.55f, 0.40f, 0.15f, 0.70f, 0.30f, { 0.40f, 1.00f, 0.50f, 0.10f, 0.20f, 0.60f }, 2, 4 },
        { "MELODIC",    0.38f, 0.25f, 0.35f, 0.15f, 0.30f, 0.50f, 0.15f, { 1.00f, 0.20f, 0.30f, 0.80f, 0.70f, 0.10f }, 2, 3 },
        { "TEXTURE",    0.28f, 0.30f, 0.40f, 0.10f, 0.50f, 0.60f, 0.30f, { 0.60f, 0.30f, 0.60f, 1.00f, 0.50f, 0.40f }, 2, 3 },
        { "MINIMAL",    0.20f, 0.20f, 0.25f, 0.50f, 0.60f, 0.40f, 0.05f, { 0.30f, 0.20f, 0.40f, 0.50f, 0.20f, 0.10f }, 1, 2 },
        { "CHAOS",      0.60f, 0.45f, 0.80f, 0.30f, 0.35f, 0.90f, 0.90f, { 0.70f, 0.70f, 0.70f, 0.70f, 0.70f, 0.70f }, 3, 5 },
    };
    return all;
}

} // namespace kerf::PatternDice::detail

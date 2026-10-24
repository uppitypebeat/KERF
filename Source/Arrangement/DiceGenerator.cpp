#include "DiceInternal.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <numeric>
#include <set>

namespace kerf::PatternDice::detail
{

namespace
{
    constexpr int barSteps = PatternData::stepsPerBar;

    // How much each step of a bar counts (the one, the backbeats, the 8ths, the 16ths).
    constexpr std::array<float, barSteps> stepWeight { 1.0f, 0.2f, 0.4f, 0.25f, 0.8f, 0.2f, 0.45f, 0.3f,
                                                       0.9f, 0.2f, 0.4f, 0.25f, 0.8f, 0.2f, 0.5f, 0.35f };

    // Thresholds and amounts (named, so they can be tuned in one place).
    constexpr float usagePenalty = 0.7f;      // each use of a chop in this roll divides its chance by (1 + this * uses)
    constexpr float recentPenalty = 0.6f;     // a chop the last rolls leaned on is a little less likely
    constexpr float firstBarVariation = 0.0f; // the first statement of an idea is as made
    constexpr float answerAmount = 0.25f;     // the second bar of a two-bar phrase: a light answer
    constexpr int maxMotifChops = 4;
    constexpr int maxHoldSteps = 8;

    /** One step of an idea (a bar: steps 0..15). */
    struct Slot
    {
        int step = 0, length = 1;
        int chopId = 0;
        float velocity = 0.85f;
        bool tuned = false;   // its pitch comes from `degree` (melodies, a hook, an 808, a log drum)
        bool toTonic = false; // tuned around the key's root (the 808, the log drum)
        int degree = 0;       // scale steps from the chop's own note (or the root)
        float chance = 1.0f;
        int repeats = 1;
        int random = 0;
        int priority = 1;     // 3: anchors (the one, backbeats) 2: motif heads 1: the rest 0: ornaments
    };
    using Bar = std::vector<Slot>;

    /** Who plays in this roll for the genre roles (chosen once: a roll has its own band). */
    struct Players
    {
        int kick = 0, kick2 = 0, snare = 0, snare2 = 0, hat = 0, hat2 = 0, openHat = 0, perc = 0, bass = 0, hook = 0;
        std::vector<int> melodic;
    };

    class Generator
    {
    public:
        Generator (const std::vector<ChopTraits>& c, const Options& o, GenerationContext& x)
            : chops (c), options (o), ctx (x), rng (x.rng), mode (*x.mode)
        {
            for (const auto& t : chops)
                byId[t.id] = &t;
            stepSeconds = options.bpm > 0.0 ? 60.0 / options.bpm / 4.0 : 0.15;
            if (options.recent != nullptr) // the chops the last rolls leaned on
                for (const auto& pattern : *options.recent)
                    for (const auto& e : pattern)
                        ++recentUse[e.chopId];
        }

        std::vector<PatternEvent> run();

    private:
        const std::vector<ChopTraits>& chops;
        const Options& options;
        GenerationContext& ctx;
        juce::Random& rng;
        const ModeProfile& mode;
        std::map<int, const ChopTraits*> byId;
        std::map<int, int> uses, recentUse;
        std::map<int, Notes> notes;
        Players players;
        double stepSeconds = 0.15;
        bool drums = false, tonal = false, backbone = false;

        // --- small helpers --------------------------------------------------------------
        float chance() { return rng.nextFloat(); }
        bool odds (float p) { return rng.nextFloat() < p; }
        template <typename T> const T& oneOf (const std::vector<T>& v) { return v[(size_t) rng.nextInt ((int) v.size())]; }
        int weighted (const std::vector<float>& w)
        {
            const auto sum = std::accumulate (w.begin(), w.end(), 0.0f);
            if (sum <= 0.0f)
                return rng.nextInt ((int) w.size());
            auto x = rng.nextFloat() * sum;
            for (size_t i = 0; i < w.size(); ++i)
                if ((x -= w[i]) < 0.0f)
                    return (int) i;
            return (int) w.size() - 1;
        }
        const ChopTraits& traits (int id) const { return *byId.at (id); }
        int stepsOf (int id) const { return juce::jlimit (1, maxHoldSteps, (int) std::lround (traits (id).seconds / stepSeconds)); }
        bool canTune (int id) const { return traits (id).pitchClass >= 0 || traits (id).sustain > 0.3f; }

        /** A chop for a role: suitability, then less of what this roll (and the last ones)
            already used - no chop is forced in, none is shut out. */
        int pickChop (Role role, const std::set<int>& avoid = {})
        {
            std::vector<float> w;
            for (const auto& c : chops)
            {
                auto weight = std::pow (std::max (0.02f, roleFit (c, role)), 2.0f);
                // The best chops: a quiet one or a scrap (under 40 ms) seldom gets a part.
                weight *= juce::jlimit (0.1f, 1.0f, c.level * 2.5f) * (c.seconds < 0.04 ? 0.2f : 1.0f);
                weight /= 1.0f + usagePenalty * (float) uses[c.id];
                if (const auto r = recentUse.find (c.id); r != recentUse.end())
                    weight /= 1.0f + recentPenalty * std::min (3.0f, (float) r->second / 8.0f);
                if (avoid.count (c.id) > 0 && (int) chops.size() > (int) avoid.size())
                    weight *= 0.05f;
                w.push_back (weight);
            }
            return chops[(size_t) weighted (w)].id;
        }

        const Notes& notesOf (int id, bool toTonic)
        {
            const auto key = toTonic ? -id : id;
            if (auto it = notes.find (key); it != notes.end())
                return it->second;
            const auto pc = traits (id).pitchClass;
            ctx.inKey = ctx.inKey || (pc >= 0 && options.tonic >= 0);
            return notes[key] = notesFor (pc, options.tonic, options.minor, toTonic);
        }

        static void tidy (Bar& bar, int steps = barSteps)
        {
            // One event per step (the higher priority stays), each ending before the next.
            std::stable_sort (bar.begin(), bar.end(), [] (const Slot& a, const Slot& b) { return a.step < b.step || (a.step == b.step && a.priority > b.priority); });
            Bar out;
            for (const auto& s : bar)
                if (s.step >= 0 && s.step < steps && (out.empty() || out.back().step != s.step))
                    out.push_back (s);
            for (size_t i = 0; i < out.size(); ++i)
            {
                const auto next = i + 1 < out.size() ? out[i + 1].step : steps;
                out[i].length = juce::jlimit (1, std::max (1, next - out[i].step), out[i].length);
            }
            bar = std::move (out);
        }
        static bool taken (const Bar& bar, int step)
        {
            return std::any_of (bar.begin(), bar.end(), [step] (const Slot& s) { return step >= s.step && step < s.step + s.length; });
        }

        // --- the plan ---------------------------------------------------------------------
        void chooseTools();
        void choosePlayers();
        juce::String chooseForm (int sections);
        std::vector<float> densityCurve (int sections);

        // --- ideas --------------------------------------------------------------------------
        Bar genreBar (const std::string& line);
        Bar motifBar (float density);
        Bar transform (const Bar& bar, float amount);
        void fitDensity (Bar& bar, float target);
        void transition (Bar& bar, bool genre);
        void ending (Bar& bar, const Bar& opening, bool genre);
        Bar fillSteps (const std::string& fill, int from);
    };

    //==============================================================================
    void Generator::chooseTools()
    {
        // The budget: a few tools from the mode's weights - never all at once.
        const auto count = mode.minTools + rng.nextInt (mode.maxTools - mode.minTools + 1);
        std::vector<float> w (mode.tools.begin(), mode.tools.end());
        if (! tonal)
            w[pitchTool] *= 0.3f; // little to tune
        for (int k = 0; k < count; ++k)
        {
            const auto t = weighted (w);
            ctx.tools[(size_t) t] = true;
            w[(size_t) t] = 0.0f;
        }
    }

    void Generator::choosePlayers()
    {
        // The genre roles: any chop nearly as good as the best (a roll has its own band).
        auto roles = assignRoles (chops);
        if (chops.size() >= 5)
        {
            auto near = [&] (float (*score) (const ChopTraits&), int current, const std::vector<int>& taken, float margin)
            {
                float top = -1.0e9f;
                for (const auto& c : chops)
                    if (std::find (taken.begin(), taken.end(), c.id) == taken.end())
                        top = std::max (top, score (c));
                std::vector<int> pool;
                for (const auto& c : chops)
                    if (std::find (taken.begin(), taken.end(), c.id) == taken.end() && score (c) >= top - margin)
                        pool.push_back (c.id);
                return pool.empty() ? current : oneOf (pool);
            };
            auto swapIn = [] (int& role, int& second, int pick) { if (pick != role) { if (second == pick || second == 0) second = role; role = pick; } };
            swapIn (roles.kick, roles.kick2, near (kickScore, roles.kick, { roles.snare, roles.hat }, 0.3f));
            swapIn (roles.snare, roles.snare2, near (snareScore, roles.snare, { roles.kick, roles.hat, roles.kick2 }, 0.3f));
            swapIn (roles.hat, roles.hat2, near (hatScore, roles.hat, { roles.kick, roles.snare, roles.kick2, roles.snare2 }, 0.3f));
            if (roles.openHat == roles.hat)
                roles.openHat = 0;
        }
        players.kick = roles.kick; players.kick2 = roles.kick2; players.snare = roles.snare; players.snare2 = roles.snare2;
        players.hat = roles.hat; players.hat2 = roles.hat2; players.openHat = roles.openHat; players.perc = roles.perc;

        // Tonal chops (pitched ones first), in a new order each roll.
        std::vector<ChopTraits> sorted (chops);
        std::sort (sorted.begin(), sorted.end(), [] (const ChopTraits& a, const ChopTraits& b) { return tonalScore (a) > tonalScore (b); });
        const auto pitched = std::any_of (sorted.begin(), sorted.end(), [] (const ChopTraits& c) { return c.sustain > 0.2f && c.pitchClass >= 0; });
        for (size_t i = 0; i < sorted.size() && players.melodic.size() < 5; ++i)
            if (sorted[i].sustain > 0.2f && (sorted[i].pitchClass >= 0 || ! pitched))
                players.melodic.push_back (sorted[i].id);
        if (players.melodic.empty())
            players.melodic.push_back (sorted.front().id);
        for (size_t i = players.melodic.size(); i > 1; --i)
            std::swap (players.melodic[i - 1], players.melodic[(size_t) rng.nextInt ((int) i)]);
        players.melodic.resize (std::min<size_t> (players.melodic.size(), (size_t) (1 + rng.nextInt (3))));

        // The bass / log drum: low and long, not the kick when another one will do.
        players.bass = players.kick;
        float top = -1.0e9f;
        for (const auto& c : chops)
            if (c.id != players.snare && c.id != players.hat && (c.id != players.kick || chops.size() < 5))
                if (const auto s = bassScore (c); s > top && c.low > 0.2f) { top = s; players.bass = c.id; }

        // The hook: a chop that is none of the drums.
        std::vector<int> pool;
        for (const auto& c : chops)
            if (c.id != players.kick && c.id != players.snare && c.id != players.hat && c.id != players.hat2 && c.id != players.bass)
                pool.push_back (c.id);
        if (! pool.empty() && odds (0.8f))
            players.hook = oneOf (pool);
    }

    juce::String Generator::chooseForm (int sections)
    {
        // The shape of the whole: which parts return. Odder forms with more experiment.
        if (sections <= 1)
            return "A";
        if (sections == 2)
            return odds (0.45f + 0.3f * mode.variation) ? "AB" : "AA";
        const std::vector<const char*> forms { "AABA", "ABAB", "AAAB", "ABCB", "ABAC", "AABC", "ABCD" };
        const auto e = mode.experimental;
        return forms[(size_t) weighted ({ 3.0f, 2.5f, 2.0f + (1.0f - mode.variation), 1.5f, 1.5f, 1.2f, 0.2f + 2.5f * e })];
    }

    std::vector<float> Generator::densityCurve (int sections)
    {
        // How full each part is: a shape over the whole, never flat from start to end.
        struct Shape { const char* name; std::array<float, 4> values; };
        static const std::vector<Shape> shapes {
            { "building up",            { -0.6f, -0.2f, 0.3f, 0.8f } },
            { "thinning out",           { 0.6f, 0.3f, -0.2f, -0.6f } },
            { "in waves",               { -0.4f, 0.6f, -0.4f, 0.7f } },
            { "back and forth",         { 0.3f, -0.6f, 0.3f, -0.4f } },
            { "sparse - dense - sparse", { -0.6f, 0.5f, 0.7f, -0.4f } },
            { "dense - sparse - dense",  { 0.5f, -0.6f, -0.3f, 0.7f } },
            { "a drop in the middle",   { 0.2f, 0.4f, -0.8f, 0.6f } },
        };
        const auto& shape = shapes[(size_t) rng.nextInt ((int) shapes.size())];
        ctx.densityShape = sections > 1 ? shape.name : "";
        std::vector<float> out;
        for (int i = 0; i < sections; ++i)
        {
            const auto at = sections == 1 ? 0 : (sections == 2 ? (i == 0 ? 0 : 3) : i);
            const auto jitter = (rng.nextFloat() - 0.5f) * 0.2f;
            out.push_back (juce::jlimit (0.05f, 1.0f, mode.density * (1.0f + mode.densitySwing * 1.6f * shape.values[(size_t) at]) + jitter * mode.density));
        }
        return out;
    }

    //==============================================================================
    Bar Generator::genreBar (const std::string& line)
    {
        // A genre bar as the backbone: its symbols become the roll's players.
        const auto& style = styles()[(size_t) ctx.style];
        static const int eightOhEight[] = { 0, 0, 0, -1, 1, -2, 2, 3 }; // scale steps from the root
        static const int logDrum[] = { 0, 0, 1, 2, -1, 3, 0, -2 };
        auto either = [this] (int second, float p, int first) { return second != 0 && odds (p) ? second : first; };
        Bar bar;
        for (int s = 0; s < (int) line.size() && s < barSteps; ++s)
        {
            const auto c = line[(size_t) s];
            if (c == '-' && ! bar.empty() && bar.back().step + bar.back().length == s)
            {
                ++bar.back().length;
                continue;
            }
            if (c == '.' || c == '-')
                continue;
            Slot e;
            e.step = s;
            const auto onBeat = s % 4 == 0;
            switch (c)
            {
                case 'K': e.chopId = players.kick; e.velocity = onBeat ? 1.0f : 0.9f; e.priority = (s == 0 || s == 8) ? 3 : 2;
                          if (style.eightOhEight) { e.tuned = e.toTonic = true; e.degree = eightOhEight[rng.nextInt ((int) std::size (eightOhEight))]; }
                          break;
                case 'k': e.chopId = either (players.kick2, 0.6f, players.kick); e.velocity = 0.6f; e.chance = 0.8f; e.priority = 0; break;
                case 'S': e.chopId = either (players.snare2, 0.2f, players.snare); e.velocity = 0.95f; e.priority = (s == 4 || s == 12) ? 3 : 2; break;
                case 's': e.chopId = either (players.snare2, 0.5f, players.snare); e.velocity = 0.45f; e.chance = 0.7f; e.priority = 0; break;
                case 'r': e.chopId = players.snare; e.velocity = 0.75f; e.repeats = style.rollRepeats; break;
                case 'H': e.chopId = players.hat; e.velocity = onBeat ? 0.8f : 0.65f; break;
                case 'h': e.chopId = either (players.hat2, 0.5f, players.hat); e.velocity = 0.5f; e.chance = 0.85f; e.priority = 0; break;
                case 'R': e.chopId = players.hat; e.velocity = 0.7f; e.repeats = juce::jlimit (2, PatternEvent::maxRepeats, style.rollRepeats - rng.nextInt (2)); break;
                case 'T': e.chopId = players.hat; e.velocity = 0.7f; e.repeats = 3; break;
                case 'O': e.chopId = players.openHat != 0 ? players.openHat : players.hat; e.velocity = 0.75f; break;
                case 'P': e.chopId = players.perc; e.velocity = 0.7f; break;
                case 'X': e.chopId = chops[(size_t) rng.nextInt ((int) chops.size())].id; e.velocity = 0.7f; e.random = 1; e.chance = 0.8f; e.priority = 0; break;
                case 'C': e.chopId = players.hook; e.velocity = 0.8f; e.tuned = canTune (players.hook); e.priority = 2; break;
                case 'L': e.chopId = players.bass; e.velocity = onBeat ? 0.95f : 0.85f; e.tuned = e.toTonic = true;
                          e.degree = logDrum[rng.nextInt ((int) std::size (logDrum))]; e.priority = 2; break;
                case 'M': e.chopId = players.melodic[(size_t) (odds (0.6f) ? 0 : rng.nextInt ((int) players.melodic.size()))];
                          e.velocity = onBeat ? 0.95f : 0.8f; e.tuned = true; e.degree = rng.nextInt (5) - 1; e.priority = onBeat ? 2 : 1; break;
                default: continue;
            }
            if (e.chopId == 0)
                continue;
            if (s == 0)
                e.priority = 3;
            bar.push_back (e);
        }
        tidy (bar);
        return bar;
    }

    Bar Generator::motifBar (float density)
    {
        // A motif: a rhythm (euclidean, by the beat's weight, or off the beat) and a few chops
        // in a shape over it (A B A C ...). The one is always its head.
        const auto pulses = juce::jlimit (2, 14, (int) std::lround (density * (float) barSteps));
        std::set<int> onsets { 0 };
        const auto kind = weighted ({ 1.0f, 1.0f, 0.4f + 0.6f * mode.experimental });
        if (kind == 0)
        {
            const auto rotation = rng.nextInt (barSteps);
            // Euclidean: `pulses` hits spread as evenly as the 16 steps allow, turned round.
            for (int i = 0; i < barSteps; ++i)
                if ((i * pulses) % barSteps < pulses)
                    onsets.insert ((i + rotation) % barSteps);
        }
        else
        {
            const auto syncopation = kind == 2 ? 0.85f : 0.2f + 0.5f * mode.experimental;
            std::vector<float> w;
            for (int s = 0; s < barSteps; ++s)
                w.push_back (s == 0 ? 0.0f : std::pow (stepWeight[(size_t) s], 1.0f - syncopation) * (kind == 2 && s % 2 == 0 && s % 4 != 0 ? 1.6f : 1.0f));
            while ((int) onsets.size() < pulses)
            {
                const auto s = weighted (w);
                onsets.insert (s);
                w[(size_t) s] = 0.0f;
            }
        }

        // The motif's chops: a head (groove or impact), then a few others.
        const auto melodicMode = mode.tools[pitchTool] >= 1.0f; // MELODIC
        const auto headRole = melodicMode ? Role::melodic : (odds (0.5f) ? Role::groove : Role::impact);
        const auto chopLimit = std::min ((int) chops.size(), maxMotifChops);
        const auto count = juce::jlimit (1, chopLimit, 2 + rng.nextInt (std::max (1, chopLimit - 1)) - (mode.density < 0.3f ? 1 : 0));
        std::vector<int> motif { pickChop (headRole) };
        std::set<int> inMotif { motif.front() };
        static const std::array<Role, 5> others { Role::percussive, Role::fill, Role::groove, Role::melodic, Role::texture };
        while ((int) motif.size() < count)
        {
            const auto role = melodicMode ? (odds (0.7f) ? Role::melodic : Role::sustained) : others[(size_t) rng.nextInt ((int) others.size())];
            const auto id = pickChop (role, inMotif);
            motif.push_back (id);
            inMotif.insert (id);
        }
        for (const auto id : motif)
            ++uses[id];

        // Its shape over the onsets, and a little melody that comes back with it.
        static const std::vector<std::vector<int>> shapes { { 0, 1, 0, 2 }, { 0, 1, 0, 1 }, { 0, 0, 1, 2 }, { 0, 1, 2, 1 }, { 0, 1, 2, 3 }, { 0, 2, 1, 2 } };
        const auto& shape = oneOf (shapes);
        std::vector<int> degrees;
        for (int k = 0; k < 4; ++k)
            degrees.push_back (k == 0 ? 0 : (odds (0.5f) ? 0 : oneOf (std::vector<int> { -2, -1, 1, 2, 3 })));

        Bar bar;
        int index = 0;
        const std::vector<int> ordered (onsets.begin(), onsets.end());
        for (size_t i = 0; i < ordered.size(); ++i)
        {
            const auto s = ordered[i];
            const auto gap = (i + 1 < ordered.size() ? ordered[i + 1] : barSteps) - s;
            Slot e;
            e.step = s;
            const auto strong = s == 0 || s == 8;
            const auto which = strong ? 0 : std::min ((int) motif.size() - 1, shape[(size_t) (index % 4)]);
            ++index;
            e.chopId = motif[(size_t) which];
            e.velocity = s % 4 == 0 ? 0.95f : (s % 2 == 0 ? 0.82f : 0.7f);
            e.priority = s == 0 ? 3 : (strong || s % 4 == 0 ? 2 : (s % 2 == 0 ? 1 : 0));
            // Held notes: long chops ring on (HOLD), others a step or two.
            const auto own = stepsOf (e.chopId);
            e.length = ctx.tools[holdTool] && own > 1 ? std::min (gap, own) : std::min (gap, own >= 2 && odds (0.4f) ? 2 : 1);
            if (ctx.tools[pitchTool] && canTune (e.chopId))
            {
                e.tuned = true;
                e.degree = degrees[(size_t) which];
            }
            if (ctx.tools[ghostTool] && e.priority == 0 && odds (0.5f))
            {
                e.chance = 0.6f + 0.25f * rng.nextFloat();
                e.velocity *= 0.75f;
            }
            if (ctx.tools[ratchetTool] && e.priority <= 1 && gap <= 2 && odds (0.15f))
                e.repeats = 2 + rng.nextInt (2);
            if (ctx.tools[scatterTool] && e.priority == 0 && odds (0.3f))
                e.random = 1;
            bar.push_back (e);
        }
        tidy (bar);
        return bar;
    }

    //==============================================================================
    Bar Generator::transform (const Bar& source, float amount)
    {
        // An idea coming back: the same, but a few things changed - more the later it comes.
        // The one (its anchor) always stays, so it is recognisably the same idea.
        auto bar = source;
        if (bar.empty())
            return bar;
        auto ops = (int) std::floor (amount * 3.0f + rng.nextFloat());
        enum Op { substitute, lastNote, firstNote, shift, drop, pickup, transpose, ratchet, ghost, reorder, numOps };
        std::array<float, numOps> w { 1.0f, 1.0f, 0.5f, 0.6f, 0.6f, 0.5f,
                                      ctx.tools[pitchTool] ? 1.0f : 0.0f, ctx.tools[ratchetTool] ? 0.7f : 0.0f,
                                      ctx.tools[ghostTool] ? 0.7f : 0.0f, 0.4f * mode.experimental };
        auto weak = [&bar] (bool allowAnchor = false)
        {
            std::vector<size_t> out;
            for (size_t i = 0; i < bar.size(); ++i)
                if (bar[i].step != 0 && (allowAnchor || bar[i].priority < 3))
                    out.push_back (i);
            return out;
        };
        while (ops-- > 0)
        {
            const auto op = (Op) weighted (std::vector<float> (w.begin(), w.end()));
            const auto candidates = weak();
            if (candidates.empty())
                break;
            auto& e = bar[oneOf (candidates)];
            switch (op)
            {
                case substitute: { const auto id = pickChop (Role::groove, { e.chopId }); ++uses[id]; e.chopId = id; break; }
                case lastNote:
                {
                    auto& last = bar[weak().back()];
                    if (last.tuned && odds (0.5f)) last.degree += odds (0.5f) ? 1 : -1;
                    else { const auto id = pickChop (Role::fill, { last.chopId }); ++uses[id]; last.chopId = id; }
                    break;
                }
                case firstNote: { auto& first = bar[weak().front()]; const auto id = pickChop (Role::percussive, { first.chopId }); ++uses[id]; first.chopId = id; break; }
                case shift:
                {
                    const auto to = e.step + (odds (0.5f) ? 1 : -1);
                    if (to > 0 && to < barSteps && ! taken (bar, to)) { e.step = to; e.length = 1; }
                    break;
                }
                case drop: if (e.priority <= 1) e.chopId = 0; break; // space where a note was
                case pickup:
                {
                    // A pickup: a short hit just before a strong one.
                    for (const auto& head : bar)
                        if (head.priority >= 2 && head.step > 0 && ! taken (bar, head.step - 1))
                        {
                            Slot p;
                            p.step = head.step - 1;
                            p.chopId = pickChop (Role::fill);
                            p.velocity = 0.6f;
                            p.priority = 0;
                            bar.push_back (p);
                            break;
                        }
                    break;
                }
                case transpose:
                {
                    const auto by = oneOf (std::vector<int> { 1, -1, 2, 5, -5 }); // a step, two, an octave (5 pentatonic steps)
                    for (auto& t : bar)
                        if (t.tuned && ! t.toTonic) t.degree += by;
                    break;
                }
                case ratchet: e.repeats = juce::jlimit (2, PatternEvent::maxRepeats, 2 + rng.nextInt (3)); break;
                case ghost: e.chance = 0.6f + 0.25f * rng.nextFloat(); e.velocity *= 0.75f; break;
                case reorder:
                {
                    auto& other = bar[oneOf (candidates)];
                    std::swap (e.chopId, other.chopId);
                    break;
                }
                case numOps: break;
            }
            bar.erase (std::remove_if (bar.begin(), bar.end(), [] (const Slot& s) { return s.chopId == 0; }), bar.end());
            tidy (bar);
        }
        return bar;
    }

    void Generator::fitDensity (Bar& bar, float target)
    {
        // Thin out (the weakest first, the one never) or fill in (on the steps that count
        // most), so the parts follow the roll's density curve.
        const auto wanted = juce::jlimit (1, 15, (int) std::lround (target * (float) barSteps));
        while ((int) bar.size() > wanted)
        {
            auto worst = bar.end();
            for (auto it = bar.begin(); it != bar.end(); ++it)
                if (it->step != 0 && it->priority < 3
                    && (worst == bar.end() || it->priority < worst->priority
                        || (it->priority == worst->priority && stepWeight[(size_t) it->step] < stepWeight[(size_t) worst->step])))
                    worst = it;
            if (worst == bar.end())
                break;
            bar.erase (worst);
        }
        int guard = 0;
        while ((int) bar.size() < wanted && ++guard < 32)
        {
            std::vector<float> w;
            for (int s = 0; s < barSteps; ++s)
                w.push_back (taken (bar, s) ? 0.0f : stepWeight[(size_t) s] * (s % 2 == 1 ? 0.7f : 1.0f));
            if (std::accumulate (w.begin(), w.end(), 0.0f) <= 0.0f)
                break;
            Slot e;
            e.step = weighted (w);
            e.chopId = backbone && players.hat != 0 && odds (0.6f) ? players.hat : pickChop (odds (0.5f) ? Role::percussive : Role::fill);
            e.velocity = 0.6f;
            e.priority = 0;
            if (ctx.tools[ghostTool] && odds (0.6f))
                e.chance = 0.7f;
            bar.push_back (e);
            tidy (bar);
        }
    }

    Bar Generator::fillSteps (const std::string& fill, int from)
    {
        auto line = std::string ((size_t) from, '.') + fill;
        line.resize ((size_t) barSteps, '.');
        return genreBar (line);
    }

    void Generator::transition (Bar& bar, bool genre)
    {
        // The end of a phrase leading into the next: a fill, a pickup, a gap, a run up, a held note.
        enum Kind { fill, pickup, gap, run, hold, styleFill, numKinds };
        const auto& style = styles()[(size_t) ctx.style];
        const auto kind = weighted ({ ctx.tools[ratchetTool] ? 1.0f : 0.3f, 0.8f, 0.4f + 2.0f * mode.silence,
                                      ctx.tools[pitchTool] && tonal ? 1.0f : 0.0f, ctx.tools[holdTool] ? 0.8f : 0.0f,
                                      genre && ! style.fills.empty() ? 1.2f : 0.0f });
        const auto from = odds (0.6f) ? 12 : 14;
        auto clearFrom = [&bar] (int s) { bar.erase (std::remove_if (bar.begin(), bar.end(), [s] (const Slot& e) { return e.step >= s && e.step != 0; }), bar.end()); };
        switch (kind)
        {
            case fill:
            {
                clearFrom (from);
                const auto id = backbone ? players.snare : pickChop (Role::fill);
                for (int s = from; s < barSteps; ++s)
                {
                    Slot e;
                    e.step = s;
                    e.chopId = id;
                    e.velocity = 0.65f + 0.08f * (float) (s - from);
                    e.repeats = ctx.tools[ratchetTool] ? juce::jlimit (1, PatternEvent::maxRepeats, 1 + (s - from)) : 1;
                    bar.push_back (e);
                }
                break;
            }
            case pickup:
            {
                for (int s : { 15, 14 })
                    if (! taken (bar, s) && (s == 15 || odds (0.5f)))
                    {
                        Slot e;
                        e.step = s;
                        e.chopId = pickChop (Role::fill);
                        e.velocity = s == 15 ? 0.75f : 0.6f;
                        bar.push_back (e);
                    }
                break;
            }
            case gap: clearFrom (from); break; // silence before the next phrase
            case run:
            {
                clearFrom (from);
                const auto id = players.melodic.front();
                const auto up = odds (0.6f);
                for (int s = from; s < barSteps; ++s)
                {
                    Slot e;
                    e.step = s;
                    e.chopId = id;
                    e.tuned = true;
                    e.degree = (up ? 1 : -1) * (s - from);
                    e.velocity = 0.7f + 0.06f * (float) (s - from);
                    bar.push_back (e);
                }
                break;
            }
            case hold:
            {
                clearFrom (12);
                Slot e;
                e.step = 12;
                e.length = 4;
                e.chopId = pickChop (Role::sustained);
                e.velocity = 0.85f;
                e.tuned = ctx.tools[pitchTool] && canTune (e.chopId);
                bar.push_back (e);
                break;
            }
            case styleFill:
            {
                clearFrom (12);
                for (const auto& e : fillSteps (oneOf (style.fills), 12))
                    bar.push_back (e);
                break;
            }
            default: break;
        }
        tidy (bar);
    }

    void Generator::ending (Bar& bar, const Bar& opening, bool genre)
    {
        // The last bar: a strong final hit, a held note, a fill, a stutter out, an empty last
        // beat, a fall in pitch, or the opening coming back.
        enum Kind { strong, sustain, fill, stutter, empty, pitchDown, callback, numKinds };
        static const char* names[] = { "a strong last hit", "a held ending", "a fill to finish", "a stutter out",
                                       "an empty last beat", "falling at the end", "the opening again at the end" };
        const auto kind = (Kind) weighted ({ 0.8f, ctx.tools[holdTool] ? 0.9f : 0.3f, ctx.tools[ratchetTool] || genre ? 1.0f : 0.4f, 0.6f,
                                             0.4f + mode.silence, ctx.tools[pitchTool] && tonal ? 0.8f : 0.0f, 0.6f });
        ctx.ending = names[kind];
        auto clearFrom = [&bar] (int s) { bar.erase (std::remove_if (bar.begin(), bar.end(), [s] (const Slot& e) { return e.step >= s && e.step != 0; }), bar.end()); };
        switch (kind)
        {
            case strong:
            {
                clearFrom (12);
                Slot e;
                e.step = 12;
                e.length = 2;
                e.chopId = backbone ? players.kick : pickChop (Role::impact);
                e.velocity = 1.0f;
                e.priority = 2;
                bar.push_back (e);
                break;
            }
            case sustain:
            {
                const auto from = odds (0.5f) ? 8 : 12;
                clearFrom (from);
                Slot e;
                e.step = from;
                e.length = barSteps - from;
                e.chopId = pickChop (Role::sustained);
                e.velocity = 0.9f;
                bar.push_back (e);
                break;
            }
            case fill:   transition (bar, genre); break;
            case stutter:
            {
                clearFrom (12);
                const auto id = bar.empty() ? chops.front().id : bar.back().chopId;
                for (int s = 12; s < barSteps; ++s)
                {
                    Slot e;
                    e.step = s;
                    e.chopId = id;
                    e.velocity = 0.9f - 0.12f * (float) (s - 12);
                    bar.push_back (e);
                }
                break;
            }
            case empty: clearFrom (12); break;
            case pitchDown:
            {
                clearFrom (12);
                Slot e;
                e.step = 12;
                e.length = 4;
                e.chopId = players.melodic.front();
                e.tuned = true;
                e.degree = odds (0.5f) ? -5 : -2; // an octave or two steps down
                e.velocity = 0.9f;
                bar.push_back (e);
                break;
            }
            case callback:
            {
                clearFrom (12);
                for (const auto& o : opening)
                    if (o.step < 4)
                    {
                        auto e = o;
                        e.step += 12;
                        e.priority = std::min (e.priority, 2);
                        bar.push_back (e);
                    }
                break;
            }
            case numKinds: break;
        }
        tidy (bar);
    }

    //==============================================================================
    std::vector<PatternEvent> Generator::run()
    {
        const auto bars = juce::jlimit (1, 8, options.bars);
        const auto punchiest = std::max_element (chops.begin(), chops.end(), [] (const ChopTraits& a, const ChopTraits& b) { return a.punch < b.punch; });
        // Drums: hits - short and hard - among the chops (a sung phrase starts hard too, but rings on).
        const auto hits = std::count_if (chops.begin(), chops.end(), [] (const ChopTraits& c) { return c.punch > 0.25f && c.sustain < 0.35f; });
        drums = punchiest->punch > 0.15f && chops.size() >= 2 && hits >= std::max<std::ptrdiff_t> (1, (std::ptrdiff_t) chops.size() / 3);
        // Tonal: chops that ring on with a pitch heard (a dense drum loop rings on too, without one).
        const auto tones = std::count_if (chops.begin(), chops.end(), [] (const ChopTraits& c) { return c.sustain > 0.25f && c.pitchClass >= 0; });
        tonal = tones >= std::max<std::ptrdiff_t> (1, (std::ptrdiff_t) chops.size() / 5);
        std::vector<double> lengths;
        for (const auto& c : chops)
            lengths.push_back (c.seconds);
        std::nth_element (lengths.begin(), lengths.begin() + (std::ptrdiff_t) lengths.size() / 2, lengths.end());
        const auto slices = chops.size() >= 3 && lengths[lengths.size() / 2] >= stepSeconds * 2.0;
        const auto placed = placesInLoop (chops, options.bpm, options.originSeconds).size() >= 3; // the loop's own groove is there

        ctx.style = options.style >= 0 && options.style < numStyles ? options.style
                  : pickStyle (options.bpm, drums, tonal, slices, placed, options.avoidStyle, rng);
        if (ctx.style == chopUp)
        {
            ctx.form = placed ? "the loop's own groove, flipped" : "SLICES IN ORDER";
            return chopUpBars (chops, bars, options.bpm, options.originSeconds, rng);
        }

        chooseTools();
        choosePlayers();
        // A genre bar as the backbone (drum styles, when the sample has drums), or motifs.
        backbone = (isDrumStyle (ctx.style) && drums && odds (mode.templateUse)) || ctx.style == flip;
        ctx.backbone = backbone;
        const auto& style = styles()[(size_t) ctx.style];

        // The plan: parts of one or two bars (one bar: the whole of it), a form, a density curve.
        const auto sections = bars == 1 ? 1 : (bars == 2 ? 2 : 4);
        const auto barsPerSection = bars / sections;
        ctx.form = chooseForm (sections);
        const auto curve = densityCurve (sections);

        // The ideas: each letter of the form once, then varied when it returns.
        std::map<juce::juce_wchar, Bar> ideas;
        std::map<juce::juce_wchar, int> seen;
        std::vector<Bar> sheet;
        Bar opening;
        for (int part = 0; part < sections; ++part)
        {
            const auto letter = ctx.form[part];
            if (ideas.find (letter) == ideas.end())
            {
                Bar idea;
                if (backbone)
                {
                    auto line = std::string (oneOf (style.bars));
                    line = varyBar (line, rng, 0.08f + 0.1f * mode.variation);
                    // The hook: on the same few offbeats, as the idea's own colour.
                    static const std::vector<std::vector<int>> hookSpots { { 3 }, { 6 }, { 7 }, { 10 }, { 14 }, { 3, 11 }, { 6, 14 }, { 7, 15 }, { 2, 10 }, { 11, 14 } };
                    if (players.hook != 0 && odds (0.7f))
                        for (const auto p : oneOf (hookSpots))
                            if (auto& c = line[(size_t) p]; c == '.' || c == 'h' || c == 'H' || c == 'k')
                                c = 'C';
                    idea = genreBar (line);
                }
                else
                    idea = motifBar (curve[(size_t) part]);
                ideas[letter] = idea;
            }
            const auto occurrence = seen[letter]++;
            const auto amount = occurrence == 0 ? firstBarVariation
                                                : mode.variation * (0.5f + 0.25f * (float) occurrence) + 0.1f * (float) part / (float) sections;
            auto bar = transform (ideas[letter], amount);
            if (! backbone || odds (0.5f))
                fitDensity (bar, backbone ? std::max (curve[(size_t) part], (float) bar.size() / (float) barSteps * 0.8f) : curve[(size_t) part]);
            if (part == 0)
                opening = bar;
            for (int b = 0; b < barsPerSection; ++b)
            {
                // The second bar of a two-bar part answers the first.
                sheet.push_back (b == 0 ? bar : transform (bar, answerAmount + 0.3f * mode.variation));
            }
        }

        // Phrase ends: a transition now and then (not at every boundary); the last bar its ending.
        for (int part = 0; part + 1 < sections; ++part)
        {
            const auto boundary = (part + 1) * barsPerSection - 1;
            const auto middle = part + 1 == sections / 2;
            if (odds (mode.transition * (middle ? 1.15f : 0.75f)))
                transition (sheet[(size_t) boundary], backbone);
        }
        if (bars > 1 || odds (0.5f))
            ending (sheet.back(), opening, backbone);

        // Energy: the denser parts a little louder; the one always there.
        std::vector<PatternEvent> events;
        for (int b = 0; b < bars; ++b)
        {
            auto& bar = sheet[(size_t) b];
            tidy (bar);
            if (bar.empty() || bar.front().step != 0)
            {
                Slot one;
                one.chopId = opening.empty() ? chops.front().id : opening.front().chopId;
                one.velocity = 0.95f;
                one.priority = 3;
                bar.insert (bar.begin(), one);
                tidy (bar);
            }
            const auto energy = 0.85f + 0.15f * juce::jlimit (0.0f, 1.0f, (float) bar.size() / (float) barSteps / std::max (0.1f, mode.density * 1.4f));
            for (const auto& s : bar)
            {
                PatternEvent e;
                e.step = b * barSteps + s.step;
                e.length = s.length;
                e.chopId = s.chopId;
                const auto humanise = s.priority >= 3 ? 0.0f : (rng.nextFloat() - 0.5f) * 0.06f; // a touch, never on the anchors
                e.velocity = juce::jlimit (0.05f, 1.0f, s.velocity * energy + humanise);
                if (s.tuned)
                    e.pitch = juce::jlimit (-PatternEvent::maxPitch, PatternEvent::maxPitch, noteAt (notesOf (s.chopId, s.toTonic), s.degree));
                e.chance = juce::jlimit (0.05f, 1.0f, s.chance);
                e.repeats = juce::jlimit (1, PatternEvent::maxRepeats, s.repeats);
                e.random = s.random;
                events.push_back (e);
            }
        }

        // Slides: from a tuned note into the next one close behind it (SLIDE, or an 808).
        if (ctx.tools[slideTool] || style.eightOhEight || ctx.style == amapiano)
            for (size_t i = 0; i + 1 < events.size(); ++i)
            {
                auto& e = events[i];
                const auto& next = events[i + 1];
                if (next.step - e.step <= 4 && next.chopId == e.chopId && next.pitch != e.pitch && (e.pitch != 0 || next.pitch != 0) && odds (0.55f))
                    e.slide = true;
            }

        if (events.size() > (size_t) PatternData::maxEvents)
            events.resize ((size_t) PatternData::maxEvents);
        return events;
    }
}

std::vector<PatternEvent> generate (const std::vector<ChopTraits>& chops, const Options& options, GenerationContext& context)
{
    if (chops.empty() || options.bars < 1)
        return {};
    context.mode = &modes()[(size_t) juce::jlimit (0, (int) modes().size() - 1, options.mode)];
    Generator generator (chops, options, context);
    return generator.run();
}

} // namespace kerf::PatternDice::detail

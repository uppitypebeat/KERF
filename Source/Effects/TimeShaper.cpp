#include "TimeShaper.h"

namespace kerf
{

//==============================================================================
float TimeCurve::valueAt (float position) const noexcept
{
    if (numPoints <= 0)
        return 0.0f;
    if (position <= x[0])
        return y[0];
    // The last point at or before the position (after a jump: the point after it).
    int i = 0;
    while (i + 1 < numPoints && x[(size_t) i + 1] <= position)
        ++i;
    if (i + 1 >= numPoints)
        return y[(size_t) i];
    const auto span = x[(size_t) i + 1] - x[(size_t) i];
    const auto t = span > 0.0f ? (position - x[(size_t) i]) / span : 1.0f;
    return y[(size_t) i] + t * (y[(size_t) i + 1] - y[(size_t) i]);
}

float TimeCurve::slopeAt (float position) const noexcept
{
    int i = 0;
    while (i + 1 < numPoints && x[(size_t) i + 1] <= position)
        ++i;
    if (i + 1 >= numPoints)
        return 0.0f;
    const auto dx = x[(size_t) i + 1] - x[(size_t) i];
    return dx > 1.0e-4f ? (y[(size_t) i + 1] - y[(size_t) i]) / dx : 0.0f;
}

void TimeCurve::sort() noexcept
{
    // Insertion sort: stable, and the curves are short.
    for (int i = 1; i < numPoints; ++i)
        for (int j = i; j > 0 && x[(size_t) j - 1] > x[(size_t) j]; --j)
        {
            std::swap (x[(size_t) j - 1], x[(size_t) j]);
            std::swap (y[(size_t) j - 1], y[(size_t) j]);
        }
}

juce::String TimeCurve::toString() const
{
    juce::StringArray parts;
    for (int i = 0; i < numPoints; ++i)
        parts.add (juce::String (x[(size_t) i], 5) + "," + juce::String (y[(size_t) i], 5));
    return parts.joinIntoString (" ");
}

TimeCurve TimeCurve::fromString (const juce::String& text)
{
    TimeCurve c;
    for (const auto& token : juce::StringArray::fromTokens (text, " ", {}))
        if (token.containsChar (','))
            c.add (juce::jlimit (0.0f, 1.0f, token.upToFirstOccurrenceOf (",", false, false).getFloatValue()),
                   juce::jlimit (0.0f, 1.0f, token.fromFirstOccurrenceOf (",", false, false).getFloatValue()));
    c.sort();
    return c;
}

bool TimeCurve::operator== (const TimeCurve& o) const noexcept
{
    if (numPoints != o.numPoints)
        return false;
    for (int i = 0; i < numPoints; ++i)
        if (std::abs (x[(size_t) i] - o.x[(size_t) i]) > 1.0e-4f || std::abs (y[(size_t) i] - o.y[(size_t) i]) > 1.0e-4f)
            return false;
    return true;
}

//==============================================================================
namespace TimeFactory
{
    namespace
    {
        // The order the slots are built in below (the shapes refer to each other by it).
        const char* const builtNames[numSlots] = { "HALF TIME", "TAPE STOP", "SPIN UP", "STUTTER 8", "STUTTER 16", "REVERSE",
                                                   "SCRATCH", "SLOW DOWN", "BEAT REPEAT", "DRAG", "GATE 16", "SIDECHAIN",
                                                   "STUTTER 3", "ROLL", "REWIND", "WARBLE", "GLITCH", "GATE 332", "SWELL", "PUMP 4",
                                                   "HALF FIRST", "SLOWMO", "QUARTER", "STUTTER 4", "STUTTER 32", "ROLL DOWN", "ROLL 3", "FILL 16", "BOUNCE", "REVERSE 8",
                                                   "REV HALF", "ECHO BEAT", "REPEAT 2", "DOTTED", "POLY 3", "SHUFFLE", "LAZY", "BRAKE", "STOP START", "SPINBACK",
                                                   "WOW", "FLUTTER", "SCRATCH 8", "GATE 8", "GATE 4", "GATE 3", "GATE DOT", "OFFBEAT", "TRANCE 16", "PUMP 8",
                                                   "TREMOLO", "BREATHE", "RISE", "DROP", "ECHO", "REV SWELL", "HALF GATE", "TAPE FADE", "STUTTER FADE", "BUILD UP" };

        // The order on the page, in themed banks of 20 (projects store this index).
        const char* const shownNames[numSlots] = {
            // A: tempo and tape
            "HALF TIME", "HALF FIRST", "SLOWMO", "QUARTER", "DRAG", "LAZY", "SHUFFLE", "TAPE STOP", "SLOW DOWN", "BRAKE",
            "SPIN UP", "STOP START", "WOW", "WARBLE", "FLUTTER", "REWIND", "SPINBACK", "REVERSE", "REVERSE 8", "REV HALF",
            // B: repeats and stutters
            "STUTTER 3", "STUTTER 4", "STUTTER 8", "STUTTER 16", "STUTTER 32", "ROLL", "ROLL DOWN", "ROLL 3", "FILL 16", "BEAT REPEAT",
            "REPEAT 2", "ECHO BEAT", "DOTTED", "POLY 3", "BOUNCE", "SCRATCH", "SCRATCH 8", "GLITCH", "BUILD UP", "STUTTER FADE",
            // C: gates and volume
            "GATE 16", "GATE 8", "GATE 4", "GATE 3", "GATE DOT", "GATE 332", "OFFBEAT", "TRANCE 16", "HALF GATE", "SIDECHAIN",
            "PUMP 8", "TREMOLO", "BREATHE", "SWELL", "REV SWELL", "RISE", "DROP", "ECHO", "TAPE FADE", "PUMP 4" };

        int builtIndex (const char* name)
        {
            for (int k = 0; k < numSlots; ++k)
                if (juce::String (builtNames[k]) == name)
                    return k;
            jassertfalse;
            return 0;
        }
    }

    const char* name (int slot)
    {
        return shownNames[juce::jlimit (0, numSlots - 1, slot)];
    }

    const std::array<TimeSlot, numSlots>& slots()
    {
        // Built in beats of a 4-beat cycle (x = beat / 4, y = beats behind / 4), in the build
        // order above, then put into the order of the page.
        static const auto built = []
        {
            std::array<TimeSlot, numSlots> s;
            auto at = [] (TimeCurve& c, float beat, float lagBeats) { c.add (beat / 4.0f, lagBeats / 4.0f); };
            for (auto& slot : s) { slot.time = {}; }

            // HALF TIME: the bar plays at half speed (2 beats behind by its end).
            at (s[0].time, 0.0f, 0.0f); at (s[0].time, 4.0f, 2.0f);

            // TAPE STOP: normal for two beats, then slowing to a stop.
            at (s[1].time, 0.0f, 0.0f);
            for (int k = 0; k <= 8; ++k) { const auto t = 2.0f + 0.25f * (float) k; at (s[1].time, t, (t - 2.0f) * (t - 2.0f) / 4.0f); }

            // SPIN UP: starts from standstill and speeds up to normal by beat 3.
            for (int k = 0; k <= 8; ++k) { const auto t = 0.25f * (float) k; at (s[2].time, t, t - t * t / 4.0f); }
            at (s[2].time, 4.0f, 1.0f);

            // STUTTER 8: every eighth plays its first sixteenth twice.
            for (int k = 0; k < 8; ++k) { const auto b = 0.5f * (float) k; at (s[3].time, b, 0.0f); at (s[3].time, b + 0.25f, 0.0f); at (s[3].time, b + 0.25f, 0.25f); at (s[3].time, b + 0.5f, 0.25f); }

            // STUTTER 16: every sixteenth plays its first thirty-second twice.
            for (int k = 0; k < 16; ++k) { const auto b = 0.25f * (float) k; at (s[4].time, b, 0.0f); at (s[4].time, b + 0.125f, 0.0f); at (s[4].time, b + 0.125f, 0.125f); at (s[4].time, b + 0.25f, 0.125f); }

            // REVERSE: every beat plays the one before it backwards.
            for (int k = 0; k < 4; ++k) { at (s[5].time, (float) k, 0.0f); at (s[5].time, (float) k + 1.0f, 2.0f); }

            // SCRATCH: a quick back-and-forth on every beat, then it runs on.
            for (int k = 0; k < 4; ++k) { const auto b = (float) k; at (s[6].time, b, 0.0f); at (s[6].time, b + 0.25f, 0.5f); at (s[6].time, b + 0.5f, 0.0f); at (s[6].time, b + 1.0f, 0.0f); }

            // SLOW DOWN: the whole bar slows to a stop.
            for (int k = 0; k <= 8; ++k) { const auto t = 0.5f * (float) k; at (s[7].time, t, t * t / 8.0f); }

            // BEAT REPEAT: the first beat four times.
            for (int k = 0; k < 4; ++k) { at (s[8].time, (float) k, (float) k); at (s[8].time, (float) k + 1.0f, (float) k); }

            // DRAG: beats 3 and 4 at half speed.
            at (s[9].time, 0.0f, 0.0f); at (s[9].time, 2.0f, 0.0f); at (s[9].time, 4.0f, 1.0f);

            // GATE 16: open for 60 % of every sixteenth.
            s[10].time = TimeCurve::flat (0.0f);
            s[10].volume = {};
            for (int k = 0; k < 16; ++k) { const auto b = 0.25f * (float) k; at (s[10].volume, b, 4.0f); at (s[10].volume, b + 0.15f, 4.0f); at (s[10].volume, b + 0.15f, 0.0f); at (s[10].volume, b + 0.25f, 0.0f); }

            // SIDECHAIN: ducked on every beat, swelling back.
            s[11].time = TimeCurve::flat (0.0f);
            s[11].volume = {};
            for (int k = 0; k < 4; ++k)
            {
                const auto b = (float) k;
                at (s[11].volume, b, 0.0f); at (s[11].volume, b + 0.08f, 0.6f); at (s[11].volume, b + 0.25f, 2.4f);
                at (s[11].volume, b + 0.5f, 3.7f); at (s[11].volume, b + 1.0f, 4.0f);
            }

            // STUTTER 3: in triplets: every triplet eighth plays its first half twice.
            for (int k = 0; k < 12; ++k)
            {
                const auto b = (float) k / 3.0f, h = 1.0f / 6.0f;
                at (s[12].time, b, 0.0f); at (s[12].time, b + h, 0.0f); at (s[12].time, b + h, h); at (s[12].time, b + 2.0f * h, h);
            }

            // ROLL: a drum roll building up: beat 1 plays, then each beat repeats its start in
            // ever shorter pieces (eighths, sixteenths, thirty-seconds).
            at (s[13].time, 0.0f, 0.0f); at (s[13].time, 1.0f, 0.0f);
            for (int k = 1; k < 4; ++k)
            {
                const auto piece = 1.0f / (float) (1 << k); // 1/2, 1/4, 1/8 of a beat
                for (int j = 0; j < (1 << k); ++j)
                {
                    const auto b = (float) k + piece * (float) j;
                    at (s[13].time, b, piece * (float) j); at (s[13].time, b + piece, piece * (float) j);
                }
            }

            // REWIND: three beats play, the last rewinds them at double speed.
            at (s[14].time, 0.0f, 0.0f); at (s[14].time, 3.0f, 0.0f); at (s[14].time, 4.0f, 3.0f);

            // WARBLE: a worn tape: the speed wavers round normal once a beat (the pitch with it).
            for (int k = 0; k <= 32; ++k)
            {
                const auto t = 0.125f * (float) k;
                at (s[15].time, t, 0.03f * (1.0f - std::cos (2.0f * juce::MathConstants<float>::pi * t)));
            }

            // GLITCH: sixteenths that repeat, stick and flip at odd places.
            {
                // Per sixteenth: how many sixteenths back it reads (-1: the one before, backwards).
                static constexpr int back[16] = { 0, 0, 0, 1, 0, 0, 1, 2, 0, 0, 0, -1, 0, 1, 1, 2 };
                for (int k = 0; k < 16; ++k)
                {
                    const auto b = 0.25f * (float) k;
                    if (back[k] < 0) { at (s[16].time, b, 0.0f); at (s[16].time, b + 0.25f, 0.5f); }
                    else             { at (s[16].time, b, 0.25f * (float) back[k]); at (s[16].time, b + 0.25f, 0.25f * (float) back[k]); }
                }
            }

            // GATE 332: a trance gate on the 3-3-2 rhythm (sixteenths 1, 4, 7, 9, 12, 15).
            s[17].time = TimeCurve::flat (0.0f);
            s[17].volume = {};
            at (s[17].volume, 0.0f, 0.0f);
            for (const auto step : { 0, 3, 6, 8, 11, 14 })
            {
                const auto b = 0.25f * (float) step;
                at (s[17].volume, b, 0.0f); at (s[17].volume, b, 4.0f); at (s[17].volume, b + 0.4f, 4.0f); at (s[17].volume, b + 0.4f, 0.0f);
            }
            at (s[17].volume, 4.0f, 0.0f);

            // SWELL: every beat fades in from silence, like a sound played backwards.
            s[18].time = TimeCurve::flat (0.0f);
            s[18].volume = {};
            for (int k = 0; k < 4; ++k)
            {
                const auto b = (float) k;
                at (s[18].volume, b, 0.0f); at (s[18].volume, b + 0.7f, 2.6f); at (s[18].volume, b + 0.95f, 4.0f); at (s[18].volume, b + 1.0f, 4.0f);
            }

            // --- Bank B: time ---------------------------------------------------------
            // A stretch of the bar in pieces of `piece` beats, each reading from `from` again.
            auto repeatPieces = [&at] (TimeCurve& c, float from, float to, float piece)
            {
                for (int j = 0; from + piece * (float) j < to - 1.0e-4f; ++j) // by index: no drift
                {
                    const auto lag = piece * (float) j;
                    const auto end = from + piece * (float) (j + 1);
                    at (c, from + lag, lag); at (c, end > to - 1.0e-4f ? to : end, lag);
                }
            };
            // Volume: open from each start for `length` beats, closed in between.
            auto every = [] (float step, float offset = 0.0f)
            {
                std::vector<float> v;
                for (auto b = offset; b < 4.0f - 1.0e-4f; b += step) v.push_back (b);
                return v;
            };
            auto gate = [&at] (TimeCurve& c, const std::vector<float>& starts, float length)
            {
                c = {};
                at (c, 0.0f, 0.0f);
                for (const auto b : starts)
                {
                    const auto e = std::min (4.0f, b + length);
                    at (c, b, 0.0f); at (c, b, 4.0f); at (c, e, 4.0f); at (c, e, 0.0f);
                }
                at (c, 4.0f, 0.0f);
            };
            const auto twoPi = 2.0f * juce::MathConstants<float>::pi;

            // HALF FIRST: the first half of the bar at half speed, then live again.
            at (s[20].time, 0.0f, 0.0f); at (s[20].time, 2.0f, 1.0f); at (s[20].time, 2.0f, 0.0f); at (s[20].time, 4.0f, 0.0f);
            // SLOWMO: the bar at three quarters speed.
            at (s[21].time, 0.0f, 0.0f); at (s[21].time, 4.0f, 1.0f);
            // QUARTER: the bar at a quarter speed (deep and dark).
            at (s[22].time, 0.0f, 0.0f); at (s[22].time, 4.0f, 3.0f);
            // STUTTER 4: every beat plays its first eighth twice.
            for (int k = 0; k < 4; ++k) repeatPieces (s[23].time, (float) k, (float) k + 1.0f, 0.5f);
            // STUTTER 32: every eighth is its first thirty-second, four times.
            for (int k = 0; k < 8; ++k) repeatPieces (s[24].time, 0.5f * (float) k, 0.5f * (float) k + 0.5f, 0.125f);
            // ROLL DOWN: a roll the other way: thirty-seconds, sixteenths, eighths, then it plays.
            repeatPieces (s[25].time, 0.0f, 1.0f, 0.125f); repeatPieces (s[25].time, 1.0f, 2.0f, 0.25f);
            repeatPieces (s[25].time, 2.0f, 3.0f, 0.5f);   at (s[25].time, 3.0f, 0.0f); at (s[25].time, 4.0f, 0.0f);
            // ROLL 3: a roll in triplets.
            at (s[26].time, 0.0f, 0.0f); at (s[26].time, 1.0f, 0.0f);
            repeatPieces (s[26].time, 1.0f, 2.0f, 1.0f / 3.0f); repeatPieces (s[26].time, 2.0f, 3.0f, 1.0f / 6.0f); repeatPieces (s[26].time, 3.0f, 4.0f, 1.0f / 12.0f);
            // FILL 16: three beats play, the last one stutters its first sixteenth.
            at (s[27].time, 0.0f, 0.0f); at (s[27].time, 3.0f, 0.0f); repeatPieces (s[27].time, 3.0f, 4.0f, 0.25f);
            // BOUNCE: every eighth plays its first sixteenth, then that sixteenth backwards.
            for (int k = 0; k < 8; ++k) { const auto e = 0.5f * (float) k; at (s[28].time, e, 0.0f); at (s[28].time, e + 0.25f, 0.0f); at (s[28].time, e + 0.5f, 0.5f); }
            // REVERSE 8: every eighth plays the one before it backwards.
            for (int k = 0; k < 8; ++k) { const auto e = 0.5f * (float) k; at (s[29].time, e, 0.0f); at (s[29].time, e + 0.5f, 1.0f); }
            // REV HALF: the second half plays the first half backwards.
            at (s[30].time, 0.0f, 0.0f); at (s[30].time, 2.0f, 0.0f); at (s[30].time, 4.0f, 4.0f);
            // ECHO BEAT: beats 2 and 4 repeat beats 1 and 3.
            for (int k = 0; k < 4; k += 2) { const auto b = (float) k; at (s[31].time, b, 0.0f); at (s[31].time, b + 1.0f, 0.0f); at (s[31].time, b + 1.0f, 1.0f); at (s[31].time, b + 2.0f, 1.0f); }
            // REPEAT 2: the first half bar, twice.
            at (s[32].time, 0.0f, 0.0f); at (s[32].time, 2.0f, 0.0f); at (s[32].time, 2.0f, 2.0f); at (s[32].time, 4.0f, 2.0f);
            // DOTTED: the first dotted eighth over and over, across the beat (3 against 4).
            repeatPieces (s[33].time, 0.0f, 4.0f, 0.75f);
            // POLY 3: the first half-note triplet, three times over the bar.
            repeatPieces (s[34].time, 0.0f, 4.0f, 4.0f / 3.0f);
            // SHUFFLE: every second eighth comes a triplet late (a swing made of time).
            for (int k = 0; k < 4; ++k) { const auto b = (float) k; at (s[35].time, b, 0.0f); at (s[35].time, b + 0.5f, 0.0f); at (s[35].time, b + 0.5f, 1.0f / 6.0f); at (s[35].time, b + 1.0f, 1.0f / 6.0f); }
            // LAZY: the second half of every beat drags (0.7x), then catches up.
            for (int k = 0; k < 4; ++k) { const auto b = (float) k; at (s[36].time, b, 0.0f); at (s[36].time, b + 0.5f, 0.0f); at (s[36].time, b + 1.0f, 0.15f); }
            // BRAKE: a turntable brake on the last beat.
            at (s[37].time, 0.0f, 0.0f);
            for (int k = 0; k <= 8; ++k) { const auto t = 3.0f + 0.125f * (float) k; at (s[37].time, t, (t - 3.0f) * (t - 3.0f) / 2.0f); }
            // STOP START: slows to a stop by the half bar, then spins back up.
            for (int k = 0; k <= 16; ++k)
            {
                const auto t = 0.25f * (float) k;
                at (s[38].time, t, t <= 2.0f ? t * t / 4.0f : 1.0f + (t - 2.0f) - (t - 2.0f) * (t - 2.0f) / 4.0f);
            }
            // SPINBACK: a fast spin backwards at the end of the bar (3x).
            at (s[39].time, 0.0f, 0.0f); at (s[39].time, 3.5f, 0.0f); at (s[39].time, 4.0f, 2.0f);

            // --- Bank C: tape, gates and combinations ------------------------------------
            // WOW: a deep, slow wow (the pitch sways over two beats).
            for (int k = 0; k <= 32; ++k) { const auto t = 0.125f * (float) k; at (s[40].time, t, 0.1f * (1.0f - std::cos (0.5f * twoPi * t))); }
            // FLUTTER: a fast, small flutter (a worn capstan).
            for (int k = 0; k <= 48; ++k) { const auto t = (float) k / 12.0f; at (s[41].time, t, 0.006f * (1.0f - std::cos (2.0f * twoPi * t))); }
            // SCRATCH 8: a quick back-and-forth on every eighth.
            for (int k = 0; k < 8; ++k) { const auto e = 0.5f * (float) k; at (s[42].time, e, 0.0f); at (s[42].time, e + 0.125f, 0.25f); at (s[42].time, e + 0.25f, 0.0f); at (s[42].time, e + 0.5f, 0.0f); }
            // Gates and levels (time stays live).
            for (const auto k : { 43, 44, 45, 46, 47, 48, 49, 50, 51, 52, 53 }) s[(size_t) k].time = TimeCurve::flat (0.0f);
            gate (s[43].volume, every (0.5f), 0.3f);          // GATE 8
            gate (s[44].volume, every (1.0f), 0.6f);          // GATE 4
            gate (s[45].volume, every (1.0f / 3.0f), 0.2f);   // GATE 3: triplet eighths
            gate (s[46].volume, every (0.75f), 0.4f);         // GATE DOT: dotted eighths
            gate (s[47].volume, every (1.0f, 0.5f), 0.35f);   // OFFBEAT: only the off-beats
            // TRANCE 16: sixteenths with accents, each with a short attack and a decay.
            {
                static constexpr float levels[16] = { 1.0f, 0.35f, 0.7f, 0.35f, 1.0f, 0.35f, 0.7f, 1.0f, 0.35f, 1.0f, 0.7f, 0.35f, 1.0f, 0.35f, 0.7f, 0.35f };
                s[48].volume = {};
                at (s[48].volume, 0.0f, 0.0f);
                for (int k = 0; k < 16; ++k) { const auto b = 0.25f * (float) k; at (s[48].volume, b + 0.02f, 4.0f * levels[k]); at (s[48].volume, b + 0.2f, 0.0f); }
                at (s[48].volume, 4.0f, 0.0f);
            }
            // PUMP 8: sidechain ducking on every eighth.
            s[49].volume = {};
            for (int k = 0; k < 8; ++k) { const auto e = 0.5f * (float) k; at (s[49].volume, e, 0.0f); at (s[49].volume, e + 0.05f, 0.6f); at (s[49].volume, e + 0.2f, 2.8f); at (s[49].volume, e + 0.5f, 4.0f); }
            // TREMOLO: the level sways on every eighth.
            s[50].volume = {};
            for (int k = 0; k <= 48; ++k) { const auto t = (float) k / 12.0f; at (s[50].volume, t, 4.0f * (0.625f + 0.375f * std::cos (2.0f * twoPi * t))); }
            // BREATHE: a slow swell and fade over two beats.
            s[51].volume = {};
            for (int k = 0; k <= 16; ++k) { const auto t = 0.25f * (float) k; at (s[51].volume, t, 4.0f * (0.575f + 0.425f * std::cos (0.5f * twoPi * t))); }
            // RISE: the bar fades in from silence (a riser).
            s[52].volume = {}; at (s[52].volume, 0.0f, 0.0f); at (s[52].volume, 4.0f, 4.0f);
            // DROP: three beats, then the last one drops out.
            s[53].volume = {}; at (s[53].volume, 0.0f, 4.0f); at (s[53].volume, 3.0f, 4.0f); at (s[53].volume, 3.0f, 0.0f); at (s[53].volume, 4.0f, 0.0f);
            // ECHO: beat 1, then three echoes of it, each quieter.
            s[54].volume = {};
            for (int k = 0; k < 4; ++k)
            {
                const auto b = (float) k, level = 4.0f * std::pow (0.6f, (float) k);
                at (s[54].time, b, b); at (s[54].time, b + 1.0f, b);
                at (s[54].volume, b, level); at (s[54].volume, b + 1.0f, level);
            }
            // REV SWELL: every beat backwards and swelling up, like a reversed cymbal.
            s[55].volume = {};
            for (int k = 0; k < 4; ++k)
            {
                const auto b = (float) k;
                at (s[55].time, b, 0.0f); at (s[55].time, b + 1.0f, 2.0f);
                at (s[55].volume, b, 0.4f); at (s[55].volume, b + 0.95f, 4.0f); at (s[55].volume, b + 1.0f, 4.0f);
            }
            // HALF GATE: half time, chopped into eighths.
            at (s[56].time, 0.0f, 0.0f); at (s[56].time, 4.0f, 2.0f);
            gate (s[56].volume, every (0.5f), 0.32f);
            // TAPE FADE: a tape stop that fades away with it.
            at (s[57].time, 0.0f, 0.0f);
            for (int k = 0; k <= 8; ++k) { const auto t = 2.0f + 0.25f * (float) k; at (s[57].time, t, (t - 2.0f) * (t - 2.0f) / 4.0f); }
            s[57].volume = {}; at (s[57].volume, 0.0f, 4.0f); at (s[57].volume, 2.0f, 4.0f); at (s[57].volume, 4.0f, 0.0f);
            // STUTTER FADE: sixteenth stutters that fade on every beat.
            s[58].time = s[4].time;
            s[58].volume = {};
            for (int k = 0; k < 4; ++k) { const auto b = (float) k; at (s[58].volume, b, 4.0f); at (s[58].volume, b + 1.0f, 1.0f); }
            // BUILD UP: the roll, rising from quiet to full.
            s[59].time = s[13].time;
            s[59].volume = {}; at (s[59].volume, 0.0f, 1.4f); at (s[59].volume, 4.0f, 4.0f);

            // PUMP 4: sidechain ducking on every beat (deeper and slower than PUMP 8).
            {
                auto& pump = s[(size_t) builtIndex ("PUMP 4")];
                at (pump.time, 0.0f, 0.0f); at (pump.time, 4.0f, 0.0f);
                pump.volume = {};
                for (int k = 0; k < 4; ++k)
                {
                    const auto b = (float) k;
                    at (pump.volume, b, 0.0f); at (pump.volume, b + 0.08f, 0.4f); at (pump.volume, b + 0.4f, 2.6f); at (pump.volume, b + 1.0f, 4.0f);
                }
            }

            for (auto& slot : s) { slot.time.sort(); slot.volume.sort(); }
            return s;
        }();

        static const auto all = []
        {
            std::array<TimeSlot, numSlots> s;
            for (int k = 0; k < numSlots; ++k)
                s[(size_t) k] = built[(size_t) builtIndex (shownNames[k])];
            return s;
        }();
        return all;
    }
}

//==============================================================================
void TimeShaper::prepare (double sampleRate)
{
    rate = sampleRate > 0.0 ? sampleRate : 48000.0;
    size = (int) std::ceil (historySeconds * rate);
    history.setSize (2, size);
    fadeStep = 1.0f / (float) std::max (1.0, 0.006 * rate);
    smoothing = 1.0f - std::exp (-1.0f / (float) (0.0015 * rate));
    reset();
}

void TimeShaper::reset()
{
    history.clear();
    writePos = 0;
    lastDelay = fadeDelay = 0.0;
    fade = 0.0f;
    gain = 1.0f;
    mix = 0.0f;
}

float TimeShaper::read (int channel, double delaySamples) const noexcept
{
    // writePos is the sample just written; reading `delaySamples` behind it.
    const auto* d = history.getReadPointer (channel);
    const auto p = (double) writePos - delaySamples;
    const auto i = (int) std::floor (p);
    const auto f = (float) (p - (double) i);
    auto at = [this, d] (int k) { return d[(k % size + size) % size]; };
    if (delaySamples >= 2.0)
    {
        // 4-point Hermite: half time and tape stops stay clear.
        const auto xm1 = at (i - 1), x0 = at (i), x1 = at (i + 1), x2 = at (i + 2);
        const auto c1 = 0.5f * (x1 - xm1);
        const auto c2 = xm1 - 2.5f * x0 + 2.0f * x1 - 0.5f * x2;
        const auto c3 = 0.5f * (x2 - xm1) + 1.5f * (x0 - x1);
        return ((c3 * f + c2) * f + c1) * f + x0;
    }
    return f > 0.0f ? at (i) + f * (at (i + 1) - at (i)) : at (i);
}

void TimeShaper::process (juce::AudioBuffer<float>& buffer, int numSamples, const Settings& s, double ppq, double bpm) noexcept
{
    if (size <= 1 || numSamples <= 0)
        return;

    const auto channels = std::min (2, buffer.getNumChannels());
    auto* outL = buffer.getWritePointer (0);
    auto* outR = channels > 1 ? buffer.getWritePointer (1) : nullptr;
    const auto tempo = juce::jlimit (20.0, 400.0, bpm > 0.0 ? bpm : 120.0);
    const auto samplesPerBeat = rate * 60.0 / tempo;
    const auto beatsPerSample = tempo / 60.0 / rate;
    const auto cycle = juce::jlimit (1.0, 8.0, s.cycleBeats);
    const auto maxDelay = (double) size - 8.0;
    const auto targetMix = s.slot != nullptr ? juce::jlimit (0.0f, 1.0f, s.mix) : 0.0f;
    const auto mixStep = 1.0f / (float) (0.02 * rate); // on / off over 20 ms

    float position = 0.0f;
    double delay = 0.0;
    for (int n = 0; n < numSamples; ++n)
    {
        // Keep the past, whatever happens.
        writePos = (writePos + 1) % size;
        history.setSample (0, writePos, outL[n]);
        history.setSample (1, writePos, outR != nullptr ? outR[n] : outL[n]);

        mix += juce::jlimit (-mixStep, mixStep, targetMix - mix);
        const auto beat = ppq + (double) n * beatsPerSample;
        position = (float) (std::fmod (std::fmod (beat, cycle) + cycle, cycle) / cycle);

        if (mix <= 0.0f && targetMix <= 0.0f)
        {
            lastDelay = 0.0;
            fade = 0.0f;
            continue; // dry (bit-exact)
        }

        delay = s.slot != nullptr ? juce::jlimit (0.0, maxDelay, (double) s.slot->time.valueAt (position) * cycle * samplesPerBeat) : 0.0;

        // A jump (a stutter, a repeat, the lag let go, another slot): crossfade from where
        // the old read head was (it runs on in real time meanwhile).
        if (std::abs (delay - lastDelay) > 32.0)
        {
            fadeDelay = lastDelay;
            fade = 1.0f;
        }
        lastDelay = delay;

        const auto volume = s.slot != nullptr ? juce::jlimit (0.0f, 1.0f, s.slot->volume.valueAt (position)) : 1.0f;
        gain += (volume - gain) * smoothing;

        for (int ch = 0; ch < channels; ++ch)
        {
            auto wet = read (ch, delay);
            if (fade > 0.0f)
                wet = wet * (1.0f - fade) + read (ch, fadeDelay) * fade;
            auto* out = ch == 0 ? outL : outR;
            out[n] = out[n] * (1.0f - mix) + wet * gain * mix;
        }
        if (fade > 0.0f)
            fade = std::max (0.0f, fade - fadeStep);
    }

    // The speed the tape runs at: how fast the lag changes (1 = normal, 0 = stopped,
    // negative = backwards): the slope of the curve's segment here.
    const auto speed = s.slot != nullptr && targetMix > 0.0f ? 1.0f - s.slot->time.slopeAt (position) : 1.0f;
    const auto shownPosition = s.clockRunning ? position
                                              : (float) (std::fmod (std::fmod (s.stoppedBeat, cycle) + cycle, cycle) / cycle);
    displayPosition.store (shownPosition, std::memory_order_relaxed);
    displayRunning.store (s.clockRunning, std::memory_order_relaxed);
    displaySpeed.store (speed, std::memory_order_relaxed);
    displayLag.store ((float) (delay / samplesPerBeat), std::memory_order_relaxed);
    displayEngaged.store (targetMix > 0.0f, std::memory_order_relaxed);
}

} // namespace kerf

#pragma once

#include <juce_core/juce_core.h>
#include <vector>
#include "PatternData.h"
#include "../AudioEngine/ChopTypes.h"
#include "../AudioEngine/SampleData.h"

namespace kerf::PatternDice
{

/** How a chop sounds, measured from its audio (its first 0.3 s). All 0..1 but `seconds`,
    `f0` and `pitchClass`. */
struct ChopTraits
{
    int id = 0;
    double seconds = 0.0;
    float low = 0.0f;     // share of the energy below ~150 Hz (a kick, a bass)
    float bright = 0.0f;  // share of the energy up high (a hi-hat, a shaker)
    float punch = 0.0f;   // how hard it starts compared with what follows (drums)
    float sustain = 0.0f; // how much is left after the start (tones, pads, vocals)
    float f0 = 0.0f;      // its pitch in Hz (0 = none heard: noise, a click)
    int pitchClass = -1;  // that pitch as a note: 0 = C ... 11 = B, -1 = none
};

ChopTraits describe (const SampleData& sample, const ChopParams& chop);

/** What the chops are good for: the best kick, snare and hat (another chop when there is
    no better one), a second kick / snare / hat for variety when there is a good one, an
    open hat, and the tonal chops for melodies, the best first. */
struct Roles
{
    int kick = 0, snare = 0, hat = 0, perc = 0;
    int kick2 = 0, snare2 = 0, hat2 = 0, openHat = 0; // 0 = none
    std::vector<int> melodic;
};

Roles assignRoles (const std::vector<ChopTraits>& chops);

/** The styles the die knows, in order (an index into this is a style). */
juce::StringArray styleNames();

/** What to roll. */
struct Options
{
    int bars = 1;
    double bpm = 0.0;     // the sample's tempo (0 = unknown)
    int tonic = -1;       // the key: 0 = C ... 11 = B, -1 = unknown
    bool minor = true;
    int style = -1;       // a style to use (an index into styleNames()), -1 = one that suits the tempo
    int avoidStyle = -1;  // with style -1: not this one (the last roll's - a fresh idea each time)
};

struct Result
{
    std::vector<PatternEvent> events;
    int style = 0;
    juce::String name;    // the style's name
    bool inKey = false;   // the tuned notes were fitted to the key (the chops' own pitch heard)
};

/** What a roll looks like in the grid (velocities aside): the same number, the same pattern. */
juce::int64 fingerprint (const std::vector<PatternEvent>& events);

/** The die: a pattern of `bars` bars in a style that suits the tempo (boom bap, trap, drill,
    house, drum and bass, a breakbeat stutter, jersey club, amapiano with a log drum, a
    melodic flip of the tonal chops, or a chop-up re-arranging the slices in time), with
    held (merged) notes, ratchets, triplets, ghost notes on a chance, pitch, slides and
    accents. The players change from roll to roll (any chop nearly as good as the best for
    a role), and a hook - a chop that is no drum - answers on a few offbeats. Every bar a
    variation of the first, growing busier, with fills where phrases end. Tuned notes
    (melodies, an 808, a log drum, the hook) stay in the key: KERF hears each chop's own
    pitch and only uses notes of the scale. */
Result roll (const std::vector<ChopTraits>& chops, const Options& options, juce::Random& random);

} // namespace kerf::PatternDice

#pragma once

#include <juce_core/juce_core.h>
#include <vector>
#include <map>
#include <tuple>
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
    float level = 0.0f;   // how loud it is (its first 0.3 s; 1 = about -10 dB RMS)
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

/** The die's characters, in order (an index into this is a mode): BALANCED (the default),
    GROOVE, CHOPPY, MELODIC, TEXTURE, MINIMAL, CHAOS. They only move the generator's
    weights (density, variation, which tools a roll may use). */
juce::StringArray modeNames();

/** Remembers each chop's traits, so a roll does not listen to the audio again (a new sample,
    or a chop moved: measured anew). Message thread. */
class TraitCache
{
public:
    std::vector<ChopTraits> get (const SampleData& sample, const std::vector<ChopParams>& chops);

private:
    const SampleData* sampleSeen = nullptr;
    std::map<std::tuple<int, int64_t, int64_t>, ChopTraits> known; // id, start, end
};

/** What to roll. */
struct Options
{
    int bars = 1;
    double bpm = 0.0;     // the sample's tempo (0 = unknown)
    int tonic = -1;       // the key: 0 = C ... 11 = B, -1 = unknown
    bool minor = true;
    int style = -1;       // a style to use (an index into styleNames()), -1 = one that suits the tempo
    int avoidStyle = -1;  // with style -1: not this one (the last roll's - a fresh idea each time)
    int mode = 0;         // an index into modeNames()
    juce::int64 seed = 0; // every decision comes from it: the same seed and options, the same pattern
    const std::vector<std::vector<PatternEvent>>* recent = nullptr; // the last rolls: a new one must differ
};

struct Result
{
    std::vector<PatternEvent> events;
    int style = 0;
    juce::String name;    // the style's name ("MOTIFS" when no genre bar carries it)
    juce::String idea;    // what this roll is, in a few words ("AABA, building up, pitch + ratchets")
    bool inKey = false;   // the tuned notes were fitted to the key (the chops' own pitch heard)
    juce::int64 seed = 0; // the seed of the pattern chosen (it may be a later try than options.seed)
    int attempts = 1;     // candidates made before one passed the checks
};

/** What a roll looks like in the grid (velocities aside): the same number, the same pattern. */
juce::int64 fingerprint (const std::vector<PatternEvent>& events);

/** The die. It does not throw notes about at random: it listens to the chops (their roles,
    pitch, length), picks a form for the bars (AABA, ABAB, AAAB ...), a density and energy
    curve, and a small budget of tools for this roll (pitch, ratchets, ghost notes, held
    notes, slides, scattered neighbours - never all at once). Each part gets a motif: a short
    rhythm with a few chops (or a genre bar as the backbone, when the sample has drums);
    when a part returns, its motif comes back changed a little. Phrase ends get transitions
    (a fill, a pickup, a gap, a run), the last bar an ending of its own. Tuned notes stay in
    the key. Every candidate is checked (too repetitive? one chop everywhere? too dense or
    too empty? a motif? does it develop? like one of the last rolls?) and a failed one is
    made again with the next seed, a few times at most. */
Result roll (const std::vector<ChopTraits>& chops, const Options& options);

} // namespace kerf::PatternDice

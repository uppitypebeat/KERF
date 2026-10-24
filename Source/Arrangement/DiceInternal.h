#pragma once

// The die's building blocks, shared by its parts (not for the rest of KERF):
//   DiceStyles.cpp     the genre bars (boom bap, trap ...), the chop-up, notes in the key
//   DiceGenerator.cpp  one candidate: structure, motifs, variation, transitions, the ending
//   DiceEvaluator.cpp  is it musical, and new? (PatternDice::roll keeps the best of a few)

#include "PatternDice.h"
#include <array>
#include <string>
#include <vector>

namespace kerf::PatternDice::detail
{

//==============================================================================
// Genre bars. One bar of a style, 16 steps:
//   K kick   k ghost kick   S snare   s ghost snare   r snare roll
//   H hat    h soft hat     R hat roll   T hat triplets (3 hits; "T-": over an 8th)
//   O open hat (held)   P percussion   L log drum / bass (tuned, in the key)
//   X any chop (a neighbour each pass)   M a note of the tonal chops
//   C the hook (put in by the die: a chop that is no drum, on a few offbeats)
//   - the note before held on (merged)   . rest
struct Style
{
    const char* name;
    std::vector<const char*> bars;  // ideas for a bar
    std::vector<const char*> fills; // phrase endings (the last four steps)
    bool eightOhEight = false;      // kicks are tuned and slide (trap, drill)
    int rollRepeats = 2;            // hits in a roll step
    const char* fillAlphabet = "rRKS";
};

enum StyleIndex { boomBap, trap, drill, house, drumAndBass, breakStutter, jerseyClub, amapiano, flip, chopUp, numStyles };

const std::vector<Style>& styles();
bool isDrumStyle (int style);
int pickStyle (double bpm, bool drums, bool tonal, bool slices, bool placed, int avoid, juce::Random& rng);
/** A genre bar changed a little: ghost notes come and go, a hat becomes a roll or triplets... */
std::string varyBar (std::string bar, juce::Random& rng, float amount);
/** CHOP UP: the loop's own groove - every chop on the step where it sits in the sample - then
    flipped, more with every bar: chops swap places, stutter, a beat repeats or runs backwards,
    a hit drops out, a beat comes from another bar. Without a tempo (or places): the slices in
    their order and length, re-arranged the same way. */
std::vector<PatternEvent> chopUpBars (const std::vector<ChopTraits>& chops, int bars, double bpm, double originSeconds, juce::Random& rng,
                                       juce::String* recipeName = nullptr);
/** Where the chops sit in the loop: (index into chops, step from the grid's start), by step,
    one per step. Empty without a tempo, or when they do not lie in time (all at the start). */
std::vector<std::pair<size_t, int>> placesInLoop (const std::vector<ChopTraits>& chops, double bpm, double originSeconds);

//==============================================================================
// Notes in the key: every shift (-12..+12) that puts a chop's own pitch on the scale.
struct Notes
{
    std::vector<int> shifts; // ascending
    int home = 0;            // index of the shift to start from
};
Notes notesFor (int pitchClass, int tonic, bool minor, bool toTonic);
int noteAt (const Notes& notes, int degree); // `degree` scale steps from home (clamped)

//==============================================================================
// What a chop is good for, as weights (0..~1.5). Heuristic: they only steer the choice.
enum class Role { percussive, groove, impact, sustained, melodic, texture, fill, numRoles };
float roleFit (const ChopTraits& c, Role role);

float kickScore (const ChopTraits& c);
float snareScore (const ChopTraits& c);
float hatScore (const ChopTraits& c);
float bassScore (const ChopTraits& c);
float tonalScore (const ChopTraits& c);

//==============================================================================
// The generation modes (the die's character): how dense, how much it varies, which tools.
enum Tool { pitchTool, ratchetTool, ghostTool, holdTool, slideTool, scatterTool, numTools };

struct ModeProfile
{
    const char* name;
    float density;        // share of the steps that sound in an average bar (0..1)
    float densitySwing;   // how far the sections' densities move away from it
    float variation;      // how much a returning idea changes (0..1)
    float templateUse;    // chance a genre bar is the backbone when the sample has drums
    float silence;        // chance of a deliberate gap (a break, an empty beat)
    float transition;     // chance a phrase boundary gets a transition
    float experimental;   // wider pitch, chromatic steps, odder forms (0..1)
    std::array<float, numTools> tools; // how likely each tool is in a roll's budget
    int minTools, maxTools;
};
const std::vector<ModeProfile>& modes();

//==============================================================================
/** One roll's decisions, all from its seed (one juce::Random, nothing else). */
struct GenerationContext
{
    explicit GenerationContext (juce::int64 seedIn) : seed (seedIn), rng (seedIn) {}
    juce::int64 seed;
    juce::Random rng;
    const ModeProfile* mode = nullptr;
    int style = 0;
    std::array<bool, numTools> tools {}; // the budget: what this roll may use
    juce::String form;                    // e.g. "AABA"
    juce::String densityShape;            // e.g. "building up"
    juce::String ending;                  // e.g. "pitch-down ending"
    bool inKey = false;                   // tuned notes were fitted to the key (a chop's pitch heard)
    bool backbone = false;                // a genre bar carries it (else: free motifs)
};

/** One candidate pattern (not yet checked). */
std::vector<PatternEvent> generate (const std::vector<ChopTraits>& chops, const Options& options, GenerationContext& context);

//==============================================================================
/** How good a candidate is: the number of failed checks (0 = all fine) and a score for ties. */
struct Evaluation
{
    int failures = 0;
    float score = 0.0f;
    juce::StringArray failed; // the names of the failed checks (tests, debugging)
};
Evaluation evaluate (const std::vector<PatternEvent>& events, const std::vector<ChopTraits>& chops, const Options& options, int modeIndex);

/** 0..1: how much two patterns share (the same chop on the same step). */
float similarity (const std::vector<PatternEvent>& a, const std::vector<PatternEvent>& b);

} // namespace kerf::PatternDice::detail

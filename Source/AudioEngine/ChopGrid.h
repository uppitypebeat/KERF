#pragma once

#include <cstdint>
#include <vector>

namespace kerf
{

/** Rhythmic note values. The value is the length in quarter-note beats. */
enum class NoteValue
{
    whole,        // 4 beats
    half,         // 2 beats
    quarter,      // 1 beat
    eighth,       // 1/2 beat
    sixteenth,    // 1/4 beat
    thirtySecond  // 1/8 beat
};

/** How a note value is modified.
    - straight: the plain note value
    - triplet:  3 notes in the time of 2   -> length x 2/3
    - duplet:   2 notes in the time of 3   -> length x 3/2
    - dotted:   note value plus half again -> length x 3/2 (same length as a duplet,
                kept separate because musicians think of them differently)
    - quintuplet: 5 notes in the time of 4 -> length x 4/5
*/
enum class NoteModifier
{
    straight,
    triplet,
    duplet,
    dotted,
    quintuplet
};

double beatsForNoteValue (NoteValue v) noexcept;
double factorForModifier (NoteModifier m) noexcept;

/** Length of one grid step in beats (quarter notes). */
double gridStepBeats (NoteValue v, NoteModifier m) noexcept;

/** Length of one grid step in samples. Returns 0 for invalid input. */
double gridStepSamples (NoteValue v, NoteModifier m, double bpm, double sampleRate) noexcept;

/** Boundary positions (in samples) dividing [regionStart, regionEnd) by a rhythmic grid.
    The result always starts with regionStart and ends with regionEnd. Steps shorter than
    minStepSamples are refused (returns just {start, end}) so a bad tempo cannot produce
    thousands of slivers. */
std::vector<int64_t> rhythmicGrid (int64_t regionStart, int64_t regionEnd,
                                   double bpm, double sampleRate,
                                   NoteValue v, NoteModifier m,
                                   int maxSlices = 128,
                                   double minStepSamples = 64.0);

/** Boundaries dividing [regionStart, regionEnd) into `count` equal slices. */
std::vector<int64_t> equalSlices (int64_t regionStart, int64_t regionEnd, int count);

/** Snap a position to the nearest grid line of a grid anchored at `anchor`. */
int64_t snapToGrid (int64_t position, int64_t anchor, double stepSamples) noexcept;

} // namespace kerf

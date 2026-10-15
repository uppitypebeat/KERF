#pragma once

#include <array>

namespace kerf
{

/** One step in the pattern: plays a chop (by stable id) for `length` 16th-note steps. */
struct PatternEvent
{
    int step = 0;          // 0 .. numSteps-1 (16th notes)
    int length = 1;        // in steps
    int chopId = 0;
    float velocity = 0.85f;
    int pitch = 0;         // semitones on top of the chop's own pitch (-12 .. +12)
    float chance = 1.0f;   // probability that the step plays on a pass (0.05 .. 1)
    int repeats = 1;       // ratchet: hits spread evenly over the step's length (1 .. 4)
    int random = 0;        // 0 = its own chop, 1 = a random one of its neighbours (+-2), 2 = any chop
    bool slide = false;    // glides into the next step: that one starts at this step's pitch

    static constexpr int maxPitch = 12;
    static constexpr int maxRepeats = 4;
};

/** Immutable pattern data for the audio thread (part of the engine snapshot). */
struct PatternData
{
    static constexpr int maxEvents = 128;
    static constexpr int stepsPerBar = 16;

    std::array<PatternEvent, maxEvents> events {};
    std::array<int, maxEvents> notes {};   // MIDI note resolved from chopId (-1 = chop gone)
    std::array<int, maxEvents> chopIndex {}; // the event's chop in chopNotes (-1 = chop gone)
    std::array<int, 128> chopNotes {};       // every chop's note, in timeline order (random steps pick from it)
    int numChopNotes = 0;
    int numEvents = 0;
    int numSteps = 16;
};

} // namespace kerf

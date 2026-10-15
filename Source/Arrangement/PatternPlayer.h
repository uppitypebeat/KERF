#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include "PatternModel.h"

namespace kerf
{

/** Feel: SWING delays every second 16th (0 = straight, 1 = 75 % swing, half a step);
    HUMANIZE adds small timing and velocity variations (the same every time for a step,
    so playback, export and MIDI agree). (Outside PatternPlayer: a default argument in it
    uses its member defaults, which clang allows only for a class complete there.) */
struct PatternGroove
{
    float swing = 0.0f;    // 0..1
    float humanize = 0.0f; // 0..1
};

/** Generates MIDI from a PatternData on the audio thread.

    Steps are 16th notes. The position comes from a beat clock (the host's PPQ while the
    host plays, otherwise KERF's internal preview clock). Handles loops/jumps in the
    host timeline and stopping by releasing every note it started. No allocation:
    events are written into a MidiBuffer that was pre-sized by the caller. */
class PatternPlayer
{
public:
    void prepare (double sampleRate);

    /** Stops all notes this player started (at sample 0 of `out`). */
    void allNotesOff (juce::MidiBuffer& out);

    using Groove = PatternGroove;

    /** Where step `step` actually starts, in steps (swing + humanize). */
    static double grooveTime (int64_t step, const Groove& groove) noexcept;
    /** The velocity (0..1) of step `step` after humanize. */
    static float grooveVelocity (int64_t step, float velocity, const Groove& groove) noexcept;

    /** Whether an event with this chance plays on the pass that reaches absolute step
        `step` (the count goes on across loops, so each pass rolls again, but the same way
        every time: playback, export and MIDI agree). */
    static bool stepPlays (int64_t step, float chance) noexcept;

    /** The notes one event plays when its step is reached: its repeats spread evenly over
        its length. Times in steps; returns the number of hits (0 if the chance said no). */
    struct Hit { double on = 0.0, off = 0.0; float velocity = 0.0f; };
    static int hitsFor (const PatternEvent& e, int64_t step, const Groove& groove, std::array<Hit, PatternEvent::maxRepeats>& hits) noexcept;

    /** The pattern sends a step's pitch as a pitch-wheel message on its channel right before
        the note-on (the engine applies it to that note only): 256 units per semitone. */
    static int pitchToWheel (int semitones) noexcept { return juce::jlimit (0, 16383, 8192 + semitones * 256); }
    static int wheelToPitch (int wheel) noexcept     { return (int) std::lround ((wheel - 8192) / 256.0); }

    /** The note an event plays on the pass that reaches absolute step `step`: its own chop,
        or (random steps) one of its chop's neighbours or any chop - picked the same way
        every time for that pass. -1 = none (the chop is gone). */
    static int noteFor (const PatternData& pattern, int eventIndex, int64_t step) noexcept;

    /** Slide: the step after a sliding one starts at the sliding step's pitch and glides to
        its own. Sent on the pattern channel right before that note-on: the start pitch
        (64 + semitones) and the glide time (tens of milliseconds). */
    static constexpr int glideFromController = 102, glideTimeController = 103;

    /** Adds events for a block of `numSamples` starting at beat position `ppqAtStart`. */
    void process (const PatternData& pattern, bool running, double ppqAtStart, double bpm,
                  int numSamples, juce::MidiBuffer& out, const Groove& groove = {});

    /** Current step (for the UI), -1 when not running. */
    int getCurrentStep() const noexcept { return currentStep.load (std::memory_order_relaxed); }

    static constexpr int midiChannel = 16; // pattern notes, distinct from live input

private:
    struct ActiveNote { int note = -1; double offStep = 0.0; };
    struct QueuedHit { int note = -1; int pitch = 0; float velocity = 0.0f; double on = 0.0, off = 0.0; int glideFrom = 0, glideTens = 0; bool glide = false; };

    /** Where in the block (in samples) a position in steps falls. */
    struct BlockClock
    {
        double stepStart, stepsPerSample;
        int numSamples;
        int offsetFor (double step) const noexcept
        {
            return juce::jlimit (0, numSamples - 1, (int) std::floor ((step - stepStart) / stepsPerSample));
        }
    };

    /** Note-offs for the sounding notes that end before `beforeStep`. */
    void releaseDue (double beforeStep, const BlockClock& clock, juce::MidiBuffer& out);

    double sampleRate = 44100.0;
    std::array<ActiveNote, 32> active {};
    std::array<QueuedHit, 64> queue {}; // hits scheduled for later (repeats inside a step)
    int queued = 0;
    double expectedNextStep = -1.0;
    double lastEmittedStep = -1.0e9; // guards against double triggers from host rounding
    bool wasRunning = false;
    std::atomic<int> currentStep { -1 };
};

} // namespace kerf

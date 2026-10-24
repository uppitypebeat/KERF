#pragma once

#include <juce_data_structures/juce_data_structures.h>
#include <optional>
#include <vector>
#include "PatternData.h"

namespace kerf
{

/** The editable pattern (message thread, ValueTree-backed, undoable).
    Monophonic step sequence: at most one event starts on each step. */
class PatternModel
{
public:
    static const juce::Identifier PATTERN, EVENT, bars, step, length, chop, velocity, pitch, chance, repeats, random, slide, locks;

    explicit PatternModel (juce::UndoManager* undo = nullptr);

    juce::ValueTree& getState() noexcept { return state; }
    void replaceState (const juce::ValueTree& newState);

    int getBars() const;
    void setBars (int numBars);
    int getNumSteps() const { return getBars() * PatternData::stepsPerBar; }

    std::vector<PatternEvent> getEvents() const;
    std::optional<PatternEvent> eventAt (int stepIndex) const; // event covering this step (start or held)

    /** Places a chop at a step (replacing whatever starts there). Clamps length. */
    void setEvent (int stepIndex, int chopId, int length = 1, float velocity = 0.85f);
    void removeEventStartingAt (int stepIndex);
    void setLength (int stepIndex, int length);
    /** Like setLength, but runs over later events: the ones it now covers are removed and
        returned as they were, so a drag can put them back (restoreEvent) when it shrinks. */
    std::vector<juce::ValueTree> setLengthOver (int stepIndex, int length);
    void restoreEvent (const juce::ValueTree& event);

    /** Per-step expression of the event starting at `stepIndex` (clamped; no-op if none). */
    void setVelocity (int stepIndex, float velocity);
    void setPitch (int stepIndex, int semitones);
    void setChance (int stepIndex, float chance);
    void setRepeats (int stepIndex, int repeats);
    void setRandom (int stepIndex, int mode);   // 0 own chop, 1 nearby, 2 any
    void setSlide (int stepIndex, bool slide);
    void clear();

    /** LOCKS: steps the die keeps as they are (a chop on them, or empty), kept with the pattern;
        undoable. */
    bool isLocked (int stepIndex) const;
    void setLocked (int stepIndex, bool locked);
    std::vector<int> getLocks() const;   // ascending, inside the pattern's length
    void clearLocks();


    /** Drops events whose chop no longer exists. */
    void removeEventsForMissingChops (const std::vector<int>& existingChopIds);

private:
    juce::ValueTree findEvent (int stepIndex) const;
    static PatternEvent fromTree (const juce::ValueTree&);

    juce::ValueTree state { PATTERN };
    juce::UndoManager* undoManager;
};

} // namespace kerf

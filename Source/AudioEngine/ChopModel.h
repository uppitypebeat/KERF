#pragma once

#include <juce_data_structures/juce_data_structures.h>
#include <limits>
#include <optional>
#include <vector>
#include "ChopTypes.h"

namespace kerf
{

namespace IDs
{
    #define KERF_ID(name) inline const juce::Identifier name { #name };
    KERF_ID (CHOPS)
    KERF_ID (CHOP)
    KERF_ID (nextId)
    KERF_ID (rootNote)
    KERF_ID (sampleLength)
    KERF_ID (beatOrigin) // SHIFT: the frame of the sample where the beat grid's bar 1 is
    KERF_ID (id)
    KERF_ID (start)
    KERF_ID (end)
    KERF_ID (note)
    KERF_ID (mode)
    KERF_ID (pitch)
    KERF_ID (fine)
    KERF_ID (reverse)
    KERF_ID (gain)
    KERF_ID (pan)
    KERF_ID (fadeIn)
    KERF_ID (fadeOut)
    KERF_ID (spaceSend)
    KERF_ID (grainSend)
    KERF_ID (choke)
    KERF_ID (decay)
    KERF_ID (sustain)
    #undef KERF_ID
}

enum class ChopEdge { start, end };

/** The editable list of chops. Lives on the message thread and is backed by a ValueTree,
    which gives us undo/redo (via juce::UndoManager) and host state persistence.

    Every chop has a stable integer id. Ids are never reused, so references to a chop
    (selection, patterns, MIDI mapping) survive edits and reordering.

    Note mapping is stable: editing or splitting chops never changes the note of an
    existing chop. New chops get the lowest free note at or above the root note.
    Only remapByPosition() (an explicit user action) renumbers. */
class ChopModel
{
public:
    explicit ChopModel (juce::UndoManager* undo = nullptr);

    juce::ValueTree& getState() noexcept { return state; }
    void replaceState (const juce::ValueTree& newState, bool clearUndo = true); // restoring a project (or a remembered sample)

    //==========================================================================
    int64_t getSampleLength() const;
    int getRootNote() const;
    /** SHIFT: where in the sample the beat grid starts (frames; the sample moved later than
        the grid: negative). The beat lines, BEAT / GRID chops, snapping and the metronome. */
    double getBeatOrigin() const { return (double) state.getProperty (IDs::beatOrigin, 0.0); }
    /** How many chops there can be: one per key from the root up to the highest MIDI note
        (root C5 = MIDI 60: 68). Every chop then sits on a key. */
    int chopLimit() const { return juce::jlimit (1, maxChops, 128 - getRootNote()); }
    int getNumChops() const { return state.getNumChildren(); }

    /** Sets the sample length and replaces all chops with one chop covering the sample.
        `recordUndo` false: not an undo step of its own (a sample load is one, as a whole). */
    void resetForSample (int64_t sampleLength, bool recordUndo = true);

    /** All chops, sorted by start position. */
    std::vector<ChopParams> getChops() const;
    std::optional<ChopParams> getChop (int id) const;
    int chopIdAt (int64_t position) const; // topmost chop containing position, or 0
    int chopIdForNote (int note) const;

    //==========================================================================
    // Editing operations. All are undoable and return false (doing nothing) if invalid.

    /** Adds a chop over [start, end). Returns the new id, or 0 if refused. */
    int addChop (int64_t start, int64_t end);

    /** Splits the chop containing `position` (or `chopId` if given). Returns the new chop's id. */
    int splitAt (int64_t position, int chopId = 0);

    /** Moves one edge of a chop. If `linked`, a neighbour whose edge touched the moved edge
        moves with it, so contiguous slices stay contiguous. Clamped to valid positions. */
    bool moveEdge (int chopId, ChopEdge edge, int64_t newPosition, bool linked = true);

    /** Moves a whole chop (keeping its length). */
    bool moveChop (int chopId, int64_t newStart);

    bool removeChop (int chopId);

    /** Merges a chop with the next chop to its right. The left chop keeps its id and note. */
    bool mergeWithNext (int chopId);

    /** Replaces every chop with contiguous chops between consecutive boundaries,
        mapped sequentially from the root note. Used by grid/transient/random chopping. */
    bool replaceWithBoundaries (const std::vector<int64_t>& boundaries);

    /** Replaces one chop with sub-chops between `boundaries` (clamped to that chop).
        The first sub-chop keeps the original id, note and sound settings; the others get
        new ids and the lowest free notes, and inherit the sound settings. */
    bool subdivide (int chopId, const std::vector<int64_t>& boundaries);

    /** Assigns a note. If another chop already uses it, the two chops swap notes. */
    bool setNote (int chopId, int note);

    void setRootNote (int note);

    /** Explicit user action: renumber notes in timeline order starting at the root note. */
    void remapByPosition();

    /** Sets one per-chop parameter (pitch, gain...). Values are clamped. */
    bool setParam (int chopId, const juce::Identifier& param, const juce::var& value);

    /** Writes every field of `params` (except id) to the chop with params.id. */
    bool setChop (const ChopParams& params);

    //==========================================================================
    static ChopParams fromTree (const juce::ValueTree& t);
    static void writeToTree (juce::ValueTree& t, const ChopParams& p, juce::UndoManager* um);
    static ChopParams sanitised (ChopParams p, int64_t sampleLength);

private:
    juce::ValueTree findChop (int id) const;
    int allocateId();
    int lowestFreeNote() const;
    juce::ValueTree createChopTree (int64_t start, int64_t end, int note);

    juce::ValueTree state { IDs::CHOPS };
    juce::UndoManager* undoManager;
};

} // namespace kerf

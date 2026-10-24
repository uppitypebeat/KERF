#pragma once

#include "Controls.h"
#include "../PluginProcessor.h"
#include "../Arrangement/PatternDice.h"
#include <deque>

namespace kerf::ui
{

/** A handle to drag a file straight into the DAW (AUDIO: the rendered pattern, MIDI: its
    notes). Pressing it makes the file; once it is ready, dragging out of the plugin drops
    it wherever the mouse is released (FL Studio: playlist, channel rack, piano roll). */
class DragChip final : public juce::Component, public juce::SettableTooltipClient, private juce::Timer
{
public:
    explicit DragChip (const juce::String& label);

    /** Makes the file and calls `done` with it (an empty File on failure). May be async. */
    std::function<void (std::function<void (juce::File)> done)> prepare;

    void paint (juce::Graphics&) override;
    void mouseEnter (const juce::MouseEvent&) override { hover = true; repaint(); }
    void mouseExit (const juce::MouseEvent&) override  { hover = false; repaint(); }
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;

private:
    void timerCallback() override { phase = std::fmod (phase + 0.06f, 1.0f); repaint(); }

    juce::String label;
    juce::File ready;
    bool preparing = false, hover = false, dragging = false;
    int generation = 0;
    float phase = 0.0f;
};


/** Step sequencer for building new phrases from chops (one bar = 16 steps).

      click empty step   place the selected chop (a faint preview shows which, under the mouse)
      click a note       remove it (anywhere on it, a held one too)
      drag its right edge  hold it longer or shorter: it runs over (merges) the steps in its way
                           (they come back when it shrinks); dragging a note sideways does too,
                           shift-click on a step lengthens the note before it up to there
      drag up / down     velocity
      wheel              pitch (semitones)
      right-click        chop (swap), repeat (ratchet), chance, pitch, velocity, random chop, slide, clear

    A step shows its velocity as the height of its fill, its pitch in the corner, its
    repeats as divisions and a chance below 100 % as a dashed, fainter outline. */
class PatternView final : public juce::Component,
                          private AnimationClock::Client,
                          private juce::ValueTree::Listener,
                          private juce::ChangeListener
{
public:
    PatternView (KerfAudioProcessor& processor, AnimationClock& clock);
    ~PatternView() override;

    /** The die: a pattern from the chops in a style for the tempo, as many bars as set (one undo step). */
    void randomPattern();

    /** Where a step's cell is (this page), for tests: its centre and its right edge. */
    juce::Rectangle<float> stepCell (int stepInPage) const { return cellBounds (stepInPage); }

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;

private:
    void animationTick (double dt) override;
    void valueTreePropertyChanged (juce::ValueTree&, const juce::Identifier&) override { refresh(); }
    void valueTreeChildAdded (juce::ValueTree&, juce::ValueTree&) override             { refresh(); }
    void valueTreeChildRemoved (juce::ValueTree&, juce::ValueTree&, int) override      { refresh(); }
    void valueTreeChildOrderChanged (juce::ValueTree&, int, int) override              {}
    void changeListenerCallback (juce::ChangeBroadcaster*) override                    { refresh(); }

    void refresh();
    juce::Rectangle<float> cellBounds (int stepInPage) const;
    int stepAt (juce::Point<float>) const; // absolute step, -1 if none
    int chopIndexOf (int chopId) const;    // 0-based display index, -1 if missing
    void showStepMenu (int step);

    KerfAudioProcessor& processor;
    AnimationClock& clock;

    std::vector<PatternEvent> events;
    std::vector<ChopParams> chops;
    juce::Rectangle<float> gridArea;
    int page = 0, hoveredStep = -1, playingStep = -1;
    int hoveredEdge = -1; // a note whose right edge is under the mouse (its start step): dragging it sets its length
    int edgeOf (juce::Point<float>) const; // the note whose right edge is at this point (its start step), -1 if none
    juce::Rectangle<float> spanBounds (const PatternEvent&) const; // a note across its cells (this page)
    int dragStep = -1; // step whose length is being dragged
    std::vector<juce::ValueTree> swallowed; // steps the length drag ran over (they come back if it shrinks)
    void setDragLength (int length);

    // A press on a step: a click (place / remove), or a drag that turns into a length
    // (sideways) or velocity (up / down) edit once it moves.
    enum class Drag { none, undecided, length, velocity };
    Drag drag = Drag::none;
    int pressStep = -1;          // start step of the pressed event
    bool pressPlaced = false;    // the press put this event there
    float pressVelocity = 0.85f;
    double lastWheelTime = 0.0;
    int lastWheelStep = -1;
    float wheelAccumulator = 0.0f;
    void report (int step, const juce::String& what); // footer readout of an edit

    Segmented bars { { "1", "2", "4", "8" } };
    Segmented pages { { "1" } };
    IconButton play { "Play pattern", IconButton::Icon::play };
    PillButton clear { "Clear" }, exportButton { "Export" };
    IconButton dice { "Random pattern", IconButton::Icon::dice, false }; // a new pattern, as long as the bars set
    int diceStyle = -1;     // a style picked on the die's menu (-1: one that suits the tempo)
    int lastDiceStyle = -1; // the last roll's (the next one takes another)
    int lastAutoMode = -1;  // MODE Auto: the last roll's mode (the next one takes another)
    std::deque<juce::int64> recentRolls; // fingerprints of the last rolls (never the same twice)
    std::deque<std::vector<PatternEvent>> recentPatterns; // the last few rolls: a new one must differ
    PatternDice::TraitCache diceTraits;  // each chop listened to once
    float diceSpin = 1.0f;               // the die's turn after a roll (0..1, 1 = at rest)
    void showDiceMenu();
    DragChip audioDrag { "AUDIO" }, midiDrag { "MIDI" };
    ValueField swingField { 50, 75, 1, 0 }, humanField { 0, 100, 1, 0 };
    std::unique_ptr<juce::ParameterAttachment> swingAttachment, humanAttachment;
    juce::Rectangle<float> swingLabel, humanLabel;
    std::array<juce::Rectangle<float>, 2> dividers; // between the control columns
    juce::File dragFile (const juce::String& extension) const;
    std::unique_ptr<juce::FileChooser> chooser;
    void showExportMenu();
    void exportTo (bool pattern, int bitDepth, int loops);
};

} // namespace kerf::ui

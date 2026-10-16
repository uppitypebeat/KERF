#include "PatternView.h"
#include "../Arrangement/PatternDice.h"
#include "../Analysis/MusicalAnalysis.h"

namespace kerf::ui
{

juce::String noteName (int midiNote);

namespace
{
    constexpr int stepsPerPage = PatternData::stepsPerBar;
    const int barOptions[] = { 1, 2, 4, 8 };
}

//==============================================================================
DragChip::DragChip (const juce::String& l) : label (l)
{
    setMouseCursor (juce::MouseCursor::DraggingHandCursor);
}

void DragChip::paint (juce::Graphics& g)
{
    const auto r = getLocalBounds().toFloat().reduced (0.5f);
    g.setColour (hover || dragging ? Colours::surfaceHover : Colours::surfaceRaised);
    g.fillRoundedRectangle (r, Metrics::radiusSmall);
    g.setColour (ready.existsAsFile() && ! preparing ? Colours::accent.withAlpha (0.7f) : Colours::line);
    g.drawRoundedRectangle (r, Metrics::radiusSmall, Metrics::hairline);

    // Grip: two columns of three dots ("you can drag this").
    g.setColour (Colours::textDim);
    for (int col = 0; col < 2; ++col)
        for (int row = 0; row < 3; ++row)
            g.fillEllipse (juce::Rectangle<float> (2.0f, 2.0f).withCentre ({ r.getX() + 7.0f + 4.0f * (float) col, r.getCentreY() - 4.0f + 4.0f * (float) row }));

    drawText (g, label, r.withTrimmedLeft (16.0f), TextStyle::label, hover ? Colours::text : Colours::textDim, juce::Justification::centredLeft);

    // Making the file: a short accent segment running along the bottom edge.
    if (preparing)
    {
        const auto w = r.getWidth() * 0.3f;
        g.setColour (Colours::accent);
        g.fillRect (juce::Rectangle<float> (r.getX() + (r.getWidth() - w) * phase, r.getBottom() - 2.0f, w, 1.5f));
    }
}

void DragChip::mouseDown (const juce::MouseEvent&)
{
    if (prepare == nullptr || dragging)
        return;

    ready = juce::File();
    preparing = true;
    startTimerHz (30);
    const auto gen = ++generation;
    juce::Component::SafePointer<DragChip> safe (this);
    prepare ([safe, gen] (juce::File file)
    {
        if (auto* self = safe.getComponent(); self != nullptr && self->generation == gen)
        {
            self->ready = file;
            self->preparing = false;
            self->stopTimer();
            self->repaint();
        }
    });
}

void DragChip::mouseDrag (const juce::MouseEvent& e)
{
    // The file may still be rendering: the drag starts as soon as it is there.
    if (dragging || preparing || ! ready.existsAsFile() || e.getDistanceFromDragStart() < 4)
        return;

    dragging = true;
    repaint();
    juce::Component::SafePointer<DragChip> safe (this);
    juce::DragAndDropContainer::performExternalDragDropOfFiles ({ ready.getFullPathName() }, false, this, [safe]
    {
        if (auto* self = safe.getComponent())
        {
            self->dragging = false;
            self->repaint();
        }
    });
}

//==============================================================================
PatternView::PatternView (KerfAudioProcessor& p, AnimationClock& c) : processor (p), clock (c)
{
    addAndMakeVisible (bars);
    bars.setTooltip ("Pattern length in bars");
    bars.onChange = [this] (int i)
    {
        processor.getUndoManager().beginNewTransaction ("Pattern length");
        processor.getPattern().setBars (barOptions[juce::jlimit (0, 3, i)]);
    };

    addAndMakeVisible (pages);
    pages.setTooltip ("Which bar is shown");
    pages.onChange = [this] (int i) { page = i; repaint(); };

    addAndMakeVisible (play);
    play.setIconWhenOn (IconButton::Icon::stop);
    play.setTooltip ("Play / stop the pattern. After using it or editing the steps, Space plays the pattern too. While the DAW plays, the pattern follows its timeline.");
    play.onClick = [this] { processor.setPatternPlaying (play.getToggleState()); };

    addAndMakeVisible (clear);
    clear.setTooltip ("Remove every step (undoable)");
    clear.onClick = [this]
    {
        processor.getUndoManager().beginNewTransaction ("Clear pattern");
        processor.getPattern().clear();
    };

    addAndMakeVisible (dice);
    dice.setTooltip ("Random pattern for your chops. Right-click: pick a style");
    dice.onClick = [this] { randomPattern(); };
    dice.onRightClick = [this] { showDiceMenu(); };

    addAndMakeVisible (exportButton);
    exportButton.setTooltip ("Render the pattern (or the current sample) to a WAV file");
    exportButton.onClick = [this] { showExportMenu(); };


    // Feel: SWING (50 % straight .. 75 %) and HUMAN (timing / velocity variation).
    auto attachField = [this] (ValueField& field, const char* paramId, double scale, double offset)
    {
        auto* param = processor.getParameters().getParameter (paramId);
        auto attachment = std::make_unique<juce::ParameterAttachment> (*param, [&field, scale, offset] (float v)
        {
            field.setValue (offset + v * scale, juce::dontSendNotification);
        });
        auto* raw = attachment.get();
        field.onChange = [raw, scale, offset] (double v) { raw->setValueAsCompleteGesture ((float) ((v - offset) / scale)); };
        attachment->sendInitialUpdate();
        addAndMakeVisible (field);
        return attachment;
    };
    swingField.setSuffix ("%");
    swingField.setTooltip ("SWING: delays every second 16th. 50% = straight, 66% = triplet feel, 75% = heavy. Drag or double-click.");
    swingAttachment = attachField (swingField, Params::swing, 25.0, 50.0);
    humanField.setSuffix ("%");
    for (auto* f : { &swingField, &humanField })
        f->setJustification (juce::Justification::centredRight); // flush with the column: even space both sides
    humanField.setTooltip ("HUMAN: small random timing and velocity changes, so the pattern feels played. Drag or double-click.");
    humanAttachment = attachField (humanField, Params::humanize, 100.0, 0.0);
    // Drag straight into the DAW.
    addAndMakeVisible (audioDrag);
    audioDrag.setTooltip ("Drag the pattern into your DAW as audio (24-bit WAV, rendered with all effects)");
    audioDrag.prepare = [this] (std::function<void (juce::File)> done)
    {
        if (events.empty())
        {
            processor.setStatusMessage ("The pattern is empty");
            done ({});
            return;
        }
        const auto file = dragFile ("wav");
        processor.setStatusMessage ("Rendering the pattern ...");
        juce::Component::SafePointer<PatternView> safe (this);
        processor.exportPatternAsync (file, 24, 1, [safe, done, file] (juce::String error)
        {
            if (auto* self = safe.getComponent())
                self->processor.setStatusMessage (error.isEmpty() ? juce::String ("Drag it into your DAW") : error);
            done (error.isEmpty() ? file : juce::File());
        });
    };

    addAndMakeVisible (midiDrag);
    midiDrag.setTooltip ("Drag the pattern into your DAW as MIDI (on a KERF channel the notes play the same chops)");
    midiDrag.prepare = [this] (std::function<void (juce::File)> done)
    {
        if (events.empty())
        {
            processor.setStatusMessage ("The pattern is empty");
            done ({});
            return;
        }
        const auto file = dragFile ("mid");
        done (processor.writePatternMidi (file) ? file : juce::File());
    };

    processor.getPattern().getState().addListener (this);
    processor.getChops().getState().addListener (this);
    processor.addChangeListener (this);
    clock.add (this);
    refresh();
}

PatternView::~PatternView()
{
    swingAttachment.reset(); // before the fields they update
    humanAttachment.reset();
    clock.remove (this);
    processor.removeChangeListener (this);
    processor.getChops().getState().removeListener (this);
    processor.getPattern().getState().removeListener (this);
}

void PatternView::refresh()
{
    events = processor.getPattern().getEvents();
    chops = processor.getChops().getChops();

    const auto numBars = processor.getPattern().getBars();
    for (int i = 0; i < 4; ++i)
        if (barOptions[i] == numBars)
            bars.setSelected (i, juce::dontSendNotification);

    juce::StringArray pageNames;
    for (int b = 1; b <= numBars; ++b)
        pageNames.add (juce::String (b));
    pages.setOptions (pageNames);
    page = juce::jlimit (0, numBars - 1, page);
    pages.setSelected (page, juce::dontSendNotification);
    pages.setVisible (numBars > 1);

    play.setToggleState (processor.isPatternPlaying(), juce::dontSendNotification);
    resized();
    repaint();
}

void PatternView::resized()
{
    auto r = getLocalBounds();

    // Play / stop as a symbol at the start of the pattern.
    play.setBounds (r.removeFromLeft (30).withSizeKeepingCentre (30, 30));
    r.removeFromLeft (12);

    // Three small columns on the right, two rows each, divided by hairlines:
    //   [1|2|4|8]   SWING 50%   [EXPORT]
    //   [CLEAR]     HUMAN  0%   [AUDIO][MIDI]
    constexpr int rowH = 20, rowGap = 8, colGap = 12;
    constexpr int barsW = 92, feelW = 80, outW = 108;
    auto right = r.removeFromRight (barsW + feelW + outW + 4 * colGap + 2);
    const auto top = r.getCentreY() - rowH - rowGap / 2;
    auto column = [&right, top] (int width)
    {
        auto c = right.removeFromLeft (width);
        return std::pair { juce::Rectangle<int> (c.getX(), top, width, rowH),
                           juce::Rectangle<int> (c.getX(), top + rowH + rowGap, width, rowH) };
    };
    auto divider = [&right, top] { right.removeFromLeft (colGap); auto d = right.removeFromLeft (1); right.removeFromLeft (colGap);
                                   return juce::Rectangle<float> ((float) d.getX(), (float) top - 2.0f, 1.0f, (float) (2 * rowH + rowGap) + 4.0f); };

    const auto [barsRow, clearRow] = column (barsW);
    bars.setBounds (barsRow);
    {
        auto row = clearRow; // [CLEAR][dice]
        dice.setBounds (row.removeFromRight (26));
        row.removeFromRight (4);
        clear.setBounds (row);
    }
    dividers[0] = divider();

    constexpr int fieldW = 38;
    auto [swingRow, humanRow] = column (feelW);
    swingField.setBounds (swingRow.removeFromRight (fieldW).withSizeKeepingCentre (fieldW, 18));
    swingLabel = swingRow.toFloat();
    humanField.setBounds (humanRow.removeFromRight (fieldW).withSizeKeepingCentre (fieldW, 18));
    humanLabel = humanRow.toFloat();
    dividers[1] = divider();

    auto [exportRow, dragRow] = column (outW);
    exportButton.setBounds (exportRow);
    audioDrag.setBounds (dragRow.removeFromLeft ((outW - 4) / 2));
    dragRow.removeFromLeft (4);
    midiDrag.setBounds (dragRow);

    const auto numBars = processor.getPattern().getBars();
    if (numBars > 1)
    {
        auto left = r.removeFromLeft (std::min (160, 22 * numBars));
        pages.setBounds (left.withSizeKeepingCentre (left.getWidth(), 20));
        r.removeFromLeft (10);
    }

    r.removeFromRight (16);
    gridArea = r.toFloat();
}

juce::Rectangle<float> PatternView::cellBounds (int stepInPage) const
{
    // 4 groups of 4 steps with a small gap between beats.
    const auto gap = 2.0f, beatGap = 8.0f;
    const auto w = (gridArea.getWidth() - 3.0f * beatGap - 12.0f * gap) / (float) stepsPerPage;
    const auto x = gridArea.getX() + (float) stepInPage * (w + gap) + (float) (stepInPage / 4) * (beatGap - gap);
    return { x, gridArea.getY() + 2.0f, w, gridArea.getHeight() - 4.0f };
}

int PatternView::stepAt (juce::Point<float> pt) const
{
    for (int i = 0; i < stepsPerPage; ++i)
        if (cellBounds (i).expanded (1.0f, 0.0f).contains (pt))
            return page * stepsPerPage + i;
    return -1;
}

int PatternView::chopIndexOf (int chopId) const
{
    for (size_t i = 0; i < chops.size(); ++i)
        if (chops[i].id == chopId)
            return (int) i;
    return -1;
}

void PatternView::paint (juce::Graphics& g)
{
    drawText (g, "SWING", swingLabel, TextStyle::label, Colours::textDim);
    drawText (g, "HUMAN", humanLabel, TextStyle::label, Colours::textDim);
    for (const auto& d : dividers)
        drawHairline (g, d);

    const auto selected = (int) processor.getSettings().getProperty ("selectedChop", 0);

    for (int i = 0; i < stepsPerPage; ++i)
    {
        const auto step = page * stepsPerPage + i;
        const auto b = cellBounds (i);
        const auto isPlaying = step == playingStep;

        g.setColour (step == hoveredStep ? Colours::surfaceHover : Colours::surfaceRaised);
        g.fillRoundedRectangle (b, Metrics::radiusSmall);
        g.setColour (i % 4 == 0 ? Colours::line : Colours::hairline);
        g.drawRoundedRectangle (b.reduced (0.5f), Metrics::radiusSmall, Metrics::hairline);

        if (isPlaying)
        {
            g.setColour (Colours::text.withAlpha (0.85f));
            g.fillRect (b.reduced (1.0f, 0.0f).withHeight (2.0f));
        }

        drawText (g, juce::String (i + 1), b.reduced (5.0f, 4.0f).withHeight (10.0f), TextStyle::micro, Colours::textFaint);
    }

    // Events (drawn after the cells so held steps can span several cells)
    for (const auto& e : events)
    {
        const auto first = e.step - page * stepsPerPage;
        const auto last = first + e.length - 1;
        if (last < 0 || first >= stepsPerPage)
            continue;

        const auto a = cellBounds (juce::jlimit (0, stepsPerPage - 1, first));
        const auto z = cellBounds (juce::jlimit (0, stepsPerPage - 1, last));
        const auto span = juce::Rectangle<float> (a.getX(), a.getY(), z.getRight() - a.getX(), a.getHeight());
        const auto index = chopIndexOf (e.chopId);
        const auto missing = index < 0;
        const auto isSel = e.chopId == selected;

        // Each step in its chop's colour (the selected chop a little stronger). A step that
        // may not play (chance) is fainter, with a dashed outline.
        const auto colour = chopColour (e.chopId, chops.empty() ? 0 : chops.front().id);
        const auto maybe = e.chance < 0.999f;
        const auto presence = maybe ? 0.45f + 0.55f * e.chance : 1.0f;
        const auto body = span.reduced (1.0f);

        g.setColour (missing ? Colours::surfaceRaised : colour.withAlpha ((isSel ? 0.2f : 0.11f) * presence));
        g.fillRoundedRectangle (body, Metrics::radiusSmall);

        // Velocity: how high the fill reaches.
        if (! missing)
        {
            const auto level = body.withTrimmedTop (body.getHeight() * (1.0f - juce::jlimit (0.05f, 1.0f, e.velocity)));
            g.setColour (colour.withAlpha ((isSel ? 0.26f : 0.17f) * presence));
            g.fillRoundedRectangle (level, Metrics::radiusSmall);
        }

        // Repeats: the step divided into its hits.
        if (! missing && e.repeats > 1)
        {
            g.setColour (colour.withAlpha (0.55f * presence));
            for (int k = 1; k < e.repeats; ++k)
            {
                const auto x = body.getX() + body.getWidth() * (float) k / (float) e.repeats;
                g.fillRect (juce::Rectangle<float> (x - 0.5f, body.getY() + 6.0f, 1.0f, body.getHeight() - 10.0f));
            }
        }

        const auto outline = missing ? Colours::textFaint : colour.withAlpha ((isSel ? 1.0f : 0.7f) * presence);
        if (maybe && ! missing)
        {
            juce::Path border, dashed;
            border.addRoundedRectangle (body, Metrics::radiusSmall);
            const float dashes[] = { 3.0f, 2.5f };
            juce::PathStrokeType (isSel ? Metrics::stroke : Metrics::hairline).createDashedStroke (dashed, border, dashes, 2);
            g.setColour (outline);
            g.fillPath (dashed);
        }
        else
        {
            g.setColour (outline);
            g.drawRoundedRectangle (body, Metrics::radiusSmall, isSel ? Metrics::stroke : Metrics::hairline);
        }
        if (! missing)
        {
            g.setColour (colour.withMultipliedAlpha (presence));
            g.fillRect (span.reduced (2.0f, 1.0f).withHeight (2.5f));
        }

        if (first >= 0)
        {
            // While its velocity is dragged, the step shows the value instead of the chop.
            const auto editing = drag == Drag::velocity && dragStep == e.step;
            // A random step: "07?" picks a neighbour of chop 07, "??" any chop.
            const auto chopLabel = e.random == 2 ? juce::String ("??")
                                 : juce::String (index + 1).paddedLeft ('0', 2) + (e.random == 1 ? "?" : "");
            const auto label = editing ? juce::String (juce::roundToInt (e.velocity * 100.0f))
                             : (missing ? juce::String ("--") : chopLabel);
            drawText (g, label, a.reduced (5.0f, 4.0f).withTrimmedTop (a.getHeight() * 0.4f), TextStyle::value,
                      missing ? Colours::textFaint : (isSel || editing ? inkColour (colour, 0.4f) : Colours::text));

            if (e.pitch != 0)
                drawText (g, (e.pitch > 0 ? "+" : "") + juce::String (e.pitch), a.reduced (4.0f, 5.0f).withHeight (10.0f),
                          TextStyle::micro, inkColour (colour), juce::Justification::centredRight);
        }

        // Slide: a curve from the step's end up into the next step.
        if (e.slide && ! missing && last < stepsPerPage)
        {
            const auto x0 = body.getRight() - 12.0f, y0 = body.getBottom() - 7.0f;
            const auto x1 = body.getRight() + 7.0f, y1 = body.getY() + body.getHeight() * 0.35f;
            juce::Path glide;
            glide.startNewSubPath (x0, y0);
            glide.quadraticTo (x1 - 2.0f, y0, x1, y1);
            g.setColour (inkColour (colour).withMultipliedAlpha (presence));
            g.strokePath (glide, juce::PathStrokeType (1.6f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
            g.fillEllipse (juce::Rectangle<float> (4.0f, 4.0f).withCentre ({ x1, y1 }));
        }
    }

}

void PatternView::animationTick (double)
{
    const auto step = processor.isPatternPlaying() ? processor.getPatternStep() : -1;
    if (step != playingStep)
    {
        playingStep = step;

        // Follow playback to the bar being played.
        if (step >= 0 && step / stepsPerPage != page && processor.getPattern().getBars() > 1)
        {
            page = step / stepsPerPage;
            pages.setSelected (page, juce::dontSendNotification);
        }
        repaint();
    }

    if (play.getToggleState() != processor.isPatternPlaying())
        play.setToggleState (processor.isPatternPlaying(), juce::dontSendNotification);
}

void PatternView::mouseDown (const juce::MouseEvent& e)
{
    const auto step = stepAt (e.position);
    if (step < 0)
        return;

    auto& pattern = processor.getPattern();
    auto& um = processor.getUndoManager();

    if (e.mods.isPopupMenu())
    {
        showStepMenu (step);
        return;
    }

    const auto selected = (int) processor.getSettings().getProperty ("selectedChop", 0);
    const auto existing = pattern.eventAt (step);
    drag = Drag::none;
    pressStep = -1;
    swallowed.clear();

    if (e.mods.isShiftDown() && existing.has_value())
    {
        um.beginNewTransaction ("Step length");
        pattern.setLengthOver (existing->step, step - existing->step + 1);
        dragStep = existing->step;
        drag = Drag::length;
        return;
    }

    if (existing.has_value() && existing->step == step)
    {
        // On a step that starts here: wait. A click removes / swaps it on release, a drag
        // edits its length or velocity.
        pressStep = step;
        pressPlaced = false;
        pressVelocity = existing->velocity;
        drag = Drag::undecided;
        return;
    }

    if (selected != 0)
    {
        um.beginNewTransaction ("Edit step");
        pattern.setEvent (step, selected);
        pressStep = step;
        pressPlaced = true;
        pressVelocity = 0.85f;
        drag = Drag::undecided;
    }
}

void PatternView::mouseDrag (const juce::MouseEvent& e)
{
    auto& pattern = processor.getPattern();

    if (drag == Drag::undecided && e.getDistanceFromDragStart() > 4)
    {
        const auto vertical = std::abs (e.getDistanceFromDragStartY()) > std::abs (e.getDistanceFromDragStartX());
        drag = vertical ? Drag::velocity : Drag::length;
        dragStep = pressStep;
        if (! pressPlaced) // a fresh step's edits belong to placing it
            processor.getUndoManager().beginNewTransaction (vertical ? "Step velocity" : "Step length");
    }

    if (drag == Drag::length && dragStep >= 0)
    {
        const auto step = stepAt ({ e.position.x, gridArea.getCentreY() });
        if (step >= dragStep)
            setDragLength (step - dragStep + 1);
    }
    else if (drag == Drag::velocity && dragStep >= 0)
    {
        // Up = louder; the whole cell height is the full range.
        const auto v = juce::jlimit (0.05f, 1.0f, pressVelocity - (float) e.getDistanceFromDragStartY() / std::max (40.0f, gridArea.getHeight() * 1.5f));
        pattern.setVelocity (dragStep, v);
        report (dragStep, "velocity " + juce::String (juce::roundToInt (v * 100.0f)) + "%");
    }
}

void PatternView::mouseUp (const juce::MouseEvent&)
{
    // A click (no drag) on a step that was already there: the selected chop removes it,
    // another chop takes its place (keeping its velocity, pitch, chance and repeats).
    if (drag == Drag::undecided && ! pressPlaced && pressStep >= 0)
    {
        auto& pattern = processor.getPattern();
        const auto selected = (int) processor.getSettings().getProperty ("selectedChop", 0);
        if (const auto existing = pattern.eventAt (pressStep); existing.has_value() && existing->step == pressStep)
        {
            processor.getUndoManager().beginNewTransaction ("Edit step");
            if (existing->chopId == selected || selected == 0)
                pattern.removeEventStartingAt (pressStep);
            else
                pattern.setEvent (pressStep, selected, existing->length);
        }
    }

    drag = Drag::none;
    dragStep = -1;
    pressStep = -1;
    swallowed.clear();
}

void PatternView::setDragLength (int length)
{
    // The held step runs over the steps in its way (they are joined into it); dragged back,
    // the ones it no longer covers come back as they were. All one undo step.
    auto& pattern = processor.getPattern();
    for (auto& removed : pattern.setLengthOver (dragStep, length))
        swallowed.push_back (std::move (removed));

    const auto held = pattern.eventAt (dragStep);
    const auto end = held.has_value() ? held->step + held->length : dragStep + length;
    for (auto it = swallowed.begin(); it != swallowed.end();)
    {
        if ((int) it->getProperty (PatternModel::step, -1) >= end)
        {
            pattern.restoreEvent (*it);
            it = swallowed.erase (it);
        }
        else
        {
            ++it;
        }
    }
}

void PatternView::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel)
{
    // Wheel on a step: its pitch in semitones. Elsewhere the wheel goes to the parent.
    const auto step = stepAt (e.position);
    const auto existing = step >= 0 ? processor.getPattern().eventAt (step) : std::nullopt;
    if (! existing.has_value())
    {
        Component::mouseWheelMove (e, wheel);
        return;
    }

    wheelAccumulator += (wheel.isReversed ? -wheel.deltaY : wheel.deltaY);
    if (std::abs (wheelAccumulator) < 0.05f) // trackpads send many small deltas
        return;
    const auto delta = wheelAccumulator > 0.0f ? 1 : -1;
    wheelAccumulator = 0.0f;

    // One undo step for a quick run of wheel ticks on the same step.
    const auto now = juce::Time::getMillisecondCounterHiRes();
    if (existing->step != lastWheelStep || now - lastWheelTime > 800.0)
        processor.getUndoManager().beginNewTransaction ("Step pitch");
    lastWheelStep = existing->step;
    lastWheelTime = now;

    const auto pitch = juce::jlimit (-PatternEvent::maxPitch, PatternEvent::maxPitch, existing->pitch + delta);
    processor.getPattern().setPitch (existing->step, pitch);
    report (existing->step, "pitch " + juce::String (pitch > 0 ? "+" : "") + juce::String (pitch) + " st");
}

void PatternView::report (int step, const juce::String& what)
{
    processor.setStatusMessage ("Step " + juce::String (step + 1) + ": " + what);
}

void PatternView::mouseMove (const juce::MouseEvent& e)
{
    const auto step = stepAt (e.position);
    setMouseCursor (step >= 0 ? juce::MouseCursor::PointingHandCursor : juce::MouseCursor::NormalCursor);
    if (step != hoveredStep) { hoveredStep = step; repaint(); }
}

void PatternView::mouseExit (const juce::MouseEvent&)
{
    if (hoveredStep != -1) { hoveredStep = -1; repaint(); }
}

void PatternView::showStepMenu (int step)
{
    auto& pattern = processor.getPattern();
    auto& um = processor.getUndoManager();

    juce::PopupMenu menu, chopMenu;
    const auto existing = pattern.eventAt (step);
    const auto starts = existing.has_value() && existing->step == step;

    for (size_t i = 0; i < chops.size(); ++i)
    {
        const auto id = chops[i].id;
        const auto label = "Chop " + juce::String ((int) i + 1).paddedLeft ('0', 2) + "   " + noteName (chops[i].note);
        chopMenu.addItem (label, true, starts && existing->chopId == id, [&pattern, &um, step, id, len = starts ? existing->length : 1]
        {
            um.beginNewTransaction ("Edit step");
            pattern.setEvent (step, id, len);
        });
    }

    if (! existing.has_value())
    {
        // An empty step: just the chops.
        menu.addSectionHeader ("STEP " + juce::String (step + 1));
        for (auto it = juce::PopupMenu::MenuItemIterator (chopMenu); it.next();)
            menu.addItem (it.getItem());
    }
    else
    {
        const auto s = existing->step;
        auto percent = [] (float v) { return juce::String (juce::roundToInt (v * 100.0f)) + "%"; };
        auto signedSt = [] (int v) { return (v > 0 ? "+" : "") + juce::String (v) + " st"; };
        auto edit = [&um] (const juce::String& name, std::function<void()> f)
        {
            return [&um, name, f] { um.beginNewTransaction (name); f(); };
        };

        menu.addSectionHeader ("STEP " + juce::String (s + 1));
        menu.addSubMenu ("Chop", chopMenu);

        juce::PopupMenu repeatMenu, chanceMenu, pitchMenu, velocityMenu;
        for (int r = 1; r <= PatternEvent::maxRepeats; ++r)
            repeatMenu.addItem (r == 1 ? juce::String ("Once") : juce::String (r) + juce::String::fromUTF8 ("\xc3\x97"), true, existing->repeats == r,
                                edit ("Step repeat", [&pattern, s, r] { pattern.setRepeats (s, r); }));
        for (float c : { 1.0f, 0.75f, 0.5f, 0.25f, 0.1f })
            chanceMenu.addItem (percent (c), true, std::abs (existing->chance - c) < 0.005f,
                                edit ("Step chance", [&pattern, s, c] { pattern.setChance (s, c); }));
        for (int v : { 12, 7, 5, 3, 2, 1, 0, -1, -2, -3, -5, -7, -12 })
            pitchMenu.addItem (v == 0 ? juce::String ("0 st") : signedSt (v), true, existing->pitch == v,
                               edit ("Step pitch", [&pattern, s, v] { pattern.setPitch (s, v); }));
        for (float v : { 1.0f, 0.85f, 0.7f, 0.5f, 0.3f })
            velocityMenu.addItem (percent (v), true, std::abs (existing->velocity - v) < 0.005f,
                                  edit ("Step velocity", [&pattern, s, v] { pattern.setVelocity (s, v); }));

        const auto times = juce::String::fromUTF8 ("\xc3\x97");
        menu.addSubMenu ("Repeat   " + (existing->repeats == 1 ? juce::String ("once") : juce::String (existing->repeats) + times), repeatMenu);
        menu.addSubMenu ("Chance   " + percent (existing->chance), chanceMenu);
        menu.addSubMenu ("Pitch   " + (existing->pitch == 0 ? juce::String ("0 st") : signedSt (existing->pitch)), pitchMenu);
        menu.addSubMenu ("Velocity   " + percent (existing->velocity), velocityMenu);

        juce::PopupMenu randomMenu;
        const char* randomNames[] = { "Its own chop", "A neighbour (+-2 chops)", "Any chop" };
        for (int r = 0; r < 3; ++r)
            randomMenu.addItem (randomNames[r], true, existing->random == r, edit ("Step random", [&pattern, s, r] { pattern.setRandom (s, r); }));
        const char* randomShort[] = { "off", "nearby", "any" };
        menu.addSubMenu ("Random chop   " + juce::String (randomShort[existing->random]), randomMenu);
        menu.addItem ("Slide into the next step", true, existing->slide, edit ("Step slide", [&pattern, s, slide = ! existing->slide] { pattern.setSlide (s, slide); }));

        menu.addSeparator();
        const auto plain = existing->repeats == 1 && existing->chance >= 0.999f && existing->pitch == 0 && std::abs (existing->velocity - 0.85f) < 0.005f
                        && existing->random == 0 && ! existing->slide;
        menu.addItem ("Reset step", ! plain, false, edit ("Reset step", [&pattern, s]
        {
            pattern.setVelocity (s, 0.85f);
            pattern.setPitch (s, 0);
            pattern.setChance (s, 1.0f);
            pattern.setRepeats (s, 1);
            pattern.setRandom (s, 0);
            pattern.setSlide (s, false);
        }));
        menu.addItem ("Clear step", edit ("Clear step", [&pattern, s] { pattern.removeEventStartingAt (s); }));
    }

    menu.setLookAndFeel (&getLookAndFeel()); // the plugin's menu style (menus use the default look otherwise)

    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this).withMousePosition());
}

} // namespace kerf::ui

namespace kerf::ui
{


juce::File PatternView::dragFile (const juce::String& extension) const
{
    // A new file per drag: the DAW keeps referring to the dropped file.
    const auto& info = processor.getSampleInfo();
    const auto base = juce::File::createLegalFileName (info.sample != nullptr ? info.sample->name : juce::String ("KERF"));
    const auto name = base + " pattern " + juce::String (juce::roundToInt (processor.getEffectiveTempo())) + "bpm "
                    + juce::Time::getCurrentTime().formatted ("%H%M%S");
    KerfAudioProcessor::getDragFolder().createDirectory();
    return KerfAudioProcessor::getDragFolder().getNonexistentChildFile (name, "." + extension, false);
}
void PatternView::showExportMenu()
{
    juce::PopupMenu menu;
    const bool hasPattern = ! events.empty();

    for (int bits : { 24, 16, 32 })
    {
        juce::PopupMenu sub;
        for (int loops : { 1, 2, 4 })
            sub.addItem ("Pattern, " + juce::String (loops) + (loops == 1 ? " loop" : " loops"), hasPattern,
                         false, [this, bits, loops] { exportTo (true, bits, loops); });
        sub.addSeparator();
        sub.addItem ("Current sample", processor.getSampleInfo().sample != nullptr, false, [this, bits] { exportTo (false, bits, 1); });
        menu.addSubMenu (bits == 32 ? "WAV 32-bit float" : "WAV " + juce::String (bits) + "-bit", sub);
    }

    menu.setLookAndFeel (&getLookAndFeel()); // the plugin's menu style (menus use the default look otherwise)

    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&exportButton));
}

void PatternView::exportTo (bool pattern, int bitDepth, int loops)
{
    const auto& info = processor.getSampleInfo();
    const auto base = info.sample != nullptr ? info.sample->name : juce::String ("KERF");
    const auto suggested = KerfAudioProcessor::getRecordingsFolder().getParentDirectory().getChildFile ("Exports")
                               .getChildFile (base + (pattern ? " pattern" : "") + ".wav");
    suggested.getParentDirectory().createDirectory();

    chooser = std::make_unique<juce::FileChooser> ("Export WAV", suggested, "*.wav");
    chooser->launchAsync (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles
                              | juce::FileBrowserComponent::warnAboutOverwriting,
                          [this, pattern, bitDepth, loops] (const juce::FileChooser& fc)
                          {
                              auto file = fc.getResult();
                              if (file == juce::File())
                                  return;
                              file = file.withFileExtension ("wav");
                              processor.setStatusMessage ("Exporting " + file.getFileName() + " ...");
                              if (pattern)
                                  processor.exportPatternAsync (file, bitDepth, loops, {});
                              else
                                  processor.exportSampleAsync (file, bitDepth, {});
                          });
}

void PatternView::showDiceMenu()
{
    // Right-click on the die: roll in a style of your choice (it stays until AUTO).
    juce::PopupMenu menu;
    menu.addSectionHeader ("ROLL A STYLE");
    menu.addItem (1, "Auto - fits the tempo, a new one every roll", true, diceStyle < 0);
    menu.addSeparator();
    const auto names = PatternDice::styleNames();
    for (int i = 0; i < names.size(); ++i)
        menu.addItem (100 + i, names[i], true, diceStyle == i);
    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&dice),
                        [safe = juce::Component::SafePointer<PatternView> (this)] (int result)
                        {
                            if (safe == nullptr || result == 0)
                                return;
                            safe->diceStyle = result >= 100 ? result - 100 : -1;
                            safe->randomPattern();
                        });
}

void PatternView::randomPattern()
{
    // The die (PatternDice): it listens to the chops first (which is a kick, a snare, a hat,
    // which are tones, and their pitch), picks a style for the tempo (never the last one
    // twice) and writes it with everything the step grid can do, tuned notes in the key.
    // Exactly as many bars as set; one undo step.
    const auto& info = processor.getSampleInfo();
    const auto list = processor.getChops().getChops();
    if (info.sample == nullptr || list.empty())
        return;

    std::vector<PatternDice::ChopTraits> traits;
    for (const auto& c : list)
        traits.push_back (PatternDice::describe (*info.sample, c));

    auto& pattern = processor.getPattern();
    juce::Random rng; // a new pattern every time
    const auto key = processor.getSoundingKey();
    PatternDice::Options options;
    options.bars = pattern.getBars();
    options.bpm = info.sourceBpm;
    options.tonic = key.tonic;
    options.minor = key.tonic < 0 || key.minor;
    options.style = diceStyle;
    options.avoidStyle = lastDiceStyle;
    // Never a pattern seen in the last rolls (with few chops and one bar the same can come up).
    auto rolled = PatternDice::roll (traits, options, rng);
    for (int attempt = 0; attempt < 12 && std::find (recentRolls.begin(), recentRolls.end(), PatternDice::fingerprint (rolled.events)) != recentRolls.end(); ++attempt)
        rolled = PatternDice::roll (traits, options, rng);
    recentRolls.push_back (PatternDice::fingerprint (rolled.events));
    while (recentRolls.size() > 64)
        recentRolls.pop_front();
    lastDiceStyle = rolled.style;

    auto& um = processor.getUndoManager();
    um.beginNewTransaction ("Random pattern");
    pattern.clear();
    for (const auto& e : rolled.events)
    {
        pattern.setEvent (e.step, e.chopId, e.length, e.velocity);
        if (e.pitch != 0)    pattern.setPitch (e.step, e.pitch);
        if (e.chance < 1.0f) pattern.setChance (e.step, e.chance);
        if (e.repeats > 1)   pattern.setRepeats (e.step, e.repeats);
        if (e.random != 0)   pattern.setRandom (e.step, e.random);
        if (e.slide)         pattern.setSlide (e.step, true);
    }
    um.beginNewTransaction();
    auto details = juce::String (pattern.getBars()) + (pattern.getBars() == 1 ? " bar" : " bars");
    if (rolled.inKey)
        details << ", in " << KeyAnalyzer::noteName (key.tonic) << (key.minor ? " minor" : " major");
    processor.setStatusMessage ("Random pattern: " + rolled.name + " (" + details + ") - roll again, right-click: style, Ctrl+Z: undo");
}

} // namespace kerf::ui

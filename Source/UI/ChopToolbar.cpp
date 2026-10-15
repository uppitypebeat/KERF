#include "ChopToolbar.h"
#include "WaveformView.h"

namespace kerf::ui
{

ChopToolbar::ChopToolbar (KerfAudioProcessor& p, WaveformView& w)
    : processor (p), waveform (w), settings (p.getSettings())
{
    addAndMakeVisible (modes);
    modes.setSelected ((int) settings.getProperty ("chopMode", 0), juce::dontSendNotification);
    modes.setTooltip ("How to chop: rhythmic grid, equal slices, transients, beats, random or smart (rule-based) suggestions");
    modes.onChange = [this] (int m)
    {
        if (m != (int) settings.getProperty ("chopMode", 0))
            lastAppliedMode = -1; // the same mode again keeps live re-chopping
        settings.setProperty ("chopMode", m, nullptr);
        showModeControls();
        updatePending();
        resized();
        repaint(); // the header shows the mode
    };

    // Grid
    addChildComponent (gridValue);
    gridValue.setSelected ((int) settings.getProperty ("gridValue", 4), juce::dontSendNotification);
    gridValue.setTooltip ("Note value of one grid step");
    gridValue.onChange = [this] (int v) { settings.setProperty ("gridValue", v, nullptr); settingChanged(); };

    addChildComponent (gridModifier);
    gridModifier.setSelected ((int) settings.getProperty ("gridModifier", 0), juce::dontSendNotification);
    gridModifier.setTooltip ("STR straight  -  TRI triplet (3 in the time of 2)  -  DUP duplet (2 in the time of 3)  -  DOT dotted");
    gridModifier.onChange = [this] (int v) { settings.setProperty ("gridModifier", v, nullptr); settingChanged(); };

    // Count (equal / random / smart)
    addChildComponent (count);
    count.setValue ((int) settings.getProperty ("sliceCount", 8));
    count.setTooltip ("Number of chops. Drag or double-click.");
    count.onChange = [this] (double v) { settings.setProperty ("sliceCount", (int) v, nullptr); settingChanged(); };

    // Transients
    addChildComponent (sensitivity);
    sensitivity.setSuffix ("%");
    sensitivity.setValue ((float) settings.getProperty ("sensitivity", 0.3f) * 100.0);
    sensitivity.setTooltip ("Higher finds more, quieter transients");
    sensitivity.onChange = [this] (double v) { settings.setProperty ("sensitivity", (float) v / 100.0f, nullptr); settingChanged(); };

    addChildComponent (minDistance);
    minDistance.setSuffix (" ms");
    minDistance.setValue ((float) settings.getProperty ("minDistance", 0.12f) * 1000.0);
    minDistance.setTooltip ("Minimum time between chops");
    minDistance.onChange = [this] (double v) { settings.setProperty ("minDistance", (float) v / 1000.0f, nullptr); settingChanged(); };

    // Beats
    addChildComponent (beats);
    beats.setSelected ((int) settings.getProperty ("beatsPerChop", 1), juce::dontSendNotification);
    beats.setTooltip ("Beats per chop (uses the sample tempo)");
    beats.onChange = [this] (int v) { settings.setProperty ("beatsPerChop", v, nullptr); settingChanged(); };

    // Random
    addChildComponent (reroll);
    reroll.setTooltip ("New random chops");
    reroll.onClick = [this]
    {
        settings.setProperty ("randomSeed", (int) settings.getProperty ("randomSeed", 1) + 1, nullptr);
        settingChanged();
        if (lastAppliedMode != random) apply();
    };

    addChildComponent (shift);
    shift.setTooltip ("SHIFT: moves the sample against the beat lines (+: the sample later). Line the lines up with the hits, then BEAT cuts on them; the metronome and grid snapping follow. Drag (Shift: fine), mouse wheel: 1 ms (Shift: 0.1 ms), double-click: back to 0");
    shift.onChange = [this] (double ms)
    {
        processor.setSampleShift (ms / 1000.0);
        waveform.repaint();
    };

    addAndMakeVisible (applyButton);
    applyButton.setTooltip ("Replace the chops (undoable)");
    applyButton.onClick = [this] { apply(); };
    applyButton.setPrimary (false); // lights up when there is something to apply

    // Right side
    addAndMakeVisible (scope);
    scope.setSelected ((int) settings.getProperty ("chopScope", 0), juce::dontSendNotification);
    scope.setTooltip ("ALL: chop the whole sample  -  CHOP: chop only inside the selected chop");
    scope.onChange = [this] (int v) { settings.setProperty ("chopScope", v, nullptr); lastAppliedMode = -1; updatePending(); };

    appliedSignature = settingsSignature(); // nothing to apply at first
    processor.addChangeListener (this);
    showModeControls();
}

ChopToolbar::~ChopToolbar()
{
    processor.removeChangeListener (this);
}

void ChopToolbar::changeListenerCallback (juce::ChangeBroadcaster*)
{
    const auto& info = processor.getSampleInfo();
    const auto hasSample = info.sample != nullptr;
    const auto m = modes.getSelected();
    const bool needsAnalysis = m == transient || m == smart;
    const bool needsTempo = m == beat;
    applyButton.setEnabled (hasSample && (! needsAnalysis || info.onsets != nullptr) && (! needsTempo || info.sourceBpm > 0.0));

    shift.setEnabled (hasSample);
    shift.setValue (processor.getSampleShift() * 1000.0); // (undo, another sample)

    // A new sample starts with nothing pending.
    if (info.sample.get() != lastSample)
    {
        lastSample = info.sample.get();
        appliedSignature = settingsSignature();
        updatePending();
        lastAppliedMode = -1;
    }
}

juce::String ChopToolbar::actionName (int m) const
{
    static const char* names[] = { "Grid chop", "Equal slices", "Transient chop", "Beat chop", "Random chop", "Smart chop" };
    return names[juce::jlimit (0, 5, m)];
}

void ChopToolbar::apply()
{
    const auto m = modes.getSelected();
    const auto n = (int) count.getValue();

    switch (m)
    {
        case grid:      processor.chopByGrid ((NoteValue) gridValue.getSelected(), (NoteModifier) gridModifier.getSelected()); break;
        case equal:     processor.chopEqual (n); break;
        case transient: processor.chopByTransients ((float) sensitivity.getValue() / 100.0f, minDistance.getValue() / 1000.0); break;
        case beat:
        {
            static constexpr double beatOptions[] = { 0.5, 1.0, 2.0, 4.0 };
            processor.chopByBeats (beatOptions[juce::jlimit (0, 3, beats.getSelected())]);
            break;
        }
        case random:    processor.chopRandom (n, (uint32_t) (int) settings.getProperty ("randomSeed", 1)); break;
        case smart:     processor.chopSmart (n); break;
        default: break;
    }

    lastAppliedMode = m;
    appliedSignature = settingsSignature();
    updatePending();
}

juce::String ChopToolbar::settingsSignature() const
{
    juce::String s;
    for (const auto* key : { "chopMode", "gridValue", "gridModifier", "sliceCount", "sensitivity", "minDistance", "beatsPerChop", "randomSeed", "chopScope" })
        s << settings.getProperty (key).toString() << "|";
    return s;
}

void ChopToolbar::updatePending()
{
    pending = settingsSignature() != appliedSignature;
    applyButton.setPrimary (pending);
}

void ChopToolbar::settingChanged()
{
    // The value that is already set clicked again (1/1 on 1/1): nothing changed, nothing
    // to do (no re-chop, no spawn animation).
    if (settingsSignature() == appliedSignature)
    {
        updatePending();
        return;
    }

    // Live re-chop: if the last edit was this mode's apply, replace it.
    auto& um = processor.getUndoManager();
    if (lastAppliedMode == modes.getSelected() && um.getUndoDescription() == actionName (lastAppliedMode))
    {
        um.undo();
        apply(); // applied already: nothing pending
    }
    else
    {
        updatePending();
    }
}

void ChopToolbar::showModeControls()
{
    const auto m = modes.getSelected();
    gridValue.setVisible (m == grid);
    gridModifier.setVisible (m == grid);
    count.setVisible (m == equal || m == random || m == smart);
    sensitivity.setVisible (m == transient);
    minDistance.setVisible (m == transient);
    beats.setVisible (m == beat);
    shift.setVisible (m == beat);
    reroll.setVisible (m == random);

    paramLabel = m == equal || m == random || m == smart ? "COUNT" : (m == transient ? "SENS" : (m == beat ? "BEATS" : ""));
    paramLabel2 = m == transient ? "GAP" : (m == beat ? "SHIFT" : "");
    changeListenerCallback (nullptr);
    repaint();
}

void ChopToolbar::resized()
{
    // A column (the SLICE section, left of the CHOP panel), top to bottom:
    //   mode (3 x 2)  |  the mode's settings  |  scope (ALL / CHOP) + APPLY
    auto r = getLocalBounds().reduced (18, 0);
    r.removeFromRight (14);
    const auto x = r.getX(), w = r.getWidth();
    const auto h = 22;

    labelArea = r.withY (6).withHeight (18).toFloat();
    modes.setColumns (3);
    modes.setBounds (x, 32, w, 2 * h);

    const auto m = modes.getSelected();
    paramLabelArea = {};
    paramLabel2Area = {};
    auto row1 = juce::Rectangle<int> (x, 86, w, h), row2 = juce::Rectangle<int> (x, 114, w, h);

    auto labelled = [] (juce::Rectangle<int>& row, juce::Rectangle<float>& labelArea, juce::Component& field, int fieldWidth)
    {
        labelArea = row.removeFromLeft (46).toFloat();
        field.setBounds (row.removeFromLeft (fieldWidth).withSizeKeepingCentre (fieldWidth, 18));
        row.removeFromLeft (12);
    };

    if (m == grid)
    {
        gridValue.setBounds (row1);
        gridModifier.setBounds (row2);
    }
    else if (m == beat)
    {
        paramLabelArea = row1.removeFromLeft (46).toFloat();
        beats.setBounds (row1);
        paramLabel2Area = row2.removeFromLeft (46).toFloat();
        shift.setBounds (row2);
    }
    else if (m == transient)
    {
        labelled (row1, paramLabelArea, sensitivity, 52);
        labelled (row2, paramLabel2Area, minDistance, 66);
    }
    else
    {
        labelled (row1, paramLabelArea, count, 40);
        if (m == random)
            reroll.setBounds (row1.removeFromLeft (56).withSizeKeepingCentre (56, h));
    }

    // The action, and where it acts.
    auto bottom = juce::Rectangle<int> (x, 146, w, h);
    scope.setBounds (bottom.removeFromLeft (96));
    bottom.removeFromLeft (8);
    applyButton.setBounds (bottom);
}

void ChopToolbar::paint (juce::Graphics& g)
{
    fillBackdrop (g, *this, getLocalBounds().toFloat());
    const juce::String modeNames[] = { "GRID", "EQUAL", "TRANSIENTS", "BEATS", "RANDOM", "SMART" };
    drawSectionHeader (g, labelArea, "SLICE", modeNames[juce::jlimit (0, 5, modes.getSelected())]);

    if (paramLabel.isNotEmpty() && modes.getSelected() != grid)
        drawText (g, paramLabel, paramLabelArea, TextStyle::label, Colours::textDim);
    if (paramLabel2.isNotEmpty())
        drawText (g, paramLabel2, paramLabel2Area, TextStyle::label, Colours::textDim);

    // Divider towards the CHOP panel on the right.
    drawHairline (g, { (float) getWidth() - 1.0f, 6.0f, 1.0f, (float) getHeight() - 12.0f });
}

//==============================================================================
WaveTools::WaveTools (KerfAudioProcessor& p, WaveformView& w, AnimationClock& c) : processor (p), waveform (w), clock (c)
{
    addAndMakeVisible (snap);
    snap.setToggleState ((bool) processor.getSettings().getProperty ("snap", true), juce::dontSendNotification);
    snap.setTooltip ("Snap: boundaries snap to transients and the grid (hold Shift to bypass)");
    snap.onClick = [this] { processor.getSettings().setProperty ("snap", snap.getToggleState(), nullptr); };

    addAndMakeVisible (knife);
    knife.setTooltip ("Knife (K): click the waveform to split. Without it, double-click splits.");
    knife.onClick = [this] { waveform.setKnifeMode (knife.getToggleState()); };

    addAndMakeVisible (bin);
    bin.setDangerOnHover (true);
    bin.setTooltip ("Delete the selected chops (Ctrl + click picks more). Ctrl+Z brings them back");
    bin.onClick = [this] { processor.deleteSelectedChops(); };

    clock.add (this);
}

WaveTools::~WaveTools()
{
    clock.remove (this);
}

void WaveTools::resized()
{
    auto r = getLocalBounds();
    knife.setBounds (r.removeFromLeft (26).withSizeKeepingCentre (26, 22)); // knife first, then snap
    r.removeFromLeft (4);
    snap.setBounds (r.removeFromLeft (26).withSizeKeepingCentre (26, 22));
    r.removeFromLeft (4);
    bin.setBounds (r.removeFromLeft (26).withSizeKeepingCentre (26, 22));
}

void WaveTools::animationTick (double)
{
    const auto canClear = processor.getSampleInfo().sample != nullptr && ! processor.getSelectedChops().empty()
                       && ! (processor.getChops().getNumChops() == 1 && processor.getChops().getChops().front().length() >= processor.getSampleInfo().sample->getNumSamples()); // (not the one chop over the whole sample)
    if (bin.isEnabled() != canClear)
        bin.setEnabled (canClear);

    if (knife.getToggleState() != waveform.isKnifeMode())
        knife.setToggleState (waveform.isKnifeMode(), juce::dontSendNotification);

    const auto snapOn = (bool) processor.getSettings().getProperty ("snap", true);
    if (snap.getToggleState() != snapOn)
        snap.setToggleState (snapOn, juce::dontSendNotification);
}

} // namespace kerf::ui

namespace kerf::ui
{

TransportTools::TransportTools (AnimationClock& c) : clock (c)
{
    addAndMakeVisible (play);
    play.setIconWhenOn (IconButton::Icon::pause);
    play.setTooltip ("Play the whole selected chop / pause (PLAY again goes on from there). Space: play / stop");
    play.onClick = [this] { if (onPlayPause) onPlayPause(); };

    addAndMakeVisible (stop);
    stop.setTooltip ("Stop the chop and the pattern (the next PLAY starts from the beginning)");
    stop.onClick = [this] { if (onStop) onStop(); };

    addAndMakeVisible (scope);
    scope.setTooltip ("What PLAY and Space play: CHOP = the selected chop, SAMPLE = the whole sample from the start");
    scope.onChange = [this] (int s) { if (onScope) onScope (s); };

    addAndMakeVisible (loop);
    loop.setTooltip ("Loop: the chop played with PLAY or Space repeats until you stop it");
    loop.onClick = [this] { if (onLoop) onLoop (loop.getToggleState()); };

    addAndMakeVisible (metronome);
    metronome.setTooltip ("Metronome: a click on every beat while the song or the pattern plays (not recorded)");
    metronome.onClick = [this] { if (onMetronome) onMetronome (metronome.getToggleState()); };

    clock.add (this);
}

TransportTools::~TransportTools()
{
    clock.remove (this);
}

void TransportTools::resized()
{
    auto r = getLocalBounds();
    play.setBounds (r.removeFromLeft (26).withSizeKeepingCentre (26, 22));
    r.removeFromLeft (4);
    stop.setBounds (r.removeFromLeft (26).withSizeKeepingCentre (26, 22));
    r.removeFromLeft (toolbarGroupGap);
    scope.setBounds (r.removeFromLeft (scopeWidth).withSizeKeepingCentre (scopeWidth, 22));
    r.removeFromLeft (toolbarGroupGap);
    loop.setBounds (r.removeFromLeft (26).withSizeKeepingCentre (26, 22));
    r.removeFromLeft (4);
    metronome.setBounds (r.removeFromLeft (26).withSizeKeepingCentre (26, 22));
}

void TransportTools::animationTick (double)
{
    const auto playing = isPlaying && isPlaying();
    if (play.getToggleState() != playing)
        play.setToggleState (playing, juce::dontSendNotification);
}

} // namespace kerf::ui

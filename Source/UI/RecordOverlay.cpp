#include "RecordOverlay.h"

namespace kerf::ui
{

void handleRecordButton (KerfAudioProcessor& processor, AudioRecorder::Source source, juce::Component& anchor)
{
    auto& rec = processor.getRecorder();

    if (rec.isActive())
    {
        if (rec.getSource() == source)
            rec.requestStop();
        return;
    }

    const bool isInput = source == AudioRecorder::Source::input;
    juce::PopupMenu menu;
    menu.addSectionHeader (isInput ? "RECORD DAW INPUT" : "CAPTURE KERF OUTPUT");
    menu.addItem ("Until stopped", [&processor, source] { processor.startRecording (source, 0.0); });
    for (int bars : { 1, 2, 4, 8 })
        menu.addItem (juce::String (bars) + (bars == 1 ? " bar" : " bars"), [&processor, source, bars] { processor.startRecording (source, bars); });
    menu.addSeparator();
    menu.addItem (isInput ? "Bar lengths start on the next bar while the DAW plays. Audio must be routed into KERF's input."
                          : "Bar lengths start on the next bar while the DAW plays.",
                  false, false, nullptr);

    menu.setLookAndFeel (&anchor.getLookAndFeel()); // the plugin's menu style
    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&anchor));
}

RecordOverlay::RecordOverlay (KerfAudioProcessor& p, AnimationClock& c) : processor (p), clock (c)
{
    addAndMakeVisible (stop);
    addAndMakeVisible (cancel);
    stop.setTooltip ("Finish and keep the recording as the new sample");
    cancel.setTooltip ("Discard the recording");
    stop.onClick = [this] { processor.getRecorder().requestStop(); };
    cancel.onClick = [this] { processor.getRecorder().requestCancel(); };
    setInterceptsMouseClicks (true, true); // block waveform edits while recording
    clock.add (this);
}

RecordOverlay::~RecordOverlay()
{
    clock.remove (this);
}

juce::Rectangle<float> RecordOverlay::panelBounds() const
{
    return juce::Rectangle<float> (360.0f, 112.0f).withCentre (getLocalBounds().toFloat().getCentre());
}

void RecordOverlay::resized()
{
    const auto panel = panelBounds();
    auto buttons = juce::Rectangle<float> (panel.getX() + 20.0f, panel.getBottom() - 34.0f, panel.getWidth() - 40.0f, 22.0f);
    cancel.setBounds (buttons.removeFromRight (62.0f).toNearestInt());
    buttons.removeFromRight (6.0f);
    stop.setBounds (buttons.removeFromRight (52.0f).toNearestInt());
}

void RecordOverlay::animationTick (double dt)
{
    const auto active = processor.getRecorder().isActive();
    const auto target = active ? 1.0f : 0.0f;

    if (std::abs (appear - target) > 0.001f)
        appear = target > appear ? std::min (1.0f, appear + (float) (dt / Motion::standard))
                                 : std::max (0.0f, appear - (float) (dt / Motion::standard));

    setVisible (appear > 0.001f);
    if (! isVisible())
        return;

    blink = std::fmod (blink + (float) dt, 1.0f);
    repaint();
}

void RecordOverlay::paint (juce::Graphics& g)
{
    const auto& rec = processor.getRecorder();
    const auto alpha = Motion::easeOut (appear);

    g.setColour (Colours::background.withAlpha (0.72f * alpha));
    g.fillRect (getLocalBounds());

    const auto panel = panelBounds();
    g.setColour (Colours::surface.withAlpha (alpha));
    g.fillRect (panel);
    g.setColour (Colours::record.withAlpha (0.7f * alpha));
    g.drawRect (panel, 1.0f);

    auto r = panel.reduced (20.0f, 16.0f);
    const auto isInput = rec.getSource() == AudioRecorder::Source::input;
    const auto state = rec.getState();

    // Title row with a blinking record dot
    auto title = r.removeFromTop (16.0f);
    const auto dotOn = state == AudioRecorder::State::recording ? blink < 0.6f : true;
    g.setColour ((state == AudioRecorder::State::recording ? Colours::record : Colours::textDim).withAlpha ((dotOn ? 1.0f : 0.25f) * alpha));
    g.fillEllipse (juce::Rectangle<float> (8.0f, 8.0f).withCentre ({ title.getX() + 4.0f, title.getCentreY() }));
    title.removeFromLeft (16.0f);

    juce::String heading = isInput ? "REC  DAW INPUT" : "CAPTURE  KERF OUTPUT";
    drawText (g, heading, title, TextStyle::sectionLabel, Colours::text.withAlpha (alpha));

    juce::String stateText;
    switch (state)
    {
        case AudioRecorder::State::armed:         stateText = "ARMED"; break;
        case AudioRecorder::State::waitingForBar: stateText = "WAITING FOR BAR"; break;
        case AudioRecorder::State::recording:     stateText = "RECORDING"; break;
        default: break;
    }
    drawText (g, stateText, title, TextStyle::label, Colours::record.withAlpha (alpha), juce::Justification::centredRight);

    r.removeFromTop (10.0f);

    // Time
    auto fmt = [] (double s) { return juce::String ((int) (s / 60.0)) + ":" + juce::String (std::fmod (s, 60.0), 1).paddedLeft ('0', 4); };
    auto timeText = fmt (rec.getRecordedSeconds());
    if (rec.getTargetSeconds() > 0.0)
        timeText << "  /  " << fmt (rec.getTargetSeconds());
    drawText (g, timeText, r.removeFromTop (18.0f), TextStyle::valueLarge, Colours::text.withAlpha (alpha));

    r.removeFromTop (8.0f);

    // Level meter (so silent routing is obvious immediately)
    auto meter = r.removeFromTop (3.0f).withWidth (150.0f);
    drawHairline (g, meter, Colours::hairline.withMultipliedAlpha (alpha));
    const auto db = juce::Decibels::gainToDecibels (rec.getLevel(), -60.0f);
    g.setColour ((rec.getLevel() > 0.99f ? Colours::record : Colours::text).withAlpha (0.8f * alpha));
    g.fillRect (meter.withWidth (meter.getWidth() * juce::jmap (db, -60.0f, 0.0f, 0.0f, 1.0f)));

    if (isInput && state == AudioRecorder::State::recording && rec.getRecordedSeconds() > 1.0 && rec.getLevel() < 1.0e-4f)
        drawText (g, "No input signal", juce::Rectangle<float> (meter.getRight() + 10.0f, meter.getY() - 6.0f, 140.0f, 14.0f),
                  TextStyle::label, Colours::accent.withAlpha (alpha));
}

} // namespace kerf::ui

namespace kerf::ui
{

RecordTools::RecordTools (KerfAudioProcessor& p, AnimationClock& c) : processor (p), clock (c)
{
    for (auto* b : { &recButton, &captureButton, &resampleButton })
        addAndMakeVisible (b);

    recButton.setTooltip ("REC: record audio coming into KERF from the DAW (needs input routing)");
    recButton.onClick = [this] { handleRecordButton (processor, AudioRecorder::Source::input, recButton); };
    captureButton.setTooltip ("CAPTURE: record what KERF plays (your performance or pattern) as a new sample");
    captureButton.onClick = [this] { handleRecordButton (processor, AudioRecorder::Source::output, captureButton); };
    resampleButton.setTooltip ("RESAMPLE: turn the SEQ pattern, with every effect, into a new sample (one seamless loop) to chop again. Undo brings the old sample back.");
    resampleButton.onClick = [this] { processor.resamplePatternAsync ({}); };
    clock.add (this);
}

RecordTools::~RecordTools()
{
    clock.remove (this);
}

void RecordTools::resized()
{
    // [rec] [capture] [resample]
    auto r = getLocalBounds();
    for (auto* b : { &recButton, &captureButton, &resampleButton })
    {
        b->setBounds (r.removeFromLeft (26).withSizeKeepingCentre (26, 22));
        r.removeFromLeft (4);
    }
}

void RecordTools::animationTick (double)
{
    const auto& rec = processor.getRecorder();
    const auto active = rec.isActive();
    const auto recOn = active && rec.getSource() == AudioRecorder::Source::input;
    const auto capOn = active && rec.getSource() == AudioRecorder::Source::output;
    if (recButton.getToggleState() != recOn) recButton.setToggleState (recOn, juce::dontSendNotification);
    if (captureButton.getToggleState() != capOn) captureButton.setToggleState (capOn, juce::dontSendNotification);
    recButton.setEnabled (! active || recOn);
    captureButton.setEnabled (! active || capOn);
    resampleButton.setEnabled (! active && processor.getSampleInfo().sample != nullptr);
}

} // namespace kerf::ui

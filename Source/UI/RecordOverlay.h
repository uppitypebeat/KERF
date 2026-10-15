#pragma once

#include "Controls.h"
#include "../PluginProcessor.h"

namespace kerf::ui
{

/** Shown over the waveform while REC or CAPTURE is active: state, time, input level,
    STOP (keep the take) and CANCEL (discard). */
class RecordOverlay final : public juce::Component, private AnimationClock::Client
{
public:
    RecordOverlay (KerfAudioProcessor& processor, AnimationClock& clock);
    ~RecordOverlay() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void animationTick (double dt) override;
    juce::Rectangle<float> panelBounds() const;

    KerfAudioProcessor& processor;
    AnimationClock& clock;
    PillButton stop { "Stop" }, cancel { "Cancel" };
    float blink = 0.0f;
    float appear = 0.0f;
};

/** Opens the REC / CAPTURE length menu, or stops an active recording. */
void handleRecordButton (KerfAudioProcessor& processor, AudioRecorder::Source source, juce::Component& anchor);

/** The recording symbols in the tool row: REC (DAW input), CAPTURE (KERF's output) and
    RESAMPLE (the pattern rendered into a new sample). On every page. */
class RecordTools final : public juce::Component, private AnimationClock::Client
{
public:
    RecordTools (KerfAudioProcessor& processor, AnimationClock& clock);
    ~RecordTools() override;
    void resized() override;

    static constexpr int idealWidth = 3 * 26 + 2 * 4;

private:
    void animationTick (double) override; // lights REC / CAPTURE while recording

    KerfAudioProcessor& processor;
    AnimationClock& clock;
    IconButton recButton { "Rec", IconButton::Icon::record, false, true }, captureButton { "Capture", IconButton::Icon::capture, false, true };
    IconButton resampleButton { "Resample", IconButton::Icon::resample, false };
};

} // namespace kerf::ui

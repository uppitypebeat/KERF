#pragma once

#include "Controls.h"
#include "../PluginProcessor.h"

namespace kerf::ui
{

class WaveformView;

/** Chopping tools: mode (GRID / EQUAL / TRANSIENT / BEAT / RANDOM / SMART), the active
    mode's settings, APPLY and scope (whole sample or selected chop).

    Adjusting a mode's settings right after applying it re-chops live (replacing the last
    result in a single undo step), so users can "scrub" sensitivity or slice counts. */
class ChopToolbar final : public juce::Component, private juce::ChangeListener
{
public:
    ChopToolbar (KerfAudioProcessor& processor, WaveformView& waveform);
    ~ChopToolbar() override;

    void paint (juce::Graphics&) override;
    void resized() override;

    void apply();

private:
    enum Mode { grid = 0, equal, transient, beat, random, smart };

    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    void showModeControls();
    void settingChanged();
    juce::String actionName (int m) const;

    KerfAudioProcessor& processor;
    WaveformView& waveform;
    juce::ValueTree settings;

    Segmented modes { { "GRID", "EQUAL", "TRANS", "BEAT", "RANDOM", "SMART" } };

    Segmented gridValue { { "1/1", "1/2", "1/4", "1/8", "1/16", "1/32" }, 4 };
    Segmented gridModifier { { "STR", "TRI", "DUP", "DOT" } };
    ValueField count { 2, 68, 1, 0 }; // at most 68: from the root note (middle C) up to the last MIDI note, one chop per key
    ValueField sensitivity { 0, 100, 1, 0 };
    ValueField minDistance { 10, 1000, 5, 0 };
    Segmented beats { { "1/2", "1", "2", "BAR" }, 1 };
    PillButton reroll { "New" };
    CentreSlider shift { KerfAudioProcessor::maxSampleShift * 1000.0, 0.1, 1, " ms" }; // BEAT: the sample against the beat lines
    PillButton applyButton { "Apply" };

    Segmented scope { { "ALL", "CHOP" } };

    juce::Rectangle<float> labelArea, paramLabelArea, paramLabel2Area;
    juce::String paramLabel, paramLabel2;
    int lastAppliedMode = -1;

    /** APPLY lights up (accent) only while the SLICE settings differ from the ones last
        applied (re-selecting the same mode is no change). */
    void updatePending();
    juce::String settingsSignature() const;
    juce::String appliedSignature;
    bool pending = false;
    const void* lastSample = nullptr;
};

/** The space between two groups of buttons in the tool row (inside a group: 4 px). */
inline constexpr int toolbarGroupGap = 12;

/** Small tool row above the waveform (top left): KNIFE, SNAP and the bin (delete the selected chops) as symbols. */
class WaveTools final : public juce::Component, private AnimationClock::Client
{
public:
    WaveTools (KerfAudioProcessor& processor, WaveformView& waveform, AnimationClock& clock);
    ~WaveTools() override;

    void resized() override;

private:
    void animationTick (double) override; // follows the K key and other changes

    KerfAudioProcessor& processor;
    WaveformView& waveform;
    AnimationClock& clock;
    IconButton snap { "Snap", IconButton::Icon::magnet }, knife { "Knife", IconButton::Icon::knife };
    IconButton bin { "Delete the selected chops", IconButton::Icon::trash, false }; // red under the mouse

public:
    static constexpr int idealWidth = 26 + 4 + 26 + 4 + 26;
};

/** PLAY / PAUSE and STOP at the top left, on every page. PLAY plays the whole selected chop
    (the pattern has its own PLAY in SEQ); while something plays it is PAUSE (the chop or the
    pattern hold where they are, PLAY goes on from there); STOP stops both. LOOP: the chop repeats until stopped; the METRONOME clicks while the song
    or the pattern plays (heard, never recorded). */
class TransportTools final : public juce::Component, private AnimationClock::Client
{
public:
    explicit TransportTools (AnimationClock& clock);
    ~TransportTools() override;
    void resized() override;

    std::function<void()> onPlayPause, onStop;
    std::function<bool()> isPlaying; // PLAY shows PAUSE while something plays
    std::function<void (bool)> onLoop, onMetronome;
    std::function<void (int)> onScope; // what PLAY plays: 0 the selected chop, 1 the whole sample
    void setLoop (bool on)      { loop.setToggleState (on, juce::dontSendNotification); }
    void setScope (int s)       { if (scope.getSelected() != s) scope.setSelected (s, juce::dontSendNotification); }
    void setMetronome (bool on) { metronome.setToggleState (on, juce::dontSendNotification); }
    static constexpr int scopeWidth = 112;
    static constexpr int idealWidth = 26 + 4 + 26 + toolbarGroupGap + scopeWidth + toolbarGroupGap + 26 + 4 + 26; // [play][stop] [CHOP|SAMPLE] [loop][metronome]
    static constexpr int compactWidth = 26 + 4 + 26;                  // [play][stop] (LOOP and the METRONOME: CHOP page only)
    void showExtras (bool show) { loop.setVisible (show); metronome.setVisible (show); scope.setVisible (show); }
    int getIdealWidth() const   { return loop.isVisible() ? idealWidth : compactWidth; }

private:
    void animationTick (double) override;

    AnimationClock& clock;
    IconButton play { "Play", IconButton::Icon::play, false }; // the pause bars while something plays
    IconButton stop { "Stop", IconButton::Icon::stop, false };
    IconButton loop { "Loop", IconButton::Icon::loop }, metronome { "Metronome", IconButton::Icon::metronome };
    Segmented scope { { "CHOP", "SAMPLE" } };
};

} // namespace kerf::ui

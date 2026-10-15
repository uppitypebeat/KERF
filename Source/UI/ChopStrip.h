#pragma once

#include "Controls.h"
#include "../PluginProcessor.h"

namespace kerf::ui
{

juce::String noteName (int midiNote);

/** Row of chop pads in timeline order. Shows each chop's number and MIDI note, lights up
    while the chop plays, and plays the chop while a pad is held down. */
class ChopStrip final : public juce::Component,
                        private AnimationClock::Client,
                        private juce::ValueTree::Listener,
                        private juce::ChangeListener
{
public:
    ChopStrip (KerfAudioProcessor& processor, AnimationClock& clock);
    ~ChopStrip() override;

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;

    std::function<void (int chopId)> onChopSelected;

    void showKitMenu(); // the KIT export menu (public for the screenshot tool)

private:
    void animationTick (double dt) override;
    void valueTreePropertyChanged (juce::ValueTree&, const juce::Identifier&) override { refresh(); }
    void valueTreeChildAdded (juce::ValueTree&, juce::ValueTree&) override             { refresh(); }
    void valueTreeChildRemoved (juce::ValueTree&, juce::ValueTree&, int) override      { refresh(); }
    void valueTreeChildOrderChanged (juce::ValueTree&, int, int) override              {}

    void changeListenerCallback (juce::ChangeBroadcaster*) override { refresh(); }
    void refresh();
    void rebuildThumbs();
    juce::Rectangle<float> padBounds (int index) const;
    int padAt (juce::Point<float>) const;
    void ensurePadVisible (int index);
    void releasePreview();

    KerfAudioProcessor& processor;
    AnimationClock& clock;

    std::vector<ChopParams> chops;
    std::map<int, float> glow;
    std::map<int, std::vector<float>> thumbs; // per chop: mini waveform (peaks 0..1)
    std::map<int, float> kick;                // per chop: hit flash, 1 on note-on then decays
    std::map<int, float> playPos;             // per chop: playhead 0..1 while it sounds
    std::array<uint32_t, SamplerEngine::numPhysicalVoices> lastTriggers {};
    float selX = -1.0f;                       // selection outline glides between pads (content x)
    float scrollTarget = 0.0f;
    int followedChop = 0;                     // last selection the strip scrolled to
    juce::String selectionSig;                // the chops chosen with Ctrl, as last painted
    int pendingToggle = 0;                    // Ctrl + click on this chop: (de)select it on release
    SampleData::Ptr thumbSample;
    juce::Rectangle<float> padArea, rootArea;
    float scrollX = 0.0f;
    int hovered = -1;
    int previewChopId = 0; // the pad being held

    // Dragging a pad out of KERF: the chop as audio (or MIDI with Ctrl / Cmd).
    int dragPad = -1;
    bool dragStarted = false, dragRendering = false;
    juce::File dragReady;
    int dragGeneration = 0;
    juce::File chopDragFile (int index, const juce::String& extension) const;

    ValueField rootField { 0, 127, 1, 0 };
    PillButton remapButton { "Remap" }, kitButton { "Kit" };
    std::unique_ptr<juce::FileChooser> kitChooser;

    static constexpr float padWidth = 58.0f, padGap = 4.0f;
};

} // namespace kerf::ui

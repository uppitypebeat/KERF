#pragma once

#include "PluginProcessor.h"
#include "UI/Theme.h"
#include "UI/Controls.h"
#include "UI/HeaderBar.h"
#include "UI/WaveformView.h"
#include "UI/ChopToolbar.h"
#include "UI/ChopStrip.h"
#include "UI/ControlPanel.h"
#include "UI/PatternView.h"
#include "UI/RecordOverlay.h"
#include "UI/AboutOverlay.h"
#include "UI/UpdateOverlay.h"
#include "UI/SavePresetOverlay.h"
#include "UI/StartBurst.h"
#include "UI/GuideOverlay.h"
#include "UI/GrainView.h"
#include "UI/FxView.h"
#include "UI/EqView.h"

namespace kerf::ui
{

/** Bottom strip: voice count, retrigger policy, trigger quantise, MIDI activity, hints. */
class FooterBar final : public juce::Component, private AnimationClock::Client
{
public:
    FooterBar (KerfAudioProcessor& processor, AnimationClock& clock);
    ~FooterBar() override;

    void setHint (const juce::String& h) { if (h != hint) { hint = h; repaint(); } }

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void animationTick (double dt) override;

    KerfAudioProcessor& processor;
    AnimationClock& clock;

    ValueField voices { 1, 32, 1, 0 };
    Segmented retrigger { { "POLY", "SAME", "MONO" } };
    Segmented quantize { { "OFF", "1/16", "1/8", "1/4", "BAR" } };
    std::unique_ptr<juce::ParameterAttachment> voicesAttachment, retriggerAttachment, quantizeAttachment;

    juce::Rectangle<float> voicesLabel, retrigLabel, quantLabel, midiArea, hintArea;
    juce::String hint, shownStatus;
    double statusTime = 0.0;
    int lastMidiCount = 0;
    float midiFlash = 0.0f;
    int activeVoices = 0;
};

/** All UI at its design size (1120 x 700). The editor scales this component uniformly. */
class MainView final : public juce::Component, private AnimationClock::Client
{
public:
    MainView (KerfAudioProcessor& processor, AnimationClock& clock);
    ~MainView() override;

    /** Shows the waveform, the GRAIN view or the EQ view for a control-panel page. */
    void showViewForPage (int page);

    /** The first-start guide (again), on the CHOP page with the pads. */
    void startGuide();

    void paint (juce::Graphics&) override;
    void resized() override;

    HeaderBar header;
    WaveformView waveform;
    GrainView grainView;
    FxView fxView;
    EqView eqView;
    TimeView timeView;
    WaveTools tools;
    EqTypeTools eqTools;
    TransportTools transport;
    ChopToolbar toolbar;
    ChopStrip chopStrip;
    PatternView patternView;
    Segmented performTabs { { "PADS", "SEQ" } };
    ControlPanel controls;
    PageTabs pageTabs;
    FooterBar footer;
    RecordOverlay recordOverlay;
    RecordTools recordTools;
    AboutOverlay about;
    GuideOverlay guide;
    PageOffVeil pageOffVeil;     // over a page switched off with a double-click on its tab
    UpdateOverlay updateOverlay; // "Check for updates" in the menu
    SavePresetOverlay savePreset; // "Save as..." in the preset menu
    StartBurst startBurst;       // the opening after the guide's START

private:
    void animationTick (double dt) override;
    void updateChopRowVisibility(); // pads / SEQ, chop tools: CHOP page only
    KerfAudioProcessor& processor;
    AnimationClock& clock;
    juce::Component* fadingIn = nullptr;
};

} // namespace kerf::ui

class KerfAudioProcessorEditor final : public juce::AudioProcessorEditor,
                                       public juce::FileDragAndDropTarget,
                                       private juce::Timer,
                                       private juce::ChangeListener
{
public:
    explicit KerfAudioProcessorEditor (KerfAudioProcessor&);
    ~KerfAudioProcessorEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;
    bool keyPressed (const juce::KeyPress&) override;

    bool isInterestedInFileDrag (const juce::StringArray& files) override;
    void fileDragEnter (const juce::StringArray& files, int x, int y) override;
    void fileDragMove (const juce::StringArray& files, int x, int y) override;
    void fileDragExit (const juce::StringArray& files) override;
    void filesDropped (const juce::StringArray& files, int x, int y) override;

private:
    void timerCallback() override;
    void changeListenerCallback (juce::ChangeBroadcaster*) override; // palette changed
    void openFileChooser();
    void selectAdjacentChop (int direction);
    /** Makes the plugin window the active one with the keyboard on the editor, so Ctrl+Z,
        Space and the arrows reach KERF (a file dropped from Explorer leaves the keyboard
        with Explorer). */
    void takeKeyboardFocus();
    void raiseHiddenFileDialog();
    bool isPreviewSounding() const;
    void togglePreview(); // Space
    int previewTarget() const; // the selected chop, or SamplerEngine::wholeSampleId with SAMPLE
    void switchScope (int scope); // CHOP / SAMPLE: while it plays, it goes on from the same place
    void chopChosen (int id); // a chop clicked (or picked with the arrows): queued after the one playing
    void clearQueue();
    void startPlayback(); // top-right play: the whole selected chop
    void stopPlayback();
    void playPause();      // top-left PLAY / PAUSE: pause what plays, go on from there, or play the chop
    void clearChopPause();
    bool isPlaying() const;

    KerfAudioProcessor& processor;
    juce::SharedResourcePointer<kerf::ui::ThemeHub> themeHub;
    kerf::ui::KerfLookAndFeel lookAndFeel;
    kerf::ui::AnimationClock clock;
    kerf::ui::MainView view;
    juce::TooltipWindow tooltips { this, 600 };
    std::unique_ptr<juce::FileChooser> chooser;
    double lastTick = 0.0;
    double dialogCheck = 0.0;
    int previewChopId = 0; // Space-bar preview
    int queuedChopId = 0;  // CHOP: plays when the one playing ends
    // What Space plays: the chop (top-right play) or the pattern (SEQ), whichever play / stop
    // button was used last with the mouse; any edit in the step grid counts for the pattern.
    bool spacePlaysPattern = false;
    bool chopPaused = false, patternPaused = false; // PAUSE: held where they were
    bool patternWasPlaying = false;
    struct UseListener final : juce::MouseListener
    {
        std::function<void()> onUse;
        void mouseDown (const juce::MouseEvent&) override { if (onUse) onUse(); }
        void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override { if (onUse) onUse(); }
    } patternUse, chopPlayUse;
    double previewStartedAt = 0.0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (KerfAudioProcessorEditor)
};

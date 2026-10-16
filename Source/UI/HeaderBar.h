#pragma once

#include "Controls.h"
#include "../PluginProcessor.h"
#include "SawBlade.h"

namespace kerf::ui
{

/** Top strip: identity, sample info, the saw blade, preset, tempo and key (with confidence),
    undo / redo, load, the menu (about, guide, updates). A click on KERF switches the theme.
    PRESET, BPM and KEY look alike: a small label (with its confidence dot) over a box. */
class HeaderBar final : public juce::Component,
                        private juce::ChangeListener,
                        private AnimationClock::Client
{
public:
    HeaderBar (KerfAudioProcessor& processor, AnimationClock& clock);
    ~HeaderBar() override;

    std::function<void()> onLoad;
    std::function<void()> onShowGuide;                          // "Guide" in the menu
    std::function<void()> onAbout;                              // "About KERF" in the menu
    std::function<void()> onCheckUpdates;                       // "Check for updates" in the menu
    std::function<void()> onSavePresetAs;                       // "Save as..." in the preset menu (the panel lives in the main view)
    std::function<void()> onKeyboardBack;                       // after a preset / key choice: the keyboard back on the plugin
    juce::Rectangle<int> getPresetBounds() const { return presetArea.toNearestInt(); } // for the guide
    /** The KERF logo itself (the mark and the wordmark, not its whole header cell): for the guide. */
    juce::Rectangle<int> getBrandBounds() const
    {
        return juce::Rectangle<float> (brandArea.getX(), brandArea.getCentreY() - 9.0f, 16.0f + brandInkWidth(), 18.0f).toNearestInt();
    }

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void showPresetMenu(); // the PRESET dropdown (public for the screenshot tool)
    void nextTheme();      // a click on the KERF logo (public for the screenshot tool)
    void showMainMenu();   // the menu symbol (public for the screenshot tool)

private:
    static float brandInkWidth(); // the wordmark's drawn width
    juce::ModalComponentManager::Callback* keyboardBackAfterMenu();
    void savePresetAs();
    void showTonicMenu();
    void showModeMenu();
    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    void animationTick (double dt) override;
    double updateBlink = 0.0; // the menu button pulses while an update is out and the menu not yet opened
    void updateFromProcessor();

    KerfAudioProcessor& processor;
    AnimationClock& clock;

    ValueField bpmField { 20.0, 400.0, 0.1, 1 };
    PillButton autoButton { "Auto" };    // after a manual BPM: back to the detected tempo
    PillButton keyAutoButton { "Auto" }; // after a key change: back to the detected key
    IconButton undoButton { "Undo", IconButton::Icon::undo, false }, redoButton { "Redo", IconButton::Icon::redo, false };
    IconButton loadButton { "Load", IconButton::Icon::load, false };
    IconButton menuButton { "Menu", IconButton::Icon::menu, false }; // about, guide, updates
    IconButton previousPreset { "Previous preset", IconButton::Icon::previous, false }, nextPreset { "Next preset", IconButton::Icon::next, false };

    juce::Rectangle<float> brandArea, sampleArea, bpmArea, bpmBox, keyArea, tonicArea, modeArea, presetArea, presetField;
    Segmented abToggle { { "A", "B" } };
    juce::String shownPreset;
    int lastGlobalShift = 0;
    bool shownModified = false; // the * after the preset name
    double modifiedCheck = 0.0;

public:
    SawBlade blade; // public for the screenshot tool
};

} // namespace kerf::ui

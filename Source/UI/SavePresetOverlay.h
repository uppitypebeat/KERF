#pragma once

#include "Controls.h"
#include "../PluginProcessor.h"

namespace kerf::ui
{

/** "Save as...": a small panel over the window in KERF's style (like the update check):
    a name field, SAVE and CANCEL. Enter saves, Escape or a click outside cancels. Says when
    the name is taken (saving then replaces that preset). */
class SavePresetOverlay final : public juce::Component
{
public:
    explicit SavePresetOverlay (KerfAudioProcessor& processor);

    void show();
    void hide();

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;
    bool keyPressed (const juce::KeyPress&) override;

private:
    juce::Rectangle<float> panelBounds() const;
    juce::String cleanName() const;
    juce::File target() const;
    void save();

    KerfAudioProcessor& processor;
    juce::TextEditor name;
    PillButton saveButton { "Save" }, cancel { "Cancel" };
};

} // namespace kerf::ui

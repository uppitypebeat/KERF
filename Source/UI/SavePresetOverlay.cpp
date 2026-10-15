#include "SavePresetOverlay.h"

namespace kerf::ui
{

SavePresetOverlay::SavePresetOverlay (KerfAudioProcessor& p) : processor (p)
{
    setWantsKeyboardFocus (true);

    name.setFont (Fonts::get (TextStyle::value));
    name.setJustification (juce::Justification::centredLeft);
    name.setIndents (10, 0);
    name.setSelectAllWhenFocused (true);
    name.setInputRestrictions (48);
    name.setTextToShowWhenEmpty ("Name of the preset", Colours::textFaint);
    name.onReturnKey = [this] { save(); };
    name.onEscapeKey = [this] { hide(); };
    name.onTextChange = [this]
    {
        saveButton.setEnabled (cleanName().isNotEmpty());
        repaint();
    };
    addAndMakeVisible (name);

    saveButton.setPrimary (true);
    saveButton.onClick = [this] { save(); };
    addAndMakeVisible (saveButton);
    cancel.onClick = [this] { hide(); };
    addAndMakeVisible (cancel);
}

void SavePresetOverlay::show()
{
    // Start from the current name (a new one for Default), all selected: type to replace it.
    const auto current = processor.getPresetName();
    name.setText (current == "Default" ? juce::String ("My sound") : current, juce::dontSendNotification);
    saveButton.setEnabled (cleanName().isNotEmpty());
    setVisible (true);
    toFront (true);
    resized();
    if (name.isShowing()) name.grabKeyboardFocus();
    name.selectAll();
    repaint();
}

void SavePresetOverlay::hide()
{
    setVisible (false);
    if (auto* parent = getParentComponent())
        if (parent->isShowing()) parent->grabKeyboardFocus(); // the keyboard back on KERF (Space, Ctrl+Z)
}

juce::String SavePresetOverlay::cleanName() const
{
    return juce::File::createLegalFileName (name.getText().trim());
}

juce::File SavePresetOverlay::target() const
{
    return KerfAudioProcessor::getPresetFolder().getChildFile (cleanName() + KerfAudioProcessor::presetExtension);
}

void SavePresetOverlay::save()
{
    if (cleanName().isEmpty())
        return;
    processor.savePreset (target());
    hide();
}

juce::Rectangle<float> SavePresetOverlay::panelBounds() const
{
    return juce::Rectangle<float> (400.0f, 176.0f).withCentre (getLocalBounds().toFloat().getCentre());
}

void SavePresetOverlay::resized()
{
    const auto p = panelBounds().reduced (24.0f, 20.0f);
    name.setBounds (juce::Rectangle<float> (p.getX(), p.getY() + 30.0f, p.getWidth(), 30.0f).toNearestInt());
    cancel.setBounds (juce::Rectangle<float> (72.0f, 22.0f).withPosition (p.getRight() - 72.0f - 8.0f - 72.0f, p.getBottom() - 22.0f).toNearestInt());
    saveButton.setBounds (juce::Rectangle<float> (72.0f, 22.0f).withPosition (p.getRight() - 72.0f, p.getBottom() - 22.0f).toNearestInt());

    name.setColour (juce::TextEditor::backgroundColourId, Colours::surfaceRaised);
    name.setColour (juce::TextEditor::textColourId, Colours::text);
    name.setColour (juce::TextEditor::highlightColourId, Colours::accent.withAlpha (0.35f));
    name.setColour (juce::TextEditor::highlightedTextColourId, Colours::text);
    name.setColour (juce::TextEditor::outlineColourId, Colours::line);
    name.setColour (juce::TextEditor::focusedOutlineColourId, Colours::accent);
    name.setColour (juce::CaretComponent::caretColourId, Colours::accent);
}

void SavePresetOverlay::paint (juce::Graphics& g)
{
    g.setColour (Colours::background.withAlpha (0.82f));
    g.fillRect (getLocalBounds());

    const auto panel = panelBounds();
    g.setColour (Colours::surface);
    g.fillRect (panel);
    g.setColour (Colours::line);
    g.drawRect (panel, 1.0f);
    g.setColour (Colours::accent);
    g.fillRect (panel.withHeight (2.0f)); // the accent edge on top, like the guide

    auto r = panel.reduced (24.0f, 20.0f);
    drawText (g, "SAVE PRESET", r.removeFromTop (14.0f), TextStyle::sectionLabel, Colours::textDim);

    // Under the field: where it goes, or that the name is taken.
    auto info = r.withTrimmedTop (46.0f).withHeight (16.0f);
    if (cleanName().isNotEmpty() && target().existsAsFile())
        drawText (g, "\"" + cleanName() + "\" exists: saving replaces it", info, TextStyle::micro, Colours::accent);
    else
        drawText (g, "Saved with your presets (Documents / KERF / Presets)", info, TextStyle::micro, Colours::textFaint);
}

void SavePresetOverlay::mouseDown (const juce::MouseEvent& e)
{
    if (! panelBounds().contains (e.position))
        hide();
}

bool SavePresetOverlay::keyPressed (const juce::KeyPress& key)
{
    if (key == juce::KeyPress::escapeKey)
    {
        hide();
        return true;
    }
    return true; // the keys stay here while the panel is open (Space must not start playback)
}

} // namespace kerf::ui

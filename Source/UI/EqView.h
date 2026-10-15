#pragma once

#include "Controls.h"
#include "../PluginProcessor.h"

namespace kerf::ui
{

/** EQ view: the frequency response over the live spectrum, with up to eight points.

      double-click empty space   add a point (bell) there
      double-click a point       remove it
      drag a point               left/right = frequency, up/down = gain
      wheel on a point           width (Q)
      right-click a point        bypass it (it stays, but is not heard) / on again

    The point types are in the toolbar above (EqTypeTools).

    The selected point is shared with the EQ page of the control panel ("eqSelected"). */
class EqView final : public juce::Component, private AnimationClock::Client
{
public:
    EqView (KerfAudioProcessor& processor, AnimationClock& clock);
    ~EqView() override;

    /** Opening animation: the curve rises out of the 0 dB line and the points pop in. */
    void playIntro() { intro = 0.0f; repaint(); }
    /** Nothing moving: the view has stopped repainting (the screenshot tool checks it). */
    bool isResting() const noexcept { return idle.quiet >= IdleGate::settleSeconds; }

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;

    /** The point a double-click would add at `p` (its type from where the mouse is: shelves at
        the ends, cuts at the bottom corners, a notch deep in the middle, else a bell). */
    EqProcessor::Band suggestionAt (juce::Point<float> p) const;
    /** Shows the preview for `p` as if the mouse were there (the screenshot tool). */
    void showSuggestion (juce::Point<float> p);
    juce::Rectangle<float> getPlotArea() const noexcept { return plot; }
    int selectedBand() const; // the selected point (-1: none); Delete removes it (the editor)

private:
    void animationTick (double dt) override;
    float freqToX (float hz) const noexcept;
    float xToFreq (float x) const noexcept;
    float dbToY (float db) const noexcept;
    float yToDb (float y) const noexcept;
    juce::Point<float> handlePosition (int band) const;
    int bandAt (juce::Point<float>) const;
    void select (int band);
    juce::RangedAudioParameter* param (int band, const char* field) const;

    KerfAudioProcessor& processor;
    AnimationClock& clock;

    juce::Rectangle<float> header, plot;
    EqProcessor::Settings shown;                          // glides towards the parameter values
    std::array<float, EqProcessor::maxBands> presence {}; // 0..1: points fade in and out
    std::array<float, EqProcessor::maxBands> heardAmount {}; // 0..1: bypassed points fade out of the curve
    int hovered = -1, dragging = -1;
    juce::Point<float> hoverPoint;
    bool suggesting = false; // the mouse is over empty plot space: preview the point to add
    float intro = 1.0f;
    IdleGate idle; // rests while nothing moves

    static constexpr float maxDb = 18.0f;
};

/** The shape of an EQ point type as a small curve (24 x 12, 0 dB in the middle). */
std::unique_ptr<juce::Drawable> eqTypeIcon (int type, juce::Colour colour);

/** The EQ point types in the toolbar (EQ page), each as its curve: BELL, LOW SHELF,
    HIGH SHELF, LOW CUT, HIGH CUT, NOTCH. With a point selected, a click gives it that type
    (the button of its type is lit); with none, a click adds a point of that type. */
class EqTypeTools final : public juce::Component, private AnimationClock::Client
{
public:
    EqTypeTools (KerfAudioProcessor& processor, AnimationClock& clock);
    ~EqTypeTools() override;
    void resized() override;

    static constexpr int numTypes = 6, buttonWidth = 30, gap = 4;
    static constexpr int idealWidth = numTypes * buttonWidth + (numTypes - 1) * gap;

private:
    struct TypeButton final : public juce::Button
    {
        explicit TypeButton (int t);
        void paintButton (juce::Graphics&, bool highlighted, bool down) override;
        void mouseDown (const juce::MouseEvent& e) override { if (! e.mods.isPopupMenu()) juce::Button::mouseDown (e); } // left button only
        void mouseDrag (const juce::MouseEvent& e) override { if (! e.mods.isPopupMenu()) juce::Button::mouseDrag (e); }
        int type;
    };

    void animationTick (double) override; // lights the selected point's type
    int selectedBand() const;
    void choose (int type);

    KerfAudioProcessor& processor;
    AnimationClock& clock;
    std::vector<std::unique_ptr<TypeButton>> buttons;
};

} // namespace kerf::ui

#pragma once

#include "Controls.h"
#include "../UpdateCheck.h"

namespace kerf::ui
{

/** "Check for updates": a panel over the window that checks right away. A round screen on
    the left sweeps like a radar while it asks, then settles on the answer with a pulse (a
    tick, an arrow down to a newer version, a question mark with no update page yet, a cross
    with no connection); the installed and latest version stand large on the right. Closes
    with CLOSE, Escape, or a click outside the panel. Nothing moves once the answer is in. */
class UpdateOverlay final : public juce::Component,
                            private AnimationClock::Client
{
public:
    explicit UpdateOverlay (AnimationClock& clock);
    ~UpdateOverlay() override;

    void showAndCheck();
    void hide();

    /** For the screenshot tool: show a result without the network. */
    void showResult (const updates::Result& result);
    /** Steps the animation by hand (the screenshot tool). */
    void advance (double seconds) { animationTick (seconds); }
    bool isChecking() const noexcept { return checking; }
    const updates::Result& getResult() const noexcept { return result; }

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;
    bool keyPressed (const juce::KeyPress&) override;

private:
    void animationTick (double dt) override;
    juce::Rectangle<float> panelBounds() const;
    juce::Rectangle<float> dialBounds() const;
    juce::Colour statusColour() const;
    void paintDial (juce::Graphics&, juce::Rectangle<float> area);

    AnimationClock& clock;
    PillButton download { "Download" }, close { "Close" };
    bool checking = false;
    updates::Result result;
    double spin = 0.0;    // the sweep, in turns
    float settle = 1.0f;  // 0 when the answer comes in, 1 when its pulse is over
    int generation = 0;   // a late answer from an earlier check is ignored
};

} // namespace kerf::ui

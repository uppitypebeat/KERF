#pragma once

#include "Controls.h"

namespace kerf::ui
{

/** First-start guide: a short tour that dims the window and spotlights one area at a time
    with a card explaining it (drop a sample, slice it, play the pads, shape a chop, build a
    pattern, make it yours).

    Shown once, the first time KERF opens (remembered in the user settings); again from the
    KERF wordmark menu. NEXT / BACK / SKIP, the arrow keys, Return and Escape. */
class GuideOverlay final : public juce::Component, private AnimationClock::Client
{
public:
    struct Step
    {
        juce::String title, text;
        std::function<juce::Rectangle<int>()> target; // in the parent's coordinates
        std::function<void()> onShow;                 // e.g. switch to the view the step talks about
    };

    explicit GuideOverlay (AnimationClock& clock);
    ~GuideOverlay() override;

    void setSteps (std::vector<Step> steps);
    void start();          // from the first step
    void finish();         // hides it and remembers that the guide was seen
    std::function<void()> onStarted; // START on the last step (not SKIP): the plugin opens
    int getStep() const noexcept { return step; }
    void showStep (int index);

    /** True the first time KERF opens (and not when the KERF_NO_GUIDE environment
        variable is set, as in the test tools). */
    static bool shouldShowOnStart();

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;
    bool keyPressed (const juce::KeyPress&) override;

private:
    void animationTick (double dt) override;
    juce::Rectangle<float> spotlight() const;  // the target, a little larger
    juce::Rectangle<float> cardBounds() const;

    AnimationClock& clock;
    std::vector<Step> steps;
    int step = 0;
    float fade = 0.0f;   // 0..1 when the step changes (the spotlight glides)
    double time = 0.0;
    juce::Rectangle<float> shownSpot; // glides towards the target

    PillButton next { "Next" }, back { "Back" }, skip { "Skip" };
};

} // namespace kerf::ui

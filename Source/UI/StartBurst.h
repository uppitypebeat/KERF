#pragma once

#include "Controls.h"

namespace kerf::ui
{

/** The opening after the guide's START: the theme's sky covers the window, a big saw blade
    races across at the waveform's height leaving a glowing cut and a spray of sparks, the
    sky splits open along the cut (KERF shows over the seam), and a sweep of light runs over
    the plugin while its knobs turn in. ~1.9 s; colours from the theme (no white glow in
    white mode). A click skips it. */
class StartBurst final : public juce::Component,
                         private AnimationClock::Client
{
public:
    explicit StartBurst (AnimationClock& clock);
    ~StartBurst() override;

    /** Starts it; `seamY` is where the blade cuts (in this component's coordinates). */
    void play (float seamY);
    bool isPlaying() const noexcept { return playing; }
    void advance (double seconds) { animationTick (seconds); } // the screenshot tool steps time

    std::function<void()> onReveal; // the sky opens: time for the knobs to turn in

    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;

private:
    struct Spark { juce::Point<float> position, velocity; float age = 0.0f, life = 1.0f, size = 1.0f; };

    void animationTick (double dt) override;
    void spray (juce::Point<float> from, int count, float speed, bool backwards);
    float bladeX() const;

    AnimationClock& clock;
    juce::Random random;
    std::vector<Spark> sparks;
    double time = 0.0;
    float seam = 0.0f, angle = 0.0f, sprayClock = 0.0f;
    bool playing = false, revealed = false;

    static constexpr double cutTime = 0.62, openTime = 0.75, total = 2.35; // a little slower: there is time to see it
    static constexpr float bladeRadius = 34.0f;
};

} // namespace kerf::ui

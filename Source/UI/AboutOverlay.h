#pragma once

#include "Controls.h"
#include "../PluginProcessor.h"

namespace kerf::ui
{

/** "About KERF": the whole window becomes a small universe. A galaxy of grains turns round
    a spinning saw blade with the glowing wordmark, version and credits in front.

    A toy, too: drag across it and the blade cuts through space - a glowing kerf with
    sparks, the stars flung apart (silently: it plays no sound). A click sends out a shock wave. Each cut spins the
    blade up. (No instructions on it: it is there to be found.) LICENCES shows the third-party licence texts; CLOSE or Escape leaves. */
class AboutOverlay final : public juce::Component, private AnimationClock::Client
{
public:
    explicit AboutOverlay (AnimationClock& clock);
    ~AboutOverlay() override;

    void show();
    void hide();

    /** A cut from `from` to `to` (the mouse does this; public for the screenshot tool). */
    void slice (juce::Point<float> from, juce::Point<float> to, float speed);
    /** A shock wave from `at` (a click). */
    void shockwave (juce::Point<float> at);
    /** Steps the animation by hand (the screenshot tool). */
    void advance (double seconds) { animationTick (seconds); }

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseMove (const juce::MouseEvent&) override;
    bool keyPressed (const juce::KeyPress&) override;

    static juce::String versionString();
    static juce::String licenceText();

private:
    struct Star { float radius, theta, depth, twinkle, hue, size; float x = 0, y = 0, dx = 0, dy = 0, vx = 0, vy = 0; };
    struct Cut { std::vector<juce::Point<float>> points; float age = 0.0f; };
    struct Spark { juce::Point<float> position, velocity; float life, maxLife, size; };
    struct Ring { juce::Point<float> centre; float age = 0.0f; };

    void animationTick (double dt) override;
    void buildGalaxy();
    juce::Point<float> galaxyCentre() const;

    AnimationClock& clock;

    std::vector<Star> stars, dust, clouds; // the galaxy, far background stars, its glowing gas
    std::vector<Cut> cuts;
    std::vector<Spark> sparks;
    std::vector<Ring> rings;
    juce::Random random { 0x6b657266 };

    juce::Path wordmark, blade;
    float time = 0.0f, spin = 0.35f, bladeAngle = 0.0f, rotation = 0.0f, appear = 0.0f;
    juce::Point<float> mouse, lastDrag;
    double lastDragTime = 0.0;
    float dragDistance = 0.0f;
    bool dragging = false, mouseInside = false;

    juce::TextEditor licences;
    PillButton licencesButton { "Licences", true }, close { "Close" };
};

} // namespace kerf::ui

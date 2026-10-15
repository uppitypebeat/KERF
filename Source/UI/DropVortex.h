#pragma once

#include "Theme.h"

namespace kerf::ui
{

/** The waveform screen while a file is dragged over it: a grain singularity. Hundreds of
    grains spiral in towards the mouse leaving glowing streaks, around a black core with a
    photon ring, a tilted accretion disk and a ring that ripples like a waveform waiting to
    be born; it charges up the longer the file is held there. Dropping it sets off a
    supernova: a flash, a double shock wave, the grains flung outwards. Colours from the
    theme (a deep core instead of white-hot glow in white mode). Not a component: the
    waveform view drives and paints it. */
class DropVortex
{
public:
    /** `dragging`: a file is over the view; `target`: the mouse (view coordinates). */
    void update (double dt, bool dragging, juce::Point<float> target, juce::Rectangle<float> area);
    void drop();                       // the file was dropped here: the supernova
    void paint (juce::Graphics&, juce::Rectangle<float> area) const;

    bool isActive() const noexcept     { return charge > 0.001f || burst >= 0.0f; }
    float getCharge() const noexcept   { return charge; } // 0..1
    juce::Point<float> getCore() const noexcept { return core; }

private:
    struct Grain
    {
        float angle, radius, size, hue, speed;
        juce::Point<float> position, previous, velocity; // velocity: only after the drop
    };

    void respawn (Grain&, juce::Rectangle<float> area, bool anywhere);
    float maxRadius (juce::Rectangle<float> area) const;

    std::vector<Grain> grains;
    juce::Random random { 0x766f7274 };
    juce::Point<float> core;
    float charge = 0.0f, burst = -1.0f, spin = 0.0f;
    double time = 0.0;
    bool placed = false;
};

} // namespace kerf::ui

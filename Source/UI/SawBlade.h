#pragma once

#include "Controls.h"
#include "../PluginProcessor.h"
#include <deque>

namespace kerf::ui
{

/** The header's moving picture: a saw blade (a kerf is the cut a saw leaves). It spins with
    the output level (fast attack, slow spin-down), throws sparks in the theme colour on
    every hit and a steady spray while it is loud, sends a ring out on each beat (host or
    pattern), and the sound it cut trails off to the left as a ribbon in the chop colours.
    Click it for a handful of sparks. */
class SawBlade final : public juce::Component,
                       private AnimationClock::Client
{
public:
    SawBlade (KerfAudioProcessor& processor, AnimationClock& clock);
    ~SawBlade() override;

    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;

    /** A spray of sparks from where the blade cuts. Public for the screenshot tool. */
    void burst (juce::Colour colour, int count, float strength);
    int getNumSparks() const noexcept { return (int) sparks.size(); }

    /** A saw blade: hooked teeth, an inner ring, three slots (also the start animation's). */
    static void drawBlade (juce::Graphics&, juce::Point<float> centre, float radius, float rotation, juce::Colour colour, float alpha);
    void advance (double seconds) { animationTick (seconds); } // the screenshot tool steps time

private:
    struct Spark
    {
        juce::Point<float> position, velocity;
        float age = 0.0f, life = 1.0f, size = 1.0f;
        juce::Colour colour;
    };
    struct Column { float height; juce::Colour colour; };
    struct Ring { float age; bool downbeat; };

    void animationTick (double dt) override;
    juce::Point<float> centre() const;
    float radius() const;

    KerfAudioProcessor& processor;
    AnimationClock& clock;
    juce::Random random;

    std::vector<Spark> sparks;
    std::deque<Column> ribbon;   // newest first (at the blade)
    std::vector<Ring> rings;
    std::vector<uint32_t> lastTriggers;

    float angle = 0.0f, speed = 0.0f, level = 0.0f, heat = 0.0f;
    float idleClock = 0.0f; // at rest: a frame every 1/15 s
    float columnClock = 0.0f, columnPeak = 0.0f, sprayClock = 0.0f, hubFlash = 0.0f;
    juce::Colour colour;         // the colour of the last chop that played
    double lastBeat = -1.0;
};

} // namespace kerf::ui

#include "DropVortex.h"

namespace kerf::ui
{

namespace
{
    constexpr int numGrains = 320;
    constexpr float burstLength = 1.1f;
    constexpr float pi = juce::MathConstants<float>::pi;

    float smooth (float t) { t = juce::jlimit (0.0f, 1.0f, t); return t * t * (3.0f - 2.0f * t); }

    /** White-hot at night; a deep version of the colour in white mode (white would vanish). */
    juce::Colour hot (juce::Colour c, float amount) { return c.interpolatedWith (juce::Colours::white, amount); }
}

float DropVortex::maxRadius (juce::Rectangle<float> area) const
{
    float r = 0.0f;
    for (const auto corner : { area.getTopLeft(), area.getTopRight(), area.getBottomLeft(), area.getBottomRight() })
        r = std::max (r, corner.getDistanceFrom (core));
    return r + 20.0f;
}

void DropVortex::respawn (Grain& g, juce::Rectangle<float> area, bool anywhere)
{
    const auto outer = maxRadius (area);
    g.angle = random.nextFloat() * 2.0f * pi;
    g.radius = anywhere ? 30.0f + random.nextFloat() * outer : outer * (0.7f + 0.3f * random.nextFloat());
    g.size = 0.8f + 1.8f * std::pow (random.nextFloat(), 2.0f);
    g.hue = random.nextFloat();
    g.speed = 0.6f + 0.8f * random.nextFloat();
    g.position = g.previous = core + juce::Point<float> (std::cos (g.angle), std::sin (g.angle) * 0.55f) * g.radius;
    g.velocity = {};
}

void DropVortex::drop()
{
    if (charge < 0.05f && grains.empty())
        return;
    burst = 0.0f;
    // Every grain is flung out from the core, faster the closer it was.
    for (auto& g : grains)
    {
        const auto away = g.position - core;
        const auto d = std::max (1.0f, away.getDistanceFromOrigin());
        g.velocity = away / d * (900.0f + 2400.0f / std::sqrt (d)) * (0.6f + 0.6f * random.nextFloat());
    }
}

void DropVortex::update (double seconds, bool dragging, juce::Point<float> target, juce::Rectangle<float> area)
{
    const auto dt = (float) juce::jmin (seconds, 0.05);
    time += dt;

    // The core follows the mouse (softly); it starts where the file came in.
    if (! placed || charge < 0.001f)
        core = target;
    placed = true;
    core += (target - core) * std::min (1.0f, dt * 9.0f);

    if (burst >= 0.0f)
    {
        burst += dt;
        charge = std::max (0.0f, charge - dt * 2.5f);
        for (auto& g : grains)
        {
            g.previous = g.position;
            g.velocity *= std::exp (-dt * 2.2f);
            g.position += g.velocity * dt;
        }
        if (burst >= burstLength)
        {
            burst = -1.0f;
            charge = 0.0f;
            grains.clear();
        }
        return;
    }

    // Charge builds while the file is held over the view, and drains when it leaves.
    charge = dragging ? std::min (1.0f, charge + dt / 1.2f) : std::max (0.0f, charge - dt * 2.0f);
    if (charge <= 0.0f)
    {
        grains.clear();
        return;
    }

    if (grains.empty())
    {
        grains.resize ((size_t) numGrains);
        for (auto& g : grains)
            respawn (g, area, true);
    }

    spin += dt * (1.2f + 3.0f * charge);
    for (auto& g : grains)
    {
        // A spiral: faster round and inwards near the core (and the more it is charged).
        g.previous = g.position;
        const auto r = std::max (4.0f, g.radius);
        g.angle += dt * g.speed * (0.8f + 2.6f * charge) * 140.0f / (r + 40.0f);
        g.radius -= dt * g.speed * (40.0f + 170.0f * charge) * (1.0f + 60.0f / r);
        const auto disc = juce::Point<float> (std::cos (g.angle), std::sin (g.angle) * 0.55f); // a tilted disk
        g.position = core + disc * g.radius;
        if (g.radius < 8.0f + 6.0f * charge)
        {
            respawn (g, area, false);
            g.previous = g.position;
        }
    }
}

void DropVortex::paint (juce::Graphics& g, juce::Rectangle<float> area) const
{
    if (! isActive())
        return;

    juce::Graphics::ScopedSaveState save (g);
    g.reduceClipRegion (area.toNearestInt());

    const auto accent = Colours::accent;
    const auto second = nebulaGlow();
    const auto c = charge;
    const auto b = burst >= 0.0f ? burst / burstLength : -1.0f;
    const auto pulse = 0.5f + 0.5f * std::sin ((float) time * 2.0f * pi * 1.6f);

    // The view darkens towards the edges and glows faintly round the core.
    {
        const auto outer = maxRadius (area);
        juce::ColourGradient veil (accent.withAlpha (0.10f * c), core.x, core.y, Colours::background.withAlpha (0.55f * c), core.x + outer, core.y, true);
        veil.addColour (0.25, accent.withAlpha (0.05f * c));
        g.setGradientFill (veil);
        g.fillRect (area);
    }

    // Grains: streaks from where they were to where they are, cooling from hot to the theme.
    // Batched: grains are sorted into a few colour / nearness buckets and each bucket is one
    // filled path (a few fills a frame instead of hundreds of lines: cheap on the GPU too).
    constexpr int hues = 3, bands = 4;
    std::array<juce::Path, hues * bands> cores, glows;
    for (const auto& gr : grains)
    {
        const auto d = gr.position.getDistanceFrom (core);
        const auto near = juce::jlimit (0.0f, 0.999f, 1.0f - d / 260.0f);
        const auto bucket = juce::jlimit (0, hues - 1, (int) (gr.hue * hues)) * bands + (int) (near * bands);
        auto from = gr.previous, to = gr.position;
        if (from.getDistanceFrom (to) < 1.5f)
            from = to - (to - core) / std::max (1.0f, d) * 2.0f;
        cores[(size_t) bucket].addLineSegment ({ from, to }, gr.size);
        if (near > 0.5f || burst >= 0.0f)
            glows[(size_t) bucket].addLineSegment ({ from, to }, gr.size * 3.2f);
    }
    for (int h = 0; h < hues; ++h)
        for (int n = 0; n < bands; ++n)
        {
            const auto bucket = (size_t) (h * bands + n);
            if (cores[bucket].isEmpty())
                continue;
            const auto near = ((float) n + 0.5f) / (float) bands;
            const auto colour = hot (accent.interpolatedWith (second, ((float) h + 0.5f) / (float) hues * 0.7f), 0.55f * near * near);
            const auto alpha = burst >= 0.0f ? (1.0f - b) * (1.0f - b) : juce::jlimit (0.0f, 1.0f, (0.25f + 0.75f * near) * (0.4f + 0.6f * c));
            if (! glows[bucket].isEmpty())
            {
                g.setColour (colour.withAlpha (alpha * 0.2f));
                g.fillPath (glows[bucket]);
            }
            g.setColour (colour.withAlpha (alpha));
            g.fillPath (cores[bucket]);
        }

    if (burst < 0.0f)
    {
        // The accretion disk: tilted rings of dashes turning round the core.
        for (int k = 0; k < 3; ++k)
        {
            const auto rx = 26.0f + 22.0f * (float) k + 18.0f * c;
            juce::Path arcs;
            for (int s = 0; s < 6; ++s)
            {
                const auto a0 = spin * (1.0f - 0.25f * (float) k) * (k % 2 == 0 ? 1.0f : -1.0f) + (float) s * pi / 3.0f;
                juce::Path seg;
                seg.addCentredArc (core.x, core.y, rx, rx * 0.55f, 0.0f, a0, a0 + 0.55f, true);
                arcs.addPath (seg);
            }
            const auto colour = (k == 1 ? second : accent).withAlpha ((0.25f + 0.5f * c) * (1.0f - 0.2f * (float) k));
            g.setColour (colour.withMultipliedAlpha (0.3f));
            g.strokePath (arcs, juce::PathStrokeType (5.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
            g.setColour (colour);
            g.strokePath (arcs, juce::PathStrokeType (1.4f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        }

        // A ring that ripples like a waveform: the sound waiting to be born.
        {
            const auto base = 52.0f + 34.0f * c;
            juce::Path ring;
            for (int s = 0; s <= 160; ++s)
            {
                const auto a = (float) s / 160.0f * 2.0f * pi;
                const auto wobble = std::sin (a * 7.0f + spin * 2.0f) * 0.5f + std::sin (a * 13.0f - spin * 3.1f) * 0.3f + std::sin (a * 23.0f + spin * 5.0f) * 0.2f;
                const auto r = base * (1.0f + 0.12f * c * wobble * (0.6f + 0.4f * pulse));
                const juce::Point<float> p (core.x + std::cos (a) * r, core.y + std::sin (a) * r);
                if (s == 0) ring.startNewSubPath (p); else ring.lineTo (p);
            }
            ring.closeSubPath();
            g.setColour (accent.withAlpha (0.12f * c));
            g.strokePath (ring, juce::PathStrokeType (7.0f));
            g.setColour (hot (accent, 0.3f).withAlpha (0.7f * c));
            g.strokePath (ring, juce::PathStrokeType (1.2f));
        }

        // The core: a dark disc inside a bright photon ring, breathing.
        {
            const auto r = 9.0f + 7.0f * c + 1.5f * pulse * c;
            g.setGradientFill (juce::ColourGradient (hot (accent, 0.4f).withAlpha (0.55f * c), core.x, core.y,
                                                     accent.withAlpha (0.0f), core.x + r * 5.0f, core.y, true));
            g.fillEllipse (juce::Rectangle<float> (r * 10.0f, r * 10.0f).withCentre (core));
            g.setColour (hot (accent, 0.8f).withAlpha (c));
            g.drawEllipse (juce::Rectangle<float> (r * 2.0f + 3.0f, r * 2.0f + 3.0f).withCentre (core), 1.6f);
            g.setColour (juce::Colour (0xff040306));
            g.fillEllipse (juce::Rectangle<float> (r * 2.0f, r * 2.0f).withCentre (core));
        }

        // The call, under the core.
        const auto text = juce::String ("RELEASE TO LOAD");
        g.setFont (Fonts::sans (11.0f, 600).withExtraKerningFactor (0.35f + 0.2f * c));
        g.setColour (Colours::text.withAlpha (smooth (c * 2.0f) * (0.55f + 0.45f * pulse)));
        g.drawText (text, juce::Rectangle<float> (240.0f, 16.0f).withCentre ({ core.x, core.y + 92.0f + 30.0f * c }), juce::Justification::centred, false);
        return;
    }

    // The supernova: a flash, then two shock waves (the theme colour and its second colour).
    {
        const auto flash = std::max (0.0f, 1.0f - b * 4.5f);
        if (flash > 0.0f)
        {
            g.setGradientFill (juce::ColourGradient (hot (accent, 0.9f).withAlpha (0.85f * flash), core.x, core.y,
                                                     accent.withAlpha (0.0f), core.x + maxRadius (area) * 0.8f, core.y, true));
            g.fillRect (area);
        }
        const auto outer = maxRadius (area);
        for (int k = 0; k < 2; ++k)
        {
            const auto t = juce::jlimit (0.0f, 1.0f, b * 1.25f - 0.08f * (float) k);
            const auto r = 12.0f + Motion::easeOut (t) * outer;
            const auto colour = k == 0 ? accent : second;
            const auto alpha = (1.0f - t) * (1.0f - t);
            g.setColour (colour.withAlpha (0.18f * alpha));
            g.drawEllipse (juce::Rectangle<float> (r * 2.0f, r * 2.0f).withCentre (core), 18.0f * (1.0f - t) + 2.0f);
            g.setColour (hot (colour, 0.4f).withAlpha (0.9f * alpha));
            g.drawEllipse (juce::Rectangle<float> (r * 2.0f, r * 2.0f).withCentre (core), 2.0f);
        }
    }
}

} // namespace kerf::ui

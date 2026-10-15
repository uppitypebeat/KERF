#include "StartBurst.h"
#include "SawBlade.h"

namespace kerf::ui
{

namespace
{
    constexpr float gravity = 900.0f; // px / s^2
    constexpr size_t maxSparks = 700;

    float smooth (float t) { t = juce::jlimit (0.0f, 1.0f, t); return t * t * (3.0f - 2.0f * t); }

    /** White-hot at night; in white mode a deeper version of the colour (white would vanish). */
    juce::Colour hotCore (juce::Colour c) { return lightTheme() ? c.darker (0.5f) : juce::Colours::white; }
}

StartBurst::StartBurst (AnimationClock& c) : clock (c)
{
    setInterceptsMouseClicks (true, false);
    clock.add (this);
}

StartBurst::~StartBurst()
{
    clock.remove (this);
}

void StartBurst::play (float seamY)
{
    seam = seamY;
    time = 0.0;
    angle = 0.0f;
    sprayClock = 0.0f;
    sparks.clear();
    revealed = false;
    playing = true;
    setVisible (true);
    toFront (false);
    repaint();
}

void StartBurst::mouseDown (const juce::MouseEvent&)
{
    if (! revealed && onReveal)
        onReveal();
    revealed = true;
    time = total; // skip
}

float StartBurst::bladeX() const
{
    const auto t = smooth ((float) (time / cutTime));
    return -bladeRadius + t * ((float) getWidth() + 2.0f * bladeRadius);
}

void StartBurst::spray (juce::Point<float> from, int count, float speed, bool backwards)
{
    for (int i = 0; i < count && sparks.size() < maxSparks; ++i)
    {
        Spark s;
        s.position = from;
        s.velocity = backwards ? juce::Point<float> (-(60.0f + random.nextFloat() * 260.0f), -260.0f + random.nextFloat() * 380.0f) * speed
                               : juce::Point<float> ((random.nextFloat() - 0.5f) * 220.0f, (random.nextBool() ? -1.0f : 1.0f) * (80.0f + random.nextFloat() * 260.0f)) * speed;
        s.life = 0.35f + random.nextFloat() * 0.7f;
        s.size = 1.0f + random.nextFloat() * 1.4f;
        sparks.push_back (s);
    }
}

void StartBurst::animationTick (double seconds)
{
    if (! playing)
        return;

    const auto dt = (float) juce::jmin (seconds, 0.05);
    time += dt;
    angle += dt * 38.0f;

    // While the blade crosses: a steady stream of sparks thrown back from where it cuts.
    if (time < cutTime)
    {
        sprayClock += dt * 260.0f;
        for (; sprayClock >= 1.0f; sprayClock -= 1.0f)
            spray ({ bladeX() - bladeRadius * 0.6f, seam }, 1, 1.0f, true);
    }
    else if (! revealed)
    {
        // Through: the whole cut bursts along its length, the sky opens.
        revealed = true;
        for (int k = 0; k < 140; ++k)
            spray ({ random.nextFloat() * (float) getWidth(), seam }, 1, 0.8f, false);
        if (onReveal)
            onReveal();
    }

    for (auto& s : sparks)
    {
        s.age += dt;
        s.velocity.y += gravity * dt;
        s.velocity *= std::exp (-dt * 1.2f);
        s.position += s.velocity * dt;
    }
    sparks.erase (std::remove_if (sparks.begin(), sparks.end(), [] (const Spark& s) { return s.age >= s.life; }), sparks.end());

    if (time >= total)
    {
        playing = false;
        sparks.clear();
        setVisible (false);
        return;
    }
    repaint();
}

void StartBurst::paint (juce::Graphics& g)
{
    if (! playing)
        return;

    const auto w = (float) getWidth(), h = (float) getHeight();
    const auto accent = Colours::accent;
    const auto core = hotCore (accent);
    const auto open = smooth ((float) ((time - cutTime) / openTime));
    const auto pastCut = time >= cutTime;

    // The sky over the plugin, split along the cut as it opens.
    {
        const auto topEdge = seam * (1.0f - open);
        const auto bottomEdge = seam + (h - seam) * open;
        const auto fadeOut = 1.0f - smooth ((float) ((time - cutTime - openTime * 0.6) / 0.5));
        if (fadeOut > 0.0f)
        {
            juce::Graphics::ScopedSaveState save (g);
            g.setOpacity (fadeOut);
            if (topEdge > 0.0f)
                fillBackdrop (g, *this, { 0.0f, 0.0f, w, topEdge });
            if (bottomEdge < h)
                fillBackdrop (g, *this, { 0.0f, bottomEdge, w, h - bottomEdge });

            // Glowing lips on both halves.
            if (pastCut)
                for (const auto y : { topEdge, bottomEdge })
                {
                    g.setColour (accent.withAlpha (0.55f * fadeOut));
                    g.fillRect (juce::Rectangle<float> (0.0f, y - 1.0f, w, 2.0f));
                }
        }
    }

    // The cut: a hot line with a glow, behind the blade, flaring as it goes through.
    {
        const auto end = pastCut ? w : juce::jlimit (0.0f, w, bladeX());
        const auto flare = pastCut ? 1.0f - smooth ((float) ((time - cutTime) / 0.55)) : 0.7f;
        if (flare > 0.0f && ! (pastCut && open > 0.98f))
        {
            const auto glowH = 10.0f + 26.0f * (pastCut ? flare : 0.4f);
            juce::ColourGradient band (accent.withAlpha (0.0f), 0.0f, seam - glowH, accent.withAlpha (0.0f), 0.0f, seam + glowH, false);
            band.addColour (0.5, accent.withAlpha (0.45f * flare));
            g.setGradientFill (band);
            g.fillRect (juce::Rectangle<float> (0.0f, seam - glowH, end, glowH * 2.0f));
            g.setColour (core.withAlpha (flare));
            g.fillRect (juce::Rectangle<float> (0.0f, seam - 1.0f, end, 2.0f));
        }
    }

    // KERF over the seam while it opens.
    {
        const auto a = smooth ((float) ((time - 0.15) / 0.25)) * (1.0f - smooth ((float) ((time - 0.95) / 0.35)));
        if (a > 0.0f)
        {
            const auto scale = 1.12f - 0.12f * smooth ((float) (time / 1.2));
            const auto font = Fonts::sans (46.0f * scale, 600).withExtraKerningFactor (0.42f - 0.1f * open);
            const auto textW = juce::GlyphArrangement::getStringWidth (font, "KERF");
            const auto markSize = 26.0f * scale;
            const auto totalW = markSize + 20.0f * scale + textW;
            const auto x = (w - totalW) * 0.5f, y = seam - 64.0f * scale;
            g.setColour (accent.withAlpha (a));
            g.fillRect (juce::Rectangle<float> (markSize, markSize).withPosition (x, y + (54.0f * scale - markSize) * 0.5f));
            g.setColour (Colours::text.withAlpha (a));
            g.setFont (font);
            g.drawText ("KERF", juce::Rectangle<float> (x + markSize + 20.0f * scale, y, textW + 10.0f, 54.0f * scale), juce::Justification::centredLeft, false);
        }
    }

    // A sweep of light over the plugin once it is open.
    {
        const auto t = (float) ((time - 0.75) / 0.8);
        if (t > 0.0f && t < 1.0f)
        {
            const auto x = -260.0f + smooth (t) * (w + 520.0f);
            juce::ColourGradient sweep (accent.withAlpha (0.0f), x - 130.0f, 0.0f, accent.withAlpha (0.0f), x + 130.0f, h * 0.3f, false);
            sweep.addColour (0.5, nebulaGlow().interpolatedWith (accent, 0.5f).withAlpha (0.16f * (1.0f - t)));
            g.setGradientFill (sweep);
            g.fillRect (getLocalBounds().toFloat());
        }
    }

    // The blade, spinning, while it crosses.
    if (! pastCut)
    {
        const auto centre = juce::Point<float> (bladeX(), seam);
        g.setGradientFill (juce::ColourGradient (accent.withAlpha (0.45f), centre.x, centre.y, accent.withAlpha (0.0f), centre.x + bladeRadius * 2.4f, centre.y, true));
        g.fillEllipse (juce::Rectangle<float> (bladeRadius * 4.8f, bladeRadius * 4.8f).withCentre (centre));
        for (int k = 2; k >= 1; --k)
            SawBlade::drawBlade (g, centre, bladeRadius, angle - (float) k * 0.14f, accent, 0.25f / (float) k);
        SawBlade::drawBlade (g, centre, bladeRadius, angle, accent.interpolatedWith (core, 0.25f), 1.0f);
        g.setColour (core);
        g.fillEllipse (juce::Rectangle<float> (6.0f, 6.0f).withCentre (centre));
    }

    // Sparks: hot streaks cooling to the theme colour.
    for (const auto& s : sparks)
    {
        const auto t = s.age / s.life;
        const auto alpha = 1.0f - t * t;
        const auto c = core.interpolatedWith (accent, juce::jmin (1.0f, t * 2.5f));
        const auto tail = s.position - s.velocity * 0.025f;
        g.setColour (accent.withAlpha (0.16f * alpha));
        g.fillEllipse (juce::Rectangle<float> (s.size * 4.0f, s.size * 4.0f).withCentre (s.position));
        g.setColour (c.withAlpha (alpha));
        g.drawLine ({ tail, s.position }, s.size);
    }
}

} // namespace kerf::ui

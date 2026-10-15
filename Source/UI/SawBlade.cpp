#include "SawBlade.h"

namespace kerf::ui
{

namespace
{
    constexpr float pi = juce::MathConstants<float>::pi;
    constexpr int numTeeth = 20;
    constexpr float columnWidth = 2.0f, columnsPerSecond = 45.0f;
    constexpr float gravity = 420.0f;   // px / s^2
    constexpr float ringLife = 0.6f;
    constexpr float glowRoom = 19.0f;   // right of the blade: its glow (r + 12) and the beat rings (r + 18) fit
    constexpr size_t maxSparks = 320;
}

SawBlade::SawBlade (KerfAudioProcessor& p, AnimationClock& c)
    : processor (p), clock (c), colour (Colours::accent)
{
    setInterceptsMouseClicks (true, false);
    for (const auto& v : processor.getEngine().getVoiceDisplay())
        lastTriggers.push_back (v.triggers.load (std::memory_order_relaxed));
    clock.add (this);
}

SawBlade::~SawBlade()
{
    clock.remove (this);
}

juce::Point<float> SawBlade::centre() const
{
    // Room on the right for the glow and the beat rings (they would be cut off at the edge).
    return { (float) getWidth() - radius() - glowRoom, (float) getHeight() * 0.5f };
}

float SawBlade::radius() const
{
    return juce::jmin (15.0f, (float) getHeight() * 0.5f - 6.0f);
}

void SawBlade::burst (juce::Colour c, int count, float strength)
{
    // Sparks leave the rim where the ribbon meets it (its left side), along the turning
    // direction (clockwise: up there), with a pull to the left.
    const auto mid = centre();
    const auto r = radius();
    for (int i = 0; i < count && sparks.size() < maxSparks; ++i)
    {
        const auto theta = pi - 0.35f + random.nextFloat() * 0.4f;
        const auto tangent = juce::Point<float> (-std::sin (theta), std::cos (theta));
        const auto launch = (50.0f + random.nextFloat() * 90.0f) * strength;
        Spark s;
        s.position = mid + juce::Point<float> (std::cos (theta), std::sin (theta)) * r;
        s.velocity = tangent * launch - juce::Point<float> (10.0f + random.nextFloat() * 80.0f, 0.0f) * strength;
        s.life = 0.35f + random.nextFloat() * 0.55f;
        s.size = 1.0f + random.nextFloat() * 0.8f;
        s.colour = c;
        sparks.push_back (s);
    }
    heat = juce::jmin (1.0f, heat + 0.25f * strength);
}

void SawBlade::mouseDown (const juce::MouseEvent& e)
{
    if (e.mods.isPopupMenu())
        return;
    burst (Colours::accent, 26, 1.25f);
    speed += 14.0f;
}

void SawBlade::animationTick (double seconds)
{
    const auto dt = (float) juce::jmin (seconds, 0.05);

    // Level: the output peak (it decays by itself in the processor).
    const auto out = juce::jlimit (0.0f, 1.0f, processor.getOutputLevel());
    level = out > level ? out : level + (out - level) * (1.0f - std::exp (-dt * 6.0f));

    // Hits: a spray of sparks in the theme colour (a few per frame at most); the ribbon
    // takes the chop's colour.
    const auto& voices = processor.getEngine().getVoiceDisplay();
    int bursts = 0;
    for (size_t i = 0; i < voices.size() && i < lastTriggers.size(); ++i)
    {
        const auto trig = voices[i].triggers.load (std::memory_order_relaxed);
        if (trig == lastTriggers[i])
            continue;
        lastTriggers[i] = trig;
        const auto id = voices[i].chopId.load (std::memory_order_relaxed);
        if (id == 0 || bursts >= 3)
            continue;
        const auto chops = processor.getChops().getChops();
        colour = chops.empty() ? Colours::accent : chopColour (id, chops.front().id);
        burst (Colours::accent, 10 + (int) (level * 12.0f), 1.0f);
        ++bursts;
    }

    // The whole sample playing (SAMPLE): the ribbon takes the colour of the chop it is in.
    for (const auto& v : voices)
        if (v.chopId.load (std::memory_order_relaxed) == SamplerEngine::wholeSampleId)
            if (const auto id = processor.getChops().chopIdAt ((int64_t) v.position.load (std::memory_order_relaxed)))
            {
                const auto chops = processor.getChops().getChops();
                colour = chops.empty() ? Colours::accent : chopColour (id, chops.front().id);
            }

    // While it is loud, a steady stream.
    if (level > 0.08f)
    {
        sprayClock += dt * juce::jmin (60.0f, (level - 0.08f) * 90.0f);
        for (; sprayClock >= 1.0f; sprayClock -= 1.0f)
            burst (Colours::accent, 1, 0.65f);
    }

    // Beats (while the host or the pattern plays): a ring from the blade.
    if (processor.isHostPlaying() || processor.isPatternPlaying())
    {
        const auto beat = std::floor (processor.getPumpPpq());
        if (beat != lastBeat)
        {
            if (lastBeat >= 0.0)
            {
                const auto downbeat = std::fmod (beat, 4.0) == 0.0;
                rings.push_back ({ 0.0f, downbeat });
                hubFlash = 1.0f;
            }
            lastBeat = beat;
        }
    }
    else
    {
        lastBeat = -1.0;
    }

    // Spin: fast spin-up, a slow coast down; idles slowly.
    const auto target = 0.35f + level * 30.0f;
    speed += (target - speed) * (1.0f - std::exp (-dt * (target > speed ? 8.0f : 1.2f)));
    angle = std::fmod (angle + speed * dt, 2.0f * pi);
    const auto targetHeat = juce::jmin (1.0f, level * 2.5f);
    heat += (targetHeat - heat) * (1.0f - std::exp (-dt * (targetHeat > heat ? 10.0f : 1.5f)));
    hubFlash = juce::jmax (0.0f, hubFlash - dt / (float) Motion::slow);

    // Ribbon: one column every 1/45 s, the loudest moment in it.
    columnPeak = juce::jmax (columnPeak, level);
    columnClock += dt * columnsPerSecond;
    for (; columnClock >= 1.0f; columnClock -= 1.0f)
    {
        ribbon.push_front ({ std::pow (columnPeak, 0.8f), columnPeak > 0.02f ? colour : Colours::textFaint });
        columnPeak = level;
    }
    const auto maxColumns = (size_t) (getWidth() / columnWidth) + 2;
    while (ribbon.size() > maxColumns)
        ribbon.pop_back();

    // Sparks: gravity, a little drag, a bounce off the floor.
    const auto floor = (float) getHeight() - 3.0f;
    const auto drag = std::exp (-dt * 0.8f);
    for (auto& s : sparks)
    {
        s.age += dt;
        s.velocity.y += gravity * dt;
        s.velocity *= drag;
        s.position += s.velocity * dt;
        if (s.position.y > floor && s.velocity.y > 0.0f)
        {
            s.position.y = floor;
            s.velocity.y *= -0.35f;
            s.velocity.x *= 0.6f;
        }
    }
    sparks.erase (std::remove_if (sparks.begin(), sparks.end(),
                                  [w = (float) getWidth()] (const Spark& s) { return s.age >= s.life || s.position.x < -4.0f || s.position.x > w + 4.0f; }),
                  sparks.end());

    for (auto& ring : rings)
        ring.age += dt;
    rings.erase (std::remove_if (rings.begin(), rings.end(), [] (const Ring& ring) { return ring.age >= ringLife; }), rings.end());

    // At rest (silence, no sparks, no rings, the ribbon flat) the blade only idles round:
    // 15 frames a second are plenty for that (the window stays light on the CPU).
    const auto resting = level < 0.002f && heat < 0.01f && hubFlash <= 0.0f && sparks.empty() && rings.empty()
                      && std::none_of (ribbon.begin(), ribbon.end(), [] (const auto& c) { return c.height > 0.02f; });
    idleClock += dt;
    if (isShowing() && (! resting || idleClock >= 1.0f / 15.0f))
    {
        idleClock = 0.0f;
        repaint();
    }
}

void SawBlade::drawBlade (juce::Graphics& g, juce::Point<float> mid, float r, float rotation, juce::Colour c, float alpha)
{
    const auto gullet = r - std::max (3.2f, r * 0.2f); // deeper teeth on a big blade
    const auto step = 2.0f * pi / (float) numTeeth;

    // Hooked teeth: up from the gullet to the tip, then straight down to the next gullet.
    juce::Path teeth;
    for (int k = 0; k < numTeeth; ++k)
    {
        const auto a = rotation + (float) k * step;
        const auto base = mid + juce::Point<float> (std::cos (a), std::sin (a)) * gullet;
        const auto tip = mid + juce::Point<float> (std::cos (a + step * 0.7f), std::sin (a + step * 0.7f)) * r;
        const auto next = mid + juce::Point<float> (std::cos (a + step * 0.78f), std::sin (a + step * 0.78f)) * gullet;
        if (k == 0) teeth.startNewSubPath (base);
        else        teeth.lineTo (base);
        teeth.lineTo (tip);
        teeth.lineTo (next);
    }
    teeth.closeSubPath();

    g.setColour (Colours::surfaceRaised.withAlpha (alpha));
    g.fillPath (teeth);
    g.setColour (c.withAlpha (alpha));
    g.strokePath (teeth, juce::PathStrokeType (std::max (1.1f, r * 0.05f), juce::PathStrokeType::mitered));

    // Body: an inner ring, three slots, the arbor.
    g.setColour (c.withAlpha (alpha * 0.55f));
    g.drawEllipse (juce::Rectangle<float> (r * 0.9f, r * 0.9f).withCentre (mid), 1.0f);
    for (int k = 0; k < 3; ++k)
    {
        const auto a = rotation * 1.0f + (float) k * 2.0f * pi / 3.0f;
        juce::Path slot;
        slot.addCentredArc (mid.x, mid.y, r * 0.66f, r * 0.66f, 0.0f, a, a + 0.9f, true);
        g.strokePath (slot, juce::PathStrokeType (1.4f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }
}

void SawBlade::paint (juce::Graphics& g)
{
    const auto mid = centre();
    const auto r = radius();
    const auto contact = mid.x - r;

    // The cut line: a faint baseline, then the ribbon of what the blade cut, fading out.
    {
        juce::ColourGradient fade (Colours::hairline.withAlpha (0.0f), 0.0f, mid.y, Colours::line, contact, mid.y, false);
        g.setGradientFill (fade);
        g.fillRect (juce::Rectangle<float> (0.0f, mid.y - 0.5f, contact, 1.0f));

        const auto maxHeight = (float) getHeight() * 0.5f - 13.0f; // the ribbon stays a slim band
        auto x = contact - 3.0f + columnClock * columnWidth; // scrolls smoothly between columns
        for (const auto& column : ribbon)
        {
            x -= columnWidth;
            if (x < 0.0f)
                break;
            const auto fadeOut = juce::jlimit (0.0f, 1.0f, x / (contact * 0.85f));
            const auto h = juce::jmax (0.5f, column.height * maxHeight);
            g.setColour (column.colour.withAlpha (0.85f * fadeOut * fadeOut));
            g.fillRect (juce::Rectangle<float> (x, mid.y - h, columnWidth * 0.6f, h * 2.0f));
        }
    }

    // Glow round the blade when it is hot.
    if (heat > 0.01f)
    {
        // (No wider than the room round the blade: it fades out before any edge.)
        const auto reach = juce::jmin (r + 12.0f, mid.y, (float) getWidth() - mid.x);
        juce::ColourGradient glow (Colours::accent.withAlpha (0.35f * heat), mid.x, mid.y,
                                   Colours::accent.withAlpha (0.0f), mid.x + reach, mid.y, true);
        g.setGradientFill (glow);
        g.fillEllipse (juce::Rectangle<float> (reach * 2.0f, reach * 2.0f).withCentre (mid));
    }

    // Beat rings.
    for (const auto& ring : rings)
    {
        const auto t = ring.age / ringLife;
        const auto radiusNow = r + 2.0f + Motion::easeOut (t) * 16.0f;
        g.setColour ((ring.downbeat ? Colours::accent : Colours::text).withAlpha (0.6f * (1.0f - t)));
        g.drawEllipse (juce::Rectangle<float> (radiusNow * 2.0f, radiusNow * 2.0f).withCentre (mid), 1.2f * (1.0f - t) + 0.4f);
    }

    // The blade wears the theme colour (brighter when hot); ghosts when it spins fast.
    const auto bladeColour = Colours::accent.interpolatedWith (Colours::text, 0.15f - 0.15f * heat);
    if (speed > 5.0f)
        for (int k = 2; k >= 1; --k)
            drawBlade (g, mid, r, angle - (float) k * juce::jmin (0.12f, speed * 0.006f), bladeColour, 0.22f / (float) k);
    drawBlade (g, mid, r, angle, bladeColour, 1.0f);

    g.setColour (Colours::textDim.interpolatedWith (Colours::accent, hubFlash));
    g.fillEllipse (juce::Rectangle<float> (4.0f + 2.0f * hubFlash, 4.0f + 2.0f * hubFlash).withCentre (mid));

    // Sparks: white-hot streaks cooling to the chop colour.
    for (const auto& s : sparks)
    {
        const auto t = s.age / s.life;
        const auto alpha = 1.0f - t * t;
        const auto core = lightTheme() ? s.colour.darker (0.25f) : juce::Colours::white; // white-hot on a night sky
        const auto c = core.interpolatedWith (s.colour, juce::jmin (1.0f, t * 3.0f));
        const auto tail = s.position - s.velocity * 0.03f;

        g.setColour (s.colour.withAlpha (0.14f * alpha));
        g.fillEllipse (juce::Rectangle<float> (s.size * 3.5f, s.size * 3.5f).withCentre (s.position));
        g.setColour (c.withAlpha (alpha));
        g.drawLine ({ tail, s.position }, s.size);
    }

}

} // namespace kerf::ui

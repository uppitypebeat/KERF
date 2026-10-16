#include "AboutOverlay.h"
#include "BinaryData.h"

namespace kerf::ui
{

namespace
{
    juce::String resource (const char* data, int size)
    {
        return juce::String::fromUTF8 (data, size).trim();
    }

    constexpr float twoPi = juce::MathConstants<float>::twoPi;
    constexpr int numStars = 1500, numDust = 220, numClouds = 54;
    constexpr float tilt = -0.2f;    // the disc leans a little
    constexpr float flatten = 0.36f; // and is seen from slightly above

    // Deep space at night; in the white themes white paper, the galaxy in strong colours and
    // the words in dark ink (light on light was pale).
    juce::Colour space()    { return lightTheme() ? Colours::background : juce::Colour (0xff04050a); }
    juce::Colour ink()      { return lightTheme() ? Colours::text : juce::Colour (0xfff2ede6); }
    juce::Colour inkDim()   { return lightTheme() ? Colours::textDim : juce::Colour (0xff9c968f); }
    juce::Colour inkFaint() { return lightTheme() ? Colours::textFaint : juce::Colour (0xff5d5953); }

    float distanceToSegment (juce::Point<float> p, juce::Point<float> a, juce::Point<float> b, float& side)
    {
        const auto ab = b - a, ap = p - a;
        const auto len2 = std::max (1.0e-3f, ab.x * ab.x + ab.y * ab.y);
        const auto t = juce::jlimit (0.0f, 1.0f, (ap.x * ab.x + ap.y * ab.y) / len2);
        side = (ab.x * ap.y - ab.y * ap.x) >= 0.0f ? 1.0f : -1.0f;
        return p.getDistanceFrom (a + ab * t);
    }
}

juce::String AboutOverlay::versionString()
{
    return juce::String (KERF_VERSION_LABEL).toUpperCase(); // e.g. "0.9.0 BETA"
}

juce::String AboutOverlay::licenceText()
{
    juce::String t;
    auto section = [&t] (const juce::String& title, const juce::String& body)
    {
        t << title.toUpperCase() << "\n" << juce::String::repeatedString ("-", title.length()) << "\n" << body << "\n\n\n";
    };

    section ("JUCE",
             "This software uses the JUCE framework (https://juce.com), Raw Material Software Limited,\n"
             "used under the JUCE 8 licence.");
    section ("Signalsmith Stretch (time-stretching)", resource (BinaryData::SignalsmithStretch_txt, BinaryData::SignalsmithStretch_txtSize));
    section ("Signalsmith Linear", resource (BinaryData::SignalsmithLinear_txt, BinaryData::SignalsmithLinear_txtSize));
    section ("VST3 SDK (Steinberg)", "VST is a registered trademark of Steinberg Media Technologies GmbH.\n\n"
                                     + resource (BinaryData::VST3_SDK_txt, BinaryData::VST3_SDK_txtSize));
    section ("Inter (typeface)", resource (BinaryData::InterOFL_txt, BinaryData::InterOFL_txtSize));
    section ("JetBrains Mono (typeface)", resource (BinaryData::JetBrainsMonoOFL_txt, BinaryData::JetBrainsMonoOFL_txtSize));
    return t.trimEnd();
}

AboutOverlay::AboutOverlay (AnimationClock& c) : clock (c)
{
    setWantsKeyboardFocus (true);

    licences.setMultiLine (true);
    licences.setReadOnly (true);
    licences.setCaretVisible (false);
    licences.setScrollbarsShown (true);
    licences.setFont (Fonts::mono (10.5f, false));
    licences.setText (licenceText(), false);
    addChildComponent (licences);

    addAndMakeVisible (licencesButton);
    licencesButton.setTooltip ("The licences of the libraries and typefaces KERF uses");
    licencesButton.onClick = [this] { licences.setVisible (licencesButton.getToggleState()); repaint(); };
    addAndMakeVisible (close);
    close.onClick = [this] { hide(); };

    clock.add (this);
}

AboutOverlay::~AboutOverlay()
{
    clock.remove (this);
}

void AboutOverlay::show()
{
    licences.setColour (juce::TextEditor::backgroundColourId, space().withAlpha (0.92f));
    licences.setColour (juce::TextEditor::textColourId, inkDim());
    licences.setColour (juce::TextEditor::outlineColourId, Colours::accent.withAlpha (0.35f));
    licences.setColour (juce::TextEditor::focusedOutlineColourId, Colours::accent.withAlpha (0.35f));
    licences.applyColourToAllText (inkDim());
    licences.setCaretPosition (0);
    licences.setVisible (false);
    licencesButton.setToggleState (false, juce::dontSendNotification);

    appear = 0.0f;
    spin = 3.0f; // it starts with a whirl and settles
    cuts.clear();
    sparks.clear();
    rings.clear();

    setVisible (true);
    toFront (true);
    if (isShowing()) grabKeyboardFocus(); // (not while the window is hidden)
}

void AboutOverlay::hide()
{
    setVisible (false);
    if (auto* parent = getParentComponent(); parent != nullptr && parent->isShowing())
        parent->grabKeyboardFocus();
}

juce::Point<float> AboutOverlay::galaxyCentre() const
{
    return { getWidth() * 0.5f, getHeight() * 0.42f };
}

void AboutOverlay::buildGalaxy()
{
    // Three spiral arms of grains, dense at the core; a sprinkle of far stars behind.
    stars.clear();
    for (int i = 0; i < numStars; ++i)
    {
        Star s;
        s.radius = 0.1f + 0.9f * std::pow (random.nextFloat(), 0.95f);
        const auto arm = (float) (i % 3) * twoPi / 3.0f;
        const auto spread = (random.nextFloat() - 0.5f) * (0.9f - 0.45f * s.radius);
        s.theta = arm + 2.7f * std::log (s.radius * 9.0f + 1.0f) + spread;
        s.depth = random.nextFloat();
        s.twinkle = random.nextFloat() * twoPi;
        s.hue = juce::jlimit (0.0f, 1.0f, s.radius * 1.25f + (random.nextFloat() - 0.5f) * 0.35f);
        s.size = 0.8f + 2.3f * s.depth * s.depth + (s.radius < 0.2f ? 0.5f : 0.0f);
        stars.push_back (s);
    }
    // Glowing clouds of gas along the arms.
    clouds.clear();
    for (int i = 0; i < numClouds; ++i)
    {
        Star cl;
        cl.radius = 0.16f + 0.8f * random.nextFloat();
        const auto arm = (float) (i % 3) * twoPi / 3.0f;
        cl.theta = arm + 2.7f * std::log (cl.radius * 9.0f + 1.0f) + (random.nextFloat() - 0.5f) * 0.35f;
        cl.hue = juce::jlimit (0.0f, 1.0f, cl.radius * 1.1f + (random.nextFloat() - 0.5f) * 0.3f);
        cl.size = 40.0f + 90.0f * random.nextFloat() * (1.1f - cl.radius * 0.5f);
        cl.twinkle = random.nextFloat() * twoPi;
        clouds.push_back (cl);
    }
    dust.clear();
    for (int i = 0; i < numDust; ++i)
    {
        Star d;
        d.x = random.nextFloat();
        d.y = random.nextFloat();
        d.depth = random.nextFloat();
        d.twinkle = random.nextFloat() * twoPi;
        d.size = 0.5f + 1.2f * d.depth * d.depth;
        dust.push_back (d);
    }
}

void AboutOverlay::resized()
{
    if (stars.empty())
        buildGalaxy();

    const auto c = galaxyCentre();

    // The wordmark as a path (it glows in layers).
    {
        juce::GlyphArrangement glyphs;
        glyphs.addLineOfText (Fonts::sans (78.0f, 600).withExtraKerningFactor (0.42f), "KERF", 0.0f, 0.0f);
        wordmark.clear();
        glyphs.createPath (wordmark);
        const auto b = wordmark.getBounds();
        wordmark.applyTransform (juce::AffineTransform::translation (c.x - b.getCentreX(), c.y - b.getCentreY()));
    }

    // The blade: 40 hooked teeth round a ring, centred on the galaxy.
    {
        blade.clear();
        constexpr int teeth = 40;
        constexpr float outer = 168.0f, inner = 154.0f;
        for (int k = 0; k < teeth; ++k)
        {
            const auto a0 = twoPi * (float) k / (float) teeth;
            const auto a1 = a0 + twoPi / (float) teeth * 0.62f;
            const auto a2 = a0 + twoPi / (float) teeth;
            const juce::Point<float> p0 (c.x + inner * std::sin (a0), c.y - inner * std::cos (a0));
            const juce::Point<float> p1 (c.x + outer * std::sin (a1), c.y - outer * std::cos (a1));
            const juce::Point<float> p2 (c.x + inner * std::sin (a2), c.y - inner * std::cos (a2));
            if (k == 0) blade.startNewSubPath (p0);
            blade.lineTo (p1);
            blade.lineTo (p2);
        }
        blade.closeSubPath();
        blade.addEllipse (juce::Rectangle<float> (118.0f, 118.0f).withCentre (c));
        for (int k = 0; k < 6; ++k) // slots in the body
        {
            const auto a = twoPi * (float) k / 6.0f;
            blade.addEllipse (juce::Rectangle<float> (9.0f, 9.0f).withCentre ({ c.x + 135.0f * std::sin (a), c.y - 135.0f * std::cos (a) }));
        }
    }

    auto r = getLocalBounds().reduced (24, 20);
    auto bottom = r.removeFromBottom (24);
    close.setBounds (bottom.removeFromRight (72));
    bottom.removeFromRight (8);
    licencesButton.setBounds (bottom.removeFromRight (88));
    licences.setBounds (juce::Rectangle<int> (640, 300).withCentre ({ getWidth() / 2, getHeight() / 2 + 40 }));
}

//==============================================================================
void AboutOverlay::animationTick (double seconds)
{
    if (! isVisible())
        return;

    const auto dt = (float) juce::jmin (seconds, 0.05);
    time += dt;
    appear = std::min (1.0f, appear + dt / 0.9f);

    // The blade settles to a slow turn; cuts spin it up. The galaxy turns with it a little.
    spin += (0.35f - spin) * std::min (1.0f, dt * 0.7f);
    bladeAngle = std::fmod (bladeAngle + spin * dt, twoPi);
    rotation += dt * (0.045f + 0.02f * spin);

    // The stars: an orbit, plus whatever the cuts and the mouse did to them (springing back).
    const auto c = galaxyCentre();
    const auto discRadius = getWidth() * 0.56f;
    const auto cosT = std::cos (tilt), sinT = std::sin (tilt);
    const auto damping = std::exp (-dt * 2.6f);
    for (auto& s : stars)
    {
        const auto a = s.theta + rotation * (0.55f / (0.22f + s.radius));
        const auto ox = std::cos (a) * s.radius * discRadius, oy = std::sin (a) * s.radius * discRadius * flatten;
        const auto px = c.x + ox * cosT - oy * sinT, py = c.y + ox * sinT + oy * cosT;

        if (mouseInside && ! dragging) // a faint pull towards the mouse
        {
            const auto dxm = mouse.x - (px + s.dx), dym = mouse.y - (py + s.dy);
            const auto d = std::sqrt (dxm * dxm + dym * dym);
            if (d < 150.0f && d > 1.0f)
            {
                const auto pull = (1.0f - d / 150.0f) * 140.0f * dt;
                s.vx += dxm / d * pull;
                s.vy += dym / d * pull;
            }
        }
        s.vx += -s.dx * 10.0f * dt;
        s.vy += -s.dy * 10.0f * dt;
        s.vx *= damping;
        s.vy *= damping;
        s.dx += s.vx * dt;
        s.dy += s.vy * dt;
        s.x = px + s.dx;
        s.y = py + s.dy;
        s.twinkle += dt * (0.8f + 1.6f * s.depth);
    }
    for (auto& cl : clouds) // the gas turns with the stars (no pushing about)
    {
        const auto a = cl.theta + rotation * (0.55f / (0.22f + cl.radius));
        const auto ox = std::cos (a) * cl.radius * discRadius, oy = std::sin (a) * cl.radius * discRadius * flatten;
        cl.x = c.x + ox * cosT - oy * sinT;
        cl.y = c.y + ox * sinT + oy * cosT;
        cl.twinkle += dt * 0.4f;
    }
    for (auto& d : dust)
        d.twinkle += dt * (0.5f + d.depth);

    for (auto& cut : cuts)
        if (! (dragging && &cut == &cuts.back()))
            cut.age += dt / 1.1f;
    cuts.erase (std::remove_if (cuts.begin(), cuts.end(), [] (const Cut& k) { return k.age >= 1.0f; }), cuts.end());

    const auto drag = std::exp (-dt * 1.8f);
    for (auto& s : sparks)
    {
        s.life -= dt;
        s.velocity *= drag;
        s.velocity.y += 140.0f * dt;
        s.position += s.velocity * dt;
    }
    sparks.erase (std::remove_if (sparks.begin(), sparks.end(), [] (const Spark& s) { return s.life <= 0.0f; }), sparks.end());

    for (auto& ring : rings)
        ring.age += dt / 1.2f;
    rings.erase (std::remove_if (rings.begin(), rings.end(), [] (const Ring& k) { return k.age >= 1.0f; }), rings.end());

    repaint();
}

//==============================================================================
void AboutOverlay::slice (juce::Point<float> from, juce::Point<float> to, float speed)
{
    const auto len = from.getDistanceFrom (to);
    if (len < 0.5f)
        return;
    const auto dir = (to - from) / len;
    const juce::Point<float> normal (-dir.y, dir.x);

    // Stars near the blade's path are flung to the side they were on.
    constexpr float reach = 34.0f;
    const auto force = 260.0f + std::min (speed, 4000.0f) * 0.32f;
    for (auto& s : stars)
    {
        float side = 1.0f;
        const auto d = distanceToSegment ({ s.x, s.y }, from, to, side);
        if (d < reach)
        {
            const auto k = 1.0f - d / reach;
            s.vx += (normal.x * side + dir.x * 0.3f) * force * k;
            s.vy += (normal.y * side + dir.y * 0.3f) * force * k;
        }
    }

    // Sparks off the cut.
    const auto count = juce::jlimit (1, 10, (int) (len / 5.0f));
    for (int i = 0; i < count && sparks.size() < 700; ++i)
    {
        const auto t = random.nextFloat();
        const auto side = random.nextBool() ? 1.0f : -1.0f;
        const auto kick = 120.0f + random.nextFloat() * (260.0f + std::min (speed, 3000.0f) * 0.12f);
        sparks.push_back ({ from + (to - from) * t, normal * side * kick + dir * std::min (speed, 3000.0f) * 0.15f,
                            0.0f, 0.0f, 0.8f + random.nextFloat() * 1.4f });
        auto& sp = sparks.back();
        sp.maxLife = sp.life = 0.25f + random.nextFloat() * 0.55f;
    }

    if (cuts.empty() || ! dragging)
        cuts.push_back ({ { from }, 0.0f });
    cuts.back().points.push_back (to);
    if (cuts.back().points.size() > 400)
        cuts.back().points.erase (cuts.back().points.begin());

    spin = std::min (14.0f, spin + len * 0.006f);
}

void AboutOverlay::shockwave (juce::Point<float> at)
{
    rings.push_back ({ at, 0.0f });
    for (auto& s : stars)
    {
        const auto dx = s.x - at.x, dy = s.y - at.y;
        const auto d = std::sqrt (dx * dx + dy * dy);
        if (d < 320.0f && d > 0.5f)
        {
            const auto k = 1.0f - d / 320.0f;
            s.vx += dx / d * 900.0f * k;
            s.vy += dy / d * 900.0f * k;
        }
    }
    for (int i = 0; i < 36 && sparks.size() < 700; ++i)
    {
        const auto a = twoPi * (float) i / 36.0f + random.nextFloat() * 0.2f;
        const auto v = 220.0f + random.nextFloat() * 380.0f;
        sparks.push_back ({ at, { std::cos (a) * v, std::sin (a) * v }, 0.0f, 0.0f, 1.0f + random.nextFloat() * 1.2f });
        auto& sp = sparks.back();
        sp.maxLife = sp.life = 0.4f + random.nextFloat() * 0.6f;
    }
    spin = std::min (14.0f, spin + 2.5f);
}

//==============================================================================
void AboutOverlay::mouseDown (const juce::MouseEvent& e)
{
    if (licences.isVisible() && licences.getBounds().toFloat().contains (e.position))
        return;
    dragging = true;
    lastDrag = e.position;
    lastDragTime = juce::Time::getMillisecondCounterHiRes();
    dragDistance = 0.0f;
    cuts.push_back ({ { e.position }, 0.0f });
}

void AboutOverlay::mouseDrag (const juce::MouseEvent& e)
{
    if (! dragging)
        return;
    mouse = e.position;
    const auto now = juce::Time::getMillisecondCounterHiRes();
    const auto len = lastDrag.getDistanceFrom (e.position);
    if (len < 2.0f)
        return;

    const auto speed = len / (float) std::max (0.004, (now - lastDragTime) * 0.001); // px per second
    slice (lastDrag, e.position, speed);
    dragDistance += len;
    lastDrag = e.position;
    lastDragTime = now;
}

void AboutOverlay::mouseUp (const juce::MouseEvent& e)
{
    if (dragging && dragDistance < 6.0f)
    {
        if (! cuts.empty() && cuts.back().points.size() <= 1)
            cuts.pop_back();
        shockwave (e.position);
    }
    dragging = false;
}

void AboutOverlay::mouseMove (const juce::MouseEvent& e)
{
    mouse = e.position;
    mouseInside = true;
}

bool AboutOverlay::keyPressed (const juce::KeyPress& key)
{
    if (key == juce::KeyPress::escapeKey)
    {
        hide();
        return true;
    }
    return true; // the keys stay here while it is open (Space must not start playback)
}

//==============================================================================
void AboutOverlay::paint (juce::Graphics& g)
{
    const auto bounds = getLocalBounds().toFloat();
    const auto c = galaxyCentre();
    const auto fade = Motion::easeOut (appear);
    const auto accent = Colours::accent, glow = nebulaGlow();
    // White mode: the hot core in the deep accent (white would vanish), the colours stronger.
    const auto light = lightTheme();
    const auto hot = light ? accent.darker (0.45f) : ink();
    const auto strong = light ? 1.6f : 1.0f;

    // Deep space, lit from the galaxy's core.
    g.setColour (space());
    g.fillRect (bounds);
    {
        juce::ColourGradient light (accent.withAlpha (0.22f * fade), c.x, c.y, accent.withAlpha (0.0f), c.x + bounds.getWidth() * 0.55f, c.y, true);
        g.setGradientFill (light);
        g.fillRect (bounds);
        juce::ColourGradient haze (glow.withAlpha (0.12f * fade), c.x + bounds.getWidth() * 0.22f, c.y + 60.0f,
                                   glow.withAlpha (0.0f), c.x + bounds.getWidth() * 0.62f, c.y + 60.0f, true);
        g.setGradientFill (haze);
        g.fillRect (bounds);
    }

    // Far stars.
    for (const auto& d : dust)
    {
        const auto a = (0.15f + 0.45f * d.depth) * (0.6f + 0.4f * std::sin (d.twinkle)) * fade;
        g.setColour (ink().withAlpha (a));
        g.fillRect (juce::Rectangle<float> (d.size, d.size).withCentre ({ d.x * bounds.getWidth(), d.y * bounds.getHeight() }));
    }

    // Its gas: soft glowing clouds along the arms, breathing slowly.
    for (const auto& cl : clouds)
    {
        const auto colour = accent.interpolatedWith (glow, cl.hue * 0.55f);
        const auto a = (0.07f + 0.04f * std::sin (cl.twinkle)) * fade * strong;
        const auto r = cl.size;
        g.setGradientFill (juce::ColourGradient (colour.withAlpha (a), cl.x, cl.y, colour.withAlpha (0.0f), cl.x + r, cl.y, true));
        g.fillEllipse (juce::Rectangle<float> (r * 2.0f, r * 2.0f * 0.7f).withCentre ({ cl.x, cl.y }));
    }

    // The galaxy: colour from the core outwards (white-hot, the accent, the second colour).
    for (const auto& s : stars)
    {
        const auto twinkle = 0.65f + 0.35f * std::sin (s.twinkle);
        const auto moved = std::min (1.0f, std::sqrt (s.dx * s.dx + s.dy * s.dy) / 40.0f); // flung stars burn brighter
        const auto colour = accent.interpolatedWith (glow, s.hue * 0.65f).interpolatedWith (hot, std::pow (1.0f - s.radius, 4.0f) * 0.85f);
        const auto alpha = juce::jlimit (0.0f, 1.0f, ((0.38f + 0.62f * s.depth) * twinkle + 0.5f * moved) * fade * (light ? 1.25f : 1.0f));
        const auto size = s.size * (1.0f + 0.6f * moved);
        g.setColour (colour.withAlpha (alpha));
        if (size < 1.4f)
            g.fillRect (juce::Rectangle<float> (size, size).withCentre ({ s.x, s.y }));
        else
            g.fillEllipse (juce::Rectangle<float> (size, size).withCentre ({ s.x, s.y }));
    }

    // The core: a white-hot bloom behind the blade.
    {
        juce::ColourGradient core (hot.withAlpha (0.28f * fade), c.x, c.y, accent.withAlpha (0.0f), c.x + 190.0f, c.y, true);
        core.addColour (0.35, accent.withAlpha (0.16f * fade));
        g.setGradientFill (core);
        g.fillEllipse (juce::Rectangle<float> (380.0f, 380.0f).withCentre (c));
    }

    // The blade, turning: a dark disc so the wordmark stands out, a glowing edge.
    {
        const auto turn = juce::AffineTransform::rotation (bladeAngle, c.x, c.y);
        auto shape = blade;
        shape.applyTransform (turn);
        g.setColour (space().withAlpha (0.72f * fade));
        g.fillPath (shape);
        const auto heat = juce::jlimit (0.0f, 1.0f, (spin - 0.35f) / 6.0f);
        for (const auto [width, alpha] : { std::pair { 9.0f, 0.06f }, { 4.0f, 0.14f }, { 1.4f, 0.75f } })
        {
            g.setColour (accent.interpolatedWith (ink(), heat * 0.5f).withAlpha (alpha * (0.7f + 0.3f * heat) * fade));
            g.strokePath (shape, juce::PathStrokeType (width, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        }
    }

    // The cuts: a dark kerf with glowing edges, fading.
    for (const auto& cut : cuts)
    {
        if (cut.points.size() < 2)
            continue;
        juce::Path line;
        line.startNewSubPath (cut.points.front());
        for (size_t i = 1; i < cut.points.size(); ++i)
            line.lineTo (cut.points[i]);
        const auto life = 1.0f - cut.age;
        for (const auto [width, alpha] : { std::pair { 16.0f, 0.06f }, { 8.0f, 0.16f }, { 3.2f, 0.55f } })
        {
            g.setColour (accent.withAlpha (alpha * life));
            g.strokePath (line, juce::PathStrokeType (width, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        }
        g.setColour (ink().withAlpha (0.9f * life));
        g.strokePath (line, juce::PathStrokeType (1.4f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        g.setColour (space().withAlpha (0.85f * life)); // the kerf itself: the gap the cut leaves
        g.strokePath (line, juce::PathStrokeType (0.7f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }

    // Shock waves.
    for (const auto& ring : rings)
    {
        const auto radius = 20.0f + Motion::easeOut (ring.age) * 340.0f;
        const auto a = 1.0f - ring.age;
        g.setColour (accent.withAlpha (0.12f * a));
        g.drawEllipse (juce::Rectangle<float> (radius * 2.0f, radius * 2.0f).withCentre (ring.centre), 14.0f * a);
        g.setColour (ink().withAlpha (0.55f * a));
        g.drawEllipse (juce::Rectangle<float> (radius * 2.0f, radius * 2.0f).withCentre (ring.centre), 1.4f);
    }

    // Sparks: short streaks along their flight.
    for (const auto& s : sparks)
    {
        const auto t = s.life / s.maxLife;
        g.setColour (ink().interpolatedWith (accent, 1.0f - t).withAlpha (t));
        g.drawLine ({ s.position, s.position - s.velocity * 0.018f }, s.size);
    }

    // The wordmark, glowing in layers.
    for (const auto [width, alpha] : { std::pair { 14.0f, 0.05f }, { 7.0f, 0.1f }, { 3.0f, 0.22f } })
    {
        g.setColour (accent.withAlpha (alpha * fade));
        g.strokePath (wordmark, juce::PathStrokeType (width, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }
    g.setColour (ink().withAlpha (fade));
    g.fillPath (wordmark);

    // Under it: what it is, the version; at the bottom the credits.
    auto line = [&g, fade] (const juce::String& text, juce::Rectangle<float> area, juce::Font font, juce::Colour colour)
    {
        g.setColour (colour.withMultipliedAlpha (fade));
        g.setFont (font);
        g.drawText (text, area, juce::Justification::centred, false);
    };
    line (juce::String::fromUTF8 ("SAMPLE CHOPPING  \xc2\xb7  GRANULAR  \xc2\xb7  EFFECTS"), { 0.0f, c.y + 56.0f, bounds.getWidth(), 16.0f },
          Fonts::sans (11.5f, 500).withExtraKerningFactor (0.32f), inkDim());
    line ("VERSION " + versionString(), { 0.0f, c.y + 76.0f, bounds.getWidth(), 14.0f }, Fonts::mono (10.5f, true), accent.interpolatedWith (ink(), 0.35f));

    const auto creditsY = bounds.getBottom() - 112.0f;
    line ("CREATED & DESIGNED BY", { 0.0f, creditsY, bounds.getWidth(), 14.0f }, Fonts::sans (10.0f, 500).withExtraKerningFactor (0.3f), inkFaint());
    line ("Julien Upmeier", { 0.0f, creditsY + 16.0f, bounds.getWidth(), 22.0f }, Fonts::sans (17.0f, 600), ink());
    line (juce::String::fromUTF8 ("\xc2\xa9 2026 Julien Upmeier. All rights reserved."), { 0.0f, creditsY + 40.0f, bounds.getWidth(), 14.0f },
          Fonts::sans (10.5f, 400), inkDim());

    // Behind the licence text.
    if (licences.isVisible())
    {
        g.setColour (space().withAlpha (0.6f));
        g.fillRect (licences.getBounds().toFloat().expanded (12.0f));
    }
}

} // namespace kerf::ui

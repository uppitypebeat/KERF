#include "FxView.h"
#include "ScreenStyle.h"

namespace kerf::ui
{

namespace
{
    using screen::signal;
    using screen::hot;
    using screen::hudText;
    using screen::glowStroke;

    const char* dirtModes[] = { "WARM", "FUZZ", "CRUSH" };
    const char* wobbleModes[] = { "TAPE", "CHORUS" };
    const char* pumpModes[] = { "1/4", "1/8", "1/16" };
    constexpr float twoPi = juce::MathConstants<float>::twoPi;
    constexpr float pi = juce::MathConstants<float>::pi;

    juce::String percent (float v) { return juce::String (juce::roundToInt (v * 100.0f)) + "%"; }

    juce::String hzShort (float hz)
    {
        return hz >= 1000.0f ? juce::String (hz / 1000.0f, hz >= 10000.0f ? 0 : 1) + "K" : juce::String (juce::roundToInt (hz));
    }

    /** Position (0..1) of a frequency on the spectrum's log axis. */
    float freqToU (float hz)
    {
        const auto f0 = SpectrumAnalyzer::bandFrequency (0), f1 = SpectrumAnalyzer::bandFrequency (SpectrumAnalyzer::numBands - 1);
        return juce::jlimit (0.0f, 1.0f, std::log (hz / f0) / std::log (f1 / f0));
    }

    /** Round a ring (angle 0 at the bottom, going round by the right): the lows on the left
        (u 0), the highs on the right (u 1), the same over the top as under the bottom. So a
        low-pass leaves the left of the tunnel, a high-pass the right. */
    float uOfAngle (float a)
    {
        const auto fromLeft = std::fmod (a + pi * 0.5f, twoPi);
        return fromLeft <= pi ? fromLeft / pi : (twoPi - fromLeft) / pi;
    }
    /** The two angles round a ring (under and over) where u is. */
    std::array<float, 2> anglesOf (float u) { return { u * pi - pi * 0.5f, pi * 1.5f - u * pi }; }

    // Perspective: depth 0 is here (as wide as the screen), 1 the far end (a small hole).
    constexpr float zNear = 1.0f, zFar = 15.0f;
}

FxView::FxView (KerfAudioProcessor& p, AnimationClock& c) : processor (p), clock (c)
{
    static_assert (bins == SpectrumAnalyzer::numBands, "one ring point per spectrum band");
    for (int i = 0; i < 110; ++i)
        stars.push_back ({ random.nextFloat() * twoPi, random.nextFloat(), 0.6f + 0.8f * random.nextFloat(), 0.6f + 1.4f * random.nextFloat() });
    for (int i = 0; i < maxRings; ++i) // the tunnel is there from the start (quiet rings)
    {
        Ring ring;
        ring.depth = (float) i / (float) maxRings;
        rings.push_back (ring);
    }
    clock.add (this);
    setInterceptsMouseClicks (false, false);
}

FxView::~FxView()
{
    clock.remove (this);
}

void FxView::resized()
{
    auto r = getLocalBounds().toFloat();
    header = r.removeFromTop (26.0f);
    r.removeFromTop (2.0f);
    tunnelScreen = r;
}

FxView::Settings FxView::readSettings() const
{
    auto value = [this] (const char* id) { return processor.getParameters().getRawParameterValue (id)->load(); };
    Settings st;
    const auto on = value (Params::fxOff) < 0.5f; // the tab switched off: the tunnel runs plain
    st.space = on ? value (Params::space) : 0.0f;
    st.dirt = on ? value (Params::drive) : 0.0f;
    st.dirtMode = juce::jlimit (0, 2, (int) value (Params::dirtMode));
    st.filter = on ? value (Params::filter) : 0.0f;
    st.resonance = value (Params::resonance);
    st.wobble = on ? value (Params::wobble) : 0.0f;
    st.wobbleMode = juce::jlimit (0, 1, (int) value (Params::wobbleMode));
    st.pump = on ? value (Params::pump) : 0.0f;
    st.pumpMode = juce::jlimit (0, 2, (int) value (Params::pumpMode));
    const auto delay = processor.delaySettings (st.space, processor.getEffectiveTempo());
    st.delaySeconds = std::max (0.05, delay.delaySeconds);
    st.feedback = delay.feedback;
    st.delayTime = juce::jlimit (0, PingPongDelay::Time::count - 1, (int) value (Params::spaceTime));
    st.pingPong = delay.pingPong;
    return st;
}

//==============================================================================
void FxView::animationTick (double dt)
{
    const ScreenScope screenColours; // (colours picked here are the screen's too)
    if (! isVisible())
        return;

    time += dt;
    if (intro < 1.0f)
        intro = std::min (1.0f, intro + (float) dt / 0.75f);
    const auto step = (float) std::min (dt, 0.05);

    s = readSettings();

    // The output spectrum (fast rise, slower fall): the shape of the next ring.
    const auto& bands = processor.getSpectrum().getBands();
    for (int b = 0; b < bins; ++b)
    {
        const auto v = std::pow (juce::jlimit (0.0f, 1.0f, (bands[(size_t) b] + 66.0f) / 60.0f), 1.6f);
        current[(size_t) b] += (v - current[(size_t) b]) * (v > current[(size_t) b] ? 0.6f : 0.25f);
    }

    // Output level.
    {
        auto& tap = processor.getOutputTap();
        tap.pull();
        float peak = 0.0f;
        for (int i = 0; i < 512; ++i)
            peak = std::max ({ peak, std::abs (tap.getLeft (i)), std::abs (tap.getRight (i)) });
        level = peak > level ? peak : level + (peak - level) * std::min (1.0f, step * 5.0f);
    }

    // WOBBLE and PUMP move while something plays (the host, the pattern, a chop or the
    // sample) and hold where they are when it stops.
    bool playing = processor.isHostPlaying() || processor.isPatternPlaying();
    for (const auto& v : processor.getEngine().getVoiceDisplay())
        playing = playing || v.chopId.load (std::memory_order_relaxed) != 0;
    if (playing)
    {
        motion += dt; // (playing again: it goes on from where it stood)
        pumpPpq = processor.getPumpPpq();
    }
    pumpGain = PumpProcessor::gainAt (pumpPpq, processor.getEffectiveTempo(), { s.pump, (PumpProcessor::Mode) s.pumpMode });
    const auto duck = s.pump * (1.0f - pumpGain); // 0 .. pump: how far it is ducked right now
    flash += (duck - flash) * std::min (1.0f, step * (duck > flash ? 30.0f : 6.0f));

    // The flight: cruising slowly, faster with the sound, surging with the PUMP.
    const auto sounding = processor.isSounding() || playing;
    const auto target = (sounding ? 0.16f + 0.5f * std::min (1.0f, level) : 0.05f) * (1.0f + 1.5f * duck);
    speed += (target - speed) * std::min (1.0f, step * 3.0f);
    for (auto& ring : rings)
        ring.depth -= speed * step;
    rings.erase (std::remove_if (rings.begin(), rings.end(), [] (const Ring& r) { return r.depth <= -0.02f; }), rings.end());
    spawn += speed * step;
    const auto spacing = 1.0f / (float) maxRings;
    while (spawn >= spacing && (int) rings.size() < maxRings + 2)
    {
        spawn -= spacing;
        Ring ring;
        ring.spectrum = current;
        ring.depth = 1.0f - spawn;
        rings.push_back (ring);
    }

    for (auto& star : stars)
    {
        star.depth -= speed * 1.7f * star.speed * step;
        if (star.depth <= 0.0f)
        {
            star.depth += 1.0f;
            star.angle = random.nextFloat() * twoPi;
        }
    }

    // FUZZ: sparks off the nearest rings, more the louder and dirtier.
    for (auto& sp : sparks)
    {
        sp.age += step;
        sp.pos += sp.velocity * step;
        sp.velocity *= std::exp (-step * 2.0f);
    }
    sparks.erase (std::remove_if (sparks.begin(), sparks.end(), [] (const Spark& sp) { return sp.age >= sp.life; }), sparks.end());
    if (s.dirtMode == 1 && s.dirt > 0.01f && sounding && ! tunnelScreen.isEmpty())
    {
        const auto count = (int) (s.dirt * (2.0f + 10.0f * std::min (1.0f, level)));
        for (int i = 0; i < count && sparks.size() < 260; ++i)
        {
            const auto depth = 0.08f + 0.35f * random.nextFloat();
            const auto a = random.nextFloat() * twoPi;
            const auto r = radiusAt (depth) * 0.85f;
            const juce::Point<float> dir (std::sin (a), std::cos (a));
            Spark sp;
            sp.pos = centreAt (depth) + dir * r;
            sp.velocity = dir * (120.0f + 380.0f * random.nextFloat());
            sp.life = 0.25f + 0.35f * random.nextFloat();
            sparks.push_back (sp);
        }
    }

    if (idle.update (dt, intro < 1.0f || sounding || speed > 0.01f, processor.getParameterChangeCount()))
        repaint();
}

float FxView::shapeHeight (float h) const
{
    const auto a = s.dirt;
    if (a < 0.001f)
        return h;

    switch (s.dirtMode)
    {
        case 0: // WARM: soft saturation, everything gets fuller
        {
            const auto k = 1.0f + 3.0f * a;
            return std::tanh (k * h) / std::tanh (k);
        }
        case 1: // FUZZ: driven into a hard ceiling
            return std::min (h * (1.0f + 2.0f * a), 1.0f - 0.5f * a);
        default: // CRUSH: few levels
        {
            const auto levels = std::max (3.0f, std::round (16.0f - 12.0f * a));
            return std::floor (h * levels + 0.5f) / levels;
        }
    }
}

float FxView::heightAt (const Ring& ring, float u) const
{
    const auto x = juce::jlimit (0.0f, (float) bins - 1.001f, u * (float) (bins - 1));
    const auto i = (size_t) x;
    const auto f = x - (float) i;
    return shapeHeight (ring.spectrum[i] + (ring.spectrum[std::min (i + 1, (size_t) bins - 1)] - ring.spectrum[i]) * f);
}

juce::Point<float> FxView::centreAt (float depth) const
{
    // WOBBLE bends the tunnel: the further away, the further off (the camera is here).
    auto c = tunnelScreen.getCentre().translated (0.0f, -tunnelScreen.getHeight() * 0.03f);
    if (s.wobble > 0.001f)
    {
        const auto m = (float) motion;
        const auto d = juce::jlimit (0.0f, 1.0f, depth);
        juce::Point<float> off;
        if (s.wobbleMode == 0) // TAPE: slow and uneven
            off = { std::sin (m * 0.9f + d * 4.0f) * 0.65f + std::sin (m * 0.37f + d * 7.0f) * 0.35f,
                    std::cos (m * 0.7f + d * 3.0f) * 0.5f };
        else                   // CHORUS: even
            off = { std::sin (m * 1.7f + d * 5.0f), 0.45f * std::cos (m * 1.7f + d * 5.0f) };
        c += off * s.wobble * tunnelScreen.getWidth() * 0.14f * (0.25f + d);
    }
    return c;
}

float FxView::radiusAt (float depth) const
{
    const auto rMax = std::max (tunnelScreen.getWidth(), tunnelScreen.getHeight()) * 0.62f;
    const auto pumpScale = 1.0f - 0.22f * s.pump * (1.0f - pumpGain); // PUMP: the tunnel contracts
    return rMax * zNear / (zNear + juce::jlimit (-0.05f, 1.0f, depth) * (zFar - zNear)) * pumpScale;
}

FxView::RingShape FxView::shapeOf (const Ring& ring, float depth) const
{
    // The ring's outline round its centre, once per frame (the ring, its echoes and its chorus
    // twin share it): the spectrum round it (lows at the bottom). DIRT: CRUSH breaks it into a
    // polygon, FUZZ makes it crackle. FILTER: which part it lets through. Small, far rings
    // need fewer points.
    RingShape shape;
    const auto crush = s.dirtMode == 2 && s.dirt > 0.01f;
    shape.facets = crush ? std::max (5, juce::roundToInt (40.0f - 34.0f * s.dirt))
                         : juce::jlimit (24, points, juce::roundToInt (radiusAt (depth) / 5.0f));
    const auto fuzz = s.dirtMode == 1 && depth < 0.5f ? s.dirt : 0.0f;
    const auto cutU = s.filter < -0.005f ? freqToU (ToneProcessor::lowPassCutoff (s.filter))
                    : (s.filter > 0.005f ? freqToU (ToneProcessor::highPassCutoff (s.filter)) : -1.0f);
    auto& rng = const_cast<juce::Random&> (random); // (the crackle is new every frame)
    for (int k = 0; k <= shape.facets; ++k)
    {
        const auto a = twoPi * (float) k / (float) shape.facets;
        const auto u = uOfAngle (std::fmod (a, twoPi));
        auto h = heightAt (ring, u);
        if (fuzz > 0.0f)
            h += (rng.nextFloat() - 0.5f) * 0.25f * fuzz;
        shape.scale[(size_t) k] = 0.7f + 0.42f * h;
        shape.dx[(size_t) k] = std::sin (a);
        shape.dy[(size_t) k] = std::cos (a);
        shape.kept[(size_t) k] = cutU < 0.0f || (s.filter < 0.0f ? u <= cutU : u >= cutU);
    }
    return shape;
}

juce::Path FxView::pathOf (const RingShape& shape, juce::Point<float> centre, float radius, bool passBand, bool& any) const
{
    juce::Path path;
    path.preallocateSpace ((shape.facets + 2) * 3);
    bool drawing = false;
    for (int k = 0; k <= shape.facets; ++k)
    {
        if (shape.kept[(size_t) k] != passBand)
        {
            drawing = false;
            continue;
        }
        const auto rr = radius * shape.scale[(size_t) k];
        const juce::Point<float> p (centre.x + shape.dx[(size_t) k] * rr, centre.y + shape.dy[(size_t) k] * rr);
        if (! drawing) { path.startNewSubPath (p); drawing = true; any = true; }
        else           path.lineTo (p);
    }
    return path;
}

//==============================================================================
void FxView::paint (juce::Graphics& g)
{
    fillBackdrop (g, *this, getLocalBounds().toFloat(), Backdrop::deep);

    // Header: the chain at a glance.
    {
        auto h = header.reduced (12.0f, 0.0f);
        drawText (g, "FX", h.removeFromLeft (40.0f), TextStyle::sectionLabel, Colours::text);

        juce::String readout;
        readout << "DELAY " << percent (s.space) << " " << PingPongDelay::Time::names[s.delayTime] << (s.pingPong ? " PING-PONG" : " STEREO")
                << "   DISTORTION " << dirtModes[s.dirtMode] << " " << percent (s.dirt)
                << "   FILTER " << (s.filter < -0.005f ? "LP " + hzShort (ToneProcessor::lowPassCutoff (s.filter))
                                                      : (s.filter > 0.005f ? "HP " + hzShort (ToneProcessor::highPassCutoff (s.filter)) : juce::String ("OFF")))
                << "   WOBBLE " << wobbleModes[s.wobbleMode] << " " << percent (s.wobble)
                << "   PUMP " << pumpModes[s.pumpMode] << " " << percent (s.pump);
        drawText (g, readout, h, TextStyle::micro, Colours::textFaint, juce::Justification::centredRight);
    }

    const ScreenScope screenColours; // the screen: a dark display in every theme (the frame round it is not)
    paintTunnel (g, tunnelScreen);

    if (intro < 1.0f)
        screen::powerOn (g, tunnelScreen, intro * 1.3f);
}

void FxView::paintTunnel (juce::Graphics& g, juce::Rectangle<float> area)
{
    screen::paintScreen (g, area, "WORMHOLE", 0.6f, this);
    juce::Graphics::ScopedSaveState save (g);
    g.reduceClipRegion (area.reduced (1.0f).toNearestInt());

    // A dark tunnel lit from its far end (a dark display in every theme): the near rings in the
    // theme colour, turning into the second colour into the distance.
    const auto second = nebulaGlow() != juce::Colour() ? nebulaGlow() : signal().withRotatedHue (0.12f);
    const auto nearColour = signal().brighter (0.15f);
    const auto warm = s.dirtMode == 0 ? s.dirt : 0.0f;
    const auto end = centreAt (1.0f);
    const auto hole = radiusAt (1.0f);

    // Space round the tunnel: darker towards the edges (made once per size, then reused).
    if (vignette.getWidth() != area.getWidth() || vignette.getHeight() != area.getHeight())
    {
        vignette = juce::Image (juce::Image::ARGB, std::max (1, (int) area.getWidth()), std::max (1, (int) area.getHeight()), true);
        juce::Graphics v (vignette);
        const auto centre = vignette.getBounds().toFloat().getCentre();
        v.setGradientFill (juce::ColourGradient (juce::Colours::black.withAlpha (0.0f), centre, juce::Colours::black.withAlpha (0.6f),
                                                 centre.translated ((float) vignette.getWidth() * 0.6f, 0.0f), true));
        v.fillAll();
    }
    g.setOpacity (1.0f);
    g.drawImage (vignette, area);

    // Stars streaking past (warp): out from the far end along the tunnel walls. In three
    // brightnesses, each drawn in one go.
    {
        std::array<juce::Path, 3> streaks;
        for (const auto& star : stars)
        {
            const auto d0 = star.depth;
            const auto alpha = (1.0f - d0) * 0.55f * juce::jlimit (0.0f, 1.0f, (d0 - 0.04f) / 0.12f);
            if (alpha < 0.03f)
                continue;
            const juce::Point<float> dir (std::sin (star.angle), std::cos (star.angle));
            const auto d1 = std::min (1.0f, d0 + 0.03f + speed * 0.25f);
            const auto p0 = centreAt (d0) + dir * radiusAt (d0) * 1.15f;
            auto p1 = centreAt (d1) + dir * radiusAt (d1) * 1.15f;
            if (p1.getDistanceFrom (p0) > 90.0f) // close by: a short streak, not a line across the screen
                p1 = p0 + (p1 - p0) * (90.0f / p1.getDistanceFrom (p0));
            streaks[(size_t) juce::jlimit (0, 2, (int) (alpha / 0.55f * 3.0f))].addLineSegment ({ p1, p0 }, star.size * (0.4f + (1.0f - d0)));
        }
        for (size_t b = 0; b < streaks.size(); ++b)
        {
            g.setColour (juce::Colours::white.withAlpha (0.12f + 0.16f * (float) b));
            g.fillPath (streaks[b]);
        }
    }

    // The light at the far end: pulsing with the sound, flashing with the PUMP.
    {
        const auto glow = std::min (hole * (2.5f + 6.0f * std::min (1.0f, level) + 5.0f * flash), area.getHeight() * 0.45f);
        g.setGradientFill (juce::ColourGradient (juce::Colours::white.withAlpha (0.9f), end, signal().withAlpha (0.0f), end.translated (glow, 0.0f), true));
        g.fillEllipse (juce::Rectangle<float> (glow * 2.0f, glow * 2.0f).withCentre (end));
        g.setGradientFill (juce::ColourGradient (signal().withAlpha (0.22f + 0.3f * flash), end, signal().withAlpha (0.0f),
                                                 end.translated (glow * 2.4f, 0.0f), true));
        g.fillEllipse (juce::Rectangle<float> (glow * 4.8f, glow * 4.8f).withCentre (end));
    }

    // A ring's line: in the middle distance with a soft halo, near and far plain.
    auto line = [&g] (const juce::Path& p, juce::Colour c, float width, float halo)
    {
        if (halo > 0.01f)
        {
            g.setColour (c.withMultipliedAlpha (0.2f * halo));
            g.strokePath (p, juce::PathStrokeType (width * 3.5f, juce::PathStrokeType::beveled, juce::PathStrokeType::butt));
        }
        g.setColour (c);
        g.strokePath (p, juce::PathStrokeType (width));
    };

    // The rings, far to near (so the near ones lie on top; the list runs near to far).
    const auto echoes = s.space > 0.01f ? 1 + juce::jlimit (0, 1, (int) (s.feedback * 2.5f)) : 0;
    const auto chorus = s.wobbleMode == 1 && s.wobble > 0.01f;
    const auto filtered = std::abs (s.filter) > 0.005f;
    for (auto it = rings.rbegin(); it != rings.rend(); ++it)
    {
        const auto& ring = *it;
        const auto d = ring.depth;
        if (d < -0.02f || d > 1.0f)
            continue;
        const auto fadeIn = juce::jlimit (0.0f, 1.0f, (1.0f - d) / 0.15f), fadeOut = juce::jlimit (0.0f, 1.0f, (d + 0.02f) / 0.1f);
        const auto alpha = (0.2f + 0.8f * (1.0f - d)) * fadeIn * fadeOut;
        if (alpha < 0.02f)
            continue;
        const auto colour = nearColour.interpolatedWith (second, juce::jlimit (0.0f, 1.0f, d * 1.2f)).interpolatedWith (hot (0.6f), warm * 0.6f);
        const auto width = (0.6f + 2.2f * (1.0f - d)) * (1.0f + 1.5f * warm);
        const auto halo = d > 0.05f && d < 0.5f ? (0.6f * (1.0f + 1.2f * warm) + 0.4f * flash) : 0.0f;
        const auto shape = shapeOf (ring, d);

        // DELAY: ghost rings behind this one, each fainter: thrown left and right (PING-PONG)
        // or straight behind it (STEREO); a shorter note value keeps them closer.
        if (echoes > 0 && d < 0.6f)
            for (int k = 1; k <= echoes; ++k)
            {
                const auto spacing = (float) juce::jlimit (0.4, 2.0, PingPongDelay::Time::beats[s.delayTime] / 0.75);
                const auto gd = d + (float) k * (0.03f + 0.04f * s.space) * spacing;
                const auto side = s.pingPong ? (k % 2 == 1 ? 1.0f : -1.0f) : 0.0f;
                const juce::Point<float> off (side * area.getWidth() * (0.025f + 0.03f * s.space) * (float) k * (1.0f - gd * 0.6f), 0.0f);
                bool any = false;
                const auto ghost = pathOf (shape, centreAt (gd) + off, radiusAt (gd), true, any);
                if (any)
                {
                    g.setColour (second.withAlpha (alpha * 0.55f * std::pow (std::max (0.2f, s.feedback), (float) k) * std::min (1.0f, s.space * 3.0f)));
                    g.strokePath (ghost, juce::PathStrokeType (std::max (0.6f, width * 0.6f)));
                }
            }

        // The ring: what the FILTER lets through bright, what it removes faint.
        const auto c = centreAt (d);
        const auto r = radiusAt (d);
        if (filtered)
        {
            bool any = false;
            const auto removed = pathOf (shape, c, r, false, any);
            if (any)
            {
                g.setColour (colour.withAlpha (alpha * 0.12f));
                g.strokePath (removed, juce::PathStrokeType (width * 0.7f));
            }
        }
        bool any = false;
        const auto outline = pathOf (shape, c, r, true, any);
        if (any)
        {
            line (outline, colour.withAlpha (alpha), width, halo);
            if (chorus) // CHORUS: every ring doubled, a little apart
            {
                bool anyTwin = false;
                const auto twin = pathOf (shape, centreAt (d + 0.012f) + juce::Point<float> (area.getWidth() * 0.008f * s.wobble, 0.0f),
                                          radiusAt (d + 0.012f), true, anyTwin);
                if (anyTwin)
                {
                    g.setColour (second.withAlpha (alpha * 0.45f * s.wobble));
                    g.strokePath (twin, juce::PathStrokeType (width * 0.7f));
                }
            }
        }
    }

    // FILTER: a string of lights down the tunnel at the cutoff, on both walls (resonance:
    // brighter and bigger).
    if (filtered)
    {
        const auto railColour = hot (0.5f);
        const auto cutU = s.filter < 0.0f ? freqToU (ToneProcessor::lowPassCutoff (s.filter)) : freqToU (ToneProcessor::highPassCutoff (s.filter));
        for (const auto& ring : rings)
        {
            const auto d = ring.depth;
            if (d < 0.06f || d > 0.95f)
                continue;
            const auto alpha = (1.0f - d) * juce::jlimit (0.0f, 1.0f, (d - 0.06f) / 0.1f) * (0.6f + 0.4f * s.resonance);
            const auto size = (1.5f + 5.0f * (1.0f - d)) * (1.0f + 0.8f * s.resonance);
            const auto c = centreAt (d);
            const auto r = radiusAt (d) * (0.7f + 0.42f * heightAt (ring, cutU));
            for (const auto a : anglesOf (cutU))
            {
                const juce::Point<float> p (c.x + std::sin (a) * r, c.y + std::cos (a) * r);
                g.setColour (railColour.withAlpha (alpha * 0.25f));
                g.fillEllipse (juce::Rectangle<float> (size * 3.0f, size * 3.0f).withCentre (p));
                g.setColour (railColour.withAlpha (alpha));
                g.fillEllipse (juce::Rectangle<float> (size, size).withCentre (p));
            }
        }
    }

    // FUZZ sparks (two brightnesses, each in one go).
    if (! sparks.empty())
    {
        std::array<juce::Path, 2> lines;
        for (const auto& sp : sparks)
        {
            const auto t = sp.age / sp.life;
            lines[t < 0.5f ? 1 : 0].addLineSegment ({ sp.pos, sp.pos - sp.velocity * 0.02f }, 1.4f * (1.0f - t) + 0.4f);
        }
        g.setColour (hot (0.7f).withAlpha (0.35f));
        g.fillPath (lines[0]);
        g.setColour (hot (0.7f).withAlpha (0.85f));
        g.fillPath (lines[1]);
    }

    screen::scanlines (g, area, 0.05f);
}

} // namespace kerf::ui

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

    /** Round a ring: angle 0 at the bottom (the lows), pi at the top (the highs). */
    float uOfAngle (float a) { return a <= pi ? a / pi : (twoPi - a) / pi; }

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
    const auto delay = PingPongDelay::fromMacro (st.space, processor.getEffectiveTempo());
    st.delaySeconds = std::max (0.05, delay.delaySeconds);
    st.feedback = delay.feedback;
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

juce::Path FxView::ringPath (const Ring& ring, float depth, juce::Point<float> offset, bool passBand, bool& any) const
{
    // The ring's outline: the spectrum round it (lows at the bottom). DIRT: CRUSH breaks it into
    // a polygon, FUZZ makes it crackle. FILTER: `passBand` keeps what the filter lets through
    // (or, false, what it removes) - drawn bright and dark.
    const auto c = centreAt (depth) + offset;
    const auto r = radiusAt (depth);
    const auto crush = s.dirtMode == 2 && s.dirt > 0.01f;
    const auto facets = crush ? std::max (5, juce::roundToInt (40.0f - 34.0f * s.dirt)) : points;
    const auto fuzz = s.dirtMode == 1 ? s.dirt : 0.0f;
    const auto cutU = s.filter < -0.005f ? freqToU (ToneProcessor::lowPassCutoff (s.filter))
                    : (s.filter > 0.005f ? freqToU (ToneProcessor::highPassCutoff (s.filter)) : -1.0f);
    auto kept = [this, cutU] (float u) { return cutU < 0.0f || (s.filter < 0.0f ? u <= cutU : u >= cutU); };

    juce::Path path;
    bool drawing = false;
    auto& rng = const_cast<juce::Random&> (random); // (the crackle is new every frame)
    for (int k = 0; k <= facets; ++k)
    {
        const auto a = twoPi * (float) k / (float) facets;
        const auto u = uOfAngle (std::fmod (a, twoPi));
        const auto inBand = kept (u) == passBand;
        if (! inBand)
        {
            drawing = false;
            continue;
        }
        auto h = heightAt (ring, u);
        if (fuzz > 0.0f && depth < 0.5f)
            h += (rng.nextFloat() - 0.5f) * 0.25f * fuzz;
        const auto rr = r * (0.7f + 0.42f * h);
        const juce::Point<float> p (c.x + std::sin (a) * rr, c.y + std::cos (a) * rr);
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
        readout << "ECHO " << percent (s.space)
                << "   DIRT " << dirtModes[s.dirtMode] << " " << percent (s.dirt)
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

    // Colours. At night: a dark tunnel lit from its far end, the near rings in the theme
    // colour turning into the second colour into the distance. In white mode: a bright tunnel
    // (the light at the end white), the rings in the deep theme colour, paling into the distance.
    const auto light = lightTheme();
    const auto second = nebulaGlow() != juce::Colour() ? nebulaGlow() : signal().withRotatedHue (0.12f);
    const auto nearColour = light ? signal().darker (0.25f) : signal().brighter (0.15f);
    const auto farColour = light ? signal().interpolatedWith (second, 0.45f) : second;
    const auto ghostColour = light ? second.darker (0.25f) : second;
    const auto sparkColour = light ? signal().darker (0.4f) : hot (0.7f);
    const auto railColour = light ? signal().darker (0.45f) : hot (0.5f);
    const auto starColour = light ? signal().darker (0.2f) : juce::Colours::white;
    const auto glowScale = light ? 0.35f : 1.0f; // (dark halos on white look like smudges)
    const auto warm = s.dirtMode == 0 ? s.dirt : 0.0f;
    const auto end = centreAt (1.0f);
    const auto hole = radiusAt (1.0f);

    // Space round the tunnel: at night darker towards the edges; on white, a soft light from
    // the far end.
    if (! light)
    {
        g.setGradientFill (juce::ColourGradient (juce::Colours::black.withAlpha (0.0f), end, juce::Colours::black.withAlpha (0.6f),
                                                 end.translated (area.getWidth() * 0.6f, 0.0f), true));
        g.fillRect (area);
    }

    // Stars streaking past (warp): out from the far end along the tunnel walls.
    for (const auto& star : stars)
    {
        const juce::Point<float> dir (std::sin (star.angle), std::cos (star.angle));
        const auto d0 = star.depth, d1 = std::min (1.0f, star.depth + 0.03f + speed * 0.25f);
        const auto p0 = centreAt (d0) + dir * radiusAt (d0) * 1.15f;
        auto p1 = centreAt (d1) + dir * radiusAt (d1) * 1.15f;
        if (p1.getDistanceFrom (p0) > 90.0f) // close by: a short streak, not a line across the screen
            p1 = p0 + (p1 - p0) * (90.0f / p1.getDistanceFrom (p0));
        const auto alpha = (1.0f - d0) * (light ? 0.35f : 0.55f) * juce::jlimit (0.0f, 1.0f, (d0 - 0.04f) / 0.12f);
        g.setColour (starColour.withAlpha (alpha));
        g.drawLine ({ p1, p0 }, star.size * (0.4f + (1.0f - d0)));
    }

    // The light at the far end: pulsing with the sound, flashing with the PUMP.
    {
        const auto glow = hole * (2.5f + 6.0f * std::min (1.0f, level) + 5.0f * flash);
        g.setGradientFill (juce::ColourGradient (juce::Colours::white.withAlpha (light ? 1.0f : 0.9f), end,
                                                 (light ? juce::Colours::white : signal()).withAlpha (0.0f), end.translated (glow, 0.0f), true));
        g.fillEllipse (juce::Rectangle<float> (glow * 2.0f, glow * 2.0f).withCentre (end));
        if (! light)
        {
            g.setGradientFill (juce::ColourGradient (signal().withAlpha (0.22f + 0.3f * flash), end, signal().withAlpha (0.0f),
                                                     end.translated (glow * 2.4f, 0.0f), true));
            g.fillEllipse (juce::Rectangle<float> (glow * 4.8f, glow * 4.8f).withCentre (end));
        }
        else // a halo of light round the hole (white mode)
        {
            g.setGradientFill (juce::ColourGradient (juce::Colours::white.withAlpha (0.7f), end, juce::Colours::white.withAlpha (0.0f),
                                                     end.translated (glow * 3.0f, 0.0f), true));
            g.fillEllipse (juce::Rectangle<float> (glow * 6.0f, glow * 6.0f).withCentre (end));
        }
    }

    // The rings, far to near (so the near ones lie on top).
    auto sorted = rings;
    std::sort (sorted.begin(), sorted.end(), [] (const Ring& a, const Ring& b) { return a.depth > b.depth; });
    const auto echoes = s.space > 0.01f ? 1 + juce::jlimit (0, 2, (int) (s.feedback * 3.5f)) : 0;
    const auto chorus = s.wobbleMode == 1 && s.wobble > 0.01f;
    for (const auto& ring : sorted)
    {
        const auto d = ring.depth;
        if (d < -0.02f || d > 1.0f)
            continue;
        const auto fadeIn = juce::jlimit (0.0f, 1.0f, (1.0f - d) / 0.15f), fadeOut = juce::jlimit (0.0f, 1.0f, (d + 0.02f) / 0.1f);
        const auto alpha = (light ? 0.12f + 0.88f * (1.0f - d) : 0.2f + 0.8f * (1.0f - d)) * fadeIn * fadeOut;
        const auto colour = nearColour.interpolatedWith (farColour, juce::jlimit (0.0f, 1.0f, d * 1.2f))
                                      .interpolatedWith (light ? sparkColour : hot (0.6f), warm * 0.6f);
        const auto width = (0.6f + 2.2f * (1.0f - d)) * (1.0f + 1.5f * warm);
        const auto glowAmount = ((d < 0.5f ? 0.6f : 0.25f) * (1.0f + 1.2f * warm) + 0.4f * flash) * glowScale;

        // ECHO: ghost rings behind this one, thrown left and right, each fainter.
        if (echoes > 0 && d < 0.85f)
            for (int k = 1; k <= echoes; ++k)
            {
                const auto gd = d + (float) k * (0.03f + 0.04f * s.space);
                const juce::Point<float> off ((k % 2 == 1 ? 1.0f : -1.0f) * area.getWidth() * (0.025f + 0.03f * s.space) * (float) k * (1.0f - gd * 0.6f), 0.0f);
                bool any = false;
                const auto ghost = ringPath (ring, gd, off, true, any);
                if (any)
                {
                    g.setColour (ghostColour.withAlpha (alpha * 0.55f * std::pow (std::max (0.2f, s.feedback), (float) k) * std::min (1.0f, s.space * 3.0f)));
                    g.strokePath (ghost, juce::PathStrokeType (std::max (0.6f, width * 0.6f)));
                }
            }

        // The ring: what the FILTER lets through bright, what it removes faint.
        for (const auto pass : { false, true })
        {
            bool any = false;
            const auto outline = ringPath (ring, d, {}, pass, any);
            if (! any)
                continue;
            if (! pass)
            {
                g.setColour (colour.withAlpha (alpha * 0.12f));
                g.strokePath (outline, juce::PathStrokeType (width * 0.7f));
                continue;
            }
            if (d < 0.6f && glowAmount > 0.01f)
                glowStroke (g, outline, colour.withAlpha (alpha), width, glowAmount);
            else
            {
                g.setColour (colour.withAlpha (alpha));
                g.strokePath (outline, juce::PathStrokeType (width));
            }
            if (chorus) // CHORUS: every ring doubled, a little apart
            {
                bool anyTwin = false;
                const auto twin = ringPath (ring, d + 0.012f, { area.getWidth() * 0.008f * s.wobble, 0.0f }, true, anyTwin);
                if (anyTwin)
                {
                    g.setColour (ghostColour.withAlpha (alpha * 0.45f * s.wobble));
                    g.strokePath (twin, juce::PathStrokeType (width * 0.7f));
                }
            }
        }
    }

    // FILTER: a string of lights down the tunnel at the cutoff, on both walls (resonance:
    // brighter and bigger).
    if (std::abs (s.filter) > 0.005f)
    {
        const auto cutU = s.filter < 0.0f ? freqToU (ToneProcessor::lowPassCutoff (s.filter)) : freqToU (ToneProcessor::highPassCutoff (s.filter));
        for (const auto& ring : sorted)
        {
            const auto d = ring.depth;
            if (d < 0.06f || d > 0.95f)
                continue;
            const auto alpha = (1.0f - d) * juce::jlimit (0.0f, 1.0f, (d - 0.06f) / 0.1f) * (0.6f + 0.4f * s.resonance);
            const auto size = (1.5f + 5.0f * (1.0f - d)) * (1.0f + 0.8f * s.resonance);
            for (const auto a : { cutU * pi, twoPi - cutU * pi })
            {
                const auto c = centreAt (d);
                const auto r = radiusAt (d) * (0.7f + 0.42f * heightAt (ring, cutU));
                const juce::Point<float> p (c.x + std::sin (a) * r, c.y + std::cos (a) * r);
                if (! light)
                {
                    g.setColour (railColour.withAlpha (alpha * 0.25f));
                    g.fillEllipse (juce::Rectangle<float> (size * 3.0f, size * 3.0f).withCentre (p));
                }
                g.setColour (railColour.withAlpha (alpha));
                g.fillEllipse (juce::Rectangle<float> (size, size).withCentre (p));
            }
        }
    }

    // FUZZ sparks.
    for (const auto& sp : sparks)
    {
        const auto t = sp.age / sp.life;
        g.setColour (sparkColour.withAlpha ((1.0f - t) * 0.9f));
        g.drawLine ({ sp.pos, sp.pos - sp.velocity * 0.02f }, 1.4f * (1.0f - t) + 0.4f);
    }

    screen::scanlines (g, area, 0.05f);
}

} // namespace kerf::ui

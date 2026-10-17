#include "GrainView.h"
#include "ScreenStyle.h"
#include <cstring>

namespace kerf::ui
{

juce::String noteName (int midiNote);

namespace
{
    // Shared synth-display style (ScreenStyle.h).
    using screen::signal;
    using screen::hot;
    using screen::hudText;
    using screen::glowStroke;
}

GrainView::GrainView (KerfAudioProcessor& p, AnimationClock& c) : processor (p), clock (c)
{
    clock.add (this);
    setInterceptsMouseClicks (false, false);
    spectroImage.clear (spectroImage.getBounds()); // see-through, like OSC and VECTOR
}

GrainView::~GrainView()
{
    clock.remove (this);
}

void GrainView::resized()
{
    auto r = getLocalBounds().toFloat();
    header = r.removeFromTop (26.0f);
    r.removeFromTop (2.0f);

    auto candy = r.removeFromRight (318.0f);
    r.removeFromRight (10.0f);
    engineScreen = r;

    oscScreen = candy.removeFromTop (candy.getHeight() * 0.44f);
    candy.removeFromTop (8.0f);
    const auto half = (candy.getWidth() - 8.0f) * 0.5f;
    vectorScreen = candy.removeFromLeft (half);
    candy.removeFromLeft (8.0f);
    spectroScreen = candy;
}

//==============================================================================
void GrainView::updateBaseline()
{
    // A faint "terrain" from the chop's own loudness, so the landscape is never flat.
    const auto& info = processor.getSampleInfo();
    const auto& chop = region;
    if (! chop.has_value() || info.sample == nullptr || info.waveform == nullptr)
    {
        baseline.fill (0.0f);
        baselineChopId = shownChopId;
        return;
    }

    if (baselineChopId == shownChopId && baselineStart == chop->start && baselineEnd == chop->end)
        return;

    baselineChopId = shownChopId;
    baselineStart = chop->start;
    baselineEnd = chop->end;

    const auto len = (double) std::max<int64_t> (1, chop->length());
    float peak = 1.0e-4f;
    for (int b = 0; b < bins; ++b)
    {
        const auto f0 = chop->start + (int64_t) (len * b / bins), f1 = chop->start + (int64_t) (len * (b + 1) / bins);
        baseline[(size_t) b] = info.waveform->getRange (info.sample->audio, f0, std::max (f0 + 1, f1)).rms;
        peak = std::max (peak, baseline[(size_t) b]);
    }
    for (auto& v : baseline)
        v /= peak;
}

void GrainView::animationTick (double dt)
{
    if (! isVisible())
        return;

    time += dt;
    if (intro < 1.0f)
        intro = std::min (1.0f, intro + (float) dt / 0.75f);
    const auto& voices = processor.getEngine().getVoiceDisplay();

    // Which chop: the loudest voice making grains, else the selected chop.
    int chopId = 0;
    float best = 0.0f;
    for (const auto& v : voices)
    {
        const auto id = v.chopId.load (std::memory_order_relaxed);
        const auto amount = v.grainAmount.load (std::memory_order_relaxed);
        if (id != 0 && amount > best && v.numGrains.load (std::memory_order_acquire) > 0)
        {
            best = amount;
            chopId = id;
        }
    }
    shownChopId = chopId != 0 ? chopId : (int) processor.getSettings().getProperty ("selectedChop", 0);

    // The region shown: that chop - or, while the whole sample plays (SAMPLE), the chop its
    // grains read from right now (the whole sample would squeeze them into a dot).
    region = processor.getChops().getChop (shownChopId);
    regionInSample = shownChopId == SamplerEngine::wholeSampleId;
    if (regionInSample)
    {
        double head = 0.0;
        for (const auto& v : voices)
            if (v.chopId.load (std::memory_order_relaxed) == SamplerEngine::wholeSampleId)
                head = v.numGrains.load (std::memory_order_acquire) > 0 ? v.sprayCentre.load (std::memory_order_relaxed) : v.position.load (std::memory_order_relaxed);
        if (const auto id = processor.getChops().chopIdAt ((int64_t) head))
            region = processor.getChops().getChop (id);
        else if (const auto& sample = processor.getSampleInfo().sample)
        {
            ChopParams whole;
            whole.id = SamplerEngine::wholeSampleId;
            whole.start = 0;
            whole.end = sample->getNumSamples();
            region = whole;
        }
    }
    updateBaseline();

    // --- Live grains and this frame's activity profile -----------------------------------
    live.clear();
    std::array<float, bins> frame {};
    const auto& chop = region;
    const auto sizeMs = processor.getParameters().getRawParameterValue (Params::grainSize)->load();
    float levelSum = 0.0f;

    if (chop.has_value() && chop->length() > 0)
    {
        const auto start = (double) chop->start, length = (double) chop->length();
        const auto rate = processor.getSampleInfo().sample != nullptr ? processor.getSampleInfo().sample->sampleRate : 48000.0;
        const auto sigma = juce::jlimit (1.0, 14.0, sizeMs * 0.001 * rate / length * bins * 0.5);

        for (const auto& v : voices)
        {
            if (v.chopId.load (std::memory_order_relaxed) != shownChopId)
                continue;

            const auto n = std::min (v.numGrains.load (std::memory_order_acquire), (int) v.grainPosition.size());
            if (n > 0)
            {
                sprayCentreU = (v.sprayCentre.load (std::memory_order_relaxed) - start) / length;
                sprayHalfU = v.sprayHalfWidth.load (std::memory_order_relaxed) / length;
            }

            for (int i = 0; i < n; ++i)
            {
                const auto u = (float) ((v.grainPosition[(size_t) i].load (std::memory_order_relaxed) - start) / length);
                const auto level = juce::jlimit (0.0f, 1.0f, v.grainLevel[(size_t) i].load (std::memory_order_relaxed));
                live.push_back ({ u, level, v.grainPitch[(size_t) i].load (std::memory_order_relaxed) });
                levelSum += level;

                const auto centre = u * (float) (bins - 1);
                for (int b = std::max (0, (int) (centre - 3.0 * sigma)); b <= std::min (bins - 1, (int) (centre + 3.0 * sigma)); ++b)
                {
                    const auto d = ((float) b - centre) / (float) sigma;
                    frame[(size_t) b] += level * std::exp (-d * d);
                }
            }
        }
    }

    // When nothing plays, follow the parameters so the read head still shows where grains would start.
    if (live.empty())
    {
        auto value = [this] (const char* id) { return processor.getParameters().getRawParameterValue (id)->load(); };
        sprayCentreU = value (Params::grainPosition);
        sprayHalfU = value (Params::grainSpray) * 0.5;
    }

    bool settling = false;
    for (int b = 0; b < bins; ++b)
    {
        const auto target = std::tanh (frame[(size_t) b] * 0.9f) + 0.16f * baseline[(size_t) b];
        settling = settling || std::abs (target - profile[(size_t) b]) > 0.002f;
        profile[(size_t) b] += (target - profile[(size_t) b]) * 0.55f;
    }

    // The galaxy: the grains shed sparks that drift out from the disc and fade; the stars and
    // the core's arms turn while it sounds (FREEZE: everything holds still).
    const auto frozen = processor.getParameters().getRawParameterValue (Params::grainFreeze)->load() > 0.5f;
    const auto step = (float) std::min (dt, 0.05);
    if (! frozen)
    {
        for (auto& p : particles)
        {
            p.age += step;
            p.radius += p.drift * step;
            p.height += p.rise * step;
            p.u += 0.02f * step / std::max (0.3f, p.radius); // the inner ones round faster
            p.u -= std::floor (p.u);
        }
        particles.erase (std::remove_if (particles.begin(), particles.end(), [] (const Particle& p) { return p.age >= p.life; }), particles.end());
        int born = 0;
        const auto sounding = processor.isSounding(); // (a stopped host leaves its last grains standing: no sparks from them)
        for (const auto& gr : live)
            if (sounding && gr.u >= 0.0f && gr.u <= 1.0f && particles.size() < 420 && born < 28 && sparkRandom.nextFloat() < 0.35f + 0.6f * gr.level)
            {
                Particle p;
                p.u = gr.u;
                p.radius = 1.0f;
                p.height = gr.semis;
                p.drift = (sparkRandom.nextBool() ? 1.0f : -0.55f) * (0.08f + 0.35f * sparkRandom.nextFloat());
                p.rise = (sparkRandom.nextFloat() - 0.5f) * 10.0f;
                p.life = 0.6f + 1.4f * sparkRandom.nextFloat();
                p.size = 1.0f + 2.2f * gr.level * sparkRandom.nextFloat();
                p.hue = juce::jlimit (0.0f, 1.0f, 0.5f + gr.semis / 48.0f + (sparkRandom.nextFloat() - 0.5f) * 0.3f);
                particles.push_back (p);
                ++born;
            }
        if (processor.isSounding() || ! live.empty())
        {
            starSpin += step * 0.015f;
            swirl += step * (0.4f + 1.6f * glow);
        }

        // Shooting stars now and then while it sounds; a shockwave over the disc when the
        // grains jump in loudness (a note, a hit), with a burst of sparks.
        for (auto& m : meteors) m.age += step;
        meteors.erase (std::remove_if (meteors.begin(), meteors.end(), [] (const Meteor& m) { return m.age >= m.life; }), meteors.end());
        for (auto& w : shockwaves) w.age += step;
        shockwaves.erase (std::remove_if (shockwaves.begin(), shockwaves.end(), [] (const Shockwave& w) { return w.age >= 1.4f; }), shockwaves.end());
        if (sounding)
        {
            meteorClock -= step * (0.6f + 1.5f * glow);
            if (meteorClock <= 0.0f)
            {
                meteorClock = 0.7f + 2.2f * sparkRandom.nextFloat();
                const auto left = sparkRandom.nextBool();
                Meteor m;
                m.from = { left ? 0.05f + 0.5f * sparkRandom.nextFloat() : 0.45f + 0.5f * sparkRandom.nextFloat(), 0.05f + 0.3f * sparkRandom.nextFloat() };
                m.velocity = { (left ? 1.0f : -1.0f) * (0.5f + 0.5f * sparkRandom.nextFloat()), 0.25f + 0.35f * sparkRandom.nextFloat() };
                m.life = 0.5f + 0.5f * sparkRandom.nextFloat();
                meteors.push_back (m);
            }
            const auto level = juce::jlimit (0.0f, 1.0f, levelSum / 5.0f);
            if (level - lastLevel > 0.15f && (shockwaves.empty() || shockwaves.back().age > 0.15f) && shockwaves.size() < 6)
            {
                shockwaves.push_back ({ 0.0f, juce::jlimit (0.3f, 1.0f, (level - lastLevel) * 3.0f) });
                for (int k = 0; k < 24 && particles.size() < 460; ++k)
                {
                    Particle p;
                    p.u = sparkRandom.nextFloat();
                    p.radius = 0.35f + 0.2f * sparkRandom.nextFloat();
                    p.drift = 0.6f + 0.9f * sparkRandom.nextFloat();
                    p.rise = (sparkRandom.nextFloat() - 0.5f) * 6.0f;
                    p.life = 0.5f + 0.7f * sparkRandom.nextFloat();
                    p.size = 1.2f + 1.8f * sparkRandom.nextFloat();
                    p.hue = sparkRandom.nextFloat();
                    particles.push_back (p);
                }
            }
        }
    }
    lastLevel = juce::jlimit (0.0f, 1.0f, levelSum / 5.0f);

    const auto targetGlow = juce::jlimit (0.0f, 1.0f, levelSum / 5.0f);
    glow += (targetGlow - glow) * std::min (1.0f, (float) dt * (targetGlow > glow ? 12.0f : 2.0f));

    // --- Candy ---------------------------------------------------------------------------
    freshFrames = processor.getOutputTap().pull();
    updateVectorscope();
    updateSpectrogram();

    // At rest (silence, the landscape settled, nothing changed): no repaint. Grains only
    // count while audio runs: a paused host leaves the last ones standing.
    const auto active = settling || intro < 1.0f || processor.isSounding() || ((! particles.empty() || ! meteors.empty() || ! shockwaves.empty()) && ! frozen);
    if (idle.update (dt, active, processor.getParameterChangeCount() + (uint32_t) shownChopId))
        repaint();
}

void GrainView::updateVectorscope()
{
    auto& tap = processor.getOutputTap();
    // Always show ~43 ms (2048 samples): enough for a full cycle of low notes.
    const auto count = juce::jlimit (2048, 4000, freshFrames + 64);

    // Auto-gain: fast when it gets louder (no clipping), slow when quieter (no pumping).
    float peak = 0.0f;
    double sumLR = 0.0, sumLL = 0.0, sumRR = 0.0;
    for (int i = 0; i < count; ++i)
    {
        const auto l = tap.getLeft (i), r = tap.getRight (i);
        peak = std::max ({ peak, std::abs (l), std::abs (r) });
        sumLR += (double) l * r;
        sumLL += (double) l * l;
        sumRR += (double) r * r;
    }
    const auto targetGain = juce::jlimit (1.0f, 60.0f, 0.75f / std::max (0.004f, peak));
    vectorGain += (targetGain - vectorGain) * (targetGain < vectorGain ? 0.6f : 0.12f);

    const auto denom = std::sqrt (sumLL * sumRR);
    const auto targetCorrelation = denom > 1.0e-9 ? (float) (sumLR / denom) : 0.0f;
    correlation += (targetCorrelation - correlation) * 0.15f;

    // Store this frame's trace (decimated to ~1000 points). Painting draws the last few traces
    // with decreasing brightness: a phosphor afterglow that stays sharp at any UI size.
    std::vector<juce::Point<float>> trace;
    const auto step = std::max (1, count / 1000);
    trace.reserve ((size_t) (count / step + 1));
    for (int i = count - 1; i >= 0; i -= step)
    {
        const auto l = tap.getLeft (i) * vectorGain, r = tap.getRight (i) * vectorGain;
        auto side = (l - r) * 0.7071f, mid = (l + r) * 0.7071f; // rotated 45 degrees: mono = vertical
        // Radial compression keeps quiet detail visible without distorting shapes.
        if (const auto radius = std::hypot (side, mid); radius > 1.0e-6f)
        {
            const auto f = std::pow (std::min (radius, 1.3f), 0.75f) / radius;
            side *= f;
            mid *= f;
        }
        trace.push_back ({ side, mid });
    }

    vectorTraces.push_back (std::move (trace));
    while (vectorTraces.size() > 6)
        vectorTraces.pop_front();

    vectorSilent = peak < 1.0e-5f;
}

void GrainView::updateSpectrogram()
{
    const auto w = spectroImage.getWidth(), h = spectroImage.getHeight();
    if (spectroLevels.size() != (size_t) (w * h))
        spectroLevels.assign ((size_t) (w * h), 0);
    if (spectroAccent != signal() || spectroBack != screen::bg()) // a new theme: the history goes on, in its colours
    {
        spectroAccent = signal();
        spectroBack = screen::bg();
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x)
                spectroImage.setPixelAt (x, y, spectroColour ((float) spectroLevels[(size_t) (y * w + x)] / 255.0f));
    }
    spectroImage.moveImageSection (0, 0, 1, 0, w - 1, h); // scroll left by one column
    for (int y = 0; y < h; ++y)
        std::memmove (&spectroLevels[(size_t) (y * w)], &spectroLevels[(size_t) (y * w + 1)], (size_t) (w - 1));

    const auto& bands = processor.getSpectrum().getBands();
    for (int y = 0; y < h; ++y)
    {
        const auto band = juce::jlimit (0, SpectrumAnalyzer::numBands - 1, (h - 1 - y) * SpectrumAnalyzer::numBands / h);
        const auto intensity = std::pow (juce::jlimit (0.0f, 1.0f, juce::jmap (bands[(size_t) band], -78.0f, -6.0f, 0.0f, 1.0f)), 1.6f);
        spectroLevels[(size_t) (y * w + w - 1)] = (uint8_t) juce::roundToInt (intensity * 255.0f);
        spectroImage.setPixelAt (w - 1, y, spectroColour (intensity));
    }
}

juce::Colour GrainView::spectroColour (float intensity)
{
    // The theme colour, as strong as the band is loud, over nothing (the screen shows through).
    auto colour = signal().withAlpha (intensity);
    if (intensity > 0.82f)
        colour = screen::heat (colour, (intensity - 0.82f) * 2.5f);
    return colour;
}

//==============================================================================
void GrainView::paintScreen (juce::Graphics& g, juce::Rectangle<float> screen, const juce::String& label) const
{
    kerf::ui::screen::paintScreen (g, screen, label, 0.6f, this);
}

void GrainView::paint (juce::Graphics& g)
{
    fillBackdrop (g, *this, getLocalBounds().toFloat(), Backdrop::deep);

    auto value = [this] (const char* id) { return processor.getParameters().getRawParameterValue (id)->load(); };

    // --- Header ---------------------------------------------------------------------------
    {
        auto h = header.reduced (12.0f, 0.0f);
        drawText (g, "GRANULIZER", h.removeFromLeft (104.0f), TextStyle::sectionLabel, Colours::text);
        if (const auto& chop = region; chop.has_value())
        {
            int index = -1;
            const auto all = processor.getChops().getChops();
            for (size_t i = 0; i < all.size(); ++i)
                if (all[i].id == chop->id) index = (int) i;
            const auto label = index < 0 ? juce::String ("WHOLE SAMPLE")
                                         : (regionInSample ? "SAMPLE  CHOP " : "CHOP ") + juce::String (index + 1).paddedLeft ('0', 2) + "  " + noteName (chop->note);
            drawText (g, label, h.removeFromLeft (150.0f), TextStyle::micro, Colours::textDim);
        }

        // DENSITY as grains per second, or the note value with SYNC; the RANDOM scale; FREEZE.
        const auto synced = value (Params::grainSync) > 0.5f;
        auto* densityParam = processor.getParameters().getParameter (Params::grainDensity);
        const auto density = synced ? juce::String (GrainSync::names[GrainSync::indexFor (densityParam->convertTo0to1 (value (Params::grainDensity)))])
                                    : juce::String (juce::roundToInt (value (Params::grainDensity))) + "/S";
        static const char* scales[] = { "", " OCT", " 5TH", " CHORD" };
        const auto readout = (value (Params::grainFreeze) > 0.5f ? juce::String ("FROZEN   ") : juce::String())
                           + "DENSITY " + density + "   SIZE " + juce::String (juce::roundToInt (value (Params::grainSize))) + "MS   PITCH "
                           + juce::String::fromUTF8 ("\xc2\xb1") + juce::String (juce::roundToInt (value (Params::grainPitchRnd)))
                           + scales[juce::jlimit (0, 3, (int) value (Params::grainScale))] + "   " + juce::String ((int) live.size()) + " LIVE";
        drawText (g, readout, h, TextStyle::micro, Colours::textFaint, juce::Justification::centredRight);
    }

    paintEngine (g, engineScreen);
    paintOscilloscope (g, oscScreen);
    paintVectorscope (g, vectorScreen);
    paintSpectrogram (g, spectroScreen);

    // Opening animation: each screen switches on like a CRT, one after another. A bright
    // line in the middle opens up into the picture, then the glow fades.
    if (intro < 1.0f)
    {
        const juce::Rectangle<float> screens[] = { engineScreen, oscScreen, vectorScreen, spectroScreen };
        for (int i = 0; i < 4; ++i)
        {
            const auto s = screens[i].reduced (1.0f);
            const auto t = juce::jlimit (0.0f, 1.0f, (intro * 1.45f - 0.12f * (float) i) / 1.0f);
            const auto open = Motion::easeOut (t);
            const auto band = s.withSizeKeepingCentre (s.getWidth(), std::max (1.5f, s.getHeight() * open));

            g.setColour (screen::bg());
            g.fillRect (s.withBottom (band.getY()));
            g.fillRect (s.withTop (band.getBottom()));

            const auto glowAlpha = (1.0f - open) * 0.9f + 0.1f * (1.0f - t);
            g.setColour (signal().withAlpha (glowAlpha));
            g.fillRect (juce::Rectangle<float> (s.getX(), band.getY() - 0.5f, s.getWidth(), 1.5f));
            g.fillRect (juce::Rectangle<float> (s.getX(), band.getBottom() - 1.0f, s.getWidth(), 1.5f));
        }
    }
}

namespace
{
    // The galaxy's ink: light stars and words in deep space; in white mode a star chart on
    // paper, drawn in dark ink.
    juce::Colour spaceInk() { return lightTheme() ? Colours::text : juce::Colour (0xffe8e4f0); }

    void spaceText (juce::Graphics& g, const juce::String& text, juce::Rectangle<float> area, float alpha,
                    juce::Justification just = juce::Justification::centredLeft)
    {
        drawText (g, text, area, TextStyle::micro, spaceInk().withAlpha (alpha), just);
    }
}

void GrainView::buildSky (juce::Rectangle<int> size)
{
    // Deep space behind the galaxy: a dark gradient and soft nebula clouds in the theme's
    // two colours. Built once per size and theme; the stars are drawn live (they turn).
    // In white mode: light paper with the clouds a little stronger (they are pale on white).
    skyColour = signal();
    skyLight = lightTheme();
    skyImage = juce::Image (juce::Image::ARGB, std::max (1, size.getWidth()), std::max (1, size.getHeight()), true);
    juce::Graphics g (skyImage);
    const auto w = (float) size.getWidth(), h = (float) size.getHeight();
    const auto deep = skyLight ? Colours::well.brighter (0.03f) : juce::Colour (0xff05040a);
    g.setGradientFill (juce::ColourGradient (skyLight ? deep : deep.brighter (0.08f), w * 0.5f, h * 0.55f,
                                             skyLight ? deep.darker (0.06f) : deep, 0.0f, 0.0f, true));
    g.fillAll();
    const auto cloud = skyLight ? 2.2f : 1.0f;

    juce::Random rng (0x6b657266);
    const auto second = nebulaGlow() != juce::Colour() ? nebulaGlow() : signal().withRotatedHue (0.12f);
    for (int k = 0; k < 26; ++k)
    {
        const juce::Point<float> p (w * (0.05f + 0.9f * rng.nextFloat()), h * (0.1f + 0.8f * rng.nextFloat()));
        const auto r = (0.12f + 0.28f * rng.nextFloat()) * std::min (w, h);
        const auto c = (k % 3 == 0 ? second : signal()).withMultipliedSaturation (0.8f);
        juce::Graphics::ScopedSaveState flat (g);
        g.addTransform (juce::AffineTransform::scale (1.0f, 0.65f, p.x, p.y));
        g.setGradientFill (juce::ColourGradient (c.withAlpha ((0.05f + 0.06f * rng.nextFloat()) * cloud), p, c.withAlpha (0.0f), p.translated (r, 0.0f), true));
        g.fillEllipse (juce::Rectangle<float> (r * 2.0f, r * 2.0f).withCentre (p));
    }
    // Faint dust far away.
    for (int k = 0; k < 420; ++k)
    {
        g.setColour (spaceInk().withAlpha (0.05f + 0.12f * rng.nextFloat()));
        g.fillRect (w * rng.nextFloat(), h * rng.nextFloat(), 1.0f, 1.0f);
    }

    stars.clear();
    for (int k = 0; k < 140; ++k)
        stars.push_back ({ rng.nextFloat() * juce::MathConstants<float>::twoPi, 0.08f + 1.25f * std::sqrt (rng.nextFloat()),
                           0.6f + 1.4f * rng.nextFloat() * rng.nextFloat(), rng.nextFloat() * 6.28f });
}

void GrainView::paintEngine (juce::Graphics& g, juce::Rectangle<float> screen)
{
    paintScreen (g, screen, "GRANULIZER");
    const auto boost = lightTheme() ? 1.3f : 1.0f;
    auto A = [boost] (float a) { return std::min (1.0f, a * boost); };
    auto param = [this] (const char* id) { return processor.getParameters().getRawParameterValue (id)->load(); };
    const auto frozen = param (Params::grainFreeze) > 0.5f;
    const auto shape = param (Params::grainShape);
    constexpr auto twoPi = juce::MathConstants<float>::twoPi;
    juce::Graphics::ScopedSaveState save (g);
    const auto inner = screen.reduced (1.0f);
    g.reduceClipRegion (inner.toNearestInt());

    // --- Deep space ----------------------------------------------------------------------
    if (! skyImage.isValid() || skyImage.getBounds() != inner.toNearestInt().withZeroOrigin() || skyColour != signal() || skyLight != lightTheme())
        buildSky (inner.toNearestInt().withZeroOrigin());
    g.drawImageAt (skyImage, (int) inner.getX(), (int) inner.getY());

    const auto m = screen.reduced (16.0f, 14.0f).withTrimmedTop (14.0f);
    const auto c = screen.getCentre(); // the core in the middle
    const auto tilt = 0.36f;                                                   // the disc seen from a little above
    const auto R = std::min (m.getWidth() * 0.34f, m.getHeight() * 0.95f);    // the ring: the chop, start to end
    const auto lift = m.getHeight() * 0.011f;                                  // pixels of height per semitone
    const auto second = nebulaGlow() != juce::Colour() ? nebulaGlow() : signal().withRotatedHue (0.12f);
    const auto ice = signal().interpolatedWith (juce::Colour (0xffbfe6ff), 0.65f);
    const auto tone = frozen ? ice : signal();
    // White mode: dark ink, the white highlights in the deep accent, the tints stronger.
    const auto light = lightTheme();
    const auto glint = light ? tone.darker (0.45f) : juce::Colours::white;

    // Stars: a slow turn round the core, each twinkling on its own.
    for (const auto& s : stars)
    {
        const auto a = s.angle + starSpin;
        const juce::Point<float> p (c.x + std::cos (a) * s.radius * m.getWidth() * 0.62f,
                                    c.y + std::sin (a) * s.radius * m.getHeight() * 0.62f);
        const auto twinkle = 0.45f + 0.55f * (0.5f + 0.5f * std::sin ((float) time * 1.7f + s.twinkle));
        g.setColour (spaceInk().withAlpha (A (0.25f + 0.55f * twinkle * std::min (1.0f, s.size))));
        g.fillEllipse (juce::Rectangle<float> (s.size, s.size).withCentre (p));
        if (s.size > 1.6f)
        {
            g.setColour (spaceInk().withAlpha (A (0.12f * twinkle)));
            g.fillRect (juce::Rectangle<float> (s.size * 4.0f, 0.6f).withCentre (p));
            g.fillRect (juce::Rectangle<float> (0.6f, s.size * 4.0f).withCentre (p));
        }
        if (glow > 0.25f && ! light) // loud: the stars streak outwards (warp; at night)
        {
            const auto d = (p - c) / std::max (1.0f, p.getDistanceFrom (c));
            g.setColour (spaceInk().withAlpha (A (0.35f * (glow - 0.25f) * twinkle)));
            g.drawLine ({ p, p + d * (glow - 0.25f) * 22.0f * s.radius }, std::min (1.4f, s.size * 0.7f));
        }
    }

    // Shooting stars.
    for (const auto& meteor : meteors)
    {
        const auto t = meteor.age / meteor.life;
        const auto head = inner.getRelativePoint (meteor.from.x + meteor.velocity.x * meteor.age, meteor.from.y + meteor.velocity.y * meteor.age);
        const auto tail = head - juce::Point<float> (meteor.velocity.x * inner.getWidth(), meteor.velocity.y * inner.getHeight()) * 0.16f;
        const auto alpha = std::sin (t * juce::MathConstants<float>::pi);
        g.setGradientFill (juce::ColourGradient (spaceInk().withAlpha (A (0.9f * alpha)), head, tone.withAlpha (A (0.0f)), tail, false));
        g.drawLine ({ tail, head }, 1.6f);
        g.setColour (glint.withAlpha (A (alpha)));
        g.fillEllipse (juce::Rectangle<float> (2.6f, 2.6f).withCentre (head));
    }

    // A place on the disc: u along the chop (0 at the left, round through the back),
    // `radius` from the core, `height` above the disc (pitch).
    auto at = [&] (float u, float radius, float height)
    {
        const auto a = juce::MathConstants<float>::pi + u * twoPi;
        return juce::Point<float> (c.x + std::cos (a) * radius, c.y + std::sin (a) * radius * tilt - height);
    };
    auto isBack = [] (float u) { return std::sin (juce::MathConstants<float>::pi + u * twoPi) < 0.0f; };

    // The disc: the chop's loudness as a corona of light rays round the ring, flaring where
    // grains read. The back half is the first half of the chop (round the far side).
    auto reach = [&] (float u)
    {
        const auto x = juce::jlimit (0.0f, (float) bins - 1.001f, u * (float) bins - 0.5f);
        const auto i = (size_t) x;
        const auto f = x - (float) i;
        const auto mix = [&] (const std::array<float, bins>& a) { return a[i] + (a[std::min (i + 1, (size_t) bins - 1)] - a[i]) * f; };
        return 0.03f + 0.12f * mix (baseline) + 0.26f * std::min (1.2f, mix (profile));
    };
    auto corona = [&] (bool back)
    {
        constexpr int rays = 200;
        juce::Path edge;
        bool started = false;
        for (int k = 0; k <= rays / 2; ++k)
        {
            const auto u = (float) (back ? k : rays / 2 + k) / (float) rays;
            const auto r = reach (std::min (u, 0.9999f));
            const auto o = at (u, R * (1.0f + r), 0.0f);
            if (! started) { edge.startNewSubPath (o); started = true; } else edge.lineTo (o);
            if (k == rays / 2 && back)
                break;
            const auto flare = juce::jlimit (0.0f, 1.0f, r * 3.5f);
            const auto base = at (u, R * 0.985f, 0.0f);
            if (! light) // the rays' soft width: at night (in white mode it was a dense hatching)
            {
                g.setColour (tone.withAlpha ((back ? 0.10f : 0.18f) * (0.4f + flare)));
                g.drawLine ({ base, o }, 3.0f);
            }
            g.setColour (tone.brighter (0.3f).withAlpha (A ((back ? 0.3f : 0.6f) * (0.3f + 0.7f * flare))));
            g.drawLine ({ base, o }, 0.9f);
        }
        glowStroke (g, edge, tone.withAlpha (A (back ? 0.35f : 0.8f)), back ? 0.8f : 1.2f, back ? 0.25f : 0.5f + 0.6f * glow);
    };

    // The accretion disc: a soft body of light from the core out to the ring, with dust lanes
    // turning in it (inner ones faster).
    {
        juce::Graphics::ScopedSaveState flat (g);
        g.addTransform (juce::AffineTransform::scale (1.0f, tilt, c.x, c.y));
        juce::ColourGradient disc (tone.withAlpha (A (0.0f)), c, tone.withAlpha (A (0.0f)), c.translated (R * 1.08f, 0.0f), true);
        disc.addColour (0.35, second.withAlpha (A (0.04f)));
        disc.addColour (0.75, tone.withAlpha (A (0.09f + 0.06f * glow)));
        disc.addColour (0.92, tone.withAlpha (A (0.16f + 0.08f * glow)));
        g.setGradientFill (disc);
        g.fillEllipse (juce::Rectangle<float> (R * 2.16f, R * 2.16f).withCentre (c));
    }
    for (int lane = 0; lane < 4; ++lane)
    {
        const auto rr = R * (0.5f + 0.13f * (float) lane);
        const auto turn = swirl * 0.35f / (0.5f + 0.13f * (float) lane);
        const int dots = 48 + lane * 12;
        g.setColour ((lane % 2 == 0 ? tone : second).withAlpha (A (0.32f + 0.25f * glow)));
        for (int k = 0; k < dots; ++k)
        {
            if ((k * 7 + lane * 3) % 5 == 0) // gaps: lanes, not rings
                continue;
            const auto a = turn + twoPi * (float) k / (float) dots;
            g.fillRect (juce::Rectangle<float> (1.8f, 1.2f).withCentre ({ c.x + std::cos (a) * rr, c.y + std::sin (a) * rr * tilt }));
        }
    }

    // Orbits: the ring itself, and with RANDOM on a scale the pitches grains may take, as
    // rings floating above (and below) the disc.
    auto orbit = [&] (float height, juce::Colour colour, float width, bool dashed)
    {
        juce::Path ring;
        ring.addEllipse (juce::Rectangle<float> (R * 2.0f, R * 2.0f * tilt).withCentre (c.translated (0.0f, -height)));
        if (! dashed)
        {
            g.setColour (colour);
            g.strokePath (ring, juce::PathStrokeType (width));
            return;
        }
        juce::Path d;
        const float dashes[] = { 3.0f, 5.0f };
        juce::PathStrokeType (width).createDashedStroke (d, ring, dashes, 2);
        g.setColour (colour);
        g.fillPath (d);
    };

    orbit (0.0f, tone.withAlpha (A (0.25f)), 1.0f, false);
    if (const auto scale = (int) param (Params::grainScale); scale > 0 && param (Params::grainPitchRnd) >= 1.0f)
    {
        static constexpr int octaves[] = { -24, -12, 12, 24 }, fifths[] = { -24, -17, -12, -5, 7, 12, 19, 24 };
        static constexpr int minorChord[] = { -24, -21, -17, -12, -9, -5, 3, 7, 12, 15, 19, 24 }, majorChord[] = { -24, -20, -17, -12, -8, -5, 4, 7, 12, 16, 19, 24 };
        const int* set = octaves;
        int size = 4;
        if (scale == 2) { set = fifths; size = 8; }
        if (scale == 3) { set = processor.getSoundingKey().minor ? minorChord : majorChord; size = 12; }
        for (int i = 0; i < size; ++i)
        {
            if (std::abs ((float) set[i]) > param (Params::grainPitchRnd) + 0.5f)
                continue;
            const auto h = ((float) set[i] + param (Params::grainPitch)) * lift;
            const auto octave = set[i] % 12 == 0;
            orbit (h, second.withAlpha (A (octave ? 0.32f : 0.18f)), 1.0f, true);
            spaceText (g, (set[i] > 0 ? "+" : "") + juce::String (set[i]), { c.x + R + 6.0f, c.y - h - 6.0f, 30.0f, 12.0f }, octave ? 0.6f : 0.4f);
        }
    }

    // Particles behind the core first.
    auto paintParticles = [&] (bool back)
    {
        for (const auto& p : particles)
        {
            if (isBack (p.u) != back)
                continue;
            const auto t = p.age / p.life;
            const auto pos = at (p.u, R * p.radius, p.height * lift);
            const auto alpha = (1.0f - t) * (1.0f - t) * (back ? 0.5f : 0.85f);
            const auto colour = tone.interpolatedWith (second, p.hue);
            const auto size = p.size * (1.0f - 0.5f * t);
            const auto trail = at (p.u - 0.006f / std::max (0.3f, p.radius), R * (p.radius - p.drift * 0.07f), (p.height - p.rise * 0.07f) * lift);
            g.setColour (colour.withAlpha (A (alpha * 0.45f)));
            g.drawLine ({ trail, pos }, std::max (0.6f, size * 0.55f));
            g.setColour (colour.withAlpha (A (alpha * 0.25f)));
            g.fillEllipse (juce::Rectangle<float> (size * 3.2f, size * 3.2f).withCentre (pos));
            g.setColour (colour.brighter (0.4f).withAlpha (A (alpha)));
            g.fillEllipse (juce::Rectangle<float> (size, size).withCentre (pos));
        }
    };

    corona (true);
    paintParticles (true);

    // The core: a pulsing star with turning spiral arms (still when FROZEN), a ring of
    // bent light round it, and jets of light out of its poles that grow with the sound.
    {
        const auto r = R * (0.30f + 0.10f * glow);
        {
            const auto flicker = 1.0f + 0.06f * std::sin ((float) time * 13.0f) + 0.04f * std::sin ((float) time * 31.0f);
            const auto length = R * (0.3f + 0.6f * glow) * flicker;
            for (const auto up : { -1.0f, 1.0f })
            {
                const auto tip = c.translated (0.0f, up * length);
                juce::Path jet;
                jet.startNewSubPath (c.translated (-4.0f, 0.0f));
                jet.lineTo (tip.translated (-10.0f - 8.0f * glow, 0.0f));
                jet.lineTo (tip.translated (10.0f + 8.0f * glow, 0.0f));
                jet.lineTo (c.translated (4.0f, 0.0f));
                jet.closeSubPath();
                g.setGradientFill (juce::ColourGradient (second.withAlpha (A (0.2f + 0.25f * glow)), c, second.withAlpha (A (0.0f)), tip, false));
                g.fillPath (jet);
                const auto end = c.translated (0.0f, up * length * 0.8f);
                g.setGradientFill (juce::ColourGradient (glint.withAlpha (A (0.55f + 0.35f * glow)), c, tone.withAlpha (A (0.0f)), end, false));
                g.fillRect (juce::Rectangle<float> (c.x - 0.8f, std::min (c.y, end.y), 1.6f, std::abs (end.y - c.y)));
            }
        }
        {
            const auto ring = R * (0.2f + 0.03f * glow);
            juce::Path halo;
            halo.addEllipse (juce::Rectangle<float> (ring * 2.0f, ring * 2.0f).withCentre (c));
            glowStroke (g, halo, tone.brighter (0.5f).withAlpha (A (0.45f + 0.4f * glow)), 1.1f, 0.6f + 0.5f * glow);
        }
        g.setGradientFill (juce::ColourGradient (tone.withAlpha (light ? 0.22f + 0.2f * glow : 0.55f + 0.3f * glow), c, tone.withAlpha (0.0f),
                                                 c.translated (r * (light ? 1.3f : 1.8f), 0.0f), true));
        g.fillEllipse (juce::Rectangle<float> (r * 3.6f, r * 3.6f).withCentre (c));
        for (int arm = 0; arm < 3; ++arm)
        {
            juce::Path spiral;
            for (int k = 0; k <= 40; ++k)
            {
                const auto f = (float) k / 40.0f;
                const auto a = swirl + (float) arm * twoPi / 3.0f + f * 4.2f;
                const auto rr = r * (0.15f + 1.35f * f);
                const juce::Point<float> p (c.x + std::cos (a) * rr, c.y + std::sin (a) * rr * tilt * 1.4f);
                if (k == 0) spiral.startNewSubPath (p); else spiral.lineTo (p);
            }
            glowStroke (g, spiral, second.interpolatedWith (tone, 0.4f).withAlpha (A (0.35f + 0.3f * glow)), 1.2f, 0.5f);
        }
        g.setGradientFill (juce::ColourGradient (glint.withAlpha (A (0.9f)), c, tone.withAlpha (A (0.0f)), c.translated (r * 0.55f, 0.0f), true));
        g.fillEllipse (juce::Rectangle<float> (r * 1.1f, r * 1.1f).withCentre (c));
        if (frozen) // a frost star over the core
        {
            juce::Path star;
            for (int k = 0; k < 3; ++k)
            {
                const auto a = juce::MathConstants<float>::pi * (float) k / 3.0f;
                const juce::Point<float> d (std::cos (a) * r * 0.9f, std::sin (a) * r * 0.9f);
                star.startNewSubPath (c - d);
                star.lineTo (c + d);
            }
            glowStroke (g, star, ice, 1.2f, 0.8f);
        }
    }

    // SYNC: a ring goes out from the core on every note value of the tempo.
    if (param (Params::grainSync) > 0.5f)
    {
        auto* densityParam = processor.getParameters().getParameter (Params::grainDensity);
        const auto index = GrainSync::indexFor (densityParam->convertTo0to1 (param (Params::grainDensity)));
        const auto phase = (float) std::fmod (processor.getPumpPpq() / (double) GrainSync::beats[index], 1.0);
        const auto rr = R * (0.3f + 1.0f * phase);
        g.setColour (tone.withAlpha (A (0.5f * (1.0f - phase))));
        g.drawEllipse (juce::Rectangle<float> (rr * 2.0f, rr * 2.0f * tilt).withCentre (c), 1.5f * (1.0f - phase) + 0.5f);
        spaceText (g, juce::String ("SYNC ") + GrainSync::names[index], { m.getX(), screen.getY() + 24.0f, 120.0f, 12.0f }, 0.7f);
    }

    // Shockwaves: a hit sends a ring of light out over the disc.
    for (const auto& w : shockwaves)
    {
        const auto t = w.age / 1.4f;
        const auto ease = 1.0f - (1.0f - t) * (1.0f - t);
        const auto alpha = (1.0f - t) * (1.0f - t) * w.strength;
        for (int k = 0; k < 2; ++k)
        {
            const auto rr = R * (0.3f + 1.4f * ease) * (k == 0 ? 1.0f : 0.82f);
            juce::Path ring;
            ring.addEllipse (juce::Rectangle<float> (rr * 2.0f, rr * 2.0f * tilt).withCentre (c));
            glowStroke (g, ring, (k == 0 ? tone : second).brighter (0.3f).withAlpha (A (0.8f * alpha)), 0.5f + 2.0f * (1.0f - t), 0.7f * alpha);
        }
    }

    corona (false);

    // SPRAY: the arc of the ring grains are drawn from; the read head a comet on it.
    {
        const auto u0 = (float) (sprayCentreU - sprayHalfU), u1 = (float) (sprayCentreU + sprayHalfU);
        juce::Path arc;
        const int steps = std::max (2, (int) ((u1 - u0) * 120.0f));
        for (int k = 0; k <= steps; ++k)
        {
            const auto u = u0 + (u1 - u0) * (float) k / (float) steps;
            const auto p = at (u - std::floor (u), R, 0.0f);
            if (k == 0) arc.startNewSubPath (p); else arc.lineTo (p);
        }
        g.setColour (tone.withAlpha (A (0.16f + 0.14f * glow)));
        g.strokePath (arc, juce::PathStrokeType (9.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

        const auto head = (float) juce::jlimit (0.0, 1.0, sprayCentreU);
        juce::Path tail;
        for (int k = 0; k <= 24; ++k)
        {
            const auto u = head - 0.07f * (float) k / 24.0f;
            const auto p = at (u - std::floor (u), R, 0.0f);
            if (k == 0) tail.startNewSubPath (p); else tail.lineTo (p);
        }
        glowStroke (g, tail, hot (0.4f).withAlpha (A (0.8f)), 2.0f, 0.9f);
        const auto p = at (head, R, 0.0f);
        g.setGradientFill (juce::ColourGradient (hot (0.6f), p, tone.withAlpha (A (0.0f)), p.translated (14.0f, 0.0f), true));
        g.fillEllipse (juce::Rectangle<float> (28.0f, 28.0f).withCentre (p));
        g.setColour (glint);
        g.fillEllipse (juce::Rectangle<float> (4.0f, 4.0f).withCentre (p));
        // A pillar of light up from the head (where the grains rise).
        g.setGradientFill (juce::ColourGradient (tone.withAlpha (A (0.35f + 0.3f * glow)), p, tone.withAlpha (A (0.0f)), p.translated (0.0f, -R * 0.55f), false));
        g.fillRect (juce::Rectangle<float> (p.x - 1.0f, p.y - R * 0.55f, 2.0f, R * 0.55f));
        const auto still = param (Params::grainScan) < 0.001f;
        spaceText (g, frozen ? "FROZEN" : (still ? "HOLD" : "SCAN"), { p.x + 10.0f, p.y + 4.0f, 70.0f, 12.0f }, 0.75f);
    }

    paintParticles (false);

    // Live grains: floating at their pitch above the disc, blooming with their envelope.
    // SHAPE: hard grains are square sparks, soft ones round, plucked ones comets.
    for (const auto& gr : live)
    {
        if (gr.u < 0.0f || gr.u > 1.0f)
            continue;
        const auto p = at (gr.u, R, gr.semis * lift);
        const auto r = 1.8f + 3.5f * gr.level;
        const auto colour = tone.interpolatedWith (second, juce::jlimit (0.0f, 1.0f, 0.5f + gr.semis / 48.0f));
        g.setColour (colour.withAlpha (A (0.25f * gr.level))); // a thread down to the disc
        g.drawLine (p.x, p.y, p.x, at (gr.u, R, 0.0f).y, 0.8f);
        g.setGradientFill (juce::ColourGradient (colour.withAlpha (A (0.5f * gr.level)), p, colour.withAlpha (A (0.0f)), p.translated (r * 4.0f, 0.0f), true));
        g.fillEllipse (juce::Rectangle<float> (r * 8.0f, r * 8.0f).withCentre (p));
        g.setColour (glint.interpolatedWith (colour, 0.3f).withAlpha (A (0.6f + 0.4f * gr.level)));
        const auto core = juce::Rectangle<float> (r * 1.6f, r * 1.6f).withCentre (p);
        if (shape < 0.2f)
            g.fillRect (core.reduced (r * 0.12f));
        else
            g.fillEllipse (core);
        if (shape > 0.7f)
            for (int k = 1; k <= 3; ++k)
            {
                g.setColour (colour.withAlpha (A (gr.level * (0.5f - 0.13f * (float) k) * (shape - 0.7f) / 0.3f)));
                g.fillEllipse (juce::Rectangle<float> (r * 1.2f, r * 1.2f).withCentre (p.translated (-r * 1.9f * (float) k, 0.0f)));
            }
    }

    // A lens flare over the core: a long thin streak, a short upright one, and soft ghosts
    // on a line out of the core. At night only (on white it is just a smudge over the core).
    if (! light)
    {
        const auto strength = 0.25f + 0.6f * glow;
        const auto half = R * (0.9f + 0.9f * glow);
        auto softStreak = [&] (float length, float thickness, float alpha, juce::Colour colour)
        {
            // A round glow squashed into a streak: it fades out along it and across it.
            juce::Graphics::ScopedSaveState squash (g);
            // `length` across, `thickness` up and down (half sizes).
            const auto radius = std::max (length, thickness);
            g.addTransform (juce::AffineTransform::scale (length / radius, thickness / radius, c.x, c.y));
            g.setGradientFill (juce::ColourGradient (colour.withAlpha (A (alpha)), c, colour.withAlpha (0.0f), c.translated (radius, 0.0f), true));
            g.fillEllipse (juce::Rectangle<float> (radius * 2.0f, radius * 2.0f).withCentre (c));
        };
        const auto flare = glint.interpolatedWith (tone, 0.2f);
        softStreak (half, 3.0f, 0.75f * strength, flare);         // the thin bright line
        softStreak (half * 0.5f, 5.0f, 0.12f * strength, flare);  // its soft glow
        softStreak (2.5f, half * 0.3f, 0.6f * strength, glint);   // the short upright one
        const juce::Point<float> direction (0.93f, -0.36f);
        static constexpr float along[] = { 0.55f, 0.8f, 1.25f, -0.7f }, sizes[] = { 9.0f, 20.0f, 30.0f, 14.0f };
        for (int k = 0; k < 4; ++k)
        {
            const auto p = c + direction * along[k] * R;
            const auto colour = k % 2 == 0 ? tone : second;
            g.setColour (colour.withAlpha (A (0.07f * strength)));
            g.fillEllipse (juce::Rectangle<float> (sizes[k], sizes[k]).withCentre (p));
            g.setColour (colour.withAlpha (A (0.16f * strength)));
            g.drawEllipse (juce::Rectangle<float> (sizes[k], sizes[k]).withCentre (p), 0.8f);
        }
    }

    // Where the chop starts and ends: the seam at the left of the ring.
    {
        const auto seam = at (0.0f, R * 1.0f, 0.0f);
        g.setColour (spaceInk().withAlpha (A (0.35f)));
        g.drawLine (seam.x - 10.0f, seam.y, seam.x + 6.0f, seam.y, 1.0f);
        // The chop runs round the back first (above the seam) and comes home in front (below).
        spaceText (g, "START", { seam.x - 44.0f, seam.y - 15.0f, 44.0f, 12.0f }, 0.4f);
        spaceText (g, "END", { seam.x - 44.0f, seam.y + 3.0f, 44.0f, 12.0f }, 0.4f);
    }

    const auto& info = processor.getSampleInfo();
    if (info.sample == nullptr)
    {
        const auto blink = std::fmod (time, 1.2) < 0.7;
        spaceText (g, "NO SIGNAL", screen, blink ? 0.7f : 0.25f, juce::Justification::centred);
    }
    else if (live.empty())
    {
        const auto texture = processor.getParameters().getRawParameterValue (Params::grainMix)->load();
        spaceText (g, texture < 0.001f ? "TEXTURE 0  -  TURN IT UP AND PLAY A CHOP" : "PLAY A CHOP",
                   screen.withTrimmedTop (22.0f).withHeight (14.0f), 0.45f, juce::Justification::centred);
    }
}

void GrainView::paintOscilloscope (juce::Graphics& g, juce::Rectangle<float> screen)
{
    paintScreen (g, screen, "OSC");
    const auto m = screen.reduced (12.0f, 10.0f).withTrimmedTop (12.0f);
    auto& tap = processor.getOutputTap();

    g.setColour (screen::hud().withAlpha (0.06f));
    g.fillRect (juce::Rectangle<float> (m.getX(), m.getCentreY(), m.getWidth(), 1.0f));
    for (int k = 1; k < 8; ++k)
        g.fillRect (juce::Rectangle<float> (m.getX() + m.getWidth() * (float) k / 8.0f, m.getY(), 1.0f, m.getHeight()));

    // Trigger on a rising zero crossing so the waveform stands still.
    constexpr int span = 600;
    int trigger = span;
    for (int age = span; age < span + 1400; ++age)
    {
        const auto older = tap.getLeft (age + 1) + tap.getRight (age + 1);
        const auto newer = tap.getLeft (age) + tap.getRight (age);
        if (older <= 0.0f && newer > 0.0f) { trigger = age; break; }
    }

    // One point per ~2 px (a glow stroke over 600 points costs more than the rest of the
    // view); the peak still looks at every sample.
    juce::Path wave;
    float peak = 0.0f;
    for (int i = 0; i < span; ++i)
        peak = std::max (peak, std::abs (0.5f * (tap.getLeft (trigger - i) + tap.getRight (trigger - i))));
    const auto points = juce::jlimit (32, span, (int) (m.getWidth() * 0.5f));
    for (int k = 0; k < points; ++k)
    {
        const auto i = k * (span - 1) / (points - 1);
        const auto v = 0.5f * (tap.getLeft (trigger - i) + tap.getRight (trigger - i));
        const auto x = m.getX() + m.getWidth() * (float) k / (float) (points - 1);
        const auto y = m.getCentreY() - juce::jlimit (-1.0f, 1.0f, v * 1.4f) * m.getHeight() * 0.5f;
        if (k == 0) wave.startNewSubPath (x, y); else wave.lineTo (x, y);
    }
    glowStroke (g, wave, signal().withAlpha (0.95f), 1.3f, 0.6f + juce::jlimit (0.0f, 1.0f, peak * 2.0f));

    hudText (g, juce::String (juce::Decibels::gainToDecibels (peak, -60.0f), 1) + " dB",
             screen.reduced (12.0f, 8.0f).withHeight (12.0f), 0.45f, juce::Justification::centredRight);
}

void GrainView::paintVectorscope (juce::Graphics& g, juce::Rectangle<float> screen)
{
    paintScreen (g, screen, "VECTOR");
    const auto inner = screen.reduced (8.0f).withTrimmedTop (14.0f).withTrimmedBottom (14.0f);
    const auto side = std::min (inner.getWidth(), inner.getHeight());
    const auto square = juce::Rectangle<float> (side, side).withCentre (inner.getCentre());

    g.setColour (screen::hud().withAlpha (0.07f));
    g.drawLine (square.getX(), square.getBottom(), square.getRight(), square.getY(), 1.0f);
    g.drawLine (square.getX(), square.getY(), square.getRight(), square.getBottom(), 1.0f);
    g.drawLine (square.getCentreX(), square.getY(), square.getCentreX(), square.getBottom(), 1.0f);

    // Traces: oldest first, faint; newest last, bright (phosphor afterglow).
    const auto radius = side * 0.46f;
    const auto centre = square.getCentre();
    const auto numTraces = (int) vectorTraces.size();
    for (int k = 0; k < numTraces; ++k)
    {
        const auto& pts = vectorTraces[(size_t) k];
        if (pts.size() < 2 || vectorSilent)
            continue;

        juce::Path p;
        p.startNewSubPath (centre.x + pts[0].x * radius, centre.y - pts[0].y * radius);
        for (size_t i = 1; i < pts.size(); ++i)
            p.lineTo (centre.x + pts[i].x * radius, centre.y - pts[i].y * radius);

        const auto age = (float) (k + 1) / (float) numTraces; // 1 = newest
        const auto newest = k == numTraces - 1;
        g.setColour (signal().withAlpha (0.10f * age));
        g.strokePath (p, juce::PathStrokeType (6.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        g.setColour (signal().withAlpha (0.35f * age));
        g.strokePath (p, juce::PathStrokeType (2.4f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        g.setColour (newest ? hot (0.4f) : signal().withAlpha (0.55f * age));
        g.strokePath (p, juce::PathStrokeType (newest ? 1.3f : 1.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }

    if (vectorSilent)
    {
        g.setColour (signal().withAlpha (0.6f)); // silence: a resting dot in the centre
        g.fillEllipse (juce::Rectangle<float> (4.0f, 4.0f).withCentre (centre));
    }

    // Guide rings at half and full scale.
    g.setColour (screen::hud().withAlpha (0.08f));
    for (auto f : { 0.5f, 1.0f })
    {
        const auto rr = side * 0.46f * f;
        g.drawEllipse (juce::Rectangle<float> (rr * 2.0f, rr * 2.0f).withCentre (square.getCentre()), 1.0f);
    }

    hudText (g, "AUTO +" + juce::String (juce::roundToInt (juce::Decibels::gainToDecibels (vectorGain))) + " dB",
             screen.reduced (12.0f, 8.0f).withHeight (12.0f), 0.4f, juce::Justification::centredRight);

    hudText (g, "L", { square.getX(), square.getY(), 12.0f, 12.0f }, 0.35f);
    hudText (g, "R", { square.getRight() - 12.0f, square.getY(), 12.0f, 12.0f }, 0.35f, juce::Justification::centredRight);

    // Correlation: -1 (out of phase) .. +1 (mono)
    const auto bar = juce::Rectangle<float> (screen.getX() + 12.0f, screen.getBottom() - 12.0f, screen.getWidth() - 24.0f, 2.0f);
    g.setColour (screen::hud().withAlpha (0.12f));
    g.fillRect (bar);
    const auto x = bar.getX() + (correlation * 0.5f + 0.5f) * bar.getWidth();
    g.setColour (correlation < 0.0f ? Colours::record : signal());
    g.fillRect (juce::Rectangle<float> (x - 2.0f, bar.getY() - 2.0f, 4.0f, 6.0f));
}

void GrainView::paintSpectrogram (juce::Graphics& g, juce::Rectangle<float> screen)
{
    paintScreen (g, screen, "SPECTRO");
    const auto inner = screen.reduced (8.0f).withTrimmedTop (14.0f);
    g.setImageResamplingQuality (juce::Graphics::mediumResamplingQuality);
    g.drawImage (spectroImage, inner, juce::RectanglePlacement::stretchToFit);

    for (auto [hz, label] : { std::pair<float, const char*> { 100.0f, "100" }, { 1000.0f, "1K" }, { 10000.0f, "10K" } })
    {
        const auto t = std::log (hz / 30.0f) / std::log (18000.0f / 30.0f);
        const auto y = inner.getBottom() - t * inner.getHeight();
        g.setColour (screen::hud().withAlpha (0.08f));
        g.fillRect (juce::Rectangle<float> (inner.getX(), y, inner.getWidth(), 1.0f));
        hudText (g, label, { inner.getX() + 2.0f, y - 11.0f, 30.0f, 10.0f }, 0.35f);
    }
}

} // namespace kerf::ui

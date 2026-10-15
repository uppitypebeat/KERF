#include "WaveformView.h"

namespace kerf::ui
{

namespace
{
    /** The waveform display's own neutrals: a dark screen in the night themes (like the GRAIN
        display), the theme's own light neutrals in white mode. */
    namespace Screen
    {
        juce::Colour bg()       { return lightTheme() ? Colours::well : juce::Colour (0xff0c0c0e); }
        juce::Colour raised()   { return lightTheme() ? Colours::surfaceRaised : juce::Colour (0xff1d1d21); }
        juce::Colour hairline() { return lightTheme() ? Colours::hairline : juce::Colour (0xff26262a); }
        juce::Colour line()     { return lightTheme() ? Colours::line : juce::Colour (0xff3a3a3f); }
        juce::Colour ink()      { return lightTheme() ? Colours::text : juce::Colour (0xffe6e1d8); }
        juce::Colour inkDim()   { return lightTheme() ? Colours::textDim : juce::Colour (0xff8e8a84); }
        juce::Colour inkFaint() { return lightTheme() ? Colours::textFaint : juce::Colour (0xff5c5a56); }
    }

    constexpr float edgeHitRadius = 5.0f;
    constexpr float snapRadiusPx = 8.0f;
    constexpr float rewindSeconds = 1.1f; // the stars fly home when a drag ends (taken away or dropped)

    juce::String chopIndexLabel (int index) { return juce::String (index + 1).paddedLeft ('0', 2); }
}

juce::String noteName (int midiNote);

WaveformView::WaveformView (KerfAudioProcessor& p, AnimationClock& c) : processor (p), clock (c)
{
    processor.addChangeListener (this);
    processor.getChops().getState().addListener (this);
    processor.getSettings().addListener (this);
    clock.add (this);
    setWantsKeyboardFocus (false);
    playheadChops.fill (0);
    trailChop.fill (0);
    for (size_t i = 0; i < numVoices; ++i) // only hits from now on animate
        lastTriggers[i] = processor.getEngine().getVoiceDisplay()[i].triggers.load (std::memory_order_relaxed);
    refreshChops();
    changeListenerCallback (nullptr);
}

WaveformView::~WaveformView()
{
    clock.remove (this);
    processor.getSettings().removeListener (this);
    processor.getChops().getState().removeListener (this);
    processor.removeChangeListener (this);
}

//==============================================================================
void WaveformView::resized()
{
    auto r = getLocalBounds().toFloat();
    overviewArea = r.removeFromBottom (18.0f);
    r.removeFromBottom (4.0f);
    rulerArea = r.removeFromBottom (14.0f);
    labelLane = r.removeFromTop (20.0f);
    waveArea = r;
}

int64_t WaveformView::numFrames() const noexcept
{
    return shownSample != nullptr ? shownSample->getNumSamples() : 0;
}

double WaveformView::frameToX (double frame) const noexcept
{
    return waveArea.getX() + (frame - viewStart) / std::max (1.0, viewEnd - viewStart) * waveArea.getWidth();
}

double WaveformView::xToFrame (double x) const noexcept
{
    return viewStart + (x - waveArea.getX()) / std::max (1.0f, waveArea.getWidth()) * (viewEnd - viewStart);
}

void WaveformView::clampRange (double& start, double& end) const
{
    const auto total = (double) std::max<int64_t> (1, numFrames());
    const auto minSpan = std::min (total, 64.0); // never zoom past ~64 frames across the view
    const auto span = juce::jlimit (minSpan, total, end - start);
    start = juce::jlimit (0.0, total - span, start);
    end = start + span;
}

void WaveformView::setViewNow (double start, double end)
{
    clampRange (start, end);
    viewStart = targetStart = start;
    viewEnd = targetEnd = end;
    repaint();
}

void WaveformView::animateViewTo (double start, double end)
{
    clampRange (start, end);
    targetStart = start;
    targetEnd = end;
    repaint();
}

void WaveformView::zoomToFit (bool animate)
{
    const auto total = (double) std::max<int64_t> (1, numFrames());
    if (animate) animateViewTo (0.0, total);
    else         setViewNow (0.0, total);
}

void WaveformView::zoomBy (double factor, double anchorFrame)
{
    if (anchorFrame < 0.0)
        anchorFrame = (viewStart + viewEnd) * 0.5; // zoom around the centre

    // Zoom relative to where the view is heading, so quick wheel turns add up smoothly
    // and the point under the mouse stays put.
    const auto t = (anchorFrame - viewStart) / std::max (1.0, viewEnd - viewStart);
    const auto anchor = targetStart + t * (targetEnd - targetStart);
    const auto newSpan = (targetEnd - targetStart) * factor;
    animateViewTo (anchor - t * newSpan, anchor - t * newSpan + newSpan);
}

void WaveformView::zoomToSelection()
{
    if (const auto c = processor.getChops().getChop (selectedChopId()))
    {
        const auto pad = (double) c->length() * 0.08;
        animateViewTo ((double) c->start - pad, (double) c->end + pad);
    }
}

void WaveformView::ensureVisible (int64_t start, int64_t end)
{
    if ((double) start >= targetStart && (double) end <= targetEnd)
        return;

    const auto span = targetEnd - targetStart;
    if ((double) (end - start) > span)
        animateViewTo ((double) start, (double) end);
    else
        animateViewTo ((double) start - span * 0.1, (double) start - span * 0.1 + span);
}

int WaveformView::selectedChopId() const
{
    return (int) processor.getSettings().getProperty ("selectedChop", 0);
}

void WaveformView::selectChop (int id)
{
    processor.getSettings().setProperty ("selectedChop", id, nullptr);
}

int WaveformView::playTarget() const
{
    return sampleScope ? SamplerEngine::wholeSampleId : selectedChopId();
}

bool WaveformView::targetPlaying() const
{
    return std::find (playheadChops.begin(), playheadChops.end(), playTarget()) != playheadChops.end();
}

bool WaveformView::cursorShown() const
{
    // SAMPLE: always. CHOP: once it was put somewhere in the chop (else PLAY starts at its start).
    return shownSample != nullptr && (sampleScope || cursorPlaced);
}

double WaveformView::samplePlayheadFrame() const
{
    if (dragMode == DragMode::playhead)
        return (double) playheadDragFrame;
    for (size_t i = 0; i < playheadChops.size(); ++i)
        if (playheadChops[i] == playTarget())
            return playheadFrames[i];
    return (double) processor.getSampleCursor();
}

int64_t WaveformView::playheadFrameAt (float x, bool snapToChops) const
{
    auto frame = juce::jlimit<int64_t> (0, std::max<int64_t> (0, numFrames() - 1), (int64_t) xToFrame (x));
    // CHOP: it catches lightly on every chop's start (Shift: not).
    if (! sampleScope && snapToChops)
    {
        float nearest = 6.0f;
        for (const auto& c : chops)
            if (const auto d = std::abs ((float) frameToX ((double) c.start) - x); d <= nearest)
            {
                nearest = d;
                frame = c.start;
            }
    }
    return frame;
}

void WaveformView::placeSampleCursor (int64_t frame)
{
    frame = juce::jlimit<int64_t> (0, std::max<int64_t> (0, numFrames() - 1), frame);
    const auto wasPlaying = targetPlaying();

    // CHOP: dragged into another chop, that chop is the one (selected, and PLAY plays it).
    if (! sampleScope)
        if (const auto id = processor.getChops().chopIdAt (frame); id != 0 && id != selectedChopId())
            selectChop (id);

    cursorPlaced = true;
    cursorChop = selectedChopId();
    processor.setSampleCursor (frame);
    // Playing: it jumps there (PAUSE holds it there).
    if (wasPlaying)
    {
        if (onPlayFrom) onPlayFrom (playTarget(), (double) frame);
        else            processor.previewChop (playTarget(), 0.9f, true, (double) frame);
    }
    repaint();
}

void WaveformView::refreshChops()
{
    chops = processor.getChops().getChops();
    for (auto& c : chops)
        c = processor.effectiveChop (c); // show what plays (pad settings included)

    // New boundaries (split, chop by transients, undo) drop in, one after another.
    std::set<int> ids;
    int newIndex = 0;
    for (const auto& c : chops)
    {
        ids.insert (c.id);
        if (chopsSeeded && knownChops.count (c.id) == 0)
            born[c.id] = time + 0.014 * newIndex++;
    }
    knownChops = std::move (ids);
    chopsSeeded = shownSample != nullptr;
    repaint();
}

void WaveformView::changeListenerCallback (juce::ChangeBroadcaster*)
{
    const auto& info = processor.getSampleInfo();
    if (info.sample != shownSample)
    {
        shownSample = info.sample;
        zoomToFit (false);
        reveal = 0.0f; // grow the new waveform out of the centre line
        kick.clear();
        born.clear();
        chopsSeeded = false;
        selStart = selEnd = -1.0;
    }
    refreshChops();
}

//==============================================================================
void WaveformView::animationTick (double dt)
{
    const auto& info = processor.getSampleInfo();
    bool needsRepaint = false;

    if (info.loading)
    {
        loadingPhase = std::fmod (loadingPhase + (float) dt * 0.9f, 1.0f);
        needsRepaint = true;
    }
    if (shownSample == nullptr)
    {
        updateStars (dt); // the start screen: stars drift, the prompt pulses
        // Left alone they drift slowly: 30 frames a second are plenty (60 with the mouse or a file).
        starFrameClock += dt;
        if (mouseInside || dragHighlight || rewind >= 0.0f || vortex.isActive() || starFrameClock >= 1.0 / 30.0)
        {
            starFrameClock = 0.0;
            needsRepaint = true;
        }
    }
    else if (starsWarping || rewind >= 0.0f)
    {
        // A sample covers the start screen (the drop loaded it): the stars are home at once,
        // for when it comes back (Ctrl+Z, a cleared sample).
        for (auto& s : stars) { s.hx = s.sx; s.hy = s.sy; s.ox = s.oy = s.vx = s.vy = 0.0f; }
        starsWarping = false;
        rewind = -1.0f;
        warp = 0.0f;
    }
    updateDrop (dt);
    if (vortex.isActive())
        needsRepaint = true;

    // Chops chosen with Ctrl (or let go): their tint.
    if (const auto sig = processor.getSettings().getProperty ("selectedChops").toString(); sig != selectionSig)
    {
        selectionSig = sig;
        needsRepaint = true;
    }

    // CHOP / SAMPLE next to PLAY: PLAY starts at the playhead (with SAMPLE it always shows).
    if (const auto scope = (int) processor.getSettings().getProperty ("playScope", 0) == 1; scope != sampleScope)
    {
        sampleScope = scope;
        needsRepaint = true;
    }
    if (shownSample != nullptr)
    {
        // Another chop selected: the playhead goes to its start (until it is put somewhere).
        if (const auto selected = selectedChopId(); selected != cursorChop)
        {
            cursorChop = selected;
            if (std::exchange (cursorPlaced, false))
                needsRepaint = true;
        }
        if (! cursorPlaced)
        {
            int64_t start = 0;
            for (const auto& c : chops) // as it plays (pad START included)
                if (c.id == cursorChop)
                    start = c.start;
            if (start != processor.getSampleCursor())
            {
                processor.setSampleCursor (start);
                needsRepaint = true;
            }
        }
    }

    // Playheads and per-chop activity from the audio engine (lock-free readout).
    const auto& voices = processor.getEngine().getVoiceDisplay();
    std::map<int, float> targetActivity;

    for (size_t i = 0; i < voices.size(); ++i)
    {
        const auto id = voices[i].chopId.load (std::memory_order_relaxed);
        const auto pos = voices[i].position.load (std::memory_order_relaxed);
        const auto level = voices[i].level.load (std::memory_order_relaxed);

        if (id != playheadChops[i] || (id != 0 && std::abs (pos - playheadFrames[i]) > 0.5))
            needsRepaint = true;

        playheadChops[i] = id;
        playheadFrames[i] = pos;

        if (id != 0)
            targetActivity[id] = std::max (targetActivity[id], juce::jlimit (0.0f, 1.0f, level * 1.5f + 0.25f));

        // Grains move every frame while they sound.
        if (id != 0 && voices[i].numGrains.load (std::memory_order_acquire) > 0)
            needsRepaint = true;
    }

    // Smoothly fade chop illumination in (fast) and out (slow).
    for (auto& [id, value] : activity)
    {
        const auto target = targetActivity.count (id) ? targetActivity[id] : 0.0f;
        const auto rate = target > value ? 1.0f / (float) Motion::fast : 1.0f / (float) Motion::slow;
        const auto next = value + (target - value) * std::min (1.0f, (float) dt * rate);
        if (std::abs (next - value) > 0.002f) needsRepaint = true;
        value = next;
    }
    for (const auto& [id, value] : targetActivity)
        if (! activity.count (id)) { activity[id] = 0.0f; needsRepaint = true; }

    for (auto it = activity.begin(); it != activity.end();)
        it = (it->second < 0.002f && ! targetActivity.count (it->first)) ? activity.erase (it) : std::next (it);

    tickAnimations (dt, needsRepaint);

    if (needsRepaint)
        repaint();
}

void WaveformView::tickAnimations (double dt, bool& needsRepaint)
{
    time += dt;

    // View glide: scale about the fixed point between the current and target ranges, so
    // the point under the mouse stays put while zooming (log-even at every zoom level).
    if (std::abs (viewStart - targetStart) > 0.5 || std::abs (viewEnd - targetEnd) > 0.5)
    {
        const auto k = 1.0 - std::exp (-dt * 16.0);
        const auto span = std::max (1.0, viewEnd - viewStart);
        const auto a = (targetEnd - targetStart) / span;
        if (std::abs (a - 1.0) > 1.0e-3)
        {
            const auto p = (targetStart - a * viewStart) / (1.0 - a);
            const auto ak = std::pow (a, k);
            viewStart = p + (viewStart - p) * ak;
            viewEnd = p + (viewEnd - p) * ak;
        }
        else
        {
            viewStart += (targetStart - viewStart) * k;
            viewEnd += (targetEnd - viewEnd) * k;
        }
        needsRepaint = true;
    }
    else if (viewStart != targetStart || viewEnd != targetEnd)
    {
        viewStart = targetStart;
        viewEnd = targetEnd;
        needsRepaint = true;
    }

    // New sample: reveal once the waveform overview exists.
    if (reveal < 1.0f && processor.getSampleInfo().waveform != nullptr)
    {
        reveal = std::min (1.0f, reveal + (float) dt / 0.75f);
        needsRepaint = true;
    }

    // Hits and phosphor trails.
    const auto& voices = processor.getEngine().getVoiceDisplay();
    const auto jumpFrames = (shownSample != nullptr ? shownSample->sampleRate : 44100.0) * 0.12;
    for (size_t i = 0; i < numVoices; ++i)
    {
        const auto trig = voices[i].triggers.load (std::memory_order_relaxed);
        const bool hit = trig != lastTriggers[i];
        lastTriggers[i] = trig;

        const auto id = playheadChops[i];
        const auto pos = playheadFrames[i];
        if (hit && id != 0)
            kick[id] = 1.0f;

        if (id != 0)
        {
            // A new note or a jump (loop wrap) restarts the trail instead of streaking across.
            if (hit || trailChop[i] != id || std::abs (pos - trailHead[i]) > jumpFrames)
                trailFrames[i] = pos;
            trailChop[i] = id;
            trailHead[i] = pos;
            trailLevel[i] += (1.0f - trailLevel[i]) * std::min (1.0f, (float) dt * 20.0f);
        }
        else
        {
            trailLevel[i] *= std::exp (-(float) dt * 5.0f);
        }

        trailFrames[i] += (trailHead[i] - trailFrames[i]) * (1.0 - std::exp (-dt * 4.0));
        if (trailLevel[i] > 0.01f)
            needsRepaint = true;
        else
            trailLevel[i] = 0.0f;
    }

    // Read head: where the beam crosses the waveform, and the sparks it sheds.
    const auto& info = processor.getSampleInfo();
    const auto centreY = waveArea.getCentreY();
    const auto halfH = waveArea.getHeight() * 0.5f - 6.0f;
    for (size_t i = 0; i < numVoices; ++i)
    {
        const auto id = playheadChops[i];
        if (id == 0 || shownSample == nullptr || info.waveform == nullptr)
        {
            headTop[i] = headBottom[i] = 0.0f;
            continue;
        }

        const auto gain = juce::jlimit (1.0f, 4.0f, 0.92f / std::max (0.0001f, info.waveform->getPeak()));
        const auto window = std::max (16.0, (viewEnd - viewStart) / std::max (1.0f, waveArea.getWidth()) * 3.0);
        const auto bin = info.waveform->getRange (shownSample->audio, (int64_t) (playheadFrames[i] - window), (int64_t) (playheadFrames[i] + window));
        const auto top = juce::jlimit (0.0f, 1.0f, bin.max * gain), bottom = juce::jlimit (0.0f, 1.0f, -bin.min * gain);
        headTop[i] += (top - headTop[i]) * std::min (1.0f, (float) dt * 30.0f);
        headBottom[i] += (bottom - headBottom[i]) * std::min (1.0f, (float) dt * 30.0f);

        const auto x = (float) frameToX (playheadFrames[i]);
        if (x < waveArea.getX() || x > waveArea.getRight())
            continue;

        // Sparks rise off the top of the waveform at the read head and drift back along
        // the path just played, fading out. Low-key.
        const auto level = juce::jlimit (0.0f, 1.0f, voices[i].level.load (std::memory_order_relaxed));
        const auto dir = trailHead[i] >= trailFrames[i] ? 1.0f : -1.0f;
        sparkDebt[i] += (float) dt * (8.0f + 30.0f * level);
        while (sparkDebt[i] >= 1.0f && sparks.size() < 160)
        {
            sparkDebt[i] -= 1.0f;
            const auto life = 0.4f + 0.5f * sparkRandom.nextFloat();
            sparks.push_back ({ x, centreY - headTop[i] * halfH,
                                -dir * (15.0f + 60.0f * sparkRandom.nextFloat()),
                                -(4.0f + 24.0f * sparkRandom.nextFloat()),
                                life, life, 1.0f + 1.4f * sparkRandom.nextFloat() * (0.5f + level) });
        }
        sparkDebt[i] = std::min (sparkDebt[i], 1.0f);
        needsRepaint = true;
    }

    for (auto& s : sparks)
    {
        s.life -= (float) dt;
        s.x += s.vx * (float) dt;
        s.y += s.vy * (float) dt;
        s.vx *= std::exp (-(float) dt * 2.5f); // drag: they hang in the air and fade
        s.vy *= std::exp (-(float) dt * 2.5f);
    }
    sparks.erase (std::remove_if (sparks.begin(), sparks.end(), [] (const Spark& s) { return s.life <= 0.0f; }), sparks.end());
    if (! sparks.empty())
        needsRepaint = true;

    for (auto it = kick.begin(); it != kick.end();)
    {
        it->second *= std::exp (-(float) dt * 4.5f);
        needsRepaint = true;
        it = it->second < 0.01f ? kick.erase (it) : std::next (it);
    }

    for (auto it = born.begin(); it != born.end();)
    {
        needsRepaint = true;
        it = time - it->second > 0.6 ? born.erase (it) : std::next (it);
    }

    // Selection tint glides to the newly selected chop (follows edges exactly while dragging).
    double ts = -1.0, te = -1.0;
    const auto selected = selectedChopId();
    for (const auto& c : chops)
        if (c.id == selected) { ts = (double) c.start; te = (double) c.end; }

    if (ts < 0.0 || selStart < 0.0 || dragMode == DragMode::edge)
    {
        if (selStart != ts || selEnd != te) needsRepaint = true;
        selStart = ts;
        selEnd = te;
    }
    else if (std::abs (selStart - ts) > 0.5 || std::abs (selEnd - te) > 0.5)
    {
        const auto k = 1.0 - std::exp (-dt * 18.0);
        selStart += (ts - selStart) * k;
        selEnd += (te - selEnd) * k;
        if (std::abs (selStart - ts) <= 0.5 && std::abs (selEnd - te) <= 0.5) { selStart = ts; selEnd = te; }
        needsRepaint = true;
    }
}

//==============================================================================
double WaveformView::beatLengthFrames() const
{
    const auto& info = processor.getSampleInfo();
    if (info.sourceBpm <= 0.0 || shownSample == nullptr)
        return 0.0;
    return 60.0 / info.sourceBpm * shownSample->sampleRate;
}

juce::String WaveformView::formatTime (double frames) const
{
    if (shownSample == nullptr)
        return {};
    const auto seconds = frames / shownSample->sampleRate;
    const auto mins = (int) (seconds / 60.0);
    const auto secs = seconds - mins * 60.0;
    return juce::String (mins) + ":" + juce::String (secs, 3).paddedLeft ('0', 6);
}

std::optional<WaveformView::EdgeHit> WaveformView::hitTestEdge (juce::Point<float> p) const
{
    if (! (waveArea.contains (p) || labelLane.contains (p)))
        return std::nullopt;

    std::optional<EdgeHit> best;
    float bestDistance = edgeHitRadius;
    const auto selected = selectedChopId();

    for (const auto& c : chops)
    {
        for (auto edge : { ChopEdge::start, ChopEdge::end })
        {
            const auto x = (float) frameToX ((double) (edge == ChopEdge::start ? c.start : c.end));
            auto d = std::abs (p.x - x);
            if (c.id == selected) d -= 1.5f; // prefer the selected chop's edges when they overlap
            if (d < bestDistance)
            {
                bestDistance = d;
                best = EdgeHit { c.id, edge };
            }
        }
    }

    // A start edge shared with a neighbour's end: pick the chop on the side of the cursor.
    if (best.has_value())
    {
        const auto c = processor.getChops().getChop (best->chopId);
        if (c.has_value())
        {
            const auto edgeFrame = best->edge == ChopEdge::start ? c->start : c->end;
            const auto edgeX = (float) frameToX ((double) edgeFrame);
            for (const auto& other : chops)
            {
                if (other.id == best->chopId || other.id == selected) continue;
                if (best->edge == ChopEdge::start && other.end == edgeFrame && p.x < edgeX && best->chopId != selected)
                    return EdgeHit { other.id, ChopEdge::end };
                if (best->edge == ChopEdge::end && other.start == edgeFrame && p.x > edgeX && best->chopId != selected)
                    return EdgeHit { other.id, ChopEdge::start };
            }
        }
    }

    return best;
}

int64_t WaveformView::snapFrame (int64_t frame, bool allowSnap, juce::String& snapKind) const
{
    snapKind = {};
    const auto& info = processor.getSampleInfo();
    const bool snapEnabled = (bool) processor.getSettings().getProperty ("snap", true);

    if (! allowSnap || ! snapEnabled || shownSample == nullptr)
        return frame;

    const auto framesPerPixel = (viewEnd - viewStart) / std::max (1.0f, waveArea.getWidth());
    const auto radius = (int64_t) (snapRadiusPx * framesPerPixel);
    int64_t best = frame, bestDistance = radius + 1;

    // Transients
    if (info.onsets != nullptr)
        for (const auto& o : info.onsets->onsets)
            if (o.strength > 0.12f)
                if (const auto d = std::abs (o.position - frame); d < bestDistance)
                {
                    bestDistance = d;
                    best = o.position;
                    snapKind = "TRANSIENT";
                }

    // Rhythmic grid (current grid setting, anchored at the beat grid's start: SHIFT)
    if (info.sourceBpm > 0.0)
    {
        const auto step = gridStepSamples ((NoteValue) (int) processor.getSettings().getProperty ("gridValue", 4),
                                           (NoteModifier) (int) processor.getSettings().getProperty ("gridModifier", 0),
                                           info.sourceBpm, shownSample->sampleRate);
        if (step >= 1.0)
        {
            const auto g = snapToGrid (frame, (int64_t) std::llround (processor.getChops().getBeatOrigin()), step);
            if (const auto d = std::abs (g - frame); d < bestDistance)
            {
                bestDistance = d;
                best = g;
                snapKind = "GRID";
            }
        }
    }

    return best;
}

//==============================================================================
void WaveformView::paint (juce::Graphics& g)
{
    const auto bounds = getLocalBounds().toFloat();
    if (nebulaTheme())
    {
        fillBackdrop (g, *this, bounds.withTrimmedBottom (overviewArea.getHeight() + 4.0f), Backdrop::deep);
    }
    else
    {
        g.setColour (Screen::bg());
        g.fillRect (bounds.withTrimmedBottom (overviewArea.getHeight() + 4.0f));
    }

    if (shownSample == nullptr)
    {
        paintEmptyState (g);
        paintOverview (g); // the strip is there before a sample too (no gap under the stars)
        return;
    }

    {
        juce::Graphics::ScopedSaveState save (g);
        g.reduceClipRegion (waveArea.getUnion (labelLane).toNearestInt());
        paintBeatGrid (g);
        paintChops (g);
        paintWaveform (g);
        paintEnvelope (g);
        paintGrains (g);
        paintPlayheads (g);
        paintDragReadout (g);
    }

    paintRuler (g);
    paintOverview (g);

    // A file dragged over a loaded sample: the singularity over the waveform.
    vortex.paint (g, screenArea());
}


juce::Rectangle<float> WaveformView::screenArea() const
{
    return getLocalBounds().toFloat().withTrimmedBottom (overviewArea.getHeight() + 4.0f);
}

void WaveformView::updateDrop (double dt)
{
    const auto area = screenArea();
    if (! hasDragPoint)
        dragPoint = area.getCentre();
    vortex.update (dt, dragHighlight, dragPoint, area);
    if (! dragHighlight && ! vortex.isActive())
        hasDragPoint = false;
}

void WaveformView::updateStars (double dt)
{
    const auto area = screenArea();
    if (area.isEmpty())
        return;

    if (stars.empty())
        for (int i = 0; i < 190; ++i)
        {
            const auto z = std::pow (starRandom.nextFloat(), 1.8f); // most stars far away
            stars.push_back ({ starRandom.nextFloat(), starRandom.nextFloat(), 0.0f, 0.0f, 0.0f, 0.0f,
                               0.15f + 0.85f * z, starRandom.nextFloat() * 6.28f, starRandom.nextInt (9) == 0 });
        }

    // A file comes in: remember where every star is (unless a rewind is still on its way:
    // then they are still headed for those places). Taken away without a drop: rewind.
    if (dragHighlight && ! starsWarping)
    {
        for (auto& s : stars)
            if (rewind < 0.0f) { s.sx = s.hx; s.sy = s.hy; }
        rewind = -1.0f;
    }
    else if (! dragHighlight && starsWarping)
    {
        // Taken away or dropped: either way the stars fly home (after a drop, out of the
        // supernova), so the sky is whole again when the start screen comes back.
        for (auto& s : stars) { s.fx = s.hx; s.fy = s.hy; }
        rewind = 0.0f;
    }
    starsWarping = dragHighlight;

    // Warp while a file is dragged over the view; it eases in and out (at once when rewinding:
    // the rewind draws its own streaks).
    warp += ((dragHighlight ? 1.0f : 0.0f) - warp) * std::min (1.0f, (float) dt * (rewind >= 0.0f ? 9.0f : 3.0f));

    // The rewind: fast at first, settling softly into place, each star along a slight arc.
    if (rewind >= 0.0f)
    {
        rewind = std::min (1.0f, rewind + (float) dt / rewindSeconds);
        const auto e = 1.0f - std::pow (1.0f - rewind, 3.0f);
        const auto arc = std::sin (e * juce::MathConstants<float>::pi) * 0.06f;
        for (auto& s : stars)
        {
            const auto dx = s.sx - s.fx, dy = s.sy - s.fy;
            s.hx = s.fx + dx * e - dy * arc * (0.4f + s.z);
            s.hy = s.fy + dy * e + dx * arc * (0.4f + s.z);
        }
        if (rewind >= 1.0f)
        {
            for (auto& s : stars) { s.hx = s.sx; s.hy = s.sy; }
            rewind = -1.0f;
        }
    }

    const auto mouse = mousePos;
    constexpr float radius = 120.0f;
    for (auto& s : stars)
    {
        if (rewind >= 0.0f)
        {
            s.phase += (float) dt * (0.6f + 1.4f * s.z);
            continue; // on its way home: no drift, no respawn
        }

        // Drift to the left (nearer stars faster: parallax); in warp, fall into the dragged
        // file's singularity, swirling.
        s.hx -= (float) dt * 0.008f * (0.3f + s.z);
        if (warp > 0.01f)
        {
            const auto cx = (vortex.getCore().x - area.getX()) / area.getWidth(), cy = (vortex.getCore().y - area.getY()) / area.getHeight();
            const auto dx = cx - s.hx, dy = cy - s.hy;
            const auto speed = (float) dt * warp * (0.4f + 1.2f * s.z);
            s.hx += (dx * 1.6f - dy * 0.9f) * speed;
            s.hy += (dy * 1.6f + dx * 0.9f) * speed;
            if (dx * dx + dy * dy < 0.0004f)
                s.hx = -1.0f; // swallowed: respawn at an edge
        }
        if (s.hx < -0.02f || s.hx > 1.02f || s.hy < -0.02f || s.hy > 1.02f)
        {
            // Respawn: at the right edge when drifting, on any edge in warp.
            if (warp > 0.3f)
            {
                const auto edge = starRandom.nextFloat();
                s.hx = starRandom.nextBool() ? (edge < 0.5f ? 0.0f : 1.0f) : starRandom.nextFloat();
                s.hy = s.hx == 0.0f || s.hx == 1.0f ? starRandom.nextFloat() : (edge < 0.5f ? 0.0f : 1.0f);
            }
            else
            {
                s.hx = 1.01f;
                s.hy = starRandom.nextFloat();
            }
            s.ox = s.oy = s.vx = s.vy = 0.0f;
        }

        // The mouse pushes stars away (nearer stars more); a spring brings them home.
        if (mouseInside)
        {
            const auto px = area.getX() + s.hx * area.getWidth() + s.ox, py = area.getY() + s.hy * area.getHeight() + s.oy;
            const auto dx = px - mouse.x, dy = py - mouse.y;
            const auto dist = std::sqrt (dx * dx + dy * dy);
            if (dist < radius && dist > 0.01f)
            {
                const auto push = std::pow (1.0f - dist / radius, 2.0f) * 2600.0f * (0.35f + 0.65f * s.z);
                s.vx += dx / dist * push * (float) dt;
                s.vy += dy / dist * push * (float) dt;
            }
        }
        s.vx += -s.ox * 9.0f * (float) dt;
        s.vy += -s.oy * 9.0f * (float) dt;
        const auto damping = std::exp (-(float) dt * 5.0f);
        s.vx *= damping;
        s.vy *= damping;
        s.ox += s.vx * (float) dt;
        s.oy += s.vy * (float) dt;
        s.phase += (float) dt * (0.6f + 1.4f * s.z);
    }
}

void WaveformView::paintStars (juce::Graphics& g, juce::Rectangle<float> area)
{
    juce::Graphics::ScopedSaveState save (g);
    g.reduceClipRegion (area.toNearestInt());

    // A faint nebula in the theme colour and a cooler one opposite. Such faint gradients band
    // visibly in 8 bits, so it is computed once per pixel with a little noise (dithering).
    const auto w = juce::roundToInt (area.getWidth()), h = juce::roundToInt (area.getHeight());
    if (w > 0 && h > 0 && (! nebula.isValid() || nebula.getWidth() != w || nebula.getHeight() != h || nebulaAccent != Colours::accent))
    {
        nebula = juce::Image (juce::Image::RGB, w, h, false, juce::SoftwareImageType());
        nebulaAccent = Colours::accent;
        const juce::Point<float> c1 (0.32f * (float) w, 0.42f * (float) h), c2 (0.74f * (float) w, 0.62f * (float) h);
        const auto r1 = 0.38f * (float) w, r2 = 0.30f * (float) w;
        auto smooth = [] (float t) { t = juce::jlimit (0.0f, 1.0f, t); return t * t * (3.0f - 2.0f * t); };
        uint32_t seed = 0x9e3779b9u;
        juce::Image::BitmapData data (nebula, juce::Image::BitmapData::writeOnly);
        const float bg[] = { Screen::bg().getFloatRed(), Screen::bg().getFloatGreen(), Screen::bg().getFloatBlue() };
        const float ac[] = { nebulaAccent.getFloatRed(), nebulaAccent.getFloatGreen(), nebulaAccent.getFloatBlue() };
        const float ik[] = { Screen::ink().getFloatRed(), Screen::ink().getFloatGreen(), Screen::ink().getFloatBlue() };
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x)
            {
                const juce::Point<float> p ((float) x, (float) y);
                const auto a1 = 0.07f * smooth (1.0f - p.getDistanceFrom (c1) / r1);
                const auto a2 = 0.035f * smooth (1.0f - p.getDistanceFrom (c2) / r2);
                juce::uint8 rgb[3];
                for (int ch = 0; ch < 3; ++ch)
                {
                    seed = seed * 1664525u + 1013904223u;
                    const auto noise = (float) (seed >> 8) / (float) (1u << 24) - 0.5f;
                    const auto v = (bg[ch] * (1.0f - a1 - a2) + ac[ch] * a1 + ik[ch] * a2) * 255.0f + noise;
                    rgb[ch] = (juce::uint8) juce::jlimit (0, 255, juce::roundToInt (v));
                }
                data.setPixelColour (x, y, juce::Colour (rgb[0], rgb[1], rgb[2]));
            }
    }
    if (nebula.isValid() && ! nebulaTheme()) // Nebula: the window-wide sky is already behind
        g.drawImageAt (nebula, juce::roundToInt (area.getX()), juce::roundToInt (area.getY()));

    for (const auto& s : stars)
    {
        const auto p = juce::Point<float> (area.getX() + s.hx * area.getWidth() + s.ox, area.getY() + s.hy * area.getHeight() + s.oy);
        const auto twinkle = 0.7f + 0.3f * std::sin (s.phase);
        const auto pushed = std::min (1.0f, std::sqrt (s.ox * s.ox + s.oy * s.oy) / 30.0f); // stirred stars flare up
        const auto racing = rewind >= 0.0f ? (1.0f - rewind) * (1.0f - rewind) : 0.0f; // rewinding stars flare up
        const auto alpha = juce::jlimit (0.0f, 1.0f, (0.18f + 0.72f * s.z) * twinkle + 0.35f * pushed + 0.4f * racing);
        const auto colour = (s.tinted ? Colours::accent.brighter (0.3f) : Screen::ink()).withAlpha (alpha);
        const auto size = 0.7f + 1.9f * s.z;

        // In warp, stars stretch into streaks pointing away from the centre.
        if (warp > 0.02f)
        {
            const auto core = vortex.getCore();
            const auto dir = (p - core) / std::max (1.0f, p.getDistanceFrom (core));
            const auto length = warp * (4.0f + 30.0f * s.z);
            g.setColour (colour.withMultipliedAlpha (0.8f));
            g.drawLine ({ p, p + dir * length }, size * 0.8f); // the tail points away from where it falls
        }
        // Rewinding: streaks trail behind each star towards where it came from, long while
        // it races, gone as it settles into its place.
        if (rewind >= 0.0f)
        {
            const auto speed = 3.0f * (1.0f - rewind) * (1.0f - rewind); // d(ease)/dt
            const juce::Point<float> from (area.getX() + s.fx * area.getWidth() + s.ox, area.getY() + s.fy * area.getHeight() + s.oy);
            const auto back = from - p;
            const auto length = std::min (back.getDistanceFromOrigin(), speed * (6.0f + 34.0f * s.z));
            if (length > 0.5f)
            {
                g.setColour (colour.withMultipliedAlpha (0.85f));
                g.drawLine ({ p, p + back / std::max (1.0f, back.getDistanceFromOrigin()) * length }, size * 0.8f);
            }
        }

        g.setColour (colour);
        g.fillEllipse (juce::Rectangle<float> (size, size).withCentre (p));

        // The nearest, brightest stars get a small glare cross.
        if (s.z > 0.9f)
        {
            g.setColour (colour.withMultipliedAlpha (0.35f));
            g.fillRect (juce::Rectangle<float> (size * 5.0f, 0.8f).withCentre (p));
            g.fillRect (juce::Rectangle<float> (0.8f, size * 5.0f).withCentre (p));
        }
    }
}

void WaveformView::paintEmptyState (juce::Graphics& g)
{
    const auto& info = processor.getSampleInfo();
    const auto area = getLocalBounds().toFloat().withTrimmedBottom (overviewArea.getHeight() + 4.0f);
    paintStars (g, area);
    vortex.paint (g, area);

    // The prompt makes way for the singularity while a file is held over the view.
    const auto shown = 1.0f - juce::jlimit (0.0f, 1.0f, vortex.getCharge() * 1.6f);
    if (shown <= 0.0f)
        return;
    const auto box = juce::Rectangle<float> (380.0f, 116.0f).withCentre (area.getCentre());

    // Waiting for a sample: the prompt pulses softly (a slow breath, not a hard blink).
    const auto waiting = ! info.loading && ! info.fileMissing && info.error.isEmpty() && ! dragHighlight;
    const auto pulse = waiting ? 0.5f + 0.5f * (float) std::sin (time * juce::MathConstants<double>::twoPi / 1.6) : 1.0f;

    // Corner ticks instead of a full outline: precise, quiet, hardware-like.
    const auto tick = 10.0f;
    const auto s = Metrics::stroke;
    // (Faded by multiplying each colour, not with a transparency layer: layers are costly.)
    g.setColour ((dragHighlight ? Colours::accent : (waiting ? Screen::line().interpolatedWith (Colours::accent, 0.6f * pulse) : Screen::line())).withMultipliedAlpha (shown));
    // top-left
    g.fillRect (box.getX(), box.getY(), tick, s);
    g.fillRect (box.getX(), box.getY(), s, tick);
    // top-right
    g.fillRect (box.getRight() - tick, box.getY(), tick, s);
    g.fillRect (box.getRight() - s, box.getY(), s, tick);
    // bottom-left
    g.fillRect (box.getX(), box.getBottom() - s, tick, s);
    g.fillRect (box.getX(), box.getBottom() - tick, s, tick);
    // bottom-right
    g.fillRect (box.getRight() - tick, box.getBottom() - s, tick, s);
    g.fillRect (box.getRight() - s, box.getBottom() - tick, s, tick);

    auto text = box.reduced (20.0f, 22.0f);
    juce::String title, line1, line2;
    juce::Colour titleColour = Screen::ink();

    if (info.loading)
    {
        title = "LOADING";
        line1 = "Decoding audio in the background";
    }
    else if (info.fileMissing)
    {
        title = "SAMPLE MISSING";
        titleColour = Colours::accent;
        line1 = info.missingFile.getFileName().isNotEmpty() ? info.missingFile.getFileName() : juce::String ("The project's sample could not be found");
        line2 = "Click to locate the file, or drop a replacement";
    }
    else if (info.error.isNotEmpty())
    {
        title = "COULD NOT LOAD";
        titleColour = Colours::accent;
        line1 = info.error;
        line2 = "Drop another file or click to choose one";
    }
    else
    {
        title = dragHighlight ? "RELEASE TO LOAD" : "DROP AUDIO";
        line1 = "or click to choose a file";
        line2 = juce::String::fromUTF8 ("WAV  \xc2\xb7  AIFF  \xc2\xb7  FLAC  \xc2\xb7  OGG");
    }

    const auto breath = (0.3f + 0.7f * pulse) * shown;
    drawText (g, title, text.removeFromTop (18.0f), TextStyle::sectionLabel, titleColour.withMultipliedAlpha (breath), juce::Justification::centred);
    text.removeFromTop (10.0f);
    drawText (g, line1, text.removeFromTop (16.0f), TextStyle::body, Screen::inkDim().withMultipliedAlpha (breath), juce::Justification::centred);
    text.removeFromTop (4.0f);
    drawText (g, line2, text.removeFromTop (16.0f), TextStyle::label, Screen::inkFaint().withMultipliedAlpha (breath), juce::Justification::centred);

    if (info.loading)
    {
        // Indeterminate progress: a short accent segment travelling along a hairline.
        const auto track = juce::Rectangle<float> (box.getX() + 40.0f, box.getBottom() - 14.0f, box.getWidth() - 80.0f, 1.0f);
        drawHairline (g, track, Screen::line().withMultipliedAlpha (shown));
        const auto segW = track.getWidth() * 0.22f;
        const auto x = track.getX() + Motion::easeOut (loadingPhase) * (track.getWidth() - segW);
        g.setColour (Colours::accent.withMultipliedAlpha (shown));
        g.fillRect (juce::Rectangle<float> (x, track.getY() - 0.5f, segW, 2.0f));
    }
}

void WaveformView::paintBeatGrid (juce::Graphics& g)
{
    const auto beat = beatLengthFrames();
    if (beat <= 0.0)
        return;

    const auto pixelsPerBeat = beat / (viewEnd - viewStart) * waveArea.getWidth();
    if (pixelsPerBeat < 6.0)
        return;

    // From the grid's start (SHIFT moves it against the sample).
    const auto origin = processor.getChops().getBeatOrigin();
    const auto firstBeat = std::floor ((viewStart - origin) / beat);
    for (double b = firstBeat; origin + b * beat <= viewEnd; b += 1.0)
    {
        const auto x = (float) frameToX (origin + b * beat);
        const bool bar = std::fmod (b, 4.0) == 0.0;
        drawHairline (g, { x, waveArea.getY(), 1.0f, waveArea.getHeight() },
                      bar ? Screen::hairline().brighter (0.25f) : Screen::hairline().withAlpha (0.6f));
    }
}

void WaveformView::paintChops (juce::Graphics& g)
{
    const auto selected = selectedChopId();
    const auto picked = processor.getSelectedChops(); // the selected one, and more chosen with Ctrl

    // Region tints (selected + playback glow)
    for (const auto& c : chops)
    {
        const auto x0 = (float) frameToX ((double) c.start), x1 = (float) frameToX ((double) c.end);
        if (x1 < waveArea.getX() || x0 > waveArea.getRight())
            continue;

        const auto region = juce::Rectangle<float> (x0, waveArea.getY(), x1 - x0, waveArea.getHeight());
        float glow = 0.0f;
        if (auto it = activity.find (c.id); it != activity.end())
            glow = it->second;

        // Each chop region is faintly tinted in its colour (brighter while it plays).
        if (c.id != selected) // the selected tint is drawn below (it glides)
        {
            const auto alsoSelected = std::find (picked.begin(), picked.end(), c.id) != picked.end(); // with Ctrl
            g.setColour (colourOf (c.id).withAlpha ((alsoSelected ? 0.07f : 0.03f) + glow * 0.05f));
            g.fillRect (region);
        }

        // Hit: a flash that washes over the chop from its start and fades.
        if (const auto k = kick.find (c.id); k != kick.end())
        {
            const auto sweep = Motion::easeOut (1.0f - k->second);
            g.setColour (colourOf (c.id).withAlpha (0.12f * k->second));
            g.fillRect (region);
            g.setColour (colourOf (c.id).withAlpha (0.10f * k->second));
            g.fillRect (region.withWidth (std::max (1.0f, region.getWidth() * sweep)));
        }
    }

    if (selStart >= 0.0)
    {
        const auto sx0 = (float) frameToX (selStart), sx1 = (float) frameToX (selEnd);
        const auto glow = activity.count (selected) ? activity.at (selected) : 0.0f;
        g.setColour (colourOf (selected).withAlpha (0.07f + glow * 0.05f));
        g.fillRect (juce::Rectangle<float> (sx0, waveArea.getY(), sx1 - sx0, waveArea.getHeight()));
    }

    // Boundaries and labels
    int index = 0;
    for (const auto& c : chops)
    {
        // (Every selected chop alike: the one shown and those chosen with Ctrl.)
        const auto isSelected = std::find (picked.begin(), picked.end(), c.id) != picked.end();
        const auto x0 = (float) frameToX ((double) c.start), x1 = (float) frameToX ((double) c.end);
        const auto myIndex = index++;

        if (x1 < waveArea.getX() - 1.0f || x0 > waveArea.getRight() + 1.0f)
            continue;

        const bool hoverStart = hoverEdge && hoverEdge->chopId == c.id && hoverEdge->edge == ChopEdge::start;
        const bool hoverEnd = hoverEdge && hoverEdge->chopId == c.id && hoverEdge->edge == ChopEdge::end;
        const auto fullHeight = waveArea.getBottom() - labelLane.getY();

        // A new boundary drops in: it grows from the centre line in the accent colour.
        float grow = 1.0f;
        if (const auto b = born.find (c.id); b != born.end())
            grow = Motion::easeOut ((float) ((time - b->second) / 0.35));
        if (grow <= 0.0f)
            continue;

        const auto lineColour = grow < 1.0f ? Colours::accent.withAlpha (0.4f + 0.6f * (1.0f - grow))
                              : isSelected ? Colours::accent : Screen::ink().withAlpha (0.28f);
        const auto lineH = fullHeight * grow;
        const auto lineY = grow < 1.0f ? waveArea.getCentreY() - lineH * 0.5f : labelLane.getY();

        drawHairline (g, { x0, lineY, isSelected || hoverStart || grow < 1.0f ? Metrics::stroke : 1.0f, lineH },
                      hoverStart ? Colours::accent.withAlpha (0.8f) : lineColour);

        // End line only where it is not shared with the next chop's start (or when selected).
        const bool endShared = std::any_of (chops.begin(), chops.end(), [&] (const ChopParams& o) { return o.id != c.id && o.start == c.end; });
        if (isSelected || hoverEnd || ! endShared)
            drawHairline (g, { x1 - 1.0f, labelLane.getY(), isSelected || hoverEnd ? Metrics::stroke : 1.0f, fullHeight },
                          hoverEnd ? Colours::accent.withAlpha (0.8f) : (isSelected ? Colours::accent : Screen::ink().withAlpha (0.18f)));

        // Handle flags on the selected chop.
        if (isSelected)
        {
            g.setColour (Colours::accent);
            g.fillRect (juce::Rectangle<float> (x0, labelLane.getY(), 5.0f, 9.0f));
            g.fillRect (juce::Rectangle<float> (x1 - 5.0f, labelLane.getY(), 5.0f, 9.0f));
        }

        // Label: "03 C#4" (+ LOOP / REV tags) when there is room.
        const auto width = x1 - x0;
        if (width > 24.0f)
        {
            auto lane = juce::Rectangle<float> (x0 + 8.0f, labelLane.getY() + 2.0f, width - 12.0f, labelLane.getHeight() - 4.0f);
            const auto hitLevel = kick.count (c.id) ? kick.at (c.id) : 0.0f;
            const auto indexColour = isSelected ? inkColour (colourOf (c.id)) : colourOf (c.id).withMultipliedAlpha (0.75f + 0.25f * hitLevel);
            drawText (g, chopIndexLabel (myIndex), lane, TextStyle::micro, indexColour.withMultipliedAlpha (grow));
            lane.removeFromLeft (textWidth ("00", TextStyle::micro) + 6.0f);

            if (width > 54.0f && c.note >= 0)
            {
                drawText (g, noteName (c.note), lane, TextStyle::micro, isSelected ? Screen::ink() : Screen::inkFaint());
                lane.removeFromLeft (textWidth ("C#-1", TextStyle::micro) + 6.0f);
            }

            juce::String tags;
            if (c.mode == PlayMode::loop) tags << "LOOP ";
            if (c.reverse)                tags << "REV";
            if (width > 110.0f && tags.isNotEmpty())
                drawText (g, tags.trim(), lane, TextStyle::label, isSelected ? Colours::accent.withAlpha (0.8f) : Screen::inkFaint());
        }
    }
}

void WaveformView::paintEnvelope (juce::Graphics& g)
{
    // The selected chop's envelope (attack, decay to sustain, the fade at its end) as an
    // outline over its waveform, with what it takes away shaded - only when it shapes the
    // sound. Times in source frames (ignores the chop's pitch).
    const auto c = processor.getEffectiveChop (selectedChopId());
    if (! c.has_value() || shownSample == nullptr || c->length() <= 0)
        return;
    if (c->sustain > 0.995f && c->fadeInMs < 3.0f && c->fadeOutMs < 30.0f)
        return;

    const auto rate = shownSample->sampleRate;
    const auto len = (double) c->length();
    const auto attack = std::max (1.0, (double) c->fadeInMs * 0.001 * rate);
    const auto decay = std::max (1.0, (double) c->decayMs * 0.001 * rate);
    const auto endFade = std::min ((double) c->fadeOutMs * 0.001 * rate, len * 0.5);
    const auto sustain = (double) c->sustain;
    auto level = [&] (double t) // t: frames since the chop started playing
    {
        auto e = t < attack ? t / attack : sustain + (1.0 - sustain) * std::pow (0.01, (t - attack) / decay);
        if (len - t < endFade)
            e *= std::max (0.0, (len - t) / std::max (1.0, endFade));
        return juce::jlimit (0.0, 1.0, e);
    };

    const auto x0 = std::max ((double) waveArea.getX(), frameToX ((double) c->start));
    const auto x1 = std::min ((double) waveArea.getRight(), frameToX ((double) c->end));
    if (x1 - x0 < 2.0)
        return;

    const auto centreY = waveArea.getCentreY();
    const auto halfH = waveArea.getHeight() * 0.5f - 6.0f;
    juce::Path top, bottom, shadeTop, shadeBottom;
    shadeTop.startNewSubPath ((float) x0, waveArea.getY());
    shadeBottom.startNewSubPath ((float) x0, waveArea.getBottom());
    bool first = true;
    for (auto x = x0; x <= x1 + 0.5; x += 2.0)
    {
        const auto xx = std::min (x, x1);
        const auto frame = viewStart + (xx - waveArea.getX()) / waveArea.getWidth() * (double) (viewEnd - viewStart);
        const auto t = c->reverse ? (double) c->end - frame : frame - (double) c->start;
        const auto h = (float) level (t) * halfH;
        if (first) { top.startNewSubPath ((float) xx, centreY - h); bottom.startNewSubPath ((float) xx, centreY + h); first = false; }
        else       { top.lineTo ((float) xx, centreY - h); bottom.lineTo ((float) xx, centreY + h); }
        shadeTop.lineTo ((float) xx, centreY - h);
        shadeBottom.lineTo ((float) xx, centreY + h);
    }
    shadeTop.lineTo ((float) x1, waveArea.getY());
    shadeTop.closeSubPath();
    shadeBottom.lineTo ((float) x1, waveArea.getBottom());
    shadeBottom.closeSubPath();

    g.setColour (Screen::bg().withAlpha (0.55f));
    g.fillPath (shadeTop);
    g.fillPath (shadeBottom);
    const auto colour = inkColour (colourOf (c->id));
    for (auto* p : { &top, &bottom })
    {
        g.setColour (colour.withAlpha (0.25f));
        g.strokePath (*p, juce::PathStrokeType (3.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        g.setColour (colour.withAlpha (0.9f));
        g.strokePath (*p, juce::PathStrokeType (1.2f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }
}

void WaveformView::paintWaveform (juce::Graphics& g)
{
    const auto& info = processor.getSampleInfo();
    if (shownSample == nullptr || info.waveform == nullptr)
        return;

    const auto& wf = *info.waveform;
    const auto& audio = shownSample->audio;
    const auto selected = selectedChopId();

    // Auto-gain so quiet samples are still readable (max 4x).
    const auto gain = juce::jlimit (1.0f, 4.0f, 0.92f / std::max (0.0001f, wf.getPeak()));
    const auto centreY = waveArea.getCentreY();
    const auto halfH = waveArea.getHeight() * 0.5f - 6.0f;

    drawHairline (g, { waveArea.getX(), centreY - 0.5f, waveArea.getWidth(), 1.0f }, Screen::hairline().withAlpha (0.7f));

    // Draw at physical-pixel resolution so the waveform stays sharp when the UI is scaled.
    const auto scale = std::max (1.0f, (float) g.getInternalContext().getPhysicalPixelScaleFactor());
    const auto columnWidth = 1.0f / scale;
    const auto columns = (int) (waveArea.getWidth() * scale);
    const auto framesPerColumn = (viewEnd - viewStart) / std::max (1, columns);

    // Columns run left to right and chops are sorted by start: a cursor that only moves
    // forward finds each column's chop (was a search from the first chop per column).
    size_t firstLive = 0;
    auto ownerOf = [this, selected, &firstLive] (int64_t frame)
    {
        while (firstLive < chops.size() && chops[firstLive].end <= frame)
            ++firstLive;
        int owner = 0;
        for (auto i = firstLive; i < chops.size(); ++i)
        {
            const auto& c = chops[i];
            if (c.start > frame) break;
            if (frame < c.end)
            {
                owner = c.id;
                if (owner == selected) break;
            }
        }
        return owner;
    };

    int lastColourOwner = -1;
    juce::Colour ownerColour;
    // Phosphor trails: the stretch each playhead just crossed lights up and cools down.
    struct Trail { float tail, head, level; };
    std::vector<Trail> trails;
    for (size_t i = 0; i < numVoices; ++i)
        if (trailLevel[i] > 0.01f)
            if (const auto tail = (float) frameToX (trailFrames[i]), head = (float) frameToX (trailHead[i]); std::abs (head - tail) > 1.0f)
                trails.push_back ({ tail, head, trailLevel[i] });

    for (int col = 0; col < columns; ++col)
    {
        const auto f0 = viewStart + col * framesPerColumn;
        const auto f1 = f0 + framesPerColumn;
        if (f1 < 0.0 || f0 >= (double) numFrames())
            continue;

        const auto bin = wf.getRange (audio, (int64_t) f0, (int64_t) std::ceil (f1));
        const auto owner = ownerOf ((int64_t) ((f0 + f1) * 0.5));
        const auto x = waveArea.getX() + (float) col * columnWidth;

        // New sample: columns rise out of the centre line, left to right.
        const auto u = (float) col / (float) std::max (1, columns);
        const auto rise = reveal >= 1.0f ? 1.0f : Motion::easeOut ((reveal * 1.6f - u * 0.6f) * 1.4f);
        const auto amp = gain * rise;

        float heat = 0.0f;
        for (const auto& t : trails)
        {
            const auto frac = (x - t.tail) / (t.head - t.tail);
            if (frac >= 0.0f && frac <= 1.0f)
                heat = std::max (heat, frac * frac * t.level);
        }

        // Every chop in its own colour; the selected one brightest. What is playing heats up
        // towards white behind the playhead.
        juce::Colour peakColour, rmsColour;
        if (owner != 0 && owner != lastColourOwner)
        {
            lastColourOwner = owner;
            ownerColour = colourOf (owner);
        }
        if (owner != 0 && owner == selected)
        {
            peakColour = ownerColour.withAlpha (0.55f + 0.35f * heat);
            rmsColour = ownerColour.brighter (0.25f).interpolatedWith (Screen::ink(), 0.5f * heat);
        }
        else if (owner != 0)
        {
            peakColour = ownerColour.withAlpha (0.28f + 0.4f * heat);
            rmsColour = ownerColour.withMultipliedBrightness (0.85f).withAlpha (0.8f).interpolatedWith (Screen::ink(), 0.55f * heat);
        }
        else
        {
            peakColour = Screen::ink().withAlpha (0.09f);
            rmsColour = Screen::ink().withAlpha (0.2f);
        }

        const auto top = centreY - juce::jlimit (0.0f, 1.0f, bin.max * amp) * halfH;
        const auto bottom = centreY - juce::jlimit (-1.0f, 0.0f, bin.min * amp) * halfH;
        g.setColour (peakColour);
        g.fillRect (juce::Rectangle<float> (x, top, columnWidth, std::max (columnWidth, bottom - top)));

        const auto rmsH = juce::jlimit (0.0f, 1.0f, bin.rms * amp) * halfH;
        g.setColour (rmsColour);
        g.fillRect (juce::Rectangle<float> (x, centreY - rmsH, columnWidth, std::max (columnWidth, rmsH * 2.0f)));
    }
}

void WaveformView::paintGrains (juce::Graphics& g)
{
    // GRAIN: show where the sound is taken from.
    //  - a soft band = the area grains can be drawn from (Spray around the read head)
    //  - a thin line per sounding grain, its height and strength following the grain's envelope
    const auto& voices = processor.getEngine().getVoiceDisplay();
    const auto centreY = waveArea.getCentreY();

    for (const auto& v : voices)
    {
        const auto chopId = v.chopId.load (std::memory_order_relaxed);
        const auto amount = v.grainAmount.load (std::memory_order_relaxed);
        const auto n = v.numGrains.load (std::memory_order_acquire);
        if (chopId == 0 || amount < 0.001f || n <= 0)
            continue;

        const auto strength = 0.35f + 0.65f * juce::jlimit (0.0f, 1.0f, amount);

        // Spray band, kept inside the chop it belongs to.
        float chopX0 = waveArea.getX(), chopX1 = waveArea.getRight();
        for (const auto& c : chops)
            if (c.id == chopId)
            {
                chopX0 = (float) frameToX ((double) c.start);
                chopX1 = (float) frameToX ((double) c.end);
            }

        const auto centre = v.sprayCentre.load (std::memory_order_relaxed);
        const auto half = v.sprayHalfWidth.load (std::memory_order_relaxed);
        const auto bx0 = juce::jmax (chopX0, (float) frameToX (centre - half));
        const auto bx1 = juce::jmin (chopX1, (float) frameToX (centre + half));
        const auto cx = (float) frameToX (centre);

        if (bx1 - bx0 > 2.0f)
        {
            g.setColour (Colours::accent.withAlpha (0.10f * strength));
            g.fillRect (juce::Rectangle<float> (bx0, waveArea.getY(), bx1 - bx0, waveArea.getHeight()));
            g.setColour (Colours::accent.withAlpha (0.5f * strength));
            g.fillRect (juce::Rectangle<float> (bx0, waveArea.getY(), 1.0f, waveArea.getHeight()));
            g.fillRect (juce::Rectangle<float> (bx1 - 1.0f, waveArea.getY(), 1.0f, waveArea.getHeight()));
            g.fillRect (juce::Rectangle<float> (bx0, waveArea.getBottom() - 3.0f, bx1 - bx0, 2.0f));
        }

        // Read point (centre of the spray): a short accent tick at the bottom.
        g.setColour (Colours::accent.withAlpha (0.9f * strength));
        g.fillRect (juce::Rectangle<float> (cx - 0.75f, waveArea.getBottom() - 9.0f, 1.5f, 9.0f));

        // Individual grains.
        for (int i = 0; i < std::min (n, (int) v.grainPosition.size()); ++i)
        {
            const auto pos = v.grainPosition[(size_t) i].load (std::memory_order_relaxed);
            const auto level = juce::jlimit (0.0f, 1.0f, v.grainLevel[(size_t) i].load (std::memory_order_relaxed));
            const auto x = (float) frameToX (pos);
            if (x < waveArea.getX() || x > waveArea.getRight())
                continue;

            // A halo in the background colour keeps grains readable on top of the waveform
            // (including the selected chop, which is drawn in the accent colour).
            const auto h = waveArea.getHeight() * (0.15f + 0.55f * level) * 0.5f;
            const auto alpha = (0.35f + 0.65f * level) * strength;
            const auto line = juce::Rectangle<float> (x - 0.75f, centreY - h, 1.5f, h * 2.0f);
            const auto cap = juce::Rectangle<float> (5.0f, 5.0f).withCentre ({ x, centreY - h });

            g.setColour (Screen::bg().withAlpha (0.85f * alpha));
            g.fillRect (line.expanded (1.25f, 0.0f));
            g.fillEllipse (cap.expanded (1.5f));
            g.setColour (Colours::accent.withAlpha (alpha));
            g.fillRect (line);
            g.fillEllipse (cap);
        }
    }
}

void WaveformView::paintPlayheads (juce::Graphics& g)
{
    const auto accent = Colours::accent;
    const auto centreY = waveArea.getCentreY();
    const auto halfH = waveArea.getHeight() * 0.5f - 6.0f;

    // Trail wash behind each playhead (fades out after the voice stops).
    for (size_t i = 0; i < numVoices; ++i)
    {
        if (trailLevel[i] <= 0.01f)
            continue;
        // At most 160 px long: further back the wash is nearly invisible, and a gradient over
        // the whole height costs per pixel.
        const auto head = (float) frameToX (trailHead[i]);
        const auto tail = juce::jlimit (head - 160.0f, head + 160.0f, (float) frameToX (trailFrames[i]));
        if (std::abs (head - tail) < 1.0f)
            continue;

        juce::ColourGradient wash (accent.withAlpha (0.0f), tail, 0.0f, accent.withAlpha (0.16f * trailLevel[i]), head, 0.0f, false);
        g.setGradientFill (wash);
        g.fillRect (juce::Rectangle<float>::leftTopRightBottom (std::min (tail, head), waveArea.getY(), std::max (tail, head), waveArea.getBottom()));
    }

    // Sparks shed by the read heads.
    for (const auto& s : sparks)
    {
        const auto t = s.life / s.maxLife; // 1 = just born
        const auto size = s.size * (0.5f + 0.5f * t);
        g.setColour (accent.withAlpha (0.55f * t));
        g.fillEllipse (juce::Rectangle<float> (size, size).withCentre ({ s.x, s.y }));
    }

    // SAMPLE selected next to PLAY and nothing playing: the sample's playhead waits where PLAY
    // begins (the selected chop's start, or where it was put). While it is dragged it follows
    // the mouse. (With CHOP, a playhead only shows while a chop plays.)
    const auto anyPlaying = std::any_of (playheadChops.begin(), playheadChops.end(), [] (int id) { return id != 0; });
    if (cursorShown() && (dragMode == DragMode::playhead || ! anyPlaying))
    {
        const auto frame = samplePlayheadFrame();
        const auto x = (float) frameToX (frame);
        if (x >= waveArea.getX() - 1.0f && x <= waveArea.getRight())
        {
            const auto top = waveArea.getY(), bottom = waveArea.getBottom();
            const auto centreT = (centreY - top) / std::max (1.0f, bottom - top);
            const auto xx = std::max (x, waveArea.getX() + 1.0f);
            juce::ColourGradient beam (accent.withAlpha (0.0f), xx, top, accent.withAlpha (0.0f), xx, bottom, false);
            beam.addColour (centreT, accent.withAlpha (0.12f));
            g.setGradientFill (beam);
            g.fillRect (juce::Rectangle<float> (xx - 4.0f, top, 8.0f, bottom - top));
            g.setColour (accent.withAlpha (0.8f));
            g.fillRect (juce::Rectangle<float> (xx - 0.75f, top, 1.5f, bottom - top));
            juce::Path tri; // the handle (drag it)
            tri.addTriangle (xx - 5.5f, top, xx + 5.5f, top, xx, top + 8.0f);
            g.setColour (accent);
            g.fillPath (tri);

            const auto text = juce::String (frame / shownSample->sampleRate, 3) + "s";
            const auto w = textWidth (text, TextStyle::micro) + 4.0f;
            const auto left = xx + 9.0f + w > waveArea.getRight() ? xx - 9.0f - w : xx + 9.0f;
            drawText (g, text, { left, bottom - 16.0f, w, 12.0f }, TextStyle::micro, accent.withAlpha (0.8f));
        }
    }

    for (size_t i = 0; i < playheadChops.size(); ++i)
    {
        if (playheadChops[i] == 0)
            continue;

        const auto x = (float) frameToX (playheadFrames[i]);
        if (x < waveArea.getX() || x > waveArea.getRight())
            continue;

        const auto level = juce::jlimit (0.0f, 1.0f, processor.getEngine().getVoiceDisplay()[i].level.load (std::memory_order_relaxed));
        const auto top = waveArea.getY(), bottom = waveArea.getBottom();
        const auto centreT = (centreY - top) / std::max (1.0f, bottom - top);

        // Beam of light (like GRAIN's read head): brightest where the sound is, fading
        // towards the top and bottom; it breathes with the voice level.
        juce::ColourGradient beam (accent.withAlpha (0.0f), x, top, accent.withAlpha (0.0f), x, bottom, false);
        beam.addColour (centreT, accent.withAlpha (0.14f + 0.18f * level));
        g.setGradientFill (beam);
        g.fillRect (juce::Rectangle<float> (x - 5.0f, top, 10.0f, bottom - top));

        juce::ColourGradient core (accent.withAlpha (0.25f), x, top, accent.withAlpha (0.25f), x, bottom, false);
        core.addColour (centreT, accent);
        g.setGradientFill (core);
        g.fillRect (juce::Rectangle<float> (x - 0.75f, top, 1.5f, bottom - top));

        // Blooms where the beam crosses the waveform.
        for (const auto y : { centreY - headTop[i] * halfH, centreY + headBottom[i] * halfH })
        {
            const auto r = 2.5f + 3.5f * level;
            g.setGradientFill (juce::ColourGradient (accent.withAlpha (0.4f), x, y, accent.withAlpha (0.0f), x + r * 2.2f, y, true));
            g.fillEllipse (juce::Rectangle<float> (r * 4.4f, r * 4.4f).withCentre ({ x, y }));
            g.setColour (accent);
            g.fillEllipse (juce::Rectangle<float> (5.0f, 5.0f).withCentre ({ x, y }));
            g.setColour (juce::Colours::white.withAlpha (0.9f));
            g.fillEllipse (juce::Rectangle<float> (2.0f, 2.0f).withCentre ({ x, y }));
        }

        juce::Path tri;
        tri.addTriangle (x - 4.0f, top, x + 4.0f, top, x, top + 6.0f);
        g.setColour (accent);
        g.fillPath (tri);

        // HUD readout: time into the chop (the whole sample: into the sample).
        if (playheadChops[i] == SamplerEngine::wholeSampleId && shownSample != nullptr)
        {
            const auto text = juce::String (playheadFrames[i] / shownSample->sampleRate, 3) + "s";
            const auto w = textWidth (text, TextStyle::micro) + 4.0f;
            const auto left = x + 9.0f + w > waveArea.getRight() ? x - 9.0f - w : x + 9.0f;
            drawText (g, text, { left, bottom - 16.0f, w, 12.0f }, TextStyle::micro, accent);
        }
        for (const auto& c : chops)
            if (c.id == playheadChops[i] && shownSample != nullptr)
            {
                const auto into = std::abs (playheadFrames[i] - (double) (c.reverse ? c.end : c.start)) / shownSample->sampleRate;
                const auto text = juce::String (into, 3) + "s";
                const auto w = textWidth (text, TextStyle::micro) + 4.0f;
                const auto left = x + 9.0f + w > waveArea.getRight() ? x - 9.0f - w : x + 9.0f;
                drawText (g, text, { left, bottom - 16.0f, w, 12.0f }, TextStyle::micro, accent);
                break;
            }
    }
}

void WaveformView::paintRuler (juce::Graphics& g)
{
    if (shownSample == nullptr)
        return;

    drawHairline (g, { rulerArea.getX(), rulerArea.getY(), rulerArea.getWidth(), 1.0f }, Screen::hairline());

    // A "nice" tick spacing in seconds with roughly 90 px between labels.
    const auto secondsPerPixel = (viewEnd - viewStart) / shownSample->sampleRate / std::max (1.0f, rulerArea.getWidth());
    const double candidates[] = { 0.001, 0.002, 0.005, 0.01, 0.02, 0.05, 0.1, 0.2, 0.5, 1, 2, 5, 10, 15, 30, 60, 120 };
    double spacing = 120.0;
    for (auto c : candidates)
        if (c / secondsPerPixel >= 90.0) { spacing = c; break; }

    const auto startSec = viewStart / shownSample->sampleRate;
    const auto endSec = viewEnd / shownSample->sampleRate;
    const int decimals = spacing < 0.01 ? 3 : (spacing < 0.1 ? 2 : (spacing < 1.0 ? 1 : 0));

    for (double t = std::ceil (startSec / spacing) * spacing; t <= endSec; t += spacing)
    {
        const auto x = (float) frameToX (t * shownSample->sampleRate);
        drawHairline (g, { x, rulerArea.getY(), 1.0f, 4.0f }, Screen::line());
        const auto mins = (int) (t / 60.0);
        const auto label = mins > 0 ? juce::String (mins) + ":" + juce::String (t - mins * 60.0, decimals).paddedLeft ('0', decimals > 0 ? 3 + decimals : 2)
                                    : juce::String (t, decimals) + "s";
        drawText (g, label, { x + 3.0f, rulerArea.getY() + 1.0f, 60.0f, rulerArea.getHeight() - 1.0f }, TextStyle::micro, Screen::inkFaint());
    }
}

int WaveformView::overviewEdgeAt (float x) const
{
    const auto total = (double) numFrames();
    if (shownSample == nullptr || total <= 0.0)
        return 0;
    const auto vx0 = overviewArea.getX() + (float) (viewStart / total) * overviewArea.getWidth();
    const auto vx1 = overviewArea.getX() + (float) (viewEnd / total) * overviewArea.getWidth();
    constexpr float reach = 6.0f;
    const auto d0 = std::abs (x - vx0), d1 = std::abs (x - vx1);
    if (d0 <= reach && d0 <= d1) return -1;
    if (d1 <= reach) return 1;
    return 0;
}

void WaveformView::paintOverview (juce::Graphics& g)
{
    const auto& info = processor.getSampleInfo();
    g.setColour (Colours::surface);
    g.fillRect (overviewArea);

    if (shownSample == nullptr || info.waveform == nullptr)
    {
        // No sample yet: a quiet flat line where the overview will be.
        drawHairline (g, { overviewArea.getX() + 6.0f, overviewArea.getCentreY() - 0.5f, overviewArea.getWidth() - 12.0f, 1.0f },
                      Colours::text.withAlpha (0.12f));
        return;
    }

    const auto total = (double) numFrames();
    const auto cols = (int) overviewArea.getWidth();
    const auto gain = juce::jlimit (1.0f, 4.0f, 0.92f / std::max (0.0001f, info.waveform->getPeak()));
    const auto cy = overviewArea.getCentreY();
    const auto h = overviewArea.getHeight() * 0.5f - 2.0f;

    g.setColour (Colours::text.withAlpha (0.35f));
    for (int x = 0; x < cols; ++x)
    {
        const auto f0 = (int64_t) (total * x / cols), f1 = (int64_t) (total * (x + 1) / cols);
        const auto bin = info.waveform->getRange (shownSample->audio, f0, std::max (f0 + 1, f1));
        const auto a = juce::jlimit (0.0f, 1.0f, std::max (std::abs (bin.min), std::abs (bin.max)) * gain) * h;
        g.fillRect (juce::Rectangle<float> (overviewArea.getX() + (float) x, cy - a, 1.0f, std::max (1.0f, a * 2.0f)));
    }

    // Chop starts as tiny ticks
    g.setColour (Colours::textDim.withAlpha (0.6f));
    for (const auto& c : chops)
        g.fillRect (juce::Rectangle<float> (overviewArea.getX() + (float) ((double) c.start / total) * overviewArea.getWidth(), overviewArea.getY(), 1.0f, 3.0f));

    // Visible window, with a grip at each end: drag one to zoom in or out (the middle pans).
    const auto vx0 = overviewArea.getX() + (float) (viewStart / total) * overviewArea.getWidth();
    const auto vx1 = overviewArea.getX() + (float) (viewEnd / total) * overviewArea.getWidth();
    const auto zoomed = vx1 - vx0 < overviewArea.getWidth() - 1.0f;
    if (zoomed)
    {
        g.setColour (Colours::background.withAlpha (0.55f));
        g.fillRect (overviewArea.withRight (vx0));
        g.fillRect (overviewArea.withLeft (vx1));
        g.setColour (Colours::textDim);
        g.drawRect (juce::Rectangle<float> (vx0, overviewArea.getY(), std::max (2.0f, vx1 - vx0), overviewArea.getHeight()), 1.0f);
    }
    for (const auto side : { -1, 1 })
    {
        const auto x = side < 0 ? vx0 : vx1;
        const auto active = (dragMode == DragMode::overviewLeft && side < 0) || (dragMode == DragMode::overviewRight && side > 0);
        const auto hover = overviewHover == side;
        if (! zoomed && ! hover && ! active)
            continue; // not zoomed: the grips show when the mouse comes near
        const auto grip = juce::Rectangle<float> (4.0f, overviewArea.getHeight() - 4.0f)
                              .withCentre ({ juce::jlimit (overviewArea.getX() + 2.0f, overviewArea.getRight() - 2.0f, x), overviewArea.getCentreY() });
        g.setColour (active || hover ? Colours::accent : Colours::textDim);
        g.fillRoundedRectangle (grip, 1.5f);
    }
}

void WaveformView::paintDragReadout (juce::Graphics& g)
{
    if (dragMode != DragMode::edge || shownSample == nullptr)
        return;

    const auto deltaMs = (double) (dragCurrentFrame - dragOriginalFrame) / shownSample->sampleRate * 1000.0;
    juce::String text = formatTime ((double) dragCurrentFrame) + "   " + (deltaMs >= 0 ? "+" : "") + juce::String (deltaMs, 1) + " ms";
    if (dragSnapKind.isNotEmpty())
        text << "   " << dragSnapKind;
    if (! dragLinked)
        text << "   UNLINKED";

    const auto w = textWidth (text, TextStyle::micro) + 16.0f;
    const auto x = juce::jlimit (waveArea.getX() + 4.0f, waveArea.getRight() - w - 4.0f, (float) frameToX ((double) dragCurrentFrame) + 8.0f);
    const auto box = juce::Rectangle<float> (x, waveArea.getBottom() - 26.0f, w, 18.0f);
    g.setColour (Screen::raised());
    g.fillRect (box);
    g.setColour (Screen::line());
    g.drawRect (box, 1.0f);
    drawText (g, text, box, TextStyle::micro, Screen::ink(), juce::Justification::centred);
}

//==============================================================================
void WaveformView::mouseMove (const juce::MouseEvent& e)
{
    if (const auto edge = overviewArea.contains (e.position) ? overviewEdgeAt (e.position.x) : 0; edge != overviewHover)
    {
        overviewHover = edge;
        repaint (overviewArea.toNearestInt().expanded (2));
    }
    mousePos = e.position;
    mouseInside = true;
    const auto hit = (shownSample != nullptr && ! knifeMode) ? hitTestEdge (e.position) : std::nullopt;
    const bool changed = hit.has_value() != hoverEdge.has_value()
                      || (hit && hoverEdge && (hit->chopId != hoverEdge->chopId || hit->edge != hoverEdge->edge));
    hoverEdge = hit;
    updateMouseCursor();

    if (shownSample == nullptr)
        setMouseCursor (juce::MouseCursor::PointingHandCursor);
    else if (overviewArea.contains (e.position))
        setMouseCursor (overviewEdgeAt (e.position.x) != 0 ? juce::MouseCursor::LeftRightResizeCursor : juce::MouseCursor::DraggingHandCursor);
    else if ((cursorShown() || targetPlaying()) && ! knifeMode && waveArea.contains (e.position)
             && std::abs (e.position.x - (float) frameToX (samplePlayheadFrame())) <= 6.0f
             && (e.position.y <= waveArea.getY() + 14.0f || ! hoverEdge))
        setMouseCursor (juce::MouseCursor::DraggingHandCursor); // the SAMPLE playhead: drag it
    else if (hoverEdge)
        setMouseCursor (juce::MouseCursor::LeftRightResizeCursor);
    else if (knifeMode)
        setMouseCursor (juce::MouseCursor::CrosshairCursor);
    else
        setMouseCursor (juce::MouseCursor::NormalCursor);

    if (changed)
        repaint();
}

void WaveformView::mouseExit (const juce::MouseEvent&)
{
    mouseInside = false;
    if (overviewHover != 0) { overviewHover = 0; repaint (overviewArea.toNearestInt().expanded (2)); }
    if (hoverEdge)
    {
        hoverEdge.reset();
        repaint();
    }
}

void WaveformView::mouseDown (const juce::MouseEvent& e)
{
    if (shownSample == nullptr)
    {
        if (onLoadRequested) onLoadRequested();
        return;
    }

    const auto frame = (int64_t) xToFrame (e.position.x);

    if (e.mods.isPopupMenu())
    {
        if (const auto id = processor.getChops().chopIdAt (frame))
            selectChop (id);
        showContextMenu (frame);
        return;
    }

    if (overviewArea.contains (e.position))
    {
        // An end of the window: zoom by dragging it; anywhere else: move the window there.
        const auto edge = overviewEdgeAt (e.position.x);
        dragMode = edge < 0 ? DragMode::overviewLeft : (edge > 0 ? DragMode::overviewRight : DragMode::overview);
        if (edge == 0)
            mouseDrag (e);
        return;
    }

    clickOnSelected = false;

    // Grab the playhead PLAY starts at, or the one playing (its handle at the top, or the line where no
    // chop boundary is in the way).
    if ((cursorShown() || targetPlaying()) && ! knifeMode && waveArea.contains (e.position)
        && std::abs (e.position.x - (float) frameToX (samplePlayheadFrame())) <= 6.0f
        && (e.position.y <= waveArea.getY() + 14.0f || ! hitTestEdge (e.position)))
    {
        dragMode = DragMode::playhead;
        playheadDragFrame = playheadFrameAt (e.position.x, ! e.mods.isShiftDown());
        repaint();
        return;
    }

    if (! knifeMode)
    {
        if (const auto hit = hitTestEdge (e.position))
        {
            dragMode = DragMode::edge;
            dragEdge = *hit;
            dragLinked = ! e.mods.isAltDown();
            const auto c = processor.getChops().getChop (hit->chopId);
            dragOriginalFrame = dragCurrentFrame = c ? (hit->edge == ChopEdge::start ? c->start : c->end) : 0;
            selectChop (hit->chopId);
            processor.getUndoManager().beginNewTransaction ("Move chop boundary");
            return;
        }
    }

    if (knifeMode && waveArea.contains (e.position))
    {
        juce::String kind;
        const auto at = snapFrame (frame, ! e.mods.isShiftDown(), kind);
        processor.getUndoManager().beginNewTransaction ("Split chop");
        if (const auto newId = splitAtFrame (at))
            selectChop (newId);
        return;
    }

    if (const auto id = processor.getChops().chopIdAt (frame); id != 0 && e.mods.isCommandDown())
    {
        // Ctrl (Cmd on a Mac) + click: the chop joins the selection, or leaves it.
        processor.toggleChopSelection (id);
        repaint();
        return;
    }

    if (const auto id = processor.getChops().chopIdAt (frame))
    {
        // The first click selects the chop (the playhead goes to its start), the
        // next ones put the playhead where they land (on release, if it was no pan).
        clickOnSelected = id == selectedChopId() && waveArea.contains (e.position);
        clickFrame = frame;
        selectChop (id);
        if (onChopClicked) onChopClicked (id);
    }

    dragMode = DragMode::pan;
    panStartViewStart = viewStart;
    panStartX = e.position.x;
}

void WaveformView::mouseDrag (const juce::MouseEvent& e)
{
    mousePos = e.position;

    switch (dragMode)
    {
        case DragMode::edge:
        {
            // Auto-scroll while dragging past either side of the view.
            const auto span = viewEnd - viewStart;
            if (e.position.x < waveArea.getX())          setViewNow (viewStart - span * 0.02, viewStart - span * 0.02 + span);
            else if (e.position.x > waveArea.getRight()) setViewNow (viewStart + span * 0.02, viewStart + span * 0.02 + span);

            dragLinked = ! e.mods.isAltDown();
            const auto raw = (int64_t) xToFrame (juce::jlimit (waveArea.getX(), waveArea.getRight(), e.position.x));
            dragCurrentFrame = snapFrame (raw, ! e.mods.isShiftDown(), dragSnapKind);
            processor.getChops().moveEdge (dragEdge.chopId, dragEdge.edge, dragCurrentFrame, dragLinked);

            if (const auto c = processor.getChops().getChop (dragEdge.chopId))
                dragCurrentFrame = dragEdge.edge == ChopEdge::start ? c->start : c->end; // after clamping
            repaint();
            break;
        }

        case DragMode::pan:
        {
            if (e.getDistanceFromDragStart() < 3)
                break;
            const auto framesPerPixel = (viewEnd - viewStart) / std::max (1.0f, waveArea.getWidth());
            const auto span = viewEnd - viewStart;
            const auto start = panStartViewStart - (e.position.x - panStartX) * framesPerPixel;
            setViewNow (start, start + span);
            setMouseCursor (juce::MouseCursor::DraggingHandCursor);
            break;
        }

        case DragMode::overview:
        {
            const auto total = (double) numFrames();
            const auto span = viewEnd - viewStart;
            const auto centre = (e.position.x - overviewArea.getX()) / std::max (1.0f, overviewArea.getWidth()) * total;
            setViewNow (centre - span * 0.5, centre + span * 0.5);
            break;
        }

        case DragMode::overviewLeft:
        case DragMode::overviewRight:
        {
            // Drag an end of the window: the big waveform zooms in or out around the other end.
            const auto total = (double) numFrames();
            const auto frame = juce::jlimit (0.0, total, (double) (e.position.x - overviewArea.getX()) / std::max (1.0f, overviewArea.getWidth()) * total);
            const auto smallest = std::max (64.0, total * 0.002);
            if (dragMode == DragMode::overviewLeft)
                setViewNow (std::min (frame, viewEnd - smallest), viewEnd);
            else
                setViewNow (viewStart, std::max (frame, viewStart + smallest));
            break;
        }

        case DragMode::playhead:
        {
            const auto x = juce::jlimit (waveArea.getX(), waveArea.getRight(), e.position.x);
            playheadDragFrame = playheadFrameAt (x, ! e.mods.isShiftDown());
            repaint();
            break;
        }

        case DragMode::none:
            break;
    }
}

void WaveformView::mouseUp (const juce::MouseEvent& e)
{
    if (dragMode == DragMode::playhead)
        placeSampleCursor (playheadDragFrame);
    else if (dragMode == DragMode::pan && clickOnSelected && e.getDistanceFromDragStart() < 3)
        placeSampleCursor (clickFrame);
    clickOnSelected = false;
    dragMode = DragMode::none;
    dragSnapKind = {};
    mouseMove (e);
    repaint();
}

void WaveformView::mouseDoubleClick (const juce::MouseEvent& e)
{
    if (shownSample == nullptr || knifeMode || ! waveArea.contains (e.position) || hitTestEdge (e.position))
        return;

    juce::String kind;
    const auto at = snapFrame ((int64_t) xToFrame (e.position.x), ! e.mods.isShiftDown(), kind);
    processor.getUndoManager().beginNewTransaction ("Split chop");
    if (const auto newId = splitAtFrame (at))
        selectChop (newId);
}

int WaveformView::splitAtFrame (int64_t at)
{
    auto& model = processor.getChops();
    const auto id = model.splitAt (at);
    if (id == 0 && model.getNumChops() >= model.chopLimit())
        processor.setStatusMessage ("Every key has a chop (" + juce::String (model.chopLimit()) + "): merge or delete one to cut again");
    return id;
}

void WaveformView::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel)
{
    if (shownSample == nullptr)
        return;

    const auto horizontal = std::abs (wheel.deltaX) > std::abs (wheel.deltaY) || e.mods.isShiftDown();

    if (horizontal)
    {
        const auto amount = std::abs (wheel.deltaX) > 0.0f ? wheel.deltaX : wheel.deltaY;
        const auto span = targetEnd - targetStart;
        const auto start = targetStart - amount * span * 0.25;
        animateViewTo (start, start + span);
        return;
    }

    const auto factor = std::pow (0.8, (double) wheel.deltaY * (wheel.isReversed ? -1.0 : 1.0) * 4.0);
    zoomBy (factor, xToFrame (e.position.x));
}

void WaveformView::mouseMagnify (const juce::MouseEvent& e, float scaleFactor)
{
    if (shownSample != nullptr && scaleFactor > 0.0f)
        zoomBy (1.0 / scaleFactor, xToFrame (e.position.x));
}

void WaveformView::showContextMenu (int64_t frame)
{
    auto& model = processor.getChops();
    auto& um = processor.getUndoManager();
    const auto selected = selectedChopId();
    const auto chop = processor.getEffectiveChop (selected);

    juce::PopupMenu menu;
    menu.addItem ("Split here", [this, frame, &um]
    {
        juce::String kind;
        um.beginNewTransaction ("Split chop");
        if (const auto id = splitAtFrame (snapFrame (frame, true, kind)))
            selectChop (id);
    });

    if (chop.has_value())
    {
        menu.addItem ("Merge with next", [selected, &model, &um] { um.beginNewTransaction ("Merge chops"); model.mergeWithNext (selected); });
        menu.addItem ("Delete chop", [this, selected, &model, &um]
        {
            um.beginNewTransaction ("Delete chop");
            model.removeChop (selected);
            selectChop (model.getNumChops() > 0 ? model.getChops().front().id : 0);
        });
        menu.addSeparator();

        juce::PopupMenu modes;
        const juce::StringArray modeNames { "One-shot", "Gate", "Loop" };
        for (int m = 0; m < 3; ++m)
            modes.addItem (modeNames[m], true, (int) chop->mode == m, [this, selected, m]
            {
                processor.setChopSound (selected, IDs::mode, (float) m);
            });
        menu.addSubMenu ("Play mode", modes);
        menu.addItem ("Reverse", true, chop->reverse, [this, selected, rev = chop->reverse]
        {
            processor.setChopSound (selected, IDs::reverse, rev ? 0.0f : 1.0f);
        });

        juce::PopupMenu choke;
        choke.addItem ("None", true, chop->chokeGroup == 0, [selected, &model, &um] { um.beginNewTransaction ("Choke group"); model.setParam (selected, IDs::choke, 0); });
        for (int grp = 1; grp <= ChopLimits::maxChokeGroup; ++grp)
            choke.addItem ("Group " + juce::String (grp), true, chop->chokeGroup == grp,
                           [selected, grp, &model, &um] { um.beginNewTransaction ("Choke group"); model.setParam (selected, IDs::choke, grp); });
        menu.addSubMenu ("Choke group", choke);

        juce::PopupMenu notes;
        for (int octave = -1; octave <= 8; ++octave)
        {
            juce::PopupMenu oct;
            for (int n = 0; n < 12; ++n)
            {
                const auto note = (octave + 1) * 12 + n;
                if (note > 127) break;
                const auto usedBy = model.chopIdForNote (note);
                oct.addItem (noteName (note) + (usedBy != 0 && usedBy != selected ? "  (swap)" : ""), true, chop->note == note,
                             [selected, note, &model, &um] { um.beginNewTransaction ("Assign note"); model.setNote (selected, note); });
            }
            notes.addSubMenu ("Octave " + juce::String (octave), oct);
        }
        menu.addSubMenu ("MIDI note", notes);
        menu.addSeparator();
        menu.addItem ("Zoom to chop", [this] { zoomToSelection(); });
    }

    menu.addItem ("Zoom to fit", [this] { zoomToFit(); });
    menu.addSeparator();
    menu.addItem ("Remap notes by position", [&model, &um] { um.beginNewTransaction ("Remap notes"); model.remapByPosition(); });

    menu.setLookAndFeel (&getLookAndFeel()); // the plugin's menu style (menus use the default look otherwise)

    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this).withMousePosition());
}

} // namespace kerf::ui

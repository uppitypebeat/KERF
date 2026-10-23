#include "ChopStrip.h"

namespace kerf::ui
{

juce::String noteName (int midiNote)
{
    // As FL Studio names them: MIDI 60 (middle C) = C5.
    return midiNote >= 0 && midiNote <= 127 ? juce::MidiMessage::getMidiNoteName (midiNote, true, true, kerf::middleCOctave) : juce::String ("--");
}

ChopStrip::ChopStrip (KerfAudioProcessor& p, AnimationClock& c) : processor (p), clock (c)
{
    processor.getChops().getState().addListener (this);
    processor.getSettings().addListener (this);
    processor.addChangeListener (this);
    clock.add (this);

    addAndMakeVisible (rootField);
    rootField.formatter = [] (double v) { return noteName ((int) v); };
    rootField.setTooltip ("MIDI note of the first chop. Drag, or double-click to type");
    rootField.onChange = [this] (double v)
    {
        processor.getUndoManager().beginNewTransaction ("Root note");
        processor.getChops().setRootNote ((int) v);
        processor.getChops().remapByPosition();
    };

    addAndMakeVisible (remapButton);
    remapButton.setTooltip ("Give the chops notes in order, from the root note up");
    remapButton.onClick = [this]
    {
        processor.getUndoManager().beginNewTransaction ("Remap notes");
        processor.getChops().remapByPosition();
    };

    addAndMakeVisible (kitButton);
    kitButton.setTooltip ("Save every chop as its own WAV file");
    kitButton.onClick = [this] { showKitMenu(); };

    for (size_t i = 0; i < lastTriggers.size(); ++i) // only hits from now on animate
        lastTriggers[i] = processor.getEngine().getVoiceDisplay()[i].triggers.load (std::memory_order_relaxed);

    refresh();
}

ChopStrip::~ChopStrip()
{
    releasePreview();
    clock.remove (this);
    processor.removeChangeListener (this);
    processor.getSettings().removeListener (this);
    processor.getChops().getState().removeListener (this);
}

void ChopStrip::refresh()
{
    chops = processor.getChops().getChops();
    for (auto& c : chops)
        c = processor.effectiveChop (c); // show what plays (pad settings included)
    rootField.setValue (processor.getChops().getRootNote());
    const auto maxScroll = std::max (0.0f, (float) chops.size() * (padWidth + padGap) - padGap - padArea.getWidth());
    scrollTarget = juce::jlimit (0.0f, maxScroll, scrollTarget);
    rebuildThumbs();
    repaint();
}

void ChopStrip::rebuildThumbs()
{
    const auto& info = processor.getSampleInfo();
    if (info.sample != thumbSample)
        thumbs.clear();
    thumbSample = info.sample;

    if (info.sample == nullptr || info.waveform == nullptr)
        return;

    constexpr int columns = (int) padWidth - 10;
    const auto gain = juce::jlimit (1.0f, 4.0f, 0.92f / std::max (0.0001f, info.waveform->getPeak()));

    std::map<int, std::vector<float>> next;
    for (const auto& c : chops)
    {
        auto& t = next[c.id];
        t.resize ((size_t) columns);
        const auto len = (double) std::max<int64_t> (1, c.end - c.start);
        for (int x = 0; x < columns; ++x)
        {
            const auto f0 = c.start + (int64_t) (len * x / columns);
            const auto f1 = std::max (f0 + 1, c.start + (int64_t) (len * (x + 1) / columns));
            const auto bin = info.waveform->getRange (info.sample->audio, f0, f1);
            t[(size_t) x] = juce::jlimit (0.0f, 1.0f, std::max (std::abs (bin.min), std::abs (bin.max)) * gain);
        }
    }
    thumbs = std::move (next);
}

void ChopStrip::resized()
{
    auto r = getLocalBounds().toFloat();
    rootArea = r.removeFromRight (150.0f);
    r.removeFromRight (12.0f);
    padArea = r;

    // [ROOT] [C5]  [REMAP]
    //              [KIT]
    auto root = rootArea.toNearestInt();
    auto buttons = root.removeFromRight (60);
    const auto top = buttons.getCentreY() - 23;
    remapButton.setBounds (buttons.getX(), top, 60, 20);
    kitButton.setBounds (buttons.getX(), top + 26, 60, 20);
    root.removeFromRight (10);
    rootField.setBounds (root.removeFromRight (34).withSizeKeepingCentre (34, 18));
}

juce::Rectangle<float> ChopStrip::padBounds (int index) const
{
    return { padArea.getX() + (float) index * (padWidth + padGap) - scrollX, padArea.getY() + 2.0f, padWidth, padArea.getHeight() - 4.0f };
}

int ChopStrip::padAt (juce::Point<float> p) const
{
    if (! padArea.contains (p))
        return -1;
    for (int i = 0; i < (int) chops.size(); ++i)
        if (padBounds (i).contains (p))
            return i;
    return -1;
}

void ChopStrip::ensurePadVisible (int index)
{
    // Measured against where the strip is scrolling to, so it glides there.
    const auto x = padArea.getX() + (float) index * (padWidth + padGap) - scrollTarget;
    if (x < padArea.getX())
        scrollTarget -= padArea.getX() - x;
    else if (x + padWidth > padArea.getRight())
        scrollTarget += x + padWidth - padArea.getRight();
    repaint();
}

void ChopStrip::paint (juce::Graphics& g)
{
    // Root note label (sits left of the root field)
    drawText (g, "ROOT", rootArea.withRight ((float) rootField.getX() - 6.0f), TextStyle::label, Colours::textDim, juce::Justification::centredRight);

    if (chops.empty())
    {
        drawText (g, "Chops appear here after loading a sample", padArea, TextStyle::label, Colours::textFaint, juce::Justification::centredLeft);
        return;
    }

    juce::Graphics::ScopedSaveState save (g);
    g.reduceClipRegion (padArea.toNearestInt());

    const auto selected = (int) processor.getSettings().getProperty ("selectedChop", 0);
    const auto picked = processor.getSelectedChops(); // the one shown, and those chosen with Ctrl

    for (int i = 0; i < (int) chops.size(); ++i)
    {
        const auto& c = chops[(size_t) i];
        const auto hit = kick.count (c.id) ? kick.at (c.id) : 0.0f;
        const auto b = padBounds (i);
        if (b.getRight() < padArea.getX() || b.getX() > padArea.getRight())
            continue;

        const auto isSelected = std::find (picked.begin(), picked.end(), c.id) != picked.end(); // (Ctrl: several)
        const auto lit = glow.count (c.id) ? glow.at (c.id) : 0.0f;

        // Body
        {
            Face::State face; // the shared key face
            face.hover = i == hovered;
            Face::key (g, b, face);
        }

        // The chop's colour: a bar along the top (always), and a wash + brighter bar while it
        // plays, plus a flash on every hit. Text stays readable.
        const auto colour = chopColour (c.id, chops.front().id);
        {
            const auto wash = (lit > 0.001f ? 0.06f + 0.16f * lit : 0.0f) + 0.22f * hit;
            const auto bar = (isSelected ? 0.9f : 0.6f) + 0.4f * lit + hit;
            if (wash > 0.0f)
            {
                g.setColour (colour.withAlpha (juce::jmin (1.0f, wash)));
                g.fillRoundedRectangle (b, Metrics::radiusSmall);
            }
            g.setColour (colour.withAlpha (juce::jmin (1.0f, bar)));
            g.fillRect (b.reduced (1.0f, 0.0f).withHeight (2.5f));
        }

        // Mini waveform of the chop; the part already played lights up behind the playhead.
        if (const auto t = thumbs.find (c.id); t != thumbs.end() && ! t->second.empty())
        {
            const auto area = b.reduced (5.0f, 9.0f);
            const auto n = (int) t->second.size();
            const auto colW = area.getWidth() / (float) n;
            const auto pos = playPos.count (c.id) ? playPos.at (c.id) : -1.0f;
            const auto cy = area.getCentreY();
            const auto rest = lightTheme() ? (isSelected ? 0.8f : 0.6f) : (isSelected ? 0.45f : 0.3f); // colour on white needs more

            for (int x = 0; x < n; ++x)
            {
                const auto u = ((float) x + 0.5f) / (float) n;
                const bool played = pos >= 0.0f && (c.reverse ? u >= pos : u <= pos);
                const auto h = std::max (0.5f, t->second[(size_t) x] * area.getHeight() * 0.5f);
                g.setColour (played ? inkColour (colour, 0.2f).withAlpha (0.6f + 0.4f * lit)
                                    : colour.withAlpha (rest));
                g.fillRect (juce::Rectangle<float> (area.getX() + (float) x * colW, cy - h, std::max (0.8f, colW - 0.4f), h * 2.0f));
            }

            if (pos >= 0.0f)
            {
                g.setColour (inkColour (colour, 0.6f));
                g.fillRect (juce::Rectangle<float> (area.getX() + pos * area.getWidth() - 0.5f, b.getY() + 4.0f, 1.0f, b.getHeight() - 8.0f));
            }
        }

        g.setColour (hit > 0.01f ? colour.withAlpha (0.4f + 0.6f * hit) : Colours::hairline);
        g.drawRoundedRectangle (b.reduced (0.5f), Metrics::radiusSmall, Metrics::hairline);

        auto inner = b.reduced (7.0f, 6.0f);
        drawText (g, juce::String (i + 1).paddedLeft ('0', 2), inner.removeFromTop (12.0f), TextStyle::micro,
                  isSelected || lit > 0.3f ? Colours::text : Colours::textDim);

        // Mode glyph (top-right): one-shot = dot, gate = bar, loop = ring
        const auto glyph = juce::Rectangle<float> (5.0f, 5.0f).withCentre ({ b.getRight() - 9.0f, b.getY() + 9.0f });
        g.setColour (Colours::textFaint);
        if (c.mode == PlayMode::oneShot)      g.fillEllipse (glyph);
        else if (c.mode == PlayMode::gate)    g.fillRect (glyph.withHeight (2.0f).withCentre (glyph.getCentre()));
        else                                  g.drawEllipse (glyph, 1.0f);

        if (c.reverse)
            drawText (g, "R", juce::Rectangle<float> (10.0f, 10.0f).withCentre ({ b.getRight() - 9.0f, b.getBottom() - 9.0f }),
                      TextStyle::micro, Colours::textFaint, juce::Justification::centred);

        // Choke group (below the mode glyph): pads with the same number cut each other off.
        if (c.chokeGroup > 0)
        {
            const auto badge = juce::Rectangle<float> (12.0f, 11.0f).withCentre ({ b.getRight() - 9.0f, b.getY() + 21.0f });
            g.setColour (Colours::textFaint.withAlpha (0.5f));
            g.drawRoundedRectangle (badge, 2.0f, 1.0f);
            drawText (g, juce::String (c.chokeGroup), badge, TextStyle::micro, Colours::textDim, juce::Justification::centred);
        }

        drawText (g, noteName (c.note), inner.removeFromBottom (13.0f), TextStyle::value,
                  isSelected ? Colours::accent : Colours::text.withAlpha (0.85f));
    }

    // Chops chosen with Ctrl besides the selected one: the same outline.
    for (int i = 0; i < (int) chops.size(); ++i)
        if (const auto id = chops[(size_t) i].id; id != selected && std::find (picked.begin(), picked.end(), id) != picked.end())
        {
            const auto sb = juce::Rectangle<float> (padArea.getX() + (float) i * (padWidth + padGap) - scrollX, padArea.getY() + 2.0f, padWidth, padArea.getHeight() - 4.0f);
            g.setColour (Colours::accent);
            g.drawRoundedRectangle (sb.reduced (0.5f), Metrics::radiusSmall, Metrics::stroke);
        }

    // Selection outline: glides to the newly selected pad.
    if (selX >= 0.0f)
    {
        const auto sb = juce::Rectangle<float> (padArea.getX() + selX - scrollX, padArea.getY() + 2.0f, padWidth, padArea.getHeight() - 4.0f);
        g.setColour (Colours::accent);
        g.drawRoundedRectangle (sb.reduced (0.5f), Metrics::radiusSmall, Metrics::stroke);
    }

    // Fade the edges when more pads are scrolled out of view.
    const auto contentWidth = (float) chops.size() * (padWidth + padGap) - padGap;
    if (scrollX + padArea.getWidth() < contentWidth - 1.0f)
    {
        const auto fade = padArea.withLeft (padArea.getRight() - 36.0f);
        const auto bg = backdropColour (*this, fade.getCentre());
        g.setGradientFill (juce::ColourGradient (bg.withAlpha (0.0f), fade.getX(), 0.0f, bg, fade.getRight(), 0.0f, false));
        g.fillRect (fade);
    }
    if (scrollX > 1.0f)
    {
        const auto fade = padArea.withWidth (36.0f);
        const auto bg = backdropColour (*this, fade.getCentre());
        g.setGradientFill (juce::ColourGradient (bg, fade.getX(), 0.0f, bg.withAlpha (0.0f), fade.getRight(), 0.0f, false));
        g.fillRect (fade);
    }
}

void ChopStrip::animationTick (double dt)
{
    std::map<int, float> target;
    for (const auto& v : processor.getEngine().getVoiceDisplay())
        if (const auto id = v.chopId.load (std::memory_order_relaxed); id != 0)
            target[id] = std::max (target[id], juce::jlimit (0.0f, 1.0f, v.level.load (std::memory_order_relaxed) * 1.4f + 0.2f));

    bool changed = false;
    for (auto& [id, value] : glow)
    {
        const auto t = target.count (id) ? target[id] : 0.0f;
        const auto rate = t > value ? 1.0f / (float) Motion::fast : 1.0f / (float) Motion::slow;
        const auto next = value + (t - value) * std::min (1.0f, (float) dt * rate);
        changed |= std::abs (next - value) > 0.002f;
        value = next;
    }
    for (const auto& [id, value] : target)
        if (! glow.count (id)) { glow[id] = 0.0f; changed = true; }
    for (auto it = glow.begin(); it != glow.end();)
        it = (it->second < 0.002f && ! target.count (it->first)) ? glow.erase (it) : std::next (it);

    // Hits and playhead positions per chop.
    const auto& voices = processor.getEngine().getVoiceDisplay();
    std::map<int, float> positions;
    for (size_t i = 0; i < voices.size(); ++i)
    {
        const auto id = voices[i].chopId.load (std::memory_order_relaxed);
        const auto trig = voices[i].triggers.load (std::memory_order_relaxed);
        if (trig != lastTriggers[i])
        {
            lastTriggers[i] = trig;
            if (id != 0) { kick[id] = 1.0f; changed = true; }
        }

        if (id == 0)
            continue;
        for (const auto& c : chops)
            if (c.id == id && c.end > c.start)
                positions[id] = juce::jlimit (0.0f, 1.0f, (float) ((voices[i].position.load (std::memory_order_relaxed) - (double) c.start)
                                                                    / (double) (c.end - c.start)));
    }
    if (positions != playPos) changed = true;
    playPos = std::move (positions);

    for (auto it = kick.begin(); it != kick.end();)
    {
        it->second *= std::exp (-(float) dt * 6.0f);
        changed = true;
        it = it->second < 0.01f ? kick.erase (it) : std::next (it);
    }

    // Chops chosen with Ctrl (or let go): their outlines.
    if (const auto sig = processor.getSettings().getProperty ("selectedChops").toString(); sig != selectionSig)
    {
        selectionSig = sig;
        changed = true;
    }

    // Selection outline glide.
    const auto selected = (int) processor.getSettings().getProperty ("selectedChop", 0);
    float targetX = -1.0f;
    for (int i = 0; i < (int) chops.size(); ++i)
        if (chops[(size_t) i].id == selected)
        {
            targetX = (float) i * (padWidth + padGap);
            if (followedChop != selected)
            {
                followedChop = selected;
                ensurePadVisible (i); // the strip follows the selection
            }
        }

    if (targetX < 0.0f || selX < 0.0f)
    {
        changed |= selX != targetX;
        selX = targetX;
    }
    else if (std::abs (selX - targetX) > 0.25f)
    {
        selX += (targetX - selX) * std::min (1.0f, 1.0f - std::exp (-(float) dt * 20.0f));
        changed = true;
    }
    else if (selX != targetX)
    {
        selX = targetX;
        changed = true;
    }

    // Smooth scrolling.
    if (std::abs (scrollX - scrollTarget) > 0.25f)
    {
        scrollX += (scrollTarget - scrollX) * (1.0f - std::exp (-(float) dt * 16.0f));
        changed = true;
    }
    else if (scrollX != scrollTarget)
    {
        scrollX = scrollTarget;
        changed = true;
    }

    if (changed)
        repaint (padArea.toNearestInt());
}

void ChopStrip::mouseDown (const juce::MouseEvent& e)
{
    const auto i = padAt (e.position);
    if (i < 0)
        return;

    const auto& c = chops[(size_t) i];
    dragPad = i; // a drag out of the plugin may follow
    dragStarted = dragRendering = false;
    dragReady = juce::File();

    // Shift + click: every chop from the one shown to this one. No sound.
    if (e.mods.isShiftDown() && ! e.mods.isCommandDown() && ! e.mods.isPopupMenu())
    {
        processor.selectChopRange (c.id);
        return;
    }

    // Ctrl (Cmd on a Mac) + click: the chop joins the selection or leaves it (on release; a
    // drag with Ctrl takes the chop out as MIDI instead). No sound.
    if (e.mods.isCommandDown() && ! e.mods.isPopupMenu())
    {
        pendingToggle = c.id;
        return;
    }

    processor.getSettings().setProperty ("selectedChop", c.id, nullptr);
    if (onChopSelected) onChopSelected (c.id);

    if (! e.mods.isPopupMenu()) // every chop plays - also those beyond the 128 MIDI notes
    {
        previewChopId = c.id;
        // Velocity from the vertical click position: top of the pad = loudest.
        const auto vel = juce::jlimit (0.3f, 1.0f, 1.0f - (e.position.y - padBounds (i).getY()) / padBounds (i).getHeight() * 0.6f);
        processor.previewChop (previewChopId, vel);
    }
}

void ChopStrip::mouseDrag (const juce::MouseEvent& e)
{
    // Drag a pad out of KERF: the chop as audio (rendered with everything you hear), or with
    // Ctrl / Cmd as a MIDI note. The audio is rendered in the background; the drag starts
    // as soon as it is ready (usually a fraction of a second).
    if (dragPad < 0 || ! juce::isPositiveAndBelow (dragPad, (int) chops.size()) || e.getDistanceFromDragStart() < 8)
        return;

    const auto& chop = chops[(size_t) dragPad];
    if (! dragStarted)
    {
        dragStarted = true;
        pendingToggle = 0; // a drag, not a Ctrl + click
        releasePreview();

        if (e.mods.isCommandDown())
        {
            const auto file = chopDragFile (dragPad, "mid");
            if (processor.writeChopMidi (chop.id, file))
                dragReady = file;
        }
        else
        {
            dragRendering = true;
            const auto file = chopDragFile (dragPad, "wav");
            const auto generation = ++dragGeneration;
            juce::Component::SafePointer<ChopStrip> safe (this);
            processor.setStatusMessage ("Rendering chop " + juce::String (dragPad + 1).paddedLeft ('0', 2) + " ...");
            processor.exportChopAsync (chop.id, file, [safe, generation, file] (juce::String error)
            {
                if (auto* self = safe.getComponent(); self != nullptr && self->dragGeneration == generation)
                {
                    self->dragRendering = false;
                    if (error.isEmpty())
                        self->dragReady = file;
                    self->processor.setStatusMessage (error.isEmpty() ? juce::String ("Drag it into your DAW") : error);
                }
            });
        }
    }

    if (dragReady.existsAsFile() && ! dragRendering)
    {
        const auto file = dragReady;
        dragReady = juce::File();
        dragPad = -1;
        juce::DragAndDropContainer::performExternalDragDropOfFiles ({ file.getFullPathName() }, false, this);
    }
}

void ChopStrip::mouseUp (const juce::MouseEvent&)
{
    if (const auto id = std::exchange (pendingToggle, 0); id != 0)
        processor.toggleChopSelection (id);
    releasePreview();
    dragPad = -1;
    dragStarted = false;
}

juce::File ChopStrip::chopDragFile (int index, const juce::String& extension) const
{
    // A new file per drag: the DAW keeps referring to the dropped file.
    const auto& info = processor.getSampleInfo();
    const auto base = juce::File::createLegalFileName (info.sample != nullptr ? info.sample->name : juce::String ("KERF"));
    const auto name = base + " chop " + juce::String (index + 1).paddedLeft ('0', 2) + " " + juce::Time::getCurrentTime().formatted ("%H%M%S");
    KerfAudioProcessor::getDragFolder().createDirectory();
    return KerfAudioProcessor::getDragFolder().getNonexistentChildFile (name, "." + extension, false);
}

void ChopStrip::releasePreview()
{
    if (previewChopId > 0)
    {
        processor.previewChop (previewChopId, 0.0f);
        previewChopId = 0;
    }
}

void ChopStrip::mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails& wheel)
{
    const auto contentWidth = (float) chops.size() * (padWidth + padGap);
    const auto maxScroll = std::max (0.0f, contentWidth - padArea.getWidth());
    const auto delta = std::abs (wheel.deltaX) > std::abs (wheel.deltaY) ? wheel.deltaX : wheel.deltaY;
    scrollTarget = juce::jlimit (0.0f, maxScroll, scrollTarget - delta * 240.0f);
}

void ChopStrip::mouseMove (const juce::MouseEvent& e)
{
    const auto i = padAt (e.position);
    setMouseCursor (i >= 0 ? juce::MouseCursor::PointingHandCursor : juce::MouseCursor::NormalCursor);
    if (i != hovered) { hovered = i; repaint(); }
}

void ChopStrip::mouseExit (const juce::MouseEvent&)
{
    if (hovered != -1) { hovered = -1; repaint(); }
}

void ChopStrip::showKitMenu()
{
    if (chops.empty())
        return;

    juce::PopupMenu menu;
    menu.addSectionHeader ("EXPORT KIT");
    auto choose = [this] (bool processed)
    {
        const auto start = KerfAudioProcessor::getRecordingsFolder().getParentDirectory().getChildFile ("Kits");
        start.createDirectory();
        kitChooser = std::make_unique<juce::FileChooser> ("Export the kit into ...", start);
        kitChooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
                                 [this, processed] (const juce::FileChooser& fc)
                                 {
                                     const auto parent = fc.getResult();
                                     if (parent == juce::File())
                                         return;
                                     processor.setStatusMessage ("Exporting the kit ...");
                                     processor.exportKitAsync (parent, processed, [] (juce::File folder, juce::String error)
                                     {
                                         if (error.isEmpty() && folder.isDirectory())
                                             folder.revealToUser();
                                     });
                                 });
    };
    menu.addItem ("As you hear it (pads, envelope, effects)", [choose] { choose (true); });
    menu.addItem ("Dry slices", [choose] { choose (false); });
    menu.setLookAndFeel (&getLookAndFeel());
    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&kitButton));
}

} // namespace kerf::ui

#include "HeaderBar.h"
#include "../FactoryPresets.h"
#include "RecordOverlay.h"
#include "../UpdateCheck.h"

namespace kerf::ui
{

namespace Readout
{
    // One grid for the header readouts (PRESET, BPM, KEY): a label line, a 3 px gap,
    // then a box; the same inner padding everywhere.
    constexpr float labelY = 6.0f, labelH = 12.0f;
    constexpr float boxY = 21.0f, boxH = 22.0f;
    constexpr float padding = 8.0f;
}

namespace
{
    /** Small dot: filled = confident, outline = uncertain, none = unknown. */
    void drawConfidence (juce::Graphics& g, juce::Point<float> centre, float confidence, bool manual)
    {
        const auto r = juce::Rectangle<float> (6.0f, 6.0f).withCentre (centre);
        if (manual)
        {
            g.setColour (Colours::text);
            g.fillRect (r.reduced (0.5f));
            return;
        }
        if (confidence >= 0.6f)
        {
            g.setColour (Colours::ok);
            g.fillEllipse (r);
        }
        else if (confidence > 0.0f)
        {
            g.setColour (Colours::textDim);
            g.drawEllipse (r.reduced (0.5f), 1.0f);
        }
    }
}

float HeaderBar::brandInkWidth()
{
    // The drawn width of the wordmark (without the letter spacing after the last letter).
    juce::GlyphArrangement glyphs;
    glyphs.addLineOfText (Fonts::get (TextStyle::brand), "KERF", 0.0f, 0.0f);
    juce::Path outline;
    glyphs.createPath (outline);
    return outline.getBounds().getRight();
}

HeaderBar::HeaderBar (KerfAudioProcessor& p, AnimationClock& c)
    : processor (p), clock (c), blade (p, c)
{
    updates::checkOnceAtStart(); // is there a newer KERF? (once per run; the menu button pulses if so)

    addAndMakeVisible (blade);

    addAndMakeVisible (bpmField);
    bpmField.setLarge (true); // the same size as the KEY values
    bpmField.setDragStep (1.0); // drag in whole BPM; Shift for tenths
    bpmField.setTooltip ("Tempo of the sample. Drag, scroll or double-click to type. Right-click: double or half");
    bpmField.onChange = [this] (double v) { processor.setSourceBpm (v, true); };

    addChildComponent (autoButton); // shows after a manual BPM
    autoButton.setTooltip ("Back to the detected tempo");
    autoButton.onClick = [this] { processor.resetTempo(); };
    addChildComponent (keyAutoButton); // shows after a key change
    keyAutoButton.setTooltip ("Back to the detected key");
    keyAutoButton.onClick = [this] { processor.resetKey(); };

    // Tempo is ambiguous by a factor of two: one click to fix a half / double reading.
    addAndMakeVisible (abToggle);
    abToggle.setTooltip ("Compare two versions of your sound");
    abToggle.onChange = [this] (int slot) { processor.switchAb (slot); if (onKeyboardBack) onKeyboardBack(); };
    previousPreset.setTooltip ("Previous preset");
    previousPreset.onClick = [this] { processor.stepPreset (-1); if (onKeyboardBack) onKeyboardBack(); };
    nextPreset.setTooltip ("Next preset");
    nextPreset.onClick = [this] { processor.stepPreset (1); if (onKeyboardBack) onKeyboardBack(); };

    bpmField.onPopupMenu = [this]
    {
        const auto bpm = processor.getSampleInfo().sourceBpm;
        if (bpm <= 0.0)
            return;
        juce::PopupMenu menu;
        menu.addSectionHeader ("SAMPLE TEMPO");
        menu.addItem ("Double  (" + juce::String (bpm * 2.0, 1) + ")", bpm * 2.0 <= 400.0, false, [this, bpm] { processor.setSourceBpm (bpm * 2.0, true); });
        menu.addItem ("Half  (" + juce::String (bpm * 0.5, 1) + ")", bpm * 0.5 >= 20.0, false, [this, bpm] { processor.setSourceBpm (bpm * 0.5, true); });
        menu.addSeparator();
        menu.addItem ("Detected tempo", [this] { processor.setSourceBpm (0.0, false); });
        menu.setLookAndFeel (&getLookAndFeel()); // the plugin's menu style (menus use the default look otherwise)
        menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&bpmField));
    };

    for (auto* b : { &undoButton, &redoButton, &loadButton, &menuButton, &previousPreset, &nextPreset })
        addAndMakeVisible (b);
    menuButton.setTooltip ("Menu: about, guide, updates");
    menuButton.onClick = [this] { showMainMenu(); };

    undoButton.onClick = [this] { processor.getUndoManager().undo(); };
    redoButton.onClick = [this] { processor.getUndoManager().redo(); };
    loadButton.onClick = [this] { if (onLoad) onLoad(); };
    loadButton.setTooltip ("Open an audio file");

    processor.addChangeListener (this);
    clock.add (this);
    updateFromProcessor();
}

HeaderBar::~HeaderBar()
{
    clock.remove (this);
    processor.removeChangeListener (this);
}

void HeaderBar::changeListenerCallback (juce::ChangeBroadcaster*)
{
    updateFromProcessor();
}

void HeaderBar::updateFromProcessor()
{
    const auto& info = processor.getSampleInfo();
    bpmField.setEnabled (info.sample != nullptr);
    if (info.sourceBpm > 0.0)
        bpmField.setValue (info.sourceBpm);
    else
        bpmField.setHasValue (false);
    bpmField.setPlaceholder (info.analysing ? "..." : "--");
    bpmField.setDimmed (! info.bpmIsManual && info.tempo.confidence < 0.6f);

    undoButton.setEnabled (processor.getUndoManager().canUndo());
    redoButton.setEnabled (processor.getUndoManager().canRedo());
    undoButton.setTooltip (processor.getUndoManager().canUndo() ? "Undo " + processor.getUndoManager().getUndoDescription() : "Nothing to undo");
    redoButton.setTooltip (processor.getUndoManager().canRedo() ? "Redo " + processor.getUndoManager().getRedoDescription() : "Nothing to redo");
    repaint();
}

void HeaderBar::animationTick (double dt)
{
    // An update is out (found when KERF opened) and the menu not opened yet: it pulses.
    if (updates::updateAvailable() && ! updates::updateNoticed())
    {
        updateBlink += dt;
        menuButton.setAttention (0.5f + 0.5f * std::sin ((float) updateBlink * juce::MathConstants<float>::twoPi * 0.8f));
    }
    else
        menuButton.setAttention (0.0f);
    // Preset name and A / B follow the processor (loading, undo, a project); a * after the
    // name once anything differs from the preset (checked five times a second).
    if (const auto name = processor.getPresetName(); name != shownPreset)
    {
        shownPreset = name;
        modifiedCheck = 0.0;
        repaint (presetArea.toNearestInt().expanded (2));
    }
    if ((modifiedCheck -= dt) <= 0.0)
    {
        modifiedCheck = 0.2;
        if (const auto modified = processor.isPresetModified(); modified != shownModified)
        {
            shownModified = modified;
            repaint (presetArea.toNearestInt().expanded (2));
        }
    }
    if (abToggle.getSelected() != processor.getAbSlot())
        abToggle.setSelected (processor.getAbSlot(), juce::dontSendNotification);

    // Global pitch and the chosen mode drive the key readout (automation, undo).
    {
        const auto k = processor.getSoundingKey();
        const auto signature = k.tonic * 2 + (k.minor ? 1 : 0);
        if (signature != lastGlobalShift)
        {
            lastGlobalShift = signature;
            repaint (keyArea.toNearestInt().expanded (4));
        }
        // AUTO next to BPM after a manual tempo, next to KEY after a key change.
        if (const auto changed = processor.tempoChanged(); autoButton.isVisible() != changed)
            autoButton.setVisible (changed);
        if (const auto changed = processor.keyChanged(); keyAutoButton.isVisible() != changed)
            keyAutoButton.setVisible (changed);
    }

    // Keep undo/redo states fresh (edits come from many places).
    const auto canUndo = processor.getUndoManager().canUndo(), canRedo = processor.getUndoManager().canRedo();
    if (canUndo != undoButton.isEnabled() || canRedo != redoButton.isEnabled())
        updateFromProcessor();
}

void HeaderBar::resized()
{
    auto r = getLocalBounds().toFloat().reduced (16.0f, 0.0f);

    // KERF: as much room after the word (to the divider) as before the mark (to the edge).
    brandArea = r.removeFromLeft (16.0f + brandInkWidth() + 16.0f);
    auto right = r.removeFromRight (354.0f);

    auto rightInt = right.toNearestInt();
    // Symbols: [undo][redo]   [load]   [menu]   (REC and RESAMPLE live in the tool row)
    menuButton.setBounds (rightInt.removeFromRight (28).withSizeKeepingCentre (28, 24));
    rightInt.removeFromRight (12);
    loadButton.setBounds (rightInt.removeFromRight (28).withSizeKeepingCentre (28, 24));
    rightInt.removeFromRight (12);
    redoButton.setBounds (rightInt.removeFromRight (28).withSizeKeepingCentre (28, 24));
    rightInt.removeFromRight (4);
    undoButton.setBounds (rightInt.removeFromRight (28).withSizeKeepingCentre (28, 24));
    rightInt.removeFromRight (24);

    // BPM and KEY: the same build - a label (and confidence dot) over a box, 20 px apart.
    auto box = [] (juce::Rectangle<float> area) { return area.withY (Readout::boxY).withHeight (Readout::boxH); };
    keyArea = rightInt.removeFromRight (94).toFloat();
    tonicArea = box (keyArea).withWidth (40.0f);
    modeArea = box (keyArea).withTrimmedLeft (46.0f);
    rightInt.removeFromRight (20);
    bpmArea = rightInt.removeFromRight (76).toFloat();
    bpmBox = box (bpmArea);
    bpmField.setBounds (bpmBox.reduced (Readout::padding, 0.0f).toNearestInt());

    // AUTO (back to the detected tempo / key): on the BPM and the KEY label line,
    // right-aligned, clear of the box below.
    autoButton.setBounds ((int) bpmArea.getRight() - 36, (int) Readout::labelY - 1, 36, (int) Readout::labelH + 1);
    keyAutoButton.setBounds ((int) keyArea.getRight() - 36, (int) Readout::labelY - 1, 36, (int) Readout::labelH + 1);

    // Preset: name (a dropdown) and A / B, styled like BPM and KEY.
    r.removeFromRight (24.0f);
    presetArea = r.removeFromRight (230.0f);
    presetField = box (presetArea).withWidth (124.0f);
    // [ name v ] [<][>]  [A|B]
    previousPreset.setBounds (presetField.withX (presetField.getRight() + 4.0f).withWidth (Readout::boxH).toNearestInt());
    nextPreset.setBounds (presetField.withX (presetField.getRight() + 8.0f + Readout::boxH).withWidth (Readout::boxH).toNearestInt());
    abToggle.setBounds (presetField.withX (presetField.getRight() + 16.0f + 2.0f * Readout::boxH).withWidth (44.0f).toNearestInt());

    // The saw blade takes what the sample name does not need (up to 240 px), and the 16 px gap
    // to the presets too: its glow and beat rings reach into it (the blade stays where it was).
    const auto bladeWidth = juce::jlimit (0.0f, 240.0f, r.getWidth() - 16.0f - 200.0f);
    blade.setBounds (r.removeFromRight (bladeWidth + 16.0f).withTrimmedBottom (1.0f).toNearestInt());
    blade.setVisible (bladeWidth >= 80.0f);
    sampleArea = r.reduced (12.0f, 0.0f);
}

void HeaderBar::paint (juce::Graphics& g)
{
    fillBackdrop (g, *this, getLocalBounds().toFloat());
    drawHairline (g, { 0.0f, (float) getHeight() - 1.0f, (float) getWidth(), 1.0f });

    // Brand: a small accent block + wordmark.
    const auto mark = juce::Rectangle<float> (8.0f, 8.0f).withCentre ({ brandArea.getX() + 4.0f, brandArea.getCentreY() });
    g.setColour (Colours::accent);
    g.fillRect (mark);
    drawText (g, "KERF", brandArea.withTrimmedLeft (16.0f), TextStyle::brand, Colours::text);

    drawHairline (g, { brandArea.getRight(), 12.0f, 1.0f, (float) getHeight() - 24.0f });

    // Sample name + metadata
    const auto& info = processor.getSampleInfo();
    auto sample = sampleArea;
    if (info.sample != nullptr)
    {
        const auto& s = *info.sample;
        drawText (g, s.name, sample.removeFromTop (getHeight() * 0.56f).withTrimmedTop (6.0f), TextStyle::bodyStrong, Colours::text, juce::Justification::bottomLeft);

        juce::String meta;
        meta << juce::String (s.sampleRate / 1000.0, 1) << " kHz  "
             << (s.getNumChannels() > 1 ? "ST" : "MONO") << "  ";
        const auto len = s.getLengthSeconds();
        meta << (int) (len / 60.0) << ":" << juce::String (std::fmod (len, 60.0), 2).paddedLeft ('0', 5);
        if (s.originalBitDepth > 0) meta << "  " << s.originalBitDepth << "-bit";
        if (s.originalFormat.isNotEmpty()) meta << "  " << s.originalFormat.upToFirstOccurrenceOf (" ", false, false).toUpperCase();
        if (info.analysing) meta << "   ANALYSING";
        drawText (g, meta, sample.withTrimmedBottom (4.0f), TextStyle::micro, Colours::textDim, juce::Justification::topLeft);
    }
    else
    {
        drawText (g, info.loading ? "Loading..." : (info.fileMissing ? "Sample missing" : "No sample"), sample, TextStyle::body, Colours::textFaint);
    }

    // PRESET, BPM and KEY look alike: a small label (with its confidence dot) over a box.
    auto readoutBox = [&g] (juce::Rectangle<float> area)
    {
        Face::track (g, area.reduced (0.5f)); // a small display, sunk into the header
    };
    auto readoutLabel = [&g] (juce::Rectangle<float> area, const juce::String& text)
    {
        drawText (g, text, area.withY (Readout::labelY).withHeight (Readout::labelH), TextStyle::label, Colours::textDim);
        return juce::Point<float> (area.getX() + textWidth (text, TextStyle::label) + 7.0f, Readout::labelY + Readout::labelH * 0.5f); // its dot
    };
    auto caret = [&g] (juce::Rectangle<float> area)
    {
        juce::Path p; // small dropdown marker
        const auto cx = area.getRight() - 7.0f, cy = area.getCentreY();
        p.addTriangle (cx - 3.0f, cy - 1.5f, cx + 3.0f, cy - 1.5f, cx, cy + 2.0f);
        g.setColour (Colours::textDim);
        g.fillPath (p);
    };

    // BPM: the value (drag / type) in its box.
    {
        const auto dot = readoutLabel (bpmArea, "BPM");
        if (info.sample != nullptr && info.sourceBpm > 0.0)
            drawConfidence (g, dot, info.tempo.confidence, info.bpmIsManual);
        readoutBox (bpmBox);
    }

    // KEY: tonic and MAJ / MIN, the key the sample sounds in (each a dropdown).
    {
        const auto dot = readoutLabel (keyArea, "KEY");
        if (info.analysing || info.key.tonic < 0)
        {
            const auto both = tonicArea.getUnion (modeArea);
            readoutBox (both);
            drawText (g, info.analysing ? "..." : "--", both.withTrimmedLeft (Readout::padding), TextStyle::valueLarge, Colours::textFaint);
        }
        else
        {
            drawConfidence (g, dot, info.key.confidence, false);
            const auto sounding = processor.getSoundingKey();
            const auto colour = info.key.confidence >= 0.5f ? Colours::text : Colours::textDim;
            for (const auto& [area, text] : { std::pair { tonicArea, KeyAnalyzer::noteName (sounding.tonic) },
                                              std::pair { modeArea, juce::String (sounding.minor ? "MIN" : "MAJ") } })
            {
                readoutBox (area);
                drawText (g, text, area.withTrimmedLeft (Readout::padding).withTrimmedRight (12.0f), TextStyle::valueLarge, colour);
                caret (area);
            }
        }
    }

    // Preset: name (a dropdown), the same build.
    {
        readoutLabel (presetArea, "PRESETS");
        readoutBox (presetField);
        // The name, and a * right after it once the sound differs from the preset.
        const auto field = presetField.withTrimmedLeft (Readout::padding).withTrimmedRight (14.0f);
        const auto name = processor.getPresetName();
        const auto starWidth = shownModified ? 10.0f : 0.0f;
        const auto nameWidth = juce::jmin (textWidth (name, TextStyle::value) + 1.0f, field.getWidth() - starWidth);
        drawText (g, name, field.withWidth (nameWidth), TextStyle::value, Colours::text);
        if (shownModified)
            drawText (g, "*", field.withTrimmedLeft (nameWidth + 3.0f).withWidth (starWidth), TextStyle::value, Colours::accent);
        caret (presetField);
    }
}

} // namespace kerf::ui

namespace kerf::ui
{

void HeaderBar::mouseMove (const juce::MouseEvent& e)
{
    const auto overKey = ((tonicArea.contains (e.position) || modeArea.contains (e.position)) && processor.getSampleInfo().key.tonic >= 0)
                      || brandArea.contains (e.position) || presetField.contains (e.position);
    setMouseCursor (overKey ? juce::MouseCursor::PointingHandCursor : juce::MouseCursor::NormalCursor);
}

void HeaderBar::mouseDown (const juce::MouseEvent& e)
{
    if (e.mods.isPopupMenu())
        return; // a right-click: nothing (only the left button clicks)
    if (presetField.contains (e.position))
        showPresetMenu();
    else if (brandArea.contains (e.position))
        nextTheme();
    else if (tonicArea.contains (e.position))
        showTonicMenu();
    else if (modeArea.contains (e.position))
        showModeMenu();
}

juce::ModalComponentManager::Callback* HeaderBar::keyboardBackAfterMenu()
{
    // A closed menu can leave the keyboard with the host: Ctrl+Z should undo the choice.
    juce::Component::SafePointer<HeaderBar> safe (this);
    return juce::ModalCallbackFunction::create ([safe] (int)
    {
        if (safe != nullptr && safe->onKeyboardBack)
            safe->onKeyboardBack();
    });
}

void HeaderBar::showPresetMenu()
{
    juce::PopupMenu menu;
    const auto current = processor.getPresetName();

    // Default: every setting back to where a new KERF starts (the sample and chops stay).
    menu.addItem ("Default", true, current == "Default", [this] { processor.initPreset(); });

    // Factory sounds, one submenu per category (they shape the effects around your chops).
    menu.addSectionHeader ("FACTORY");
    const auto& factory = Factory::presets();
    for (const auto& category : Factory::categories())
    {
        juce::PopupMenu sub;
        bool holdsCurrent = false;
        for (size_t i = 0; i < factory.size(); ++i)
            if (category == factory[i].category)
            {
                const auto isCurrent = current == factory[i].name;
                holdsCurrent = holdsCurrent || isCurrent;
                sub.addItem (factory[i].name, true, isCurrent, [this, i] { processor.loadFactoryPreset ((int) i); });
            }
        menu.addSubMenu (category, sub, true, nullptr, holdsCurrent);
    }

    menu.addSectionHeader ("YOUR PRESETS");
    const auto folder = KerfAudioProcessor::getPresetFolder();
    auto files = folder.findChildFiles (juce::File::findFiles, false, juce::String ("*") + KerfAudioProcessor::presetExtension);
    files.sort();
    for (const auto& file : files)
        menu.addItem (file.getFileNameWithoutExtension(), true, file.getFileNameWithoutExtension() == current,
                      [this, file] { processor.loadPreset (file); });
    if (files.isEmpty())
        menu.addItem ("No presets yet", false, false, nullptr);

    menu.addSeparator();
    const auto currentFile = folder.getChildFile (current + KerfAudioProcessor::presetExtension);
    menu.addItem ("Save", currentFile.existsAsFile(), false, [this, currentFile] { processor.savePreset (currentFile); });
    menu.addItem ("Save as...", [this] { savePresetAs(); });
    menu.addSeparator();
    menu.addItem ("Show preset folder", [folder]
    {
        folder.createDirectory();
        folder.revealToUser();
    });

    menu.setLookAndFeel (&getLookAndFeel()); // the plugin's menu style (menus use the default look otherwise)

    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this)
                            .withTargetScreenArea (localAreaToGlobal (presetField.toNearestInt())),
                        keyboardBackAfterMenu());
}

void HeaderBar::savePresetAs()
{
    if (onSavePresetAs)
    {
        onSavePresetAs(); // the KERF-styled panel
        return;
    }
    auto* window = new juce::AlertWindow ("Save preset", "Name:", juce::MessageBoxIconType::NoIcon, this);
    const auto current = processor.getPresetName();
    window->addTextEditor ("name", current == "Default" ? juce::String ("My sound") : current);
    window->addButton ("Save", 1, juce::KeyPress (juce::KeyPress::returnKey));
    window->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));

    juce::Component::SafePointer<HeaderBar> safe (this);
    window->enterModalState (true, juce::ModalCallbackFunction::create ([safe, window] (int result)
    {
        if (auto* self = safe.getComponent(); self != nullptr && result == 1)
        {
            const auto name = juce::File::createLegalFileName (window->getTextEditorContents ("name").trim());
            if (name.isNotEmpty())
                self->processor.savePreset (KerfAudioProcessor::getPresetFolder().getChildFile (name + KerfAudioProcessor::presetExtension));
        }
    }), true);
}

void HeaderBar::nextTheme()
{
    // A click on KERF: the next theme, right away (and remembered).
    const auto index = (currentPalette() + 1) % (int) palettes().size();
    applyPalette (index);
    savePalette (index);
    processor.setStatusMessage (juce::String ("Theme: ") + palettes()[(size_t) index].name);
}

void HeaderBar::showMainMenu()
{
    // Opened once: the button stops blinking. While an update is out, a dot by the item.
    updates::setUpdateNoticed();
    menuButton.setAttention (0.0f);
    juce::PopupMenu menu;
    menu.addItem ("About KERF", [this] { if (onAbout) onAbout(); });
    menu.addItem ("Guide", [this] { if (onShowGuide) onShowGuide(); });
    menu.addItem (updates::updateAvailable() ? "Check for updates  -  a new version is out" : "Check for updates",
                  true, updates::updateAvailable(), [this] { if (onCheckUpdates) onCheckUpdates(); });

    menu.setLookAndFeel (&getLookAndFeel()); // the plugin's menu style (menus use the default look otherwise)

    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&menuButton));
}

void HeaderBar::showTonicMenu()
{
    const auto detected = processor.getSampleInfo().key;
    if (detected.tonic < 0)
        return;

    const auto sounding = processor.getSoundingKey();
    juce::PopupMenu menu;
    menu.addSectionHeader ("KEY  (detected " + detected.toString() + (detected.confidence < 0.5f ? ", uncertain)" : ")"));

    for (int tonic = 0; tonic < 12; ++tonic)
        menu.addItem (KeyAnalyzer::noteName (tonic), true, tonic == sounding.tonic,
                      [this, tonic, minor = sounding.minor] { processor.setSoundingKey (tonic, minor); });

    menu.addSeparator();
    menu.addItem ("Original key", [this, detected] { processor.setSoundingKey (detected.tonic, detected.minor); });

    menu.setLookAndFeel (&getLookAndFeel()); // the plugin's menu style (menus use the default look otherwise)

    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this)
                            .withTargetScreenArea (localAreaToGlobal (tonicArea.toNearestInt())),
                        keyboardBackAfterMenu());
}

void HeaderBar::showModeMenu()
{
    if (processor.getSampleInfo().key.tonic < 0)
        return;

    const auto sounding = processor.getSoundingKey();
    juce::PopupMenu menu;
    menu.addSectionHeader ("MAJOR / MINOR");
    menu.addItem ("Major", true, ! sounding.minor, [this, tonic = sounding.tonic] { processor.setSoundingKey (tonic, false); });
    menu.addItem ("Minor", true, sounding.minor, [this, tonic = sounding.tonic] { processor.setSoundingKey (tonic, true); });

    menu.setLookAndFeel (&getLookAndFeel()); // the plugin's menu style (menus use the default look otherwise)

    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this)
                            .withTargetScreenArea (localAreaToGlobal (modeArea.toNearestInt())),
                        keyboardBackAfterMenu());
}

} // namespace kerf::ui

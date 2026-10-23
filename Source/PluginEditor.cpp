#include "PluginEditor.h"
#include "FactoryPresets.h"

using namespace kerf;
using namespace kerf::ui;

//==============================================================================
FooterBar::FooterBar (KerfAudioProcessor& p, AnimationClock& c) : processor (p), clock (c)
{
    auto& params = processor.getParameters();

    addAndMakeVisible (voices);
    voices.setTooltip ("How many chops can sound at once");
    voicesAttachment = std::make_unique<juce::ParameterAttachment> (*params.getParameter (Params::voices),
                                                                    [this] (float v) { voices.setValue (v); });
    voices.onChange = [this] (double v) { voicesAttachment->setValueAsCompleteGesture ((float) v); };

    addAndMakeVisible (retrigger);
    retrigger.setTooltip ("POLY: notes overlap. SAME: a note cuts itself. MONO: one note at a time");
    retriggerAttachment = std::make_unique<juce::ParameterAttachment> (*params.getParameter (Params::retrigger),
                                                                       [this] (float v) { retrigger.setSelected ((int) v, juce::dontSendNotification); });
    retrigger.onChange = [this] (int v) { retriggerAttachment->setValueAsCompleteGesture ((float) v); };

    addAndMakeVisible (quantize);
    quantize.setTooltip ("Puts played notes on the grid while the DAW plays");
    quantizeAttachment = std::make_unique<juce::ParameterAttachment> (*params.getParameter (Params::quantize),
                                                                      [this] (float v) { quantize.setSelected ((int) v, juce::dontSendNotification); });
    quantize.onChange = [this] (int v) { quantizeAttachment->setValueAsCompleteGesture ((float) v); };

    voicesAttachment->sendInitialUpdate();
    retriggerAttachment->sendInitialUpdate();
    quantizeAttachment->sendInitialUpdate();

    clock.add (this);
}

FooterBar::~FooterBar()
{
    clock.remove (this);
}

void FooterBar::animationTick (double dt)
{
    // Status messages (export finished, silent recording, ...) show for 8 seconds.
    const auto status = processor.getStatusMessage();
    if (status != shownStatus)
    {
        shownStatus = status;
        statusTime = 0.0;
        repaint();
    }
    else if (shownStatus.isNotEmpty() && statusTime < 8.0 && (statusTime += dt) >= 8.0)
    {
        repaint();
    }

    const auto count = processor.getMidiNoteCounter();
    if (count != lastMidiCount)
    {
        lastMidiCount = count;
        midiFlash = 1.0f;
    }

    const auto active = processor.getEngine().getNumActiveVoices();
    const bool changed = midiFlash > 0.0f || active != activeVoices;
    activeVoices = active;
    midiFlash = std::max (0.0f, midiFlash - (float) (dt / Motion::slow));

    if (changed)
        repaint (midiArea.getUnion (voicesLabel).toNearestInt().expanded (4));
}

void FooterBar::resized()
{
    auto r = getLocalBounds().reduced (16, 0);
    voicesLabel = r.removeFromLeft (48).toFloat();
    voices.setBounds (r.removeFromLeft (24).withSizeKeepingCentre (24, 16));
    r.removeFromLeft (40);
    retrigLabel = r.removeFromLeft (52).toFloat();
    retrigger.setBounds (r.removeFromLeft (132).withSizeKeepingCentre (132, 18));
    r.removeFromLeft (20);
    quantLabel = r.removeFromLeft (70).toFloat();
    quantize.setBounds (r.removeFromLeft (190).withSizeKeepingCentre (190, 18));
    r.removeFromLeft (20);
    midiArea = r.removeFromRight (56).toFloat();
    r.removeFromRight (16);
    hintArea = r.toFloat();
}

void FooterBar::paint (juce::Graphics& g)
{
    fillBackdrop (g, *this, getLocalBounds().toFloat());
    drawHairline (g, { 0.0f, 0.0f, (float) getWidth(), 1.0f });

    drawText (g, "VOICES", voicesLabel, TextStyle::label, Colours::textDim);
    drawText (g, "RETRIG", retrigLabel, TextStyle::label, Colours::textDim);
    drawText (g, "QUANTIZE", quantLabel, TextStyle::label, Colours::textDim);
    const bool showStatus = shownStatus.isNotEmpty() && statusTime < 8.0;

    // Hints are "a  ·  b  ·  c", most important first: drop whole ones from the end until
    // the rest fits (never a word cut in half).
    auto text = showStatus ? shownStatus : hint;
    const auto separator = juce::String::fromUTF8 ("  \xc2\xb7  ");
    while (textWidth (text, TextStyle::label) > hintArea.getWidth() && text.contains (separator))
        text = text.upToLastOccurrenceOf (separator, false, false);
    drawText (g, text, hintArea, TextStyle::label, showStatus ? Colours::text : Colours::textFaint, juce::Justification::centredRight);

    // MIDI activity
    const auto dot = juce::Rectangle<float> (6.0f, 6.0f).withCentre ({ midiArea.getRight() - 3.0f, midiArea.getCentreY() });
    g.setColour (Colours::hairline);
    g.fillRect (dot);
    if (midiFlash > 0.0f)
    {
        g.setColour (Colours::accent.withAlpha (Motion::easeOut (midiFlash)));
        g.fillRect (dot);
    }
    drawText (g, "MIDI", midiArea.withTrimmedRight (12.0f), TextStyle::label, Colours::textDim, juce::Justification::centredRight);
}

//==============================================================================
MainView::MainView (KerfAudioProcessor& p, AnimationClock& c)
    : header (p, c), waveform (p, c), grainView (p, c), fxView (p, c), eqView (p, c), timeView (p, c), tools (p, waveform, c), eqTools (p, c), transport (c), toolbar (p, waveform), chopStrip (p, c), patternView (p, c),
      controls (p, c), pageTabs (controls, c), footer (p, c), recordOverlay (p, c), guide (c), pageOffVeil (controls, c), about (c), recordTools (p, c), updateOverlay (c), savePreset (p), startBurst (c), processor (p), clock (c)
{
    setComponentID (backdropRootId); // the Nebula gradient spans this view
    addChildComponent (grainView);
    addChildComponent (fxView);
    addChildComponent (eqView);
    addChildComponent (timeView);
    clock.add (this);
    controls.onPageChanged = [this] (int page) { showViewForPage (page); };
    for (auto* comp : std::initializer_list<juce::Component*> { &header, &pageTabs, &tools, &eqTools, &transport, &recordTools, &waveform, &toolbar, &chopStrip, &patternView, &performTabs, &controls, &footer })
        addAndMakeVisible (comp);
    addChildComponent (pageOffVeil); // over the view and the panel, under the overlays
    addChildComponent (recordOverlay);
    addChildComponent (about);
    addChildComponent (updateOverlay);
    addChildComponent (savePreset);
    addChildComponent (startBurst);

    header.onAbout = [this] { about.show(); };
    header.onCheckUpdates = [this] { updateOverlay.showAndCheck(); };
    header.onSavePresetAs = [this] { savePreset.show(); };

    // First-start guide: eight stops, spotlighting the part of the window it talks about.
    addChildComponent (guide);
    auto inView = [this] (juce::Component& c) { return [this, &c] { return getLocalArea (c.getParentComponent(), c.getBounds()); }; };
    auto showPads = [this] { return [this] { performTabs.setSelected (0); }; };
    guide.setSteps ({
        { "Drop a sample", "Drag an audio file here (WAV, AIFF, FLAC or OGG) or click to choose one. KERF finds its tempo and key by itself.", inView (waveform), showPads() },
        { "Slice it", "Choose how to cut it: on the beat grid, into equal parts or at the hits (TRANS), then press APPLY. Double-click the waveform to cut by hand.", inView (toolbar), showPads() },
        { "Play the pads", "Every chop is a pad on its own MIDI note, from middle C up. Click a pad or play your keyboard. Drag a pad into your DAW to get its audio.",
          [this] { return getLocalArea (performTabs.getParentComponent(), performTabs.getBounds()).getUnion (getLocalArea (chopStrip.getParentComponent(), chopStrip.getBounds())); },
          showPads() }, // the pads with PADS / SEQ beside them
        { "Build a pattern", "This is SEQ. Click steps to place the selected chop. Drag a step to the right to merge the steps after it into one long note (drag up or down: its velocity). "
                             "Right-click a step for repeats, chance, random chops and slides.",
          [this] { return getLocalArea (performTabs.getParentComponent(), performTabs.getBounds()).getUnion (getLocalArea (patternView.getParentComponent(), patternView.getBounds())); },
          [this] { performTabs.setSelected (1); } }, // the step grid, right there
        { "Shape a chop", "Start, length, envelope, pitch and level of the selected pad. CHOKE lets an open hi-hat be cut off by the closed one.", inView (controls), showPads() },
        { "Make it yours", "GRANULIZER, FX, TIME and EQ shape the whole sound. Double-click a tab to switch that page off, again to switch it on.", inView (pageTabs), showPads() },
        { "Presets", juce::String ((int) Factory::presets().size()) + " factory presets to start from, your own presets, and A / B to compare two versions. "
                     "Default sets everything back.",
          [this] { return header.getPresetBounds() + header.getPosition(); } },
        { "Themes", "Click KERF to switch the theme. Every theme keeps the layout and changes the colours.",
          [this] { return header.getBrandBounds() + header.getPosition(); } },
    });
    header.onShowGuide = [this] { startGuide(); };
    // START on the guide's last step: the blade cuts the plugin open at the waveform.
    guide.onStarted = [this] { startBurst.play ((float) getLocalArea (waveform.getParentComponent(), waveform.getBounds()).getCentreY()); };
    startBurst.onReveal = [this] { controls.playPageIntro(); };
    if (GuideOverlay::shouldShowOnStart())
    {
        juce::Component::SafePointer<MainView> safe (this);
        juce::Timer::callAfterDelay (900, [safe] { if (safe != nullptr && safe->isShowing()) safe->startGuide(); });
    }

    // PADS: play and select chops. SEQ: arrange chops into a pattern.
    const auto showSeq = (int) p.getSettings().getProperty ("performView", 0) == 1;
    performTabs.setVertical (true); // PADS above SEQ: more room for the pads and steps
    performTabs.setSelected (showSeq ? 1 : 0, juce::dontSendNotification);
    performTabs.setTooltip ("PADS: play the chops. SEQ: build a pattern");
    performTabs.onChange = [this, &p] (int v)
    {
        p.getSettings().setProperty ("performView", v, nullptr);
        updateChopRowVisibility();
    };

    showViewForPage (controls.getPage()); // the page saved with the project (after the views exist)

    updateChopRowVisibility(); // also sets the footer hint
}

void MainView::startGuide()
{
    controls.setPage (0);
    performTabs.setSelected (0);
    guide.start();
}

void MainView::updateChopRowVisibility()
{
    const auto chopPage = controls.getPage() == 0;
    const auto showSeq = (int) processor.getSettings().getProperty ("performView", 0) == 1;
    tools.setVisible (chopPage);
    eqTools.setVisible (controls.getPage() == 3);
    recordTools.setVisible (chopPage);
    toolbar.setVisible (chopPage);
    performTabs.setVisible (chopPage);
    chopStrip.setVisible (chopPage && ! showSeq);
    patternView.setVisible (chopPage && showSeq);

    // The footer explains what the page in front does (on CHOP: the pads or the step grid).
    static constexpr const char* knobHints = "Knobs: drag (Shift: fine)  \xc2\xb7  Double-click value: type  \xc2\xb7  Double-click knob: default  \xc2\xb7  Double-click tab: off / on";
    const auto page = controls.getPage();
    footer.setHint (juce::String::fromUTF8 (page == 4
        ? "Click: add a point  \xc2\xb7  Drag: move  \xc2\xb7  Double-click: remove  \xc2\xb7  Shift: no snap  \xc2\xb7  Right-click: flat, reset, copy"
        : page == 3
        ? "Double-click: add / remove  \xc2\xb7  Drag: move  \xc2\xb7  Wheel: width  \xc2\xb7  Right-click: bypass  \xc2\xb7  Types: top left"
        : page == 1 || page == 2
        ? knobHints
        : showSeq
        ? "Drag up / down: velocity  \xc2\xb7  Wheel: pitch  \xc2\xb7  Right-click: repeat, chance  \xc2\xb7  Drag right: length"
        : "Double-click: split  \xc2\xb7  Ctrl + drag: select chops  \xc2\xb7  Drag a pad out: audio (Ctrl: MIDI)  \xc2\xb7  Space: play / stop  \xc2\xb7  Shift: no snap"));
}

void MainView::resized()
{
    auto r = getLocalBounds();
    header.setBounds (r.removeFromTop (48));
    footer.setBounds (r.removeFromBottom (26));

    const auto panelHeight = ControlPanel::heightForPage (controls.getPage());
    auto panel = r.removeFromBottom (panelHeight);
    // CHOP page: the slicing tools are the panel's first section (SLICE), left of the knobs.
    if (controls.getPage() == 0)
        toolbar.setBounds (panel.removeFromLeft (330));
    controls.setBounds (panel);
    controls.setVisible (panelHeight > 0); // the EQ page has no panel: the EQ view takes it all
    r.removeFromBottom (panelHeight > 0 ? 6 : 10);

    // CHOP page: pads / SEQ under the waveform, with room to breathe; elsewhere the view grows.
    if (controls.getPage() == 0)
    {
        r.removeFromBottom (4);
        auto row = r.removeFromBottom (60).reduced (16, 0);
        performTabs.setBounds (row.removeFromLeft (58).withSizeKeepingCentre (58, 48));
        row.removeFromLeft (12);
        chopStrip.setBounds (row);
        patternView.setBounds (row);
        r.removeFromBottom (12);
    }

    // The page tabs at the top right; the tools in the top-left corner:
    // [play][stop] [loop][click]   CHOP: [knife][snap]  [rec][resample]   EQ: the point types
    r.removeFromTop (8);
    auto top = r.removeFromTop (24).reduced (16, 0);
    pageTabs.setBounds (top.removeFromRight (pageTabs.idealWidth()));
    transport.showExtras (controls.getPage() == 0); // LOOP and the METRONOME belong to the chops
    transport.setBounds (top.removeFromLeft (transport.getIdealWidth())); // play / pause and stop on every page
    top.removeFromLeft (toolbarGroupGap); // the same space between all groups of buttons
    if (controls.getPage() == 0)
    {
        tools.setBounds (top.removeFromLeft (WaveTools::idealWidth));
        top.removeFromLeft (toolbarGroupGap);
        recordTools.setBounds (top.removeFromLeft (RecordTools::idealWidth)); // recording belongs to the chops
    }
    else if (controls.getPage() == 3)
    {
        eqTools.setBounds (top.removeFromLeft (EqTypeTools::idealWidth));
    }
    r.removeFromTop (6);

    const auto view = r.reduced (16, 0);
    waveform.setBounds (view);
    // The page: from its view down to the footer, the same area on every page (the EQ has no
    // panel: the message would sit higher there).
    pageOffVeil.setBounds (juce::Rectangle<int>::leftTopRightBottom (0, view.getY(), getWidth(), footer.getY()));
    grainView.setBounds (view);
    fxView.setBounds (view);
    eqView.setBounds (view);
    timeView.setBounds (view);
    recordOverlay.setBounds (view);
    about.setBounds (getLocalBounds());
    updateOverlay.setBounds (getLocalBounds());
    savePreset.setBounds (getLocalBounds());
    startBurst.setBounds (getLocalBounds());
    guide.setBounds (getLocalBounds());
}

void MainView::paint (juce::Graphics& g)
{
    fillBackdrop (g, *this, getLocalBounds().toFloat()); // Nebula: the sky the other backgrounds continue
    // Separator above the control panel (the EQ page has none)
    if (controls.isVisible())
        drawHairline (g, { 0.0f, (float) controls.getY() - 3.0f, (float) getWidth(), 1.0f });
}

//==============================================================================
KerfAudioProcessorEditor::KerfAudioProcessorEditor (KerfAudioProcessor& p)
    : AudioProcessorEditor (p), processor (p), view (p, clock)
{
    // Colour theme: a user preference shared by every KERF instance (click the KERF wordmark).
    applyPalette (loadSavedPalette());
    lookAndFeel.refreshColours();
    themeHub->addChangeListener (this);

    setLookAndFeel (&lookAndFeel); // per editor; never the process-wide default (other plugins share the process)
    addAndMakeVisible (view);

    view.waveform.onLoadRequested = [this] { openFileChooser(); };
    view.header.onLoad = [this] { openFileChooser(); };
    view.header.onKeyboardBack = [this] { if (isShowing()) grabKeyboardFocus(); }; // Ctrl+Z right after choosing a preset
    view.transport.onPlayPause = [this] { playPause(); };
    // LOOP and the METRONOME: kept with the project, the processor follows.
    {
        const auto loopOn = (bool) processor.getSettings().getProperty ("previewLoop", false);
        const auto clickOn = (bool) processor.getSettings().getProperty ("metronome", false);
        processor.setPreviewLoop (loopOn);
        processor.setMetronome (clickOn);
        view.transport.setLoop (loopOn);
        view.transport.setMetronome (clickOn);
    }
    view.transport.onLoop = [this] (bool on) { processor.setPreviewLoop (on); processor.getSettings().setProperty ("previewLoop", on, nullptr); };
    view.transport.onMetronome = [this] (bool on) { processor.setMetronome (on); processor.getSettings().setProperty ("metronome", on, nullptr); };
    view.transport.onScope = [this] (int s) { switchScope (s); };
    view.waveform.onChopClicked = [this] (int id) { chopChosen (id); };
    // The playhead put elsewhere while it plays (perhaps into another chop): that plays on from there.
    view.waveform.onPlayFrom = [this] (int target, double from)
    {
        clearQueue();
        if (previewChopId > 0 && previewChopId != target)
            processor.previewChop (previewChopId, 0.0f, true); // the old one fades out
        previewChopId = target;
        processor.previewChop (target, 0.9f, true, from);
        previewStartedAt = juce::Time::getMillisecondCounterHiRes();
    };
    view.transport.onStop = [this] { stopPlayback(); };
    view.transport.isPlaying = [this] { return isPlaying(); }; // a stop square while the chop or the pattern plays
    // Space follows the play / stop used last: the top-right one (the chop) or the pattern's,
    // also after any edit in the step grid (steps, length, bars); a click on a chop (the
    // waveform or a pad) gives Space back to the chop.
    patternUse.onUse = [this] { spacePlaysPattern = true; };
    chopPlayUse.onUse = [this] { spacePlaysPattern = false; };
    view.patternView.addMouseListener (&patternUse, true);
    view.transport.addMouseListener (&chopPlayUse, true);
    view.waveform.addMouseListener (&chopPlayUse, true);
    view.chopStrip.addMouseListener (&chopPlayUse, true);

    // Read the saved scale first: setResizeLimits() triggers resized(), which stores the
    // (temporary) minimum size as the new scale.
    const auto scale = juce::jlimit (0.75, 2.0, (double) processor.getSettings().getProperty ("uiScale", 1.0));

    // Fixed aspect ratio, scalable from 75 % to 200 %.
    setResizable (true, true);
    const auto w = Metrics::designWidth, h = Metrics::designHeight;
    setResizeLimits (w * 3 / 4, h * 3 / 4, w * 2, h * 2);
    if (auto* c = getConstrainer())
        c->setFixedAspectRatio ((double) w / (double) h);

    setSize ((int) std::round (w * scale), (int) std::round (h * scale));

    setWantsKeyboardFocus (true);
    lastTick = juce::Time::getMillisecondCounterHiRes();
    startTimerHz (Motion::frameRateHz);
}

KerfAudioProcessorEditor::~KerfAudioProcessorEditor()
{
    stopTimer();
    if (chopPaused) // a chop paused from this window would otherwise hold its voice for ever
    {
        clearChopPause();
        processor.previewChop (previewChopId, 0.0f, true);
    }
    view.patternView.removeMouseListener (&patternUse);
    view.transport.removeMouseListener (&chopPlayUse);
    view.waveform.removeMouseListener (&chopPlayUse);
    view.chopStrip.removeMouseListener (&chopPlayUse);
    themeHub->removeChangeListener (this);
    setLookAndFeel (nullptr);
}

void KerfAudioProcessorEditor::paint (juce::Graphics& g)
{
    g.fillAll (Colours::background);
}

void KerfAudioProcessorEditor::resized()
{
    const auto scale = (float) getWidth() / (float) Metrics::designWidth;
    view.setBounds (0, 0, Metrics::designWidth, Metrics::designHeight);
    view.setTransform (juce::AffineTransform::scale (scale));
    processor.getSettings().setProperty ("uiScale", scale, nullptr);
}

void KerfAudioProcessorEditor::changeListenerCallback (juce::ChangeBroadcaster*)
{
    lookAndFeel.refreshColours();
    repaint();
    view.repaint();
}

void KerfAudioProcessorEditor::timerCallback()
{
    const auto now = juce::Time::getMillisecondCounterHiRes();
    const auto dt = juce::jlimit (0.0, 0.1, (now - lastTick) * 0.001);
    lastTick = now;
    processor.getSpectrum().update(); // the output spectrum for the EQ, FX and GRANULIZER views
    clock.tick (dt);

    // LOOP and the METRONOME as the processor has them (a project loaded with the editor open).
    view.transport.setLoop (processor.isPreviewLoop());
    view.transport.setMetronome (processor.isMetronomeOn());
    view.transport.setScope ((int) processor.getSettings().getProperty ("playScope", 0));

    // The pattern starts (its PLAY, Space, the host): a chop playing through stops.
    const auto patternPlaying = processor.isPatternPlaying();
    if (patternPlaying && ! patternWasPlaying)
        patternPaused = false; // running again (from here or the host)
    if (patternPlaying && ! patternWasPlaying && previewChopId > 0)
    {
        clearChopPause();
        processor.previewChop (previewChopId, 0.0f, true); // fades out
        previewChopId = 0;
    }
    patternWasPlaying = patternPlaying;

    // A queued chop has started (the one before it ended): it is the one playing now.
    if (queuedChopId != 0)
    {
        bool queuedSounds = false;
        for (const auto& v : processor.getEngine().getVoiceDisplay())
            queuedSounds = queuedSounds || v.chopId.load (std::memory_order_relaxed) == queuedChopId;
        if (queuedSounds)
        {
            previewChopId = std::exchange (queuedChopId, 0);
            previewStartedAt = juce::Time::getMillisecondCounterHiRes();
        }
        else if (! isPreviewSounding())
        {
            clearQueue(); // nothing plays any more (stopped, or the chop is gone)
        }
    }

    // The host starts playing: KERF's own playback stops (the chop or sample from PLAY, and a
    // pattern that was running here already; the host's own pattern automation goes on).
    if (bool patternWasRunning = false; processor.takeHostStart (patternWasRunning))
    {
        clearQueue();
        clearChopPause();
        if (previewChopId > 0)
        {
            processor.previewChop (previewChopId, 0.0f, true); // fades out
            previewChopId = 0;
        }
        if (patternWasRunning || patternPaused)
            processor.stopPattern();
        patternPaused = false;
    }

    // A few times a second: a file dialog hidden behind the host comes back to the front.
    if ((dialogCheck += dt) > 0.4)
    {
        dialogCheck = 0.0;
        raiseHiddenFileDialog();
    }
}

void KerfAudioProcessorEditor::openFileChooser()
{
    chooser = std::make_unique<juce::FileChooser> ("Load a sample",
                                                   juce::File::getSpecialLocation (juce::File::userMusicDirectory),
                                                   processor.getLoader().getWildcard());

    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                          [this] (const juce::FileChooser& fc)
                          {
                              const auto file = fc.getResult();
                              if (file.existsAsFile())
                                  processor.loadSampleAsync (file);
                              takeKeyboardFocus(); // back from the file dialog
                          });
}

bool KerfAudioProcessorEditor::isInterestedInFileDrag (const juce::StringArray& files)
{
    for (const auto& f : files)
        if (processor.getLoader().canLoad (juce::File (f)))
            return true;
    return false;
}

void KerfAudioProcessorEditor::fileDragEnter (const juce::StringArray&, int x, int y)
{
    view.waveform.setDragPoint (view.waveform.getLocalPoint (this, juce::Point<int> (x, y)).toFloat());
    view.waveform.setDragHighlight (true);
}

void KerfAudioProcessorEditor::fileDragMove (const juce::StringArray&, int x, int y)
{
    view.waveform.setDragPoint (view.waveform.getLocalPoint (this, juce::Point<int> (x, y)).toFloat());
}

void KerfAudioProcessorEditor::fileDragExit (const juce::StringArray&)             { view.waveform.setDragHighlight (false); }

void KerfAudioProcessorEditor::filesDropped (const juce::StringArray& files, int, int)
{
    view.waveform.dropHere(); // supernova
    view.waveform.setDragHighlight (false);
    for (const auto& f : files)
        if (processor.getLoader().canLoad (juce::File (f)))
        {
            processor.loadSampleAsync (juce::File (f));
            takeKeyboardFocus();
            return;
        }
}

#if JUCE_WINDOWS
extern "C" __declspec (dllimport) void* __stdcall GetAncestor (void* hwnd, unsigned int flags);
extern "C" __declspec (dllimport) int __stdcall SetForegroundWindow (void* hwnd);
extern "C" __declspec (dllimport) void* __stdcall GetForegroundWindow();
extern "C" __declspec (dllimport) int __stdcall EnumWindows (int (__stdcall*) (void*, std::intptr_t), std::intptr_t);
extern "C" __declspec (dllimport) unsigned long __stdcall GetWindowThreadProcessId (void* hwnd, unsigned long* processId);
extern "C" __declspec (dllimport) unsigned long __stdcall GetCurrentProcessId();
extern "C" __declspec (dllimport) int __stdcall GetWindowTextW (void* hwnd, wchar_t* text, int maxCount);
extern "C" __declspec (dllimport) int __stdcall IsWindowVisible (void* hwnd);

namespace
{
    /** KERF's file dialogs (load, export WAV, export kit), by title. */
    void* findOwnFileDialog()
    {
        struct Search { void* found = nullptr; } search;
        EnumWindows ([] (void* hwnd, std::intptr_t data) -> int
        {
            unsigned long pid = 0;
            GetWindowThreadProcessId (hwnd, &pid);
            if (pid != GetCurrentProcessId() || ! IsWindowVisible (hwnd))
                return 1;
            wchar_t title[64] = {};
            GetWindowTextW (hwnd, title, 63);
            const juce::String t (title);
            if (t == "Load a sample" || t == "Export WAV" || t.startsWith ("Export the kit"))
            {
                reinterpret_cast<Search*> (data)->found = hwnd;
                return 0;
            }
            return 1;
        }, reinterpret_cast<std::intptr_t> (&search));
        return search.found;
    }
}
#endif

void KerfAudioProcessorEditor::raiseHiddenFileDialog()
{
    // A file dialog runs on its own thread without an owner window, so after switching apps
    // it can end up behind the host, still open, and every click in KERF is ignored until it
    // closes (it is modal). Whenever the host window is active while one is open, bring the
    // dialog back to the front.
   #if JUCE_WINDOWS
    if (juce::ModalComponentManager::getInstance()->getNumModalComponents() == 0)
        return;
    auto* peer = getPeer();
    auto* hwnd = peer != nullptr ? peer->getNativeHandle() : nullptr;
    auto* root = hwnd != nullptr ? GetAncestor (hwnd, 2 /* GA_ROOT */) : nullptr;
    if (root == nullptr || GetForegroundWindow() != root)
        return;
    if (auto* dialog = findOwnFileDialog())
        SetForegroundWindow (dialog);
   #endif
}

void KerfAudioProcessorEditor::takeKeyboardFocus()
{
    // The editor is a child of the host's window: activate the host window (the drop came
    // from another application), then put the keyboard on the editor.
   #if JUCE_WINDOWS
    if (auto* peer = getPeer())
        if (auto* hwnd = peer->getNativeHandle())
            if (auto* root = GetAncestor (hwnd, 2 /* GA_ROOT */))
                SetForegroundWindow (root);
   #elif JUCE_MAC
    juce::Process::makeForegroundProcess(); // the host app (a drop from the Finder leaves it in the back)
   #endif
    if (isShowing()) grabKeyboardFocus();
}

void KerfAudioProcessorEditor::selectAdjacentChop (int direction)
{
    const auto chops = processor.getChops().getChops();
    if (chops.empty())
        return;

    const auto selected = (int) processor.getSettings().getProperty ("selectedChop", 0);
    int index = 0;
    for (size_t i = 0; i < chops.size(); ++i)
        if (chops[i].id == selected)
            index = (int) i;

    index = juce::jlimit (0, (int) chops.size() - 1, index + direction);
    processor.getSettings().setProperty ("selectedChop", chops[(size_t) index].id, nullptr);
    view.waveform.ensureVisible (chops[(size_t) index].start, chops[(size_t) index].end);
    chopChosen (chops[(size_t) index].id);
}

void KerfAudioProcessorEditor::chopChosen (int id)
{
    // CHOP: another chop chosen while one plays - it plays as soon as that one ends (back to the
    // one playing: nothing queued). The engine starts it on the very sample.
    const auto chopPlaying = (int) processor.getSettings().getProperty ("playScope", 0) == 0
                          && previewChopId > 0 && previewChopId != SamplerEngine::wholeSampleId
                          && ! chopPaused && isPreviewSounding();
    if (chopPlaying && id != previewChopId)
    {
        queuedChopId = id;
        processor.queuePreview (previewChopId, id);
    }
    else
    {
        clearQueue();
    }
}

void KerfAudioProcessorEditor::clearQueue()
{
    if (std::exchange (queuedChopId, 0) != 0)
        processor.queuePreview (0, 0);
}

bool KerfAudioProcessorEditor::keyPressed (const juce::KeyPress& key)
{
    auto& um = processor.getUndoManager();
    const auto mods = key.getModifiers();
    const auto cmd = mods.isCommandDown();

    if (cmd && key.getKeyCode() == 'Z' && ! mods.isShiftDown()) { um.undo(); return true; }
    if ((cmd && key.getKeyCode() == 'Z' && mods.isShiftDown()) || (cmd && key.getKeyCode() == 'Y')) { um.redo(); return true; }
    if (cmd && key.getKeyCode() == 'O') { openFileChooser(); return true; }
    if (cmd && key.getKeyCode() == 'A' && view.controls.getPage() == 0) { processor.selectAllChops(); return true; } // every chop

    if (key.isKeyCode (juce::KeyPress::leftKey))  { selectAdjacentChop (-1); return true; }
    if (key.isKeyCode (juce::KeyPress::rightKey)) { selectAdjacentChop (1); return true; }

    if (key.isKeyCode (juce::KeyPress::deleteKey) || key.isKeyCode (juce::KeyPress::backspaceKey))
    {
        // EQ open: remove the selected point.
        if (view.eqView.isVisible())
        {
            if (const auto band = view.eqView.selectedBand(); band >= 0)
                processor.removeEqPoint (band);
            return true;
        }

        // The selected chop (or all chops selected with Ctrl / Shift) removed, like the bin.
        processor.deleteSelectedChops();
        return true;
    }

    // Space: stop whatever plays (the chop or the pattern); otherwise play what the play / stop
    // button used last plays: the selected chop, or the pattern.
    if (key.getTextCharacter() == ' ')
    {
        if (spacePlaysPattern)
        {
            // The pattern: play / stop it (a chop playing through stops either way); paused,
            // it goes on from where it was.
            if (processor.isPatternPlaying())
                stopPlayback();
            else if (patternPaused)
                playPause();
            else
                processor.setPatternPlaying (true);
        }
        else
        {
            // The selected chop, always: it stops itself while it plays, a paused one goes
            // on, otherwise it plays (and stops the pattern or another chop).
            const auto selected = previewTarget();
            if (chopPaused && previewChopId == selected)
                playPause();
            else if (previewChopId == selected && previewChopId > 0 && isPreviewSounding())
                stopPlayback();
            else
                togglePreview();
        }
        return true;
    }

    if (key.getTextCharacter() == 'k' || key.getTextCharacter() == 'K')
    {
        view.waveform.setKnifeMode (! view.waveform.isKnifeMode());
        return true;
    }

    if (key.getTextCharacter() == 'f' || key.getTextCharacter() == 'F') { view.waveform.zoomToFit(); return true; }
    if (key.getTextCharacter() == '+' || key.getTextCharacter() == '=') { view.waveform.zoomBy (1.0 / 1.6, -1.0); return true; }
    if (key.getTextCharacter() == '-') { view.waveform.zoomBy (1.6, -1.0); return true; }

    return false;
}

int KerfAudioProcessorEditor::previewTarget() const
{
    // What PLAY / Space play: the selected chop, or (SAMPLE) the whole sample.
    return (int) processor.getSettings().getProperty ("playScope", 0) == 1 ? SamplerEngine::wholeSampleId
                                                                            : (int) processor.getSettings().getProperty ("selectedChop", 0);
}

void KerfAudioProcessorEditor::togglePreview()
{
    // Play the selected chop (or with SAMPLE: the whole sample) once, to its end. Again while
    // it plays: stop. Another chop selected meanwhile: that one plays instead.
    const auto selected = previewTarget();
    clearQueue();
    clearChopPause(); // a paused chop runs again (to fade out or to be replaced)
    if (previewChopId > 0 && previewChopId != selected)
    {
        processor.previewChop (previewChopId, 0.0f, true); // the old one fades out
        previewChopId = 0;
    }
    if (previewChopId > 0 && isPreviewSounding())
    {
        processor.previewChop (previewChopId, 0.0f, true); // stop (fades out)
        previewChopId = 0;
        return;
    }

    // Every chop plays, also those beyond the 128 MIDI notes.
    const auto wholeSample = selected == SamplerEngine::wholeSampleId && processor.getSampleInfo().sample != nullptr;
    if (wholeSample || processor.getChops().getChop (selected))
    {
        if (processor.isPatternPlaying() || patternPaused)
            processor.stopPattern(); // one or the other: the chop stops the pattern
        patternPaused = false;
        previewChopId = selected;
        // From the playhead (the waveform keeps it: the chop's start, or where it was put).
        processor.previewChop (selected, 0.9f, true, (double) processor.getSampleCursor());
        previewStartedAt = juce::Time::getMillisecondCounterHiRes();
    }
}

void KerfAudioProcessorEditor::switchScope (int scope)
{
    // CHOP / SAMPLE while it plays: it goes on from the same place - the whole sample from
    // there, or the chop it is in (now selected) to that chop's end.
    double at = -1.0;
    if (previewChopId > 0 && ! chopPaused && isPreviewSounding())
        for (const auto& v : processor.getEngine().getVoiceDisplay())
            if (v.chopId.load (std::memory_order_relaxed) == previewChopId)
                at = v.position.load (std::memory_order_relaxed);

    clearQueue();
    processor.getSettings().setProperty ("playScope", scope, nullptr);
    if (at < 0.0)
        return;

    auto target = SamplerEngine::wholeSampleId;
    if (scope == 0)
    {
        target = processor.getChops().chopIdAt ((int64_t) at);
        if (target == 0)
            return;
        processor.getSettings().setProperty ("selectedChop", target, nullptr);
    }
    if (target == previewChopId)
        return;
    processor.previewChop (previewChopId, 0.0f, true); // the old one fades out
    previewChopId = target;
    processor.previewChop (target, 0.9f, true, at);
    previewStartedAt = juce::Time::getMillisecondCounterHiRes();
}

void KerfAudioProcessorEditor::startPlayback()
{
    // The whole selected chop, once (the pattern has its own PLAY in SEQ).
    if (! (previewChopId > 0 && isPreviewSounding()))
        togglePreview();
}

void KerfAudioProcessorEditor::playPause()
{
    // Playing: pause it where it is (the chop holds its place, the pattern its step).
    if (isPlaying())
    {
        if (processor.isPatternPlaying())
        {
            processor.pausePattern();
            patternPaused = true;
        }
        if (previewChopId > 0 && isPreviewSounding() && ! chopPaused)
        {
            processor.setPreviewsPaused (true);
            chopPaused = true;
        }
        return;
    }
    // Paused: go on from there. Otherwise: the selected chop.
    if (chopPaused)
    {
        processor.setPreviewsPaused (false);
        chopPaused = false;
    }
    else if (patternPaused)
    {
        processor.resumePattern();
        patternPaused = false;
    }
    else
    {
        startPlayback();
    }
}

void KerfAudioProcessorEditor::clearChopPause()
{
    // A paused chop must run again to fade out (or to be replaced).
    if (chopPaused || processor.arePreviewsPaused())
        processor.setPreviewsPaused (false);
    chopPaused = false;
}

void KerfAudioProcessorEditor::stopPlayback()
{
    clearQueue();
    clearChopPause();
    if (previewChopId > 0)
    {
        processor.previewChop (previewChopId, 0.0f, true); // fades out
        previewChopId = 0;
    }
    if (processor.isPatternPlaying() || patternPaused)
        processor.stopPattern(); // the next PLAY starts at step 1
    patternPaused = false;
}

bool KerfAudioProcessorEditor::isPlaying() const
{
    return (previewChopId > 0 && isPreviewSounding() && ! chopPaused) || processor.isPatternPlaying();
}

bool KerfAudioProcessorEditor::isPreviewSounding() const
{
    // Just triggered: the audio thread may not have started the voice yet.
    if (juce::Time::getMillisecondCounterHiRes() - previewStartedAt < 60.0)
        return true;

    for (const auto& v : processor.getEngine().getVoiceDisplay())
        if (v.chopId.load (std::memory_order_relaxed) == previewChopId)
            return true;
    return false;
}

//==============================================================================
namespace kerf::ui
{

MainView::~MainView()
{
    clock.remove (this);
}

void MainView::showViewForPage (int page)
{
    juce::Component* wanted = page == 1 ? static_cast<juce::Component*> (&fxView)
                            : page == 2 ? static_cast<juce::Component*> (&grainView)
                            : page == 3 ? static_cast<juce::Component*> (&eqView)
                            : page == 4 ? static_cast<juce::Component*> (&timeView)
                                        : static_cast<juce::Component*> (&waveform);

    for (auto* view : std::initializer_list<juce::Component*> { &waveform, &fxView, &grainView, &eqView, &timeView })
        if (view != wanted)
            view->setVisible (false);
    updateChopRowVisibility();
    resized(); // the view takes the chop rows' space on the other pages

    if (! wanted->isVisible())
    {
        wanted->setAlpha (0.0f);
        wanted->setVisible (true);
        fadingIn = wanted; // fades in over Motion::standard
    }

    // Every page opens with its own animation.
    if (wanted == &waveform)  waveform.playIntro();
    if (wanted == &fxView)    fxView.playIntro();
    if (wanted == &grainView) grainView.playIntro();
    if (wanted == &eqView)    eqView.playIntro();
    if (wanted == &timeView)  timeView.playIntro();

    recordOverlay.toFront (false);
}

void MainView::animationTick (double dt)
{
    if (fadingIn == nullptr)
        return;

    const auto a = std::min (1.0f, fadingIn->getAlpha() + (float) (dt / Motion::standard));
    fadingIn->setAlpha (a);
    if (a >= 1.0f)
        fadingIn = nullptr;
}

} // namespace kerf::ui

#include "ControlPanel.h"

namespace kerf::ui
{

namespace
{
    constexpr float sectionPad = 18.0f; // minimum space either side of a section's controls

    juce::String formatSeconds (double seconds)
    {
        if (seconds < 1.0)
            return juce::String (juce::roundToInt (seconds * 1000.0)) + " ms";
        return juce::String (seconds, seconds < 10.0 ? 3 : 2) + " s";
    }

    juce::String percentText (double v) { return juce::String (juce::roundToInt (v * 100.0)) + "%"; }

    // Vertical positions (design pixels)
    constexpr int knobY = 30;          // single row of large knobs
    constexpr int selectorY = 128;     // mode selectors / buttons under the knobs
}

ControlPanel::ControlPanel (KerfAudioProcessor& p, AnimationClock& c)
    : processor (p), clock (c), timeSlots (p, c)
{
    // --- SAMPLE ------------------------------------------------------------------
    bindChop (addKnob (start, 0.0), IDs::start, ChopBinding::Kind::start);
    bindChop (addKnob (length, 0.0), IDs::end, ChopBinding::Kind::length);
    start.setTooltip ("Where the chop starts");
    length.setTooltip ("Length of the chop");

    fadeIn.setRange (0.0, ChopLimits::maxFadeInMs, 0.1);
    fadeIn.setSkewFactorFromMidPoint (60.0);
    fadeIn.textFromValueFunction = [] (double v) { return juce::String (v, v < 10.0 ? 1 : 0) + " ms"; };
    bindChop (addKnob (fadeIn, 1.0), IDs::fadeIn);
    fadeOut.setRange (0.0, ChopLimits::maxFadeOutMs, 0.1);
    fadeOut.setSkewFactorFromMidPoint (120.0);
    fadeOut.textFromValueFunction = fadeIn.textFromValueFunction;
    bindChop (addKnob (fadeOut, 8.0), IDs::fadeOut);
    fadeIn.setTooltip ("Attack: how fast the chop fades in");
    fadeOut.setTooltip ("Release: how fast the chop fades out");

    // --- ENVELOPE: decay to a sustain level (a short decay with no sustain = a tight hit)
    decay.setRange (ChopLimits::minDecayMs, ChopLimits::maxDecayMs, 0.1);
    decay.setSkewFactorFromMidPoint (300.0);
    decay.textFromValueFunction = [] (double v) { return v >= 1000.0 ? juce::String (v / 1000.0, 2) + " s" : juce::String (juce::roundToInt (v)) + " ms"; };
    bindChop (addKnob (decay, 300.0), IDs::decay);
    decay.setTooltip ("Decay: how fast it falls to the sustain level");
    sustain.setRange (0.0, 1.0, 0.0);
    sustain.textFromValueFunction = [] (double v) { return juce::String (juce::roundToInt (v * 100.0)) + "%"; };
    bindChop (addKnob (sustain, 1.0), IDs::sustain);
    sustain.setTooltip ("Sustain: the level it holds after the decay");

    // --- CHOKE: chops in the same group cut each other off (open / closed hi-hat)
    addAndMakeVisible (choke);
    choke.setTooltip ("Chops in the same group cut each other off");
    choke.onChange = [this] (int group)
    {
        if (syncing) return;
        if (selectedChopId() == 0) return;
        processor.setSelectedChopsSound (IDs::choke, (float) group, "Choke group"); // every selected chop
    };

    addAndMakeVisible (mode);
    mode.setTooltip ("ONE plays to the end, GATE while held, LOOP loops while held");
    mode.onChange = [this] (int m)
    {
        if (syncing) return;
        processor.setSelectedChopsSound (IDs::mode, (float) m, "Edit mode"); // pad parameters (automatable) or chop model
    };

    addAndMakeVisible (reverse);
    reverse.setTooltip ("Play the chop backwards");
    reverse.onClick = [this]
    {
        if (syncing) return;
        processor.setSelectedChopsSound (IDs::reverse, reverse.getToggleState() ? 1.0f : 0.0f, "Edit reverse");
    };

    // --- PITCH -------------------------------------------------------------------
    tune.setRange (ChopLimits::minPitch, ChopLimits::maxPitch, 1.0);
    tune.textFromValueFunction = [] (double v) { const auto r = juce::roundToInt (v); return (r > 0 ? "+" : "") + juce::String (r) + " st"; };
    tune.valueFromTextFunction = [] (const juce::String& s) { return s.retainCharacters ("-0123456789.").getDoubleValue(); };
    bindChop (addKnob (tune, 0.0), IDs::pitch);
    tune.setTooltip ("Pitch of the chop in semitones");

    fine.setRange (ChopLimits::minFine, ChopLimits::maxFine, 1.0);
    fine.textFromValueFunction = [] (double v) { const auto r = juce::roundToInt (v); return (r > 0 ? "+" : "") + juce::String (r) + " ct"; };
    fine.valueFromTextFunction = tune.valueFromTextFunction;
    bindChop (addKnob (fine, 0.0), IDs::fine);

    attachParam (addKnob (globalPitch, 0.0), Params::globalPitch);
    globalPitch.setTooltip ("Pitch of all chops in semitones");

    addAndMakeVisible (sync);
    sync.setTooltip ("Follow the DAW tempo");
    buttonAttachments.push_back (std::make_unique<juce::ButtonParameterAttachment> (*processor.getParameters().getParameter (Params::tempoSync), sync));

    addAndMakeVisible (stretchMode);
    stretchMode.setTooltip ("REPITCH: the speed changes the pitch. STRETCH: the pitch stays");
    attachChoice (stretchMode, Params::stretchMode);

    // --- OUTPUT ------------------------------------------------------------------
    gain.setRange (ChopLimits::minGainDb, ChopLimits::maxGainDb, 0.1);
    gain.setSkewFactorFromMidPoint (-12.0);
    gain.textFromValueFunction = [] (double v) { return v <= -59.9 ? juce::String ("-inf") : juce::String (v, 1) + " dB"; };
    gain.valueFromTextFunction = tune.valueFromTextFunction;
    bindChop (addKnob (gain, 0.0), IDs::gain);

    pan.setRange (-1.0, 1.0, 0.0);
    pan.textFromValueFunction = [] (double v)
    {
        const auto r = juce::roundToInt (v * 100.0);
        return r == 0 ? juce::String ("C") : (r < 0 ? "L" + juce::String (-r) : "R" + juce::String (r));
    };
    bindChop (addKnob (pan, 0.0), IDs::pan);
    attachParam (addKnob (master, 0.0), Params::master);

    // --- SPACE -------------------------------------------------------------------
    attachParam (addKnob (space, 0.0), Params::space);
    space.setTooltip ("Echoes in time with the tempo");
    addAndMakeVisible (spaceTime);
    attachChoice (spaceTime, Params::spaceTime);
    spaceTime.setTooltip ("How fast the echoes come (a note value at the tempo). Click: the next one, right-click: all");
    addAndMakeVisible (spaceMode);
    attachChoice (spaceMode, Params::spaceMode);
    spaceMode.setTooltip ("PING-PONG bounces the echoes left and right, STEREO keeps each side's own");
    spaceSend.setRange (0.0, 1.0, 0.0);
    spaceSend.textFromValueFunction = percentText;
    bindChop (addKnob (spaceSend, 1.0), IDs::spaceSend);
    spaceSend.setTooltip ("How much of this chop goes into the DELAY");

    // --- GRAIN -------------------------------------------------------------------
    attachParam (addKnob (grainMix, 0.0), Params::grainMix);
    grainMix.setTooltip ("Turns the sound into a cloud of grains");
    attachParam (addKnob (grainSize, 80.0), Params::grainSize);
    grainSize.setTooltip ("Length of each grain");
    attachParam (addKnob (grainDensity, 20.0), Params::grainDensity);
    attachParam (addKnob (grainSpray, 0.1), Params::grainSpray);
    grainSpray.setTooltip ("How far the grains scatter");
    attachParam (addKnob (grainPitch, 0.0), Params::grainPitchRnd);
    attachParam (addKnob (grainScan, 1.0), Params::grainScan);
    grainScan.setTooltip ("Speed of the read point (1 = normal, 0 = stands still)");
    attachParam (addKnob (grainPosition, 0.0), Params::grainPosition);
    grainPosition.setTooltip ("Where in the chop the grains read");
    attachParam (addKnob (grainSpread, 0.5), Params::grainSpread);
    grainSpread.setTooltip ("Spreads the grains across the stereo field");
    attachParam (addKnob (grainReverse, 0.0), Params::grainReverse);
    grainReverse.setTooltip ("Chance that a grain plays backwards");
    grainSend.setRange (0.0, 1.0, 0.0);
    grainSend.textFromValueFunction = percentText;
    bindChop (addKnob (grainSend, 1.0), IDs::grainSend);
    grainSend.setTooltip ("How much of the grains this chop gets");

    attachParam (addKnob (grainDrift, 0.0), Params::grainDrift);
    grainDrift.setTooltip ("Lets the read point wander by itself");
    attachParam (addKnob (grainShimmer, 0.0), Params::grainShimmer);
    grainShimmer.setTooltip ("Adds grains an octave or two higher");
    attachParam (addKnob (grainDetune, 0.0), Params::grainDetune);
    grainDetune.setTooltip ("Detunes each grain a little: a wide, shimmering unison");
    attachParam (addKnob (grainGlide, 0.0), Params::grainGlide);
    grainGlide.setTooltip ("Each grain bends up (right) or down (left) while it plays");
    attachParam (addKnob (grainHaze, 0.0), Params::grainHaze);
    grainHaze.setTooltip ("A long, soft reverb on the grains only");
    attachParam (addKnob (grainPitchShift, 0.0), Params::grainPitch);
    grainPitchShift.setTooltip ("Pitch of all grains in semitones");
    grainPitch.setTooltip ("Random pitch for each grain");

    attachParam (addKnob (grainShape, 0.25), Params::grainShape);
    grainShape.setTooltip ("Shape of each grain: hard, soft or plucked");

    addAndMakeVisible (grainScale);
    attachChoice (grainScale, Params::grainScale);
    grainScale.setTooltip ("Which notes the random pitch can use");

    addAndMakeVisible (grainFreeze);
    grainFreeze.setTooltip ("Freeze the cloud on the current moment");
    buttonAttachments.push_back (std::make_unique<juce::ButtonParameterAttachment> (*processor.getParameters().getParameter (Params::grainFreeze), grainFreeze));

    addAndMakeVisible (grainSync);
    grainSync.setTooltip ("Grains in time with the tempo");
    buttonAttachments.push_back (std::make_unique<juce::ButtonParameterAttachment> (*processor.getParameters().getParameter (Params::grainSync), grainSync));

    // With SYNC, DENSITY reads as a note value.
    {
        auto* densityParam = processor.getParameters().getParameter (Params::grainDensity);
        auto* syncParam = processor.getParameters().getParameter (Params::grainSync); // (not its raw copy: that one is updated later)
        grainDensity.textFromValueFunction = [densityParam, syncParam, grainsPerSecond = grainDensity.textFromValueFunction] (double v)
        {
            if (syncParam->getValue() > 0.5f)
                return juce::String (GrainSync::names[GrainSync::indexFor (densityParam->convertTo0to1 ((float) v))]);
            return grainsPerSecond ? grainsPerSecond (v) : juce::String (v, 0);
        };
        auto refresh = std::make_unique<juce::ParameterAttachment> (*processor.getParameters().getParameter (Params::grainSync),
                                                                    [this] (float) { grainDensity.updateText(); grainDensity.repaint(); });
        refresh->sendInitialUpdate();
        choiceAttachments.push_back (std::move (refresh));
    }
    grainDensity.setTooltip ("Grains per second");

    // The FX headers show the modes and the filter's side: drawn anew when they change.
    for (const auto* id : { Params::dirtMode, Params::wobbleMode, Params::filter, Params::pumpMode })
    {
        auto header = std::make_unique<juce::ParameterAttachment> (*processor.getParameters().getParameter (id),
                                                                   [this] (float) { repaint (0, 0, getWidth(), 26); });
        choiceAttachments.push_back (std::move (header));
    }

    // GRAINS at 0 %: the rest of the page does nothing, so it is greyed out and locked.
    {
        auto grainsOn = std::make_unique<juce::ParameterAttachment> (*processor.getParameters().getParameter (Params::grainMix),
                                                                     [this] (float) { updateGrainControls(); });
        grainsOn->sendInitialUpdate();
        choiceAttachments.push_back (std::move (grainsOn));
    }


    // --- DIST / WOBBLE ------------------------------------------------------------
    attachParam (addKnob (dirt, 0.0), Params::drive);
    dirt.setTooltip ("Distortion: WARM saturates, FUZZ clips, CRUSH lowers the bits");
    addAndMakeVisible (dirtMode);
    attachChoice (dirtMode, Params::dirtMode);

    attachParam (addKnob (wobble, 0.0), Params::wobble);
    wobble.setTooltip ("Tape wobble or chorus");
    addAndMakeVisible (wobbleMode);
    attachChoice (wobbleMode, Params::wobbleMode);

    attachParam (addKnob (pump, 0.0), Params::pump);
    pump.setTooltip ("Ducks the sound in time with the tempo, like a sidechain");
    addAndMakeVisible (pumpMode);
    attachChoice (pumpMode, Params::pumpMode);

    for (auto* s : { &spaceTime, &spaceMode, &dirtMode, &wobbleMode, &pumpMode })
        s->setCycling (true); // FX: the mode as one word under its knob (click: the next one)
    timeLength.setVertical (true); // compact: the options stand next to the knob

    // --- TIME --------------------------------------------------------------------
    addAndMakeVisible (timeSlots);
    attachParam (addKnob (timeMix, 1.0), Params::timeMix);
    timeMix.setTooltip ("How much of the TIME effect you hear");
    addAndMakeVisible (timeLength);
    attachChoice (timeLength, Params::timeLength);
    timeLength.setTooltip ("Length of one TIME cycle");
    addAndMakeVisible (timeBank);
    timeBank.setTooltip ("Shape banks A, B and C");
    timeBank.onChange = [this] (int b) { timeSlots.setBank (b); };
    timeSlots.onBankChanged = [this] (int b) { timeBank.setSelected (b, juce::dontSendNotification); };

    // --- TONE --------------------------------------------------------------------
    attachParam (addKnob (filter, 0.0), Params::filter);
    filter.setTooltip ("Left: low-pass. Right: high-pass");
    attachParam (addKnob (resonance, 0.2), Params::resonance);
    resonance.setTooltip ("Resonance: a peak at the filter's cutoff - higher sounds sharper, ringing, more synth-like");

    //==========================================================================
    // Sections: content and layout. Knob spacing (pitch) is shared per page.
    auto largeRow = [] (juce::Rectangle<float> c, int pitch, std::initializer_list<Knob*> knobs, int y = knobY)
    {
        auto x = c.getX();
        for (auto* k : knobs)
        {
            k->setBounds ((int) x, y, Knob::preferredWidth (Knob::Size::large), Knob::preferredHeight (Knob::Size::large));
            x += (float) pitch;
        }
    };

    // Buttons / selectors centred under a row of `numKnobs` knobs.
    auto centredRow = [] (juce::Rectangle<float> c, int pitch, int numKnobs, std::initializer_list<std::pair<juce::Component*, int>> items)
    {
        constexpr int gap = 6;
        const auto rowWidth = (numKnobs - 1) * pitch + Knob::preferredWidth (Knob::Size::large);
        int total = -gap;
        for (const auto& item : items)
            total += item.second + gap;
        auto x = juce::roundToInt (c.getX()) + (rowWidth - total) / 2;
        for (const auto& [component, width] : items)
        {
            component->setBounds (x, selectorY, width, 22);
            x += width + gap;
        }
    };

    // A small switch at the right end of the section's header line.
    auto inHeader = [] (juce::Rectangle<float> c, juce::Component& item, int width)
    {
        item.setBounds (juce::roundToInt (c.getRight()) - width, 6, width, 18);
    };

    // FX page sizes: hero knobs, vertically centred under the section header.
    constexpr float heroW = (float) Knob::preferredWidth (Knob::Size::hero);
    constexpr int heroH = Knob::preferredHeight (Knob::Size::hero);
    constexpr int fxY = 24 + (panelHeight - 24 - heroH) / 2;
    constexpr int centredY = 24 + (panelHeight - 24 - Knob::preferredHeight (Knob::Size::large)) / 2; // GRANULIZER row
    constexpr float compactWidth = heroW + 8.0f + 66.0f;

    // A knob with its options stacked beside it (TIME: MIX and the cycle).
    auto compact = [] (juce::Rectangle<float> c, Knob& k, Segmented& modes)
    {
        k.setBounds ((int) c.getX(), fxY, (int) heroW, heroH);
        const auto rowH = 25;
        const auto h = rowH * modes.getNumOptions();
        modes.setBounds ((int) (c.getX() + heroW + 8.0f), fxY + (heroH - 22 - h) / 2 + 2, 66, h); // centred on the disc
    };

    // FX page: five equal columns under their headers (what the group does, its setting on the
    // right), each a big knob with its mode as one word under it (DELAY: its time and mode);
    // RES and the chop's DELAY send as small knobs at the lower right of their knob. Thin lines
    // between the columns.
    constexpr float fxColumn = heroW + 46.0f;
    constexpr int fxTop = 24 + (panelHeight - 24 - (heroH + 22)) / 2;
    auto fx = [] (juce::Rectangle<float> c, Knob& k, Segmented* modes, Knob* side, Segmented* second = nullptr)
    {
        // The knob, or the knob with its small one beside it, centred in the column as a group.
        const auto sw = Knob::preferredWidth (Knob::Size::small), sh = Knob::preferredHeight (Knob::Size::small);
        constexpr int overlap = 8; // the small knob tucks in a little at the big one's lower right
        const auto groupWidth = (int) heroW + (side != nullptr ? sw - overlap : 0);
        const auto x = juce::roundToInt (c.getCentreX()) - groupWidth / 2;
        k.setBounds (x, fxTop, (int) heroW, heroH);
        const auto cx = juce::roundToInt (c.getCentreX());
        if (modes != nullptr && second != nullptr) // two words side by side (DELAY: its time and mode)
        {
            modes->setBounds (cx - 66, fxTop + heroH + 2, 56, 20);
            second->setBounds (cx - 6, fxTop + heroH + 2, 72, 20);
        }
        else if (modes != nullptr)
            modes->setBounds (cx - 50, fxTop + heroH + 2, 100, 20);
        if (side != nullptr)
            side->setBounds (x + (int) heroW - overlap, fxTop + Knob::discBottom (Knob::Size::hero) - Knob::discBottom (Knob::Size::small), sw, sh); // its disc, name and value level with the big knob's
    };

    sections = {
        { "SAMPLE", [this] { return chopTag(); }, 0, { &start, &length }, 2, 0.0f,
          [this, largeRow] (juce::Rectangle<float> c, int pitch) { largeRow (c, pitch, { &start, &length }); } },
        { "ENVELOPE", [] { return juce::String ("ADSR"); }, 0, { &fadeIn, &decay, &sustain, &fadeOut, &mode, &reverse }, 4, 0.0f,
          [this, largeRow, centredRow] (juce::Rectangle<float> c, int pitch)
          {
              largeRow (c, pitch, { &fadeIn, &decay, &sustain, &fadeOut });
              centredRow (c, pitch, 4, { { &mode, 120 }, { &reverse, reverse.getIdealWidth() } });
          } },
        { "PITCH", [this]
          {
              if (processor.isStretchMode() && processor.getStretchPending() > 0)
                  return "RENDERING " + juce::String (processor.getStretchPending());
              return chopTag() + " + GLOBAL";
          }, 0, { &tune, &fine, &globalPitch, &stretchMode, &sync }, 3, 0.0f,
          [this, largeRow, centredRow] (juce::Rectangle<float> c, int pitch)
          {
              largeRow (c, pitch, { &tune, &fine, &globalPitch });
              centredRow (c, pitch, 3, { { &stretchMode, 120 }, { &sync, sync.getIdealWidth() } });
          } },
        { "OUTPUT", [this] { return chopTag() + " / MASTER"; }, 0, { &gain, &pan, &master, &choke }, 3, 0.0f,
          [this, largeRow] (juce::Rectangle<float> c, int pitch)
          {
              largeRow (c, pitch, { &gain, &pan, &master });
              // CHOKE label + groups, centred under the knobs.
              const auto rowWidth = 2 * pitch + Knob::preferredWidth (Knob::Size::large);
              constexpr int labelW = 44, gap = 6, groupsW = 132;
              auto x = juce::roundToInt (c.getX()) + (rowWidth - (labelW + gap + groupsW)) / 2;
              chokeLabel = juce::Rectangle<int> (x, selectorY, labelW, 22).toFloat();
              choke.setBounds (x + labelW + gap, selectorY, groupsW, 22);
          } },

        // FX page (see fx above): SPACE the echoes (and how much of them this chop sends),
        // DRIVE the distortion, MODULATION the wobble, TONE the filter, SIDECHAIN the pump.
        { "SPACE", [this] { return chopTag() + " SEND"; }, 1, { &space, &spaceSend, &spaceTime, &spaceMode }, 0, fxColumn,
          [this, fx] (juce::Rectangle<float> c, int) { fx (c, space, &spaceTime, &spaceSend, &spaceMode); } },
        { "DRIVE", [this] { return choiceText (Params::dirtMode); }, 1, { &dirt, &dirtMode }, 0, fxColumn,
          [this, fx] (juce::Rectangle<float> c, int) { fx (c, dirt, &dirtMode, nullptr); } },
        { "MODULATION", [this] { return choiceText (Params::wobbleMode); }, 1, { &wobble, &wobbleMode }, 0, fxColumn,
          [this, fx] (juce::Rectangle<float> c, int) { fx (c, wobble, &wobbleMode, nullptr); } },
        { "TONE", [this]
          {
              const auto v = processor.getParameters().getRawParameterValue (Params::filter)->load();
              return juce::String (v < -0.01f ? "LOW-PASS" : (v > 0.01f ? "HIGH-PASS" : "OPEN"));
          }, 1, { &filter, &resonance }, 0, fxColumn,
          [this, fx] (juce::Rectangle<float> c, int) { fx (c, filter, nullptr, &resonance); } },
        { "SIDECHAIN", [this] { return "EVERY " + choiceText (Params::pumpMode); }, 1, { &pump, &pumpMode }, 0, fxColumn,
          [this, fx] (juce::Rectangle<float> c, int) { fx (c, pump, &pumpMode, nullptr); } },

        // GRANULIZER page: one row of knobs, vertically centred; the switches sit at the right
        // of their section's header line (SYNC with CLOUD, FREEZE with MOTION, the RANDOM
        // scale with PITCH), so they take no row of their own.
        { "CLOUD", [] { return juce::String(); }, 2, { &grainMix, &grainDensity, &grainSize, &grainShape, &grainSync }, 4, 0.0f,
          [this, largeRow, inHeader] (juce::Rectangle<float> c, int pitch)
          {
              largeRow (c, pitch, { &grainMix, &grainDensity, &grainSize, &grainShape }, centredY);
              inHeader (c, grainSync, grainSync.getIdealWidth());
          } },
        { "MOTION", [] { return juce::String(); }, 2, { &grainScan, &grainPosition, &grainSpray, &grainDrift, &grainFreeze }, 4, 0.0f,
          [this, largeRow, inHeader] (juce::Rectangle<float> c, int pitch)
          {
              largeRow (c, pitch, { &grainScan, &grainPosition, &grainSpray, &grainDrift }, centredY);
              inHeader (c, grainFreeze, grainFreeze.getIdealWidth());
          } },
        { "PITCH", [] { return juce::String(); }, 2, { &grainPitchShift, &grainPitch, &grainDetune, &grainGlide, &grainShimmer, &grainScale }, 5, 0.0f,
          [this, largeRow, inHeader] (juce::Rectangle<float> c, int pitch)
          {
              largeRow (c, pitch, { &grainPitchShift, &grainPitch, &grainDetune, &grainGlide, &grainShimmer }, centredY);
              inHeader (c, grainScale, juce::jmin (152, juce::roundToInt (c.getWidth()) - 50));
          } },
        { "COLOUR", [this] { return chopTag(); }, 2, { &grainSpread, &grainReverse, &grainHaze, &grainSend }, 4, 0.0f,
          [this, largeRow] (juce::Rectangle<float> c, int pitch) { largeRow (c, pitch, { &grainSpread, &grainReverse, &grainHaze, &grainSend }, centredY); } },

        // TIME page: the slots as cards over all the width there is; MIX with the cycle
        // stacked beside it at the right (compact).
        { "SHAPES", [this] { const auto a = (int) processor.getParameters().getRawParameterValue (Params::timeSlot)->load();
                            return a > 0 ? juce::String (TimeFactory::name (a - 1)) + "  -  CLICK AGAIN: DESELECT" : (processor.isTimeSlotEdited (KerfAudioProcessor::ownTimeCurves) ? juce::String ("NONE  -  YOUR OWN CURVES PLAY") : juce::String ("NONE  -  CLICK ONE, OR DRAW ABOVE")); },
          4, { &timeSlots, &timeBank }, 0, 0.0f,
          [this] (juce::Rectangle<float> c, int)
          {
              timeSlots.setBounds (juce::Rectangle<float> (c.getX(), 30.0f, c.getWidth(), (float) panelHeight - 40.0f).toNearestInt());
              timeBank.setBounds ((int) c.getX() + 58, 5, 84, 18); // right after the SLOTS title
          },
          true },
        { "MIX", [] { return juce::String ("CYCLE"); }, 4, { &timeMix, &timeLength }, 0, compactWidth,
          [this, compact] (juce::Rectangle<float> c, int) { compact (c, timeMix, timeLength); } },
    };

    page = juce::jlimit (0, numPages - 1, (int) processor.getSettings().getProperty ("panelPage", 0));
    updateVisibility();

    processor.getChops().getState().addListener (this);
    processor.getSettings().addListener (this);
    processor.addChangeListener (this);
    syncFromModel();
    playPageIntro(); // opening the plugin: the page's knobs sweep in
}

void ControlPanel::valueTreePropertyChanged (juce::ValueTree& tree, const juce::Identifier& property)
{
    // The page is project state: follow it when a project is loaded into an open editor.
    if (tree == processor.getSettings() && property == juce::Identifier ("panelPage"))
    {
        setPage ((int) tree.getProperty (property, 0));
        return;
    }
    syncFromModel();
}

ControlPanel::~ControlPanel()
{
    processor.removeChangeListener (this);
    processor.getSettings().removeListener (this);
    processor.getChops().getState().removeListener (this);
    padAttachments.clear(); // before the knobs they are attached to
    sliderAttachments.clear();
    buttonAttachments.clear();
    choiceAttachments.clear();
}

void ControlPanel::attachChoice (Segmented& s, const char* paramId)
{
    auto attachment = std::make_unique<juce::ParameterAttachment> (*processor.getParameters().getParameter (paramId),
                                                                   [&s] (float v) { s.setSelected ((int) v, juce::dontSendNotification); });
    auto* raw = attachment.get();
    s.onChange = [raw] (int v) { raw->setValueAsCompleteGesture ((float) v); };
    attachment->sendInitialUpdate();
    choiceAttachments.push_back (std::move (attachment));
}

Knob& ControlPanel::addKnob (Knob& k, double defaultValue)
{
    addAndMakeVisible (k);
    k.attachClock (&clock);
    k.setDefaultValue (defaultValue);
    return k;
}

void ControlPanel::attachParam (Knob& k, const char* paramId)
{
    auto* param = processor.getParameters().getParameter (paramId);
    jassert (param != nullptr);
    sliderAttachments.push_back (std::make_unique<juce::SliderParameterAttachment> (*param, k, nullptr));

    // Double-click returns to the parameter's real default.
    k.setDefaultValue (param->convertFrom0to1 (param->getDefaultValue()));
}

void ControlPanel::bindChop (Knob& k, const juce::Identifier& id, ChopBinding::Kind kind)
{
    chopBindings.push_back ({ &k, id, kind });
    const auto binding = chopBindings.back();

    // Undo: pad parameters are recorded by the processor (one step per gesture); chop-model
    // edits use a transaction per gesture.
    k.onGestureStart = [this, id]
    {
        editingChops = true;
        auto anyModel = processor.padParameter (selectedChopId(), id) == nullptr;
        // The other selected chops (Ctrl) move along: their pad parameters as gestures with
        // this one (one undo step, see ParamUndoRecorder).
        for (const auto other : otherSelectedChops())
        {
            if (auto* p = processor.padParameter (other, id)) p->beginChangeGesture();
            else anyModel = true;
        }
        if (anyModel)
            processor.getUndoManager().beginNewTransaction ("Edit " + id.toString());
    };
    k.onGestureEnd = [this, id]
    {
        for (const auto other : otherSelectedChops())
            if (auto* p = processor.padParameter (other, id))
                p->endChangeGesture();
        editingChops = false;
    };
    k.onValueChange = [this, binding]
    {
        if (! syncing)
            applyChopValue (binding, binding.knob->getValue());
    };
}

void ControlPanel::applyChopValue (const ChopBinding& b, double value)
{
    auto& model = processor.getChops();
    const auto id = selectedChopId();
    const auto chop = model.getChop (id);
    if (! chop.has_value())
        return;

    switch (b.kind)
    {
        case ChopBinding::Kind::start:
            model.moveEdge (id, ChopEdge::start, (int64_t) value, false);
            break;
        case ChopBinding::Kind::length:
            model.moveEdge (id, ChopEdge::end, chop->start + (int64_t) value, false);
            break;
        case ChopBinding::Kind::param:
            // Pad parameters are written by their slider attachment.
            if (processor.padParameter (id, b.id) == nullptr)
                model.setParam (id, b.id, value);
            // The other selected chops: the same value - only from your hand (not when the
            // host's automation moves this chop's pad).
            if (editingChops)
                for (const auto other : otherSelectedChops())
                {
                    if (auto* p = processor.padParameter (other, b.id))
                        p->setValueNotifyingHost (p->convertTo0to1 ((float) value));
                    else
                        model.setParam (other, b.id, value);
                }
            break;
    }
}

int ControlPanel::selectedChopId() const
{
    return (int) processor.getSettings().getProperty ("selectedChop", 0);
}

std::vector<int> ControlPanel::otherSelectedChops() const
{
    auto ids = processor.getSelectedChops();
    if (! ids.empty())
        ids.erase (ids.begin()); // (the first is the one the knobs show)
    return ids;
}

juce::String ControlPanel::choiceText (const char* parameterId) const
{
    return processor.getParameters().getParameter (parameterId)->getCurrentValueAsText().toUpperCase();
}

juce::String ControlPanel::chopTag() const
{
    const auto id = selectedChopId();
    const auto chops = processor.getChops().getChops();
    for (size_t i = 0; i < chops.size(); ++i)
        if (chops[i].id == id)
        {
            // More chosen with Ctrl: they change too ("CHOP 02 +3").
            const auto others = (int) processor.getSelectedChops().size() - 1;
            return "CHOP " + juce::String ((int) i + 1).paddedLeft ('0', 2) + (others > 0 ? " +" + juce::String (others) : juce::String());
        }
    return "NO CHOP";
}

void ControlPanel::updateGrainControls()
{
    // (From the parameter itself: its raw copy is brought up to date after this is called.)
    auto* mix = processor.getParameters().getParameter (Params::grainMix);
    const auto on = mix->convertFrom0to1 (mix->getValue()) > 0.0005f;
    for (auto* c : std::initializer_list<juce::Component*> { &grainDensity, &grainSize, &grainShape, &grainSync, &grainScan, &grainPosition,
                                                            &grainSpray, &grainDrift, &grainFreeze, &grainPitchShift, &grainPitch, &grainDetune,
                                                            &grainGlide, &grainShimmer, &grainScale, &grainSpread, &grainReverse, &grainHaze })
        c->setEnabled (on);
    // CHOP AMT also needs a chop to set it for.
    const auto hasChop = processor.getSampleInfo().sample != nullptr && processor.getEffectiveChop (selectedChopId()).has_value();
    grainSend.setEnabled (on && hasChop);
}

void ControlPanel::syncFromModel()
{
    const juce::ScopedValueSetter<bool> svs (syncing, true);

    const auto& info = processor.getSampleInfo();
    const auto chop = processor.getEffectiveChop (selectedChopId()); // includes pad settings
    const auto has = chop.has_value() && info.sample != nullptr;
    rebindPads();
    const auto total = info.sample != nullptr ? (double) info.sample->getNumSamples() : 1.0;
    const auto rate = info.sample != nullptr ? info.sample->sampleRate : 44100.0;

    for (const auto& b : chopBindings)
    {
        b.knob->setEnabled (has);
        if (! has)
            continue;

        switch (b.kind)
        {
            case ChopBinding::Kind::start:
                b.knob->setRange (0.0, std::max (1.0, total - (double) ChopLimits::minLength), 1.0);
                b.knob->textFromValueFunction = [rate] (double v) { return formatSeconds (v / rate); };
                b.knob->setValue ((double) chop->start, juce::dontSendNotification);
                break;
            case ChopBinding::Kind::length:
                b.knob->setRange ((double) ChopLimits::minLength, std::max ((double) ChopLimits::minLength + 1.0, total), 1.0);
                b.knob->setSkewFactorFromMidPoint (std::max ((double) ChopLimits::minLength + 1.0, std::min (total * 0.5, rate * 1.0)));
                b.knob->textFromValueFunction = [rate] (double v) { return formatSeconds (v / rate); };
                b.knob->setValue ((double) chop->length(), juce::dontSendNotification);
                break;
            case ChopBinding::Kind::param:
            {
                if (padBound)
                    break; // the parameter attachment keeps the knob in sync (also with automation)
                double v = 0.0;
                if      (b.id == IDs::pitch)     v = chop->pitchSemitones;
                else if (b.id == IDs::fine)      v = chop->fineCents;
                else if (b.id == IDs::gain)      v = chop->gainDb;
                else if (b.id == IDs::pan)       v = chop->pan;
                else if (b.id == IDs::fadeIn)    v = chop->fadeInMs;
                else if (b.id == IDs::fadeOut)   v = chop->fadeOutMs;
                else if (b.id == IDs::spaceSend) v = chop->spaceSend;
                else if (b.id == IDs::grainSend) v = chop->grainSend;
                else if (b.id == IDs::decay)     v = chop->decayMs;
                else if (b.id == IDs::sustain)   v = chop->sustain;
                b.knob->setValue (v, juce::dontSendNotification);
                break;
            }
        }
        b.knob->repaint();
    }

    choke.setEnabled (has);
    mode.setEnabled (has);
    reverse.setEnabled (has);
    if (has)
    {
        mode.setSelected ((int) chop->mode, juce::dontSendNotification);
        reverse.setToggleState (chop->reverse, juce::dontSendNotification);
        choke.setSelected (juce::jlimit (0, choke.getNumOptions() - 1, chop->chokeGroup), juce::dontSendNotification);
    }

    updateGrainControls(); // (CHOP AMT: enabled above by the chop, but not with GRAINS at 0 %)
    repaint();
}


//==============================================================================
// Pages

void ControlPanel::setPage (int newPage)
{
    newPage = juce::jlimit (0, numPages - 1, newPage);
    if (newPage == page)
        return;

    page = newPage;
    processor.getSettings().setProperty ("panelPage", page, nullptr);
    updateVisibility();
    resized();
    repaint();
    playPageIntro();
    if (onPageChanged) onPageChanged (page);
}

void ControlPanel::playPageIntro()
{
    // Left to right, a few milliseconds apart.
    std::vector<Knob*> knobs;
    for (const auto& s : sections)
        if (s.page == page)
            for (auto* c : s.components)
                if (auto* k = dynamic_cast<Knob*> (c))
                    knobs.push_back (k);
    std::sort (knobs.begin(), knobs.end(), [] (Knob* a, Knob* b) { return a->getX() < b->getX(); });

    for (size_t i = 0; i < knobs.size(); ++i)
        knobs[i]->playIntro (0.04 + 0.035 * (double) i);
}

void ControlPanel::updateVisibility()
{
    for (const auto& s : sections)
        for (auto* c : s.components)
            c->setVisible (s.page == page);
}

namespace
{
    const char* offParameter (int pg)
    {
        switch (pg)
        {
            case 1:  return Params::fxOff;
            case 2:  return Params::grainOff;
            case 3:  return Params::eqOff;
            case 4:  return Params::timeOff;
            default: return nullptr; // CHOP is the instrument itself
        }
    }
}

bool ControlPanel::isPageOff (int pg) const
{
    const auto* id = offParameter (pg);
    return id != nullptr && processor.getParameters().getRawParameterValue (id)->load() > 0.5f;
}

void ControlPanel::togglePageOff (int pg)
{
    const auto* id = offParameter (pg);
    if (id == nullptr)
        return;
    const auto nowOff = ! isPageOff (pg);
    processor.setParameterAsGesture (id, nowOff ? 1.0f : 0.0f);
    processor.setStatusMessage (PageTabs::pageName (pg) + (nowOff ? " off  -  double-click the tab to switch it on again" : " on"));
}

bool ControlPanel::isPageActive (int pg) const
{
    auto value = [this] (const char* id) { return processor.getParameters().getRawParameterValue (id)->load(); };
    if (isPageOff (pg))
        return false;
    switch (pg)
    {
        case 1:  return value (Params::space) > 0.001f || value (Params::drive) > 0.001f || value (Params::wobble) > 0.001f
                     || std::abs (value (Params::filter)) > 0.005f || value (Params::pump) > 0.001f;
        case 2:  return value (Params::grainMix) > 0.001f;
        case 3:  return ! processor.eqSettings().isFlat();
        case 4:  return processor.isTimeOn() && value (Params::timeMix) > 0.001f;
        default: return false;
    }
}

//==============================================================================
PageTabs::PageTabs (ControlPanel& p, AnimationClock& c) : panel (p), clock (c)
{
    clock.add (this);
}

PageTabs::~PageTabs()
{
    clock.remove (this);
}

namespace
{
    // Display order CHOP, GRANULIZER, FX, TIME, EQ. Page numbers stay as they are (saved projects).
    constexpr int pageInSlot[ControlPanel::numPages] = { 0, 2, 1, 4, 3 };

    float tabWidthFor (int page)
    {
        return std::max (58.0f, textWidth (PageTabs::pageName (page), TextStyle::label) + 30.0f);
    }
}

juce::String PageTabs::pageName (int page)
{
    const char* names[] = { "CHOP", "FX", "GRANULIZER", "EQ", "TIME" };
    return names[juce::jlimit (0, ControlPanel::numPages - 1, page)];
}

int PageTabs::idealWidth() const
{
    float w = (float) ((ControlPanel::numPages - 1) * tabGap);
    for (int pg = 0; pg < ControlPanel::numPages; ++pg)
        w += tabWidthFor (pg);
    return (int) std::ceil (w);
}

juce::Rectangle<float> PageTabs::tabBounds (int pg) const
{
    float x = 0.0f;
    for (int slot = 0; slot < ControlPanel::numPages; ++slot)
    {
        const auto page = pageInSlot[slot];
        if (page == pg)
            return { x, 0.0f, tabWidthFor (page), (float) getHeight() };
        x += tabWidthFor (page) + (float) tabGap;
    }
    return {};
}

int PageTabs::tabAt (juce::Point<float> p) const
{
    for (int pg = 0; pg < ControlPanel::numPages; ++pg)
        if (tabBounds (pg).contains (p))
            return pg;
    return -1;
}

void PageTabs::animationTick (double)
{
    bool changed = shownPage != panel.getPage();
    shownPage = panel.getPage();
    for (int pg = 1; pg < ControlPanel::numPages; ++pg)
        if (const auto a = panel.isPageActive (pg); a != active[(size_t) pg])
        {
            active[(size_t) pg] = a;
            changed = true;
        }
    for (int pg = 1; pg < ControlPanel::numPages; ++pg)
        if (const auto o = panel.isPageOff (pg); o != off[(size_t) pg])
        {
            off[(size_t) pg] = o;
            changed = true;
        }
    if (changed)
        repaint();
}

void PageTabs::paint (juce::Graphics& g)
{
    for (int pg = 0; pg < ControlPanel::numPages; ++pg)
    {
        const auto b = tabBounds (pg).reduced (0.5f);
        const auto on = pg == panel.getPage();

        // The shared key face: the page in front lit.
        const auto isOff = off[(size_t) pg];
        Face::State face;
        face.on = on;
        face.hover = pg == hovered;
        Face::key (g, b, face);

        // A page switched off (double-click): its name faint and struck through, a hollow dot.
        drawText (g, pageName (pg), b, TextStyle::label, isOff ? Colours::textFaint : Face::text (face), juce::Justification::centred);
        if (isOff)
        {
            const auto w = textWidth (pageName (pg), TextStyle::label) + 6.0f;
            g.setColour (Colours::textFaint);
            g.fillRect (juce::Rectangle<float> (w, 1.0f).withCentre (b.getCentre()));
            g.drawEllipse (juce::Rectangle<float> (5.0f, 5.0f).withCentre ({ b.getRight() - 8.0f, b.getY() + 7.0f }), 1.0f);
        }

        // Effects on this page are active: a small dot, so nothing is "hidden but on".
        if (pg > 0 && active[(size_t) pg])
        {
            g.setColour (Colours::accent);
            g.fillEllipse (juce::Rectangle<float> (4.0f, 4.0f).withCentre ({ b.getRight() - 8.0f, b.getY() + 7.0f }));
        }
    }
}

void PageTabs::mouseDown (const juce::MouseEvent& e)
{
    if (e.mods.isPopupMenu())
        return; // a right-click on a tab: nothing
    const auto pg = tabAt (e.position);
    previousDown = lastDown;
    previousDownTab = lastDownTab;
    lastDown = e.eventTime;
    lastDownTab = pg;
    if (pg >= 0)
        panel.setPage (pg);
}

void PageTabs::mouseDoubleClick (const juce::MouseEvent& e)
{
    const auto pg = tabAt (e.position);
    const auto quick = pg == lastDownTab && pg == previousDownTab
                    && (lastDown - previousDown).inMilliseconds() <= (juce::int64) quickDoubleClickMs;
    if (pg > 0 && quick)
        panel.togglePageOff (pg);
}

void PageTabs::mouseMove (const juce::MouseEvent& e)
{
    const auto pg = tabAt (e.position);
    setMouseCursor (pg >= 0 ? juce::MouseCursor::PointingHandCursor : juce::MouseCursor::NormalCursor);
    if (pg != hovered) { hovered = pg; repaint(); }
}

void PageTabs::mouseExit (const juce::MouseEvent&)
{
    if (hovered != -1) { hovered = -1; repaint(); }
}

//==============================================================================
void ControlPanel::resized()
{
    const auto available = (float) getWidth();

    for (int pg = 0; pg < numPages; ++pg)
    {
        std::vector<Section*> onPage;
        int columns = 0;
        float fixed = 0.0f;
        for (auto& s : sections)
            if (s.page == pg)
            {
                onPage.push_back (&s);
                columns += s.knobColumns;
                fixed += s.fixedWidth;
            }

        if (onPage.empty())
            continue; // the EQ page has no panel

        const auto n = (float) onPage.size();

        // Knob spacing: 62 by default, up to 74 when there is room, down to 56 when tight.
        auto pitch = 62.0f;
        const auto extra = available - (n * 2.0f * sectionPad + (float) columns * pitch + fixed);
        if (columns > 0)
            pitch = juce::jlimit (56.0f, 74.0f, pitch + extra / (float) columns);

        // A section is as wide as its knobs actually are (the last knob is narrower than a
        // pitch step), so the equal padding below centres them in their column.
        const auto widthOf = [pitch] (const Section& s)
        {
            return s.knobColumns > 0 ? (float) (s.knobColumns - 1) * pitch + (float) Knob::preferredWidth (Knob::Size::large) + s.fixedWidth
                                     : s.fixedWidth;
        };

        // Whatever is left becomes equal breathing room around each section, or, with a
        // section that fills, goes to that section.
        auto used = 0.0f;
        int fillers = 0;
        for (auto* s : onPage)
        {
            used += widthOf (*s);
            fillers += s->fill ? 1 : 0;
        }
        const auto pad = fillers > 0 ? sectionPad : std::max (8.0f, (available - used) / (2.0f * n));
        const auto fillWidth = fillers > 0 ? std::max (0.0f, (available - used - 2.0f * pad * n) / (float) fillers) : 0.0f;
        const auto height = (float) getHeight();

        auto x = 0.0f;
        for (auto* s : onPage)
        {
            const auto contentWidth = widthOf (*s) + (s->fill ? fillWidth : 0.0f);
            s->area = { x, 0.0f, contentWidth + 2.0f * pad, height };
            s->content = { x + pad, 0.0f, contentWidth, height };
            if (s->layout)
                s->layout ({ x + pad, 0.0f, contentWidth, height }, juce::roundToInt (pitch));
            x += s->area.getWidth();
        }
    }
}

void ControlPanel::paint (juce::Graphics& g)
{
    fillBackdrop (g, *this, getLocalBounds().toFloat());

    bool first = true;
    for (const auto& s : sections)
    {
        if (s.page != page)
            continue;

        if (! first)
            drawHairline (g, { s.area.getX(), 6.0f, 1.0f, (float) getHeight() - 12.0f });
        first = false;

        // Header aligned with the section's controls (centred ones: 15 px in, like the rest).
        const auto left = std::min (s.content.getX(), s.area.getX() + 15.0f);
        const auto header = juce::Rectangle<float> (left, 6.0f, s.area.getRight() - left - 14.0f, 18.0f);
        drawSectionHeader (g, header, s.title, s.tag ? s.tag() : juce::String());
    }

    if (choke.isVisible())
        drawText (g, "CHOKE", chokeLabel, TextStyle::label, choke.isEnabled() ? Colours::textDim : Colours::textFaint, juce::Justification::centredRight);
}

} // namespace kerf::ui

namespace kerf::ui
{

void ControlPanel::rebindPads()
{
    // Bind the per-chop knobs to the selected chop's pad parameters (if it is on a pad),
    // so host automation moves them live.
    const auto chopId = selectedChopId();
    const auto c = processor.getChops().getChop (chopId);
    const auto slot = c.has_value() ? processor.padSlotFor (*c) : -1;

    if (chopId == boundChopId && slot == boundSlot)
        return;

    boundChopId = chopId;
    boundSlot = slot;
    padAttachments.clear();
    padBound = slot >= 0;

    if (! padBound)
        return;

    for (const auto& b : chopBindings)
        if (b.kind == ChopBinding::Kind::param)
            if (auto* param = processor.padParameter (chopId, b.id))
                padAttachments.push_back (std::make_unique<juce::SliderParameterAttachment> (*param, *b.knob, nullptr));
}


//==============================================================================
PageOffVeil::PageOffVeil (ControlPanel& p, AnimationClock& c) : panel (p), clock (c)
{
    // Shown only while a page is off: it takes the clicks, so nothing under it can be used.
    setInterceptsMouseClicks (true, false);
    clock.add (this);
}

PageOffVeil::~PageOffVeil()
{
    clock.remove (this);
}

void PageOffVeil::animationTick (double dt)
{
    const auto pg = panel.getPage();
    const auto off = pg > 0 && panel.isPageOff (pg);
    if (pg != page)
    {
        page = pg;
        amount = off ? 1.0f : 0.0f; // another page: no fade, it simply is (on or off)
        sweep = 1.0f;
        setVisible (off);
        repaint();
        return;
    }

    const auto target = off ? 1.0f : 0.0f;
    if (off && amount < 0.001f)
        sweep = 0.0f; // switched off right now: the light sweeps across once
    if (std::abs (target - amount) < 0.001f && sweep >= 1.0f)
    {
        if (isVisible() != off)
            setVisible (off);
        return; // at rest
    }
    amount += (target - amount) * std::min (1.0f, (float) dt * 7.0f);
    if (std::abs (target - amount) < 0.001f)
        amount = target;
    sweep = std::min (1.0f, sweep + (float) dt / 0.7f);
    setVisible (amount > 0.0f);
    repaint();
}

void PageOffVeil::paint (juce::Graphics& g)
{
    if (amount <= 0.0f)
        return;
    const auto bounds = getLocalBounds().toFloat();

    // The veil: the page dimmed and cooled, as if its power were off.
    g.setColour (Colours::background.withAlpha (0.58f * amount));
    g.fillRect (bounds);

    // Fine diagonal lines over everything.
    {
        juce::Path lines;
        constexpr float spacing = 9.0f;
        for (auto x = -bounds.getHeight(); x < bounds.getWidth(); x += spacing)
        {
            lines.startNewSubPath (x, bounds.getBottom());
            lines.lineTo (x + bounds.getHeight(), bounds.getY());
        }
        g.setColour (Colours::text.withAlpha (0.035f * amount));
        g.strokePath (lines, juce::PathStrokeType (1.0f));
    }

    // Going off: a band of light crosses from left to right and leaves the page dark.
    if (sweep < 1.0f)
    {
        const auto x = bounds.getX() - 120.0f + (bounds.getWidth() + 240.0f) * Motion::easeOut (sweep);
        juce::ColourGradient band (Colours::accent.withAlpha (0.0f), x - 120.0f, 0.0f, Colours::accent.withAlpha (0.0f), x + 120.0f, 0.0f, false);
        band.addColour (0.5, Colours::accent.withAlpha (0.16f * (1.0f - sweep)));
        g.setGradientFill (band);
        g.fillRect (bounds);
    }

    // In the middle of the page (its view and its controls - the same place on every page):
    // what is off, and how to switch it on.
    const auto view = bounds;
    const auto title = PageTabs::pageName (page) + " IS OFF";
    const auto hint = juce::String ("Double-click its tab to switch it on");
    const auto w = std::max (textWidth (title, TextStyle::sectionLabel), textWidth (hint, TextStyle::micro)) + 56.0f;
    const auto box = juce::Rectangle<float> (w, 58.0f).withCentre (view.getCentre());
    g.setColour (Colours::surface.withAlpha (0.92f * amount));
    g.fillRoundedRectangle (box, Metrics::radiusSmall);
    g.setColour (Colours::line.withAlpha (amount));
    g.drawRoundedRectangle (box, Metrics::radiusSmall, 1.0f);

    // A power symbol, unlit.
    const auto icon = juce::Rectangle<float> (14.0f, 14.0f).withCentre ({ box.getX() + 22.0f, box.getY() + 20.0f });
    juce::Path power;
    power.addCentredArc (icon.getCentreX(), icon.getCentreY() + 0.5f, 6.0f, 6.0f, 0.0f,
                         juce::MathConstants<float>::pi * 0.22f, juce::MathConstants<float>::pi * 1.78f, true);
    power.startNewSubPath (icon.getCentreX(), icon.getY() - 1.0f);
    power.lineTo (icon.getCentreX(), icon.getCentreY());
    g.setColour (Colours::textDim.withAlpha (amount));
    g.strokePath (power, juce::PathStrokeType (1.6f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

    drawText (g, title, juce::Rectangle<float> (box.getX() + 38.0f, box.getY() + 12.0f, box.getWidth() - 44.0f, 16.0f),
              TextStyle::sectionLabel, Colours::text.withAlpha (amount));
    drawText (g, hint, juce::Rectangle<float> (box.getX() + 38.0f, box.getY() + 32.0f, box.getWidth() - 44.0f, 14.0f),
              TextStyle::micro, Colours::textDim.withAlpha (amount));
}

} // namespace kerf::ui

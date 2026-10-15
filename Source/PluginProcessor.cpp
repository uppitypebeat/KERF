#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "FactoryPresets.h"
#include <unordered_map>

using namespace kerf;

namespace
{
    // Settings children: the sound of each recent preset (for the * after a changed preset).
    const juce::Identifier presetSoundsId ("PRESET_SOUNDS"), presetSoundId ("PRESET_SOUND");
}

namespace
{
    const juce::Identifier idKERF ("KERF"), idSAMPLE ("SAMPLE"), idSETTINGS ("SETTINGS"), idPARAMS ("PARAMS");
    const juce::Identifier idVersion ("version"), idName ("name"), idPath ("path"), idRate ("sampleRate"),
                           idFrames ("numFrames"), idAudio ("audio"), idSourceBpm ("sourceBpm"), idBpmManual ("bpmManual"),
                           idPads ("pads");

    constexpr juce::uint32 stateMagic = 0x3146524b; // "KRF1"
    constexpr int stateVersion = 1;
}

//==============================================================================
namespace
{
    /** One undo step for a parameter change. `applying` is raised while the action sets the
        parameter, so the gesture it sends to the host is not recorded as a new step. */
    struct ParameterChangeAction final : public juce::UndoableAction
    {
        ParameterChangeAction (juce::RangedAudioParameter& p, float fromValue, float toValue, bool& applyingFlag, bool alreadyApplied = false)
            : param (p), from (fromValue), to (toValue), applying (applyingFlag), skipPerform (alreadyApplied) {}

        bool perform() override
        {
            if (skipPerform) { skipPerform = false; return true; } // the control already set it
            apply (to);
            return true;
        }
        bool undo() override    { apply (from); return true; }
        int getSizeInUnits() override { return 8; }

        // Quick repeats of the same control (mouse wheel, arrow keys) merge into one step.
        juce::UndoableAction* createCoalescedAction (juce::UndoableAction* next) override
        {
            if (auto* n = dynamic_cast<ParameterChangeAction*> (next); n != nullptr && &n->param == &param)
                return new ParameterChangeAction (param, from, n->to, applying, true);
            return nullptr;
        }

        void apply (float v)
        {
            const juce::ScopedValueSetter<bool> guard (applying, true);
            param.beginChangeGesture();
            param.setValueNotifyingHost (v);
            param.endChangeGesture();
        }

        juce::RangedAudioParameter& param;
        float from, to;
        bool& applying;
        bool skipPerform;
    };
}

//==============================================================================
struct KerfAudioProcessor::ParamUndoRecorder final : public juce::AudioProcessorParameter::Listener
{
    explicit ParamUndoRecorder (KerfAudioProcessor& p) : owner (p)
    {
        startValues.resize ((size_t) owner.AudioProcessor::getParameters().size(), 0.0f);
        for (auto* param : owner.AudioProcessor::getParameters())
            param->addListener (this);
    }

    ~ParamUndoRecorder() override
    {
        for (auto* param : owner.AudioProcessor::getParameters())
            param->removeListener (this);
    }

    // May run on the audio thread (automation): only a lock-free counter the views compare.
    void parameterValueChanged (int, float) override { owner.parameterChanges.fetch_add (1, std::memory_order_relaxed); }

    void parameterGestureChanged (int index, bool starting) override
    {
        // Only the editor's own gestures (message thread); never the undo itself.
        if (owner.applyingParamUndo || ! juce::MessageManager::existsAndIsCurrentThread())
            return;
        if (! juce::isPositiveAndBelow (index, (int) startValues.size()))
            return;

        auto* param = dynamic_cast<juce::RangedAudioParameter*> (owner.AudioProcessor::getParameters()[index]);
        if (param == nullptr || param->getParameterID() == Params::patternPlay) // transport, not an edit
            return;

        if (starting)
        {
            startValues[(size_t) index] = param->getValue();
            return;
        }

        const auto from = startValues[(size_t) index], to = param->getValue();
        if (std::abs (from - to) < 1.0e-6f)
            return;

        const auto now = juce::Time::getMillisecondCounterHiRes();

        if (owner.paramGroupName.isNotEmpty())
        {
            // Several parameters as one named step (ParamUndoGroup).
            if (! owner.paramGroupStarted)
            {
                owner.undoManager.beginNewTransaction (owner.paramGroupName);
                owner.paramGroupStarted = true;
            }
            lastName = owner.paramGroupName;
        }
        else
        {
            // The same control again quickly (wheel, arrow keys), or one action moving
            // several parameters at once (dragging an EQ point): one step.
            const auto name = "Change " + param->getName (64);
            const bool sameControl = index == lastIndex && now - lastTime < 700.0;
            const bool sameMoment = now - lastTime < 60.0;
            const bool merge = (sameControl || sameMoment) && ! owner.undoManager.canRedo()
                            && owner.undoManager.getCurrentTransactionName() == lastName;
            if (! merge)
            {
                owner.undoManager.beginNewTransaction (name);
                lastName = name;
            }
        }
        lastIndex = index;
        lastTime = now;

        owner.undoManager.perform (new ParameterChangeAction (*param, from, to, owner.applyingParamUndo, true));
        startValues[(size_t) index] = to;
    }

    KerfAudioProcessor& owner;
    std::vector<float> startValues;
    int lastIndex = -1;
    juce::String lastName;
    double lastTime = 0.0;
};

/** Undoable sample tempo edit (typed / dragged BPM, AUTO). */
struct KerfAudioProcessor::SourceBpmAction final : public juce::UndoableAction
{
    SourceBpmAction (KerfAudioProcessor& p, double fromBpm, bool fromManual, double toBpm, bool toManual)
        : owner (p), fromBpm (fromBpm), toBpm (toBpm), fromManual (fromManual), toManual (toManual) {}

    bool perform() override { owner.applySourceBpm (toBpm, toManual); return true; }
    bool undo() override    { owner.applySourceBpm (fromBpm, fromManual); return true; }
    int getSizeInUnits() override { return 8; }

    juce::UndoableAction* createCoalescedAction (juce::UndoableAction* next) override
    {
        if (auto* n = dynamic_cast<SourceBpmAction*> (next))
            return new SourceBpmAction (owner, fromBpm, fromManual, n->toBpm, n->toManual);
        return nullptr;
    }

    KerfAudioProcessor& owner;
    double fromBpm, toBpm;
    bool fromManual, toManual;
};

KerfAudioProcessor::KerfAudioProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput ("Input", juce::AudioChannelSet::stereo(), true)
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      parameters (*this, nullptr, idPARAMS, Params::createLayout())
{
    auto raw = [this] (const char* id)
    {
        auto* v = parameters.getRawParameterValue (id);
        jassert (v != nullptr);
        return v;
    };

    p.master = raw (Params::master);          p.pitch = raw (Params::globalPitch);
    p.stretchMode = raw (Params::stretchMode); p.tempoSync = raw (Params::tempoSync);
    p.space = raw (Params::space);
    p.filter = raw (Params::filter);          p.resonance = raw (Params::resonance); p.drive = raw (Params::drive);
    p.dirtMode = raw (Params::dirtMode);      p.wobble = raw (Params::wobble);       p.wobbleMode = raw (Params::wobbleMode);
    p.voices = raw (Params::voices);          p.retrigger = raw (Params::retrigger); p.quantize = raw (Params::quantize);
    p.grainMix = raw (Params::grainMix);      p.grainSize = raw (Params::grainSize);   p.grainDensity = raw (Params::grainDensity);
    p.grainSpray = raw (Params::grainSpray);  p.grainPitch = raw (Params::grainPitchRnd); p.grainSpread = raw (Params::grainSpread);
    p.grainScan = raw (Params::grainScan);   p.grainPosition = raw (Params::grainPosition);
    p.grainReverse = raw (Params::grainReverse);
    p.grainPitch2 = raw (Params::grainPitch);
    p.grainDrift = raw (Params::grainDrift);  p.grainShimmer = raw (Params::grainShimmer);
    p.grainShape = raw (Params::grainShape);  p.grainScale = raw (Params::grainScale);
    p.grainFreeze = raw (Params::grainFreeze); p.grainSync = raw (Params::grainSync);
    p.timeSlot = raw (Params::timeSlot); p.timeMix = raw (Params::timeMix); p.timeLength = raw (Params::timeLength);
    p.grainOff = raw (Params::grainOff); p.fxOff = raw (Params::fxOff); p.eqOff = raw (Params::eqOff); p.timeOff = raw (Params::timeOff);
    for (int band = 0; band < Params::eqBands; ++band)
    {
        const char* fields[] = { Params::EqField::on, Params::EqField::type, Params::EqField::freq, Params::EqField::gain, Params::EqField::q, Params::EqField::bypass };
        for (size_t f = 0; f < 6; ++f)
            p.eq[(size_t) band][f] = raw (Params::eqParamID (band, fields[f]).toRawUTF8());
    }
    p.pump = raw (Params::pump);              p.pumpMode = raw (Params::pumpMode);
    p.patternPlay = raw (Params::patternPlay);
    p.swing = raw (Params::swing);            p.humanize = raw (Params::humanize);

    // Defaults for non-automatable settings (grid, chopping tools, UI).
    settings.setProperty ("gridValue", (int) NoteValue::sixteenth, nullptr);
    settings.setProperty ("gridModifier", (int) NoteModifier::straight, nullptr);
    settings.setProperty ("chopScope", 0, nullptr); // 0 = whole sample, 1 = selected chop
    settings.setProperty ("sensitivity", 0.3f, nullptr);  // TRANS: the clear hits, not every ghost note or hat
    settings.setProperty ("minDistance", 0.12f, nullptr);
    settings.setProperty ("sliceCount", 8, nullptr);
    settings.setProperty ("randomSeed", 1, nullptr);
    settings.setProperty ("selectedChop", 0, nullptr);
    settings.setProperty ("snap", true, nullptr);

    // Undo keeps replaced samples (one unit per 4 KB): at most about 128 MB of them; older
    // steps are dropped beyond that, but the last step can always be undone. (It used to keep
    // the last 10 steps whatever their size: ten loaded songs held about a gigabyte.)
    undoManager.setMaxNumberOfStoredUnits (32000, 1);

    initialisePads();
    paramUndo = std::make_unique<ParamUndoRecorder> (*this);
    chops.getState().addListener (this);
    pattern.getState().addListener (this);
    settings.addListener (this); // the TIME curves (affectsEngine filters the rest)
    publishSnapshot();
    stretchTimer.startTimer (150);
}

KerfAudioProcessor::~KerfAudioProcessor()
{
    stretchTimer.stopTimer();
    saveChopMemory();
    ++loadGeneration; // cancels running analysis
    jobs.removeAllJobs (true, 10000);
    chops.getState().removeListener (this);
    pattern.getState().removeListener (this);
    settings.removeListener (this);
    cancelPendingUpdate();
}

//==============================================================================
void KerfAudioProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    // The rate the engine is prepared with is the one it plays at (not getSampleRate(), which
    // a host could set apart from this call).
    preparedRate.store (sampleRate);
    maxChunk = juce::jlimit (32, 4096, samplesPerBlock);

    engine.prepare (sampleRate);
    space.prepare (sampleRate, maxChunk);
    tone.prepare (sampleRate, maxChunk);
    dirt.prepare (sampleRate, maxChunk);
    wobble.prepare (sampleRate, maxChunk);
    eq.prepare (sampleRate, maxChunk);
    pump.prepare (sampleRate, maxChunk);
    timeShaper.prepare (sampleRate);
    metronome.prepare (sampleRate);
    spectrum.setSampleRate (sampleRate);
    patternPlayer.prepare (sampleRate);

    spaceSend.setSize (2, maxChunk);
    mixBuffer.setSize (2, maxChunk);
    grainScratch.setSize (2, maxChunk);
    chunkMidi.ensureSize (8192);

    masterGain.reset (sampleRate, 0.03);
    masterGain.setCurrentAndTargetValue (juce::Decibels::decibelsToGain (p.master->load(), -60.0f));

    // A new sample rate invalidates the voice positions; the snapshot stays valid.
    if (audioSnapshot != nullptr)
        engine.setSnapshot (audioSnapshot.get());
}

void KerfAudioProcessor::releaseResources()
{
    engine.allNotesOff (true);
    space.reset();
    tone.reset();
    dirt.reset();
    wobble.reset();
    eq.reset();
    pump.reset();
    timeShaper.reset();
    metronome.reset();
}

bool KerfAudioProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto out = layouts.getMainOutputChannelSet();
    if (out != juce::AudioChannelSet::stereo() && out != juce::AudioChannelSet::mono())
        return false;

    const auto in = layouts.getMainInputChannelSet();
    return in.isDisabled() || in == juce::AudioChannelSet::stereo() || in == juce::AudioChannelSet::mono();
}

void KerfAudioProcessor::fillGrainExtras (kerf::GrainSettings& gs, double bpm) const
{
    // SHAPE, the RANDOM scale, FREEZE and SYNC (DENSITY's knob position picks the note value).
    if (p.grainOff->load() > 0.5f)
        gs.mix = 0.0f; // the GRANULIZER tab is switched off
    gs.shape = p.grainShape->load();
    gs.pitchQuantize = juce::jlimit (0, 3, (int) p.grainScale->load());
    gs.chordMinor = chordMinorForAudio.load (std::memory_order_relaxed);
    gs.freeze = p.grainFreeze->load() > 0.5f;
    gs.bpm = bpm;
    gs.syncBeats = 0.0f;
    if (p.grainSync->load() > 0.5f)
        if (auto* density = parameters.getParameter (Params::grainDensity))
            gs.syncBeats = GrainSync::beats[GrainSync::indexFor (density->convertTo0to1 (p.grainDensity->load()))];
}

double KerfAudioProcessor::getEffectiveTempo() const
{
    if (hasHostTempo())
        return getHostBpm();
    const auto s = sourceBpmForAudio.load();
    return s > 0.0 ? s : 120.0;
}

//==============================================================================
void KerfAudioProcessor::processBlockBypassed (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    buffer.clear();
}

void KerfAudioProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    juce::ScopedNoDenormals noDenormals;

    // Newest chop/sample state from the UI (never blocks).
    if (handoff.acquire (audioSnapshot, audioSnapshotVersion))
        engine.setSnapshot (audioSnapshot.get());

    // Host transport.
    bool playing = false, hasTempo = false;
    double bpm = 120.0, ppq = 0.0, beatsPerBar = 4.0;

    if (auto* ph = getPlayHead())
    {
        if (const auto pos = ph->getPosition())
        {
            playing = pos->getIsPlaying();
            if (const auto b = pos->getBpm()) { bpm = *b; hasTempo = bpm > 0.0; }
            if (const auto q = pos->getPpqPosition()) ppq = *q;
            if (const auto sig = pos->getTimeSignature())
                beatsPerBar = sig->denominator > 0 ? sig->numerator * 4.0 / sig->denominator : 4.0;
        }
    }

    hostBpm.store (hasTempo ? bpm : 120.0, std::memory_order_relaxed);
    hostPpq.store (ppq, std::memory_order_relaxed);
    hostPlaying.store (playing, std::memory_order_relaxed);
    hostHasTempo.store (hasTempo, std::memory_order_relaxed);

    transport = { playing, hasTempo ? bpm : 120.0, ppq, beatsPerBar };

    const auto numSamples = buffer.getNumSamples();
    firstChunk = true;

    // MIDI activity indicator for the UI.
    int noteOns = 0;
    for (const auto meta : midi)
        if (meta.numBytes >= 3 && (meta.data[0] & 0xf0) == 0x90 && meta.data[2] > 0)
            ++noteOns;
    if (noteOns > 0)
        midiNoteCounter.fetch_add (noteOns, std::memory_order_relaxed);

    for (int start = 0; start < numSamples; start += maxChunk)
        processChunk (buffer, start, std::min (maxChunk, numSamples - start), midi);

    // Clear any channels beyond the main output (e.g. if input > output channels).
    for (int ch = getTotalNumOutputChannels(); ch < buffer.getNumChannels(); ++ch)
        buffer.clear (ch, 0, numSamples);

    const auto level = buffer.getMagnitude (0, numSamples);
    outputLevel.store (std::max (level, outputLevel.load (std::memory_order_relaxed) * 0.9f), std::memory_order_relaxed);
    lastProcessMs.store (juce::Time::getMillisecondCounter(), std::memory_order_relaxed);
}

void KerfAudioProcessor::processChunk (juce::AudioBuffer<float>& buffer, int start, int num, const juce::MidiBuffer& midi)
{
    // MIDI for this chunk (pre-allocated buffer, offsets relative to the chunk).
    chunkMidi.clear();
    for (const auto meta : midi)
        if (meta.samplePosition >= start && meta.samplePosition < start + num)
            chunkMidi.addEvent (meta.data, meta.numBytes, meta.samplePosition - start);

    if (firstChunk)
    {
        firstChunk = false;
        const auto scope = uiNoteFifo.read (uiNoteFifo.getNumReady());
        auto add = [this] (int idx, int count)
        {
            for (int i = 0; i < count; ++i)
            {
                const auto& n = uiNotes[(size_t) (idx + i)];
                if (n.chopId > 0 || n.queue) // started (or queued) right before the engine renders (below)
                {
                    if (numChopPreviews < (int) chopPreviews.size())
                        chopPreviews[(size_t) numChopPreviews++] = n;
                    continue;
                }
                const auto channel = n.once ? SamplerEngine::previewOnceChannel : 1;
                chunkMidi.addEvent (n.velocity > 0.0f ? juce::MidiMessage::noteOn (channel, n.note, n.velocity)
                                                      : juce::MidiMessage::noteOff (channel, n.note), 0);
            }
        };
        add (scope.startIndex1, scope.blockSize1);
        add (scope.startIndex2, scope.blockSize2);
    }

    // Tempo used when the host provides none (pattern preview, recording length).
    const auto fallbackTempo = hostHasTempo.load (std::memory_order_relaxed) ? transport.bpm
                             : (sourceBpmForAudio.load (std::memory_order_relaxed) > 0.0 ? sourceBpmForAudio.load (std::memory_order_relaxed) : 120.0);

    // REC: capture the input bus before this chunk of the buffer is overwritten.
    {
        const float* inputs[2] = { nullptr, nullptr };
        const auto numIn = std::min (2, getTotalNumInputChannels());
        for (int ch = 0; ch < numIn; ++ch)
            inputs[ch] = buffer.getReadPointer (ch, start);
        TransportInfo recTransport = transport;
        recTransport.ppqAtBlockStart += start / (preparedRate.load (std::memory_order_relaxed) * 60.0 / transport.bpm);
        recorder.process (AudioRecorder::Source::input, inputs, numIn, num, recTransport, fallbackTempo);
    }

    mixBuffer.clear (0, num);
    spaceSend.clear (0, num);

    // --- Voices -------------------------------------------------------------------
    EngineSettings es;
    es.maxVoices = (int) p.voices->load();
    es.retrigger = (RetriggerMode) (int) p.retrigger->load();
    es.quantize = (TriggerQuantize) (int) p.quantize->load();
    es.previewsPaused = previewsPaused.load (std::memory_order_relaxed);
    es.previewLoop = previewLoop.load (std::memory_order_relaxed);
    es.voice.hostSampleRate = preparedRate.load (std::memory_order_relaxed);
    es.voice.globalSemitones = p.pitch->load();
    refreshPadTable();
    es.pads = &padTable;

    // Tempo sync (Repitch): play faster/slower so the sample's tempo matches the host.
    // This changes pitch too, like a turntable; pitch-independent stretching is Milestone 6.
    const auto sourceBpm = sourceBpmForAudio.load (std::memory_order_relaxed);
    // In Stretch mode the tempo is baked into pre-rendered chops, so voices run at 1x.
    es.voice.timeRatio = (p.stretchMode->load() < 0.5f && p.tempoSync->load() > 0.5f && sourceBpm > 0.0 && transport.bpm > 0.0)
                             ? juce::jlimit (0.25, 4.0, transport.bpm / sourceBpm)
                             : 1.0;

    TransportInfo chunkTransport = transport;
    chunkTransport.ppqAtBlockStart += start / (preparedRate.load (std::memory_order_relaxed) * 60.0 / transport.bpm);

    auto& gs = es.voice.grain;
    gs.mix = p.grainMix->load();         gs.sizeMs = p.grainSize->load();     gs.density = p.grainDensity->load();
    gs.spray = p.grainSpray->load();     gs.pitchRandom = p.grainPitch->load(); gs.spread = p.grainSpread->load();
    gs.scan = p.grainScan->load();       gs.position = p.grainPosition->load();
    gs.reverse = p.grainReverse->load();
    gs.pitch = p.grainPitch2->load();
    gs.drift = p.grainDrift->load();     gs.shimmer = p.grainShimmer->load();
    fillGrainExtras (gs, transport.bpm > 0.0 ? transport.bpm : getEffectiveTempo());

    const VoiceBusses busses { mixBuffer.getWritePointer (0), mixBuffer.getWritePointer (1),
                               spaceSend.getWritePointer (0), spaceSend.getWritePointer (1),
                               grainScratch.getWritePointer (0), grainScratch.getWritePointer (1) };
    // Pattern: follows the host transport while it plays, otherwise its own preview clock.
    // PUMP follows the same beat: the host's, the pattern preview's, or its own clock.
    auto pumpPpq = transport.isPlaying ? chunkTransport.ppqAtBlockStart : pumpFreePpq;
    if (audioSnapshot != nullptr)
    {
        // "Pattern Play" is a host parameter, so the DAW can automate start / stop.
        const bool run = p.patternPlay->load (std::memory_order_relaxed) > 0.5f;

        // The host starts playing: KERF's own playback stops (the editor does it, see
        // takeHostStart): a chop or the sample from PLAY, and the pattern if it was already
        // running here before (not if the host's automation starts it with the song).
        if (transport.isPlaying && ! hostWasPlaying)
        {
            patternRanBeforeHost.store (lastPatternRun, std::memory_order_relaxed);
            hostStarted.store (true, std::memory_order_release);
        }
        hostWasPlaying = transport.isPlaying;

        if (run && ! lastPatternRun && ! patternResume.exchange (false))
            internalPpq = 0.0; // the preview starts from step 1 (after PAUSE: where it stopped)
        lastPatternRun = run;

        const auto followHost = transport.isPlaying;
        const auto ppq = followHost ? chunkTransport.ppqAtBlockStart : internalPpq;
        patternPlayer.process (audioSnapshot->pattern, run, ppq, fallbackTempo, num, chunkMidi, groove());
        if (run)
            pumpPpq = ppq;

        if (run && ! followHost)
            internalPpq += num * fallbackTempo / 60.0 / preparedRate.load (std::memory_order_relaxed);
    }
    pumpFreePpq = pumpPpq + num * fallbackTempo / 60.0 / preparedRate.load (std::memory_order_relaxed);
    pumpPpqForUi.store (pumpPpq, std::memory_order_relaxed);

    for (int i = 0; i < numChopPreviews; ++i)
    {
        const auto& n = chopPreviews[(size_t) i];
        if (n.queue)
        {
            engine.queuePreview (n.after, n.chopId, n.velocity, es);
            continue;
        }
        engine.previewChop (n.chopId, n.velocity, n.once, es, n.from);
    }
    numChopPreviews = 0;

    // The metronome with a chop or the sample from PLAY (not the song, not the pattern): the
    // voices note the sample's beat lines they pass (at KERF's BPM, from the sample's start).
    const auto songOrPattern = transport.isPlaying || lastPatternRun;
    const auto metronomeRuns = metronomeOn.load (std::memory_order_relaxed);
    const auto kerfBpm = sourceBpm > 0.0 ? sourceBpm : 120.0;
    VoiceGlobals::BeatClicks beatClicks;
    if (metronomeRuns && ! songOrPattern && audioSnapshot != nullptr && audioSnapshot->sample != nullptr)
    {
        es.voice.beatFrames = 60.0 / kerfBpm * audioSnapshot->sample->sampleRate;
        es.voice.beatOrigin = audioSnapshot->beatOrigin;
        es.voice.beatsPerBar = std::max (1, (int) std::round (transport.beatsPerBar > 0.0 ? transport.beatsPerBar : 4.0));
        es.voice.beatClicks = &beatClicks;
    }

    engine.process (busses, num, chunkMidi, es, chunkTransport);
    es.voice.beatClicks = nullptr;

    // --- SPACE --------------------------------------------------------------------
    // One knob: echoes are tempo-locked and everything else follows the amount.
    const auto tempo = transport.bpm > 0.0 && hostHasTempo.load (std::memory_order_relaxed) ? transport.bpm
                                                                                           : (sourceBpm > 0.0 ? sourceBpm : 120.0);
    // A tab switched off (double-click) plays as if its effects were at zero (they fade out).
    const auto fxOn = p.fxOff->load() < 0.5f, eqOn = p.eqOff->load() < 0.5f;
    const auto ss = PingPongDelay::fromMacro (fxOn ? p.space->load() : 0.0f, tempo);
    space.process (spaceSend.getReadPointer (0), spaceSend.getReadPointer (1),
                   mixBuffer.getWritePointer (0), mixBuffer.getWritePointer (1), num, ss);

    // --- TONE ---------------------------------------------------------------------
    juce::AudioBuffer<float> view (mixBuffer.getArrayOfWritePointers(), 2, 0, num);
    // DIRT -> filter -> WOBBLE (so the filter can sweep the distorted sound).
    dirt.process (view, num, { fxOn ? p.drive->load() : 0.0f, (DirtProcessor::Mode) (int) p.dirtMode->load() });
    tone.process (view, num, { fxOn ? p.filter->load() : 0.0f, p.resonance->load() });
    eq.process (view, num, eqOn ? eqSettings().heard() : EqProcessor::Settings {});
    wobble.process (view, num, { fxOn ? p.wobble->load() : 0.0f, (WobbleProcessor::Mode) (int) p.wobbleMode->load() });
    pump.process (view, num, fxOn ? pumpSettings() : PumpProcessor::Settings { 0.0f, pumpSettings().mode }, pumpPpq, fallbackTempo);

    // --- TIME ----------------------------------------------------------------------
    // Last in the chain (a tape stop slows the echoes too), on the same beat as PUMP.
    {
        const auto slot = p.timeOff->load() > 0.5f ? 0 : (int) p.timeSlot->load();
        // The screen's playhead runs with the song or the pattern preview and, stopped,
        // stands where the host is (the audio clock runs on for live playing).
        const TimeShaper::Settings ts { slot > 0 && audioSnapshot != nullptr ? &audioSnapshot->timeSlots[(size_t) juce::jlimit (1, TimeFactory::numSlots, slot) - 1] : nullptr,
                                        p.timeMix->load(), getTimeCycleBeats(),
                                        transport.isPlaying || lastPatternRun, chunkTransport.ppqAtBlockStart };
        timeShaper.process (view, num, ts, pumpPpq, fallbackTempo);
    }

    // --- Master + output ------------------------------------------------------------
    masterGain.setTargetValue (juce::Decibels::decibelsToGain (p.master->load(), -59.9f));
    auto* l = mixBuffer.getWritePointer (0);
    auto* r = mixBuffer.getWritePointer (1);
    for (int i = 0; i < num; ++i)
    {
        const auto g = masterGain.getNextValue();
        l[i] *= g;
        r[i] *= g;
    }

    spectrum.push (l, r, num);
    outputTap.push (l, r, num);

    // CAPTURE: record KERF's own output.
    {
        const float* outs[2] = { l, r };
        recorder.process (AudioRecorder::Source::output, outs, 2, num, chunkTransport, fallbackTempo);
    }

    const auto outChannels = getTotalNumOutputChannels();
    if (outChannels >= 2)
    {
        buffer.copyFrom (0, start, mixBuffer, 0, 0, num);
        buffer.copyFrom (1, start, mixBuffer, 1, 0, num);
    }
    else if (outChannels == 1)
    {
        buffer.copyFrom (0, start, mixBuffer.getReadPointer (0), num, 0.5f);
        buffer.addFrom (0, start, mixBuffer, 1, 0, num, 0.5f);
    }

    // The metronome: after CAPTURE and the spectrum, so it is heard but never recorded. Always
    // at KERF's BPM (the header's), on fixed beats: a chop or the sample from PLAY clicks where
    // it passes the sample's beat lines (noted by the voices above); with the song, the beats
    // of KERF's BPM from the song's start; with the pattern, the pattern's own beat.
    if (metronomeRuns && outChannels >= 1)
    {
        auto* left = buffer.getWritePointer (0, start);
        auto* right = outChannels >= 2 ? buffer.getWritePointer (1, start) : nullptr;
        const auto hostRate = preparedRate.load (std::memory_order_relaxed);
        if (transport.isPlaying)
            metronome.process (left, right, num, true, chunkTransport.ppqAtBlockStart * kerfBpm / std::max (1.0, transport.bpm),
                               kerfBpm / 60.0 / hostRate, transport.beatsPerBar);
        else if (lastPatternRun)
            metronome.process (left, right, num, true, pumpPpq, fallbackTempo / 60.0 / hostRate, transport.beatsPerBar);
        else
            metronome.clickAt (left, right, num, beatClicks);
    }
    else
    {
        metronome.reset();
    }
}

//==============================================================================
void KerfAudioProcessor::previewNote (int note, float velocity, bool playOnce)
{
    const auto scope = uiNoteFifo.write (1);
    if (scope.blockSize1 > 0)
        uiNotes[(size_t) scope.startIndex1] = { note, velocity, playOnce };
}

void KerfAudioProcessor::previewChop (int chopId, float velocity, bool playOnce, double from)
{
    const auto scope = uiNoteFifo.write (1);
    if (scope.blockSize1 > 0)
        uiNotes[(size_t) scope.startIndex1] = { -1, velocity, playOnce, chopId, from };
}

void KerfAudioProcessor::queuePreview (int afterId, int nextId)
{
    const auto scope = uiNoteFifo.write (1);
    if (scope.blockSize1 > 0)
        uiNotes[(size_t) scope.startIndex1] = { -1, 0.9f, true, nextId, -1.0, afterId, true };
}

//==============================================================================
void KerfAudioProcessor::handleAsyncUpdate()
{
    publishSnapshot();
    sendChangeMessage();
}

void KerfAudioProcessor::fillPatternData (PatternData& out) const
{
    // Chops in timeline order (random steps pick from their notes), then the events with
    // their chop's note and place in that list.
    const auto list = chops.getChops();
    out.numChopNotes = 0;
    out.numEvents = 0;
    out.numSteps = pattern.getNumSteps();
    for (const auto& c : list)
        if (c.note >= 0 && out.numChopNotes < (int) out.chopNotes.size())
            out.chopNotes[(size_t) out.numChopNotes++] = c.note;

    for (const auto& e : pattern.getEvents())
    {
        if (out.numEvents >= PatternData::maxEvents)
            break;
        int note = -1, index = -1, k = 0;
        for (const auto& c : list)
        {
            if (c.id == e.chopId && c.note >= 0) { note = c.note; index = k; break; }
            k += c.note >= 0 ? 1 : 0;
        }
        out.events[(size_t) out.numEvents] = e;
        out.notes[(size_t) out.numEvents] = note;
        out.chopIndex[(size_t) out.numEvents] = index;
        ++out.numEvents;
    }
}

void KerfAudioProcessor::publishSnapshot()
{
    chordMinorForAudio.store (getSoundingKey().minor); // GRAIN CHORD: the sample's major / minor

    EngineSnapshot::Ptr s (new EngineSnapshot());
    s->sample = info.sample;
    s->sourceBpm = info.sourceBpm;
    s->beatOrigin = chops.getBeatOrigin();
    rootNoteForAudio.store (chops.getRootNote());

    const auto list = chops.getChops();
    for (const auto& c : list)
    {
        if (s->numChops >= maxChops)
            break;

        s->chops[(size_t) s->numChops] = effectiveChop (c);
        if (c.note >= 0 && c.note < 128)
            s->chopIndexForNote[(size_t) c.note] = (int16_t) s->numChops;
        ++s->numChops;
    }

    // Pattern: resolve chop ids to notes now, so the audio thread only reads plain data.
    fillPatternData (s->pattern);

    // TIME: every slot's curves (edited ones from the settings).
    for (int k = 0; k < TimeFactory::numSlots; ++k)
        s->timeSlots[(size_t) k] = getTimeSlot (k);

    // Stretch mode: attach renders that match the current settings exactly.
    s->stretchMode = p.stretchMode->load() > 0.5f;
    if (s->stretchMode)
    {
        for (int i = 0; i < s->numChops; ++i)
        {
            const auto& c = s->chops[(size_t) i];
            if (auto it = stretchCache.find (c.id); it != stretchCache.end() && it->second.request == desiredStretch (c))
            {
                s->rendered[(size_t) i] = it->second.rendered;
                if (it->second.rendered != nullptr)
                    releasePool.add (it->second.rendered.get());
            }
        }
    }

    if (info.sample != nullptr)
        releasePool.add (info.sample.get());

    handoff.publish (s);
}

//==============================================================================
double KerfAudioProcessor::currentStretchRatio() const
{
    // Tempo sync: host tempo / sample tempo. Without both tempos, keep the original speed.
    if (p.tempoSync->load() > 0.5f && info.sourceBpm > 0.0 && hasHostTempo())
        return juce::jlimit (0.25, 4.0, getHostBpm() / info.sourceBpm);
    return 1.0;
}

TimeStretch::Request KerfAudioProcessor::desiredStretch (const ChopParams& c) const
{
    TimeStretch::Request r;
    r.start = c.start;
    r.end = c.end;
    r.semitones = totalSemitones (c, p.pitch->load());
    // Quantise the ratio slightly so tiny host-tempo jitter does not trigger re-renders.
    r.timeRatio = std::round (currentStretchRatio() * 2000.0) / 2000.0;
    return r;
}

void KerfAudioProcessor::updateStretchRenders()
{
    const bool stretchOn = p.stretchMode->load() > 0.5f;

    if (! stretchOn || info.sample == nullptr)
    {
        if (stretchNeeded != 0 || stretchPending != 0)
        {
            stretchNeeded = stretchPending = 0;
            sendChangeMessage();
        }
        return;
    }

    const auto chopList = chops.getChops();
    int needed = 0, pending = 0;
    bool snapshotDirty = false;

    // Forget renders of chops that no longer exist.
    for (auto it = stretchCache.begin(); it != stretchCache.end();)
    {
        const bool exists = std::any_of (chopList.begin(), chopList.end(), [&] (const ChopParams& c) { return c.id == it->first; });
        it = exists ? std::next (it) : stretchCache.erase (it);
    }

    for (const auto& modelChop : chopList)
    {
        const auto c = effectiveChop (modelChop); // pad parameters included
        const auto want = desiredStretch (c);
        if (TimeStretch::isIdentity (want))
        {
            // Nothing to do: plain playback is exact. Drop any stale render.
            if (stretchCache.erase (c.id) > 0)
                snapshotDirty = true;
            continue;
        }

        ++needed;
        const auto cached = stretchCache.find (c.id);
        if (cached != stretchCache.end() && cached->second.request == want)
            continue;

        ++pending;
        const auto inFlight = stretchInFlight.find (c.id);
        if (inFlight != stretchInFlight.end() && inFlight->second == want)
            continue;

        // Launch a background render for this chop.
        stretchInFlight[c.id] = want;
        const auto sample = info.sample;
        const int generation = loadGeneration.load();
        const int chopId = c.id;
        juce::WeakReference<KerfAudioProcessor> weak (this);

        jobs.addJob ([this, weak, sample, want, generation, chopId]
        {
            auto rendered = TimeStretch::render (*sample, want, [this, generation] { return loadGeneration.load() != generation; });

            juce::MessageManager::callAsync ([weak, sample, want, generation, chopId, rendered]
            {
                auto* self = weak.get();
                if (self == nullptr || self->loadGeneration.load() != generation || self->info.sample != sample)
                    return;

                if (auto it = self->stretchInFlight.find (chopId); it != self->stretchInFlight.end() && it->second == want)
                    self->stretchInFlight.erase (it);

                if (rendered != nullptr)
                {
                    self->releasePool.add (rendered.get());
                    self->stretchCache[chopId] = { want, rendered };
                    self->publishSnapshot();
                    self->updateStretchRenders(); // refresh the status counts
                }
            });
        });
    }

    if (needed != stretchNeeded || pending != stretchPending)
    {
        stretchNeeded = needed;
        stretchPending = pending;
        snapshotDirty = true;
        sendChangeMessage();
    }

    if (snapshotDirty)
        publishSnapshot();
}

//==============================================================================
// Pattern

void KerfAudioProcessor::setPatternPlaying (bool shouldPlay)
{
    auto* param = parameters.getParameter (Params::patternPlay);
    param->beginChangeGesture();
    param->setValueNotifyingHost (shouldPlay ? 1.0f : 0.0f);
    param->endChangeGesture();
    sendChangeMessage();
}

//==============================================================================
// Recording / capture

juce::File KerfAudioProcessor::getRecordingsFolder()
{
    // Tests (and power users) can redirect recordings with an environment variable.
    if (const auto overrideDir = juce::SystemStats::getEnvironmentVariable ("KERF_RECORDINGS_DIR", {}); overrideDir.isNotEmpty())
        return juce::File (overrideDir);

    return juce::File::getSpecialLocation (juce::File::userDocumentsDirectory).getChildFile ("KERF").getChildFile ("Recordings");
}

juce::File KerfAudioProcessor::getDragFolder()
{
    return getRecordingsFolder().getParentDirectory().getChildFile ("Drag");
}

bool KerfAudioProcessor::writeChopMidi (int chopId, const juce::File& target) const
{
    const auto chop = getEffectiveChop (chopId);
    if (! chop.has_value() || chop->note < 0 || info.sample == nullptr)
        return false;

    constexpr int ppq = 960;
    const auto bpm = std::max (20.0, getEffectiveTempo());
    const auto seconds = (double) chop->length() / info.sample->sampleRate;
    const auto ticks = std::max (ppq / 16.0, seconds * bpm / 60.0 * ppq); // at least a 64th

    juce::MidiMessageSequence seq;
    seq.addEvent (juce::MidiMessage::textMetaEvent (3, "KERF chop"), 0.0);
    seq.addEvent (juce::MidiMessage::tempoMetaEvent (juce::roundToInt (60000000.0 / bpm)), 0.0);
    seq.addEvent (juce::MidiMessage::noteOn (1, chop->note, (juce::uint8) 110), 0.0);
    seq.addEvent (juce::MidiMessage::noteOff (1, chop->note), ticks);
    seq.addEvent (juce::MidiMessage::endOfTrack(), ticks);
    seq.updateMatchedPairs();

    juce::MidiFile file;
    file.setTicksPerQuarterNote (ppq);
    file.addTrack (seq);
    target.getParentDirectory().createDirectory();
    target.deleteFile();
    juce::FileOutputStream out (target);
    return out.openedOk() && file.writeTo (out);
}

bool KerfAudioProcessor::writePatternMidi (const juce::File& target) const
{
    constexpr int ppq = 960, ticksPerStep = ppq / 4; // 16th-note steps

    juce::MidiMessageSequence seq;
    seq.addEvent (juce::MidiMessage::textMetaEvent (3, "KERF pattern"), 0.0); // track name
    seq.addEvent (juce::MidiMessage::tempoMetaEvent (juce::roundToInt (60000000.0 / std::max (20.0, getEffectiveTempo()))), 0.0);
    seq.addEvent (juce::MidiMessage::timeSignatureMetaEvent (4, 4), 0.0);

    // Swing, humanize, repeats, chance and random chops exactly as the first pass plays
    // them. (A step's pitch and slide have no place in a plain note list: each chop is one
    // note.)
    const auto feel = groove();
    auto data = std::make_unique<PatternData>(); // large: not on the stack
    fillPatternData (*data);
    for (int i = 0; i < data->numEvents; ++i)
        if (const auto note = PatternPlayer::noteFor (*data, i, data->events[(size_t) i].step); note >= 0)
        {
            const auto& e = data->events[(size_t) i];
            std::array<PatternPlayer::Hit, PatternEvent::maxRepeats> hits;
            const auto n = PatternPlayer::hitsFor (e, e.step, feel, hits);
            for (int h = 0; h < n; ++h)
            {
                const auto& hit = hits[(size_t) h];
                const auto velocity = (juce::uint8) juce::jlimit (1, 127, juce::roundToInt (hit.velocity * 127.0f));
                const auto on = std::max (0.0, hit.on * ticksPerStep);
                const auto off = std::max (on + 1.0, hit.off * ticksPerStep);
                seq.addEvent (juce::MidiMessage::noteOn (1, note, velocity), on);
                seq.addEvent (juce::MidiMessage::noteOff (1, note), off);
            }
        }

    // End of track at the pattern length, so the clip is exactly the pattern in the DAW.
    seq.addEvent (juce::MidiMessage::endOfTrack(), (double) (pattern.getNumSteps() * ticksPerStep));
    seq.sort();
    seq.updateMatchedPairs();

    juce::MidiFile file;
    file.setTicksPerQuarterNote (ppq);
    file.addTrack (seq);

    target.getParentDirectory().createDirectory();
    target.deleteFile();
    juce::FileOutputStream out (target);
    return out.openedOk() && file.writeTo (out);
}

bool KerfAudioProcessor::startRecording (AudioRecorder::Source source, double lengthBars)
{
    AudioRecorder::Request r;
    r.source = source;
    r.lengthBeats = lengthBars > 0.0 ? lengthBars * 4.0 : 0.0;
    r.maxSeconds = 120.0;
    r.startOnBar = lengthBars > 0.0;

    // Bars need a tempo: if there is no host tempo, the sample's (or 120 BPM) is used.
    const auto rate = preparedRate.load() > 0.0 ? preparedRate.load() : 48000.0;
    if (lengthBars > 0.0)
        r.maxSeconds = std::min (600.0, r.lengthBeats * 60.0 / std::max (20.0, getEffectiveTempo()) * 1.5 + 2.0);

    if (! recorder.arm (r, rate))
    {
        setStatusMessage ("Recorder busy or out of memory");
        return false;
    }

    statusMessage = {};
    sendChangeMessage();
    return true;
}

void KerfAudioProcessor::pollRecorder()
{
    recorder.forceSettleIfAudioStalled();

    if (auto take = recorder.takeFinished())
        installRecording (std::move (*take), recorder.getSource());
}

void KerfAudioProcessor::installRecording (juce::AudioBuffer<float>&& audio, AudioRecorder::Source source)
{
    const auto rate = preparedRate.load() > 0.0 ? preparedRate.load() : 48000.0;

    // Silence check: an instrument only receives DAW audio if the host routes it in.
    if (source == AudioRecorder::Source::input && audio.getMagnitude (0, audio.getNumSamples()) < 1.0e-5f)
    {
        setStatusMessage ("Recording was silent - no audio reached KERF's input (check the DAW routing)");
        return;
    }

    const auto stamp = juce::Time::getCurrentTime().formatted ("%Y-%m-%d %H%M%S");
    const auto name = juce::String (source == AudioRecorder::Source::input ? "REC " : "CAPTURE ") + stamp;
    const auto file = getRecordingsFolder().getChildFile (name + ".wav");

    // Keep a copy for the file writer; the sample takes ownership of the original.
    auto toWrite = std::make_shared<juce::AudioBuffer<float>> (audio);
    auto result = loader.fromBuffer (std::move (audio), rate, name, file);
    if (result.sample == nullptr)
    {
        setStatusMessage (result.error);
        return;
    }

    ++loadGeneration;
    beginNewSample (result.sample, nullptr, source == AudioRecorder::Source::input ? "Record" : "Capture");
    setStatusMessage ((source == AudioRecorder::Source::input ? "Recorded " : "Captured ") + juce::String (result.sample->getLengthSeconds(), 2)
                      + " s  -  saved to Documents/KERF/Recordings");

    jobs.addJob ([toWrite, file, rate]
    {
        file.getParentDirectory().createDirectory();
        juce::WavAudioFormat wav;
        std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream> (file);
        if (auto writer = wav.createWriterFor (stream, juce::AudioFormatWriterOptions{}.withSampleRate (rate)
                                                                                      .withNumChannels (2).withBitsPerSample (24)))
            writer->writeFromAudioSampleBuffer (*toWrite, 0, toWrite->getNumSamples());
    });
}

//==============================================================================
// Export

namespace
{
    juce::String writeWav (const juce::File& target, const juce::AudioBuffer<float>& audio, double rate, int bitDepth)
    {
        target.deleteFile();
        std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream> (target);
        if (static_cast<juce::FileOutputStream*> (stream.get())->failedToOpen())
            return "Cannot write to " + target.getFullPathName();

        juce::WavAudioFormat wav;
        auto options = juce::AudioFormatWriterOptions{}.withSampleRate (rate).withNumChannels (audio.getNumChannels()).withBitsPerSample (bitDepth);
        if (bitDepth == 32)
            options = options.withSampleFormat (juce::AudioFormatWriterOptions::SampleFormat::floatingPoint);

        auto writer = wav.createWriterFor (stream, options);
        if (writer == nullptr)
            return "WAV writer could not be created";
        return writer->writeFromAudioSampleBuffer (audio, 0, audio.getNumSamples()) ? juce::String() : juce::String ("Writing failed");
    }
}

void KerfAudioProcessor::exportSampleAsync (const juce::File& target, int bitDepth, std::function<void (juce::String)> onDone)
{
    if (info.sample == nullptr)
    {
        if (onDone) onDone ("No sample loaded");
        return;
    }

    auto sample = info.sample;
    juce::WeakReference<KerfAudioProcessor> weak (this);
    jobs.addJob ([sample, target, bitDepth, onDone, weak]
    {
        const auto error = writeWav (target, sample->audio, sample->sampleRate, bitDepth);
        juce::MessageManager::callAsync ([onDone, error, weak, target]
        {
            if (auto* self = weak.get())
                self->setStatusMessage (error.isEmpty() ? "Exported " + target.getFileName() : error);
            if (onDone) onDone (error);
        });
    });
}

void KerfAudioProcessor::exportPatternAsync (const juce::File& target, int bitDepth, int loops, std::function<void (juce::String)> onDone)
{
    renderAsync (target, bitDepth, loops, -1, 0.0, std::move (onDone));
}

void KerfAudioProcessor::exportChopAsync (int chopId, const juce::File& target, std::function<void (juce::String)> onDone)
{
    const auto chop = getEffectiveChop (chopId);
    if (! chop.has_value() || chop->note < 0 || info.sample == nullptr)
    {
        if (onDone) onDone ("No chop to export");
        return;
    }

    // How long the chop plays (pitch changes its speed); a little extra, the tail is trimmed.
    const auto semitones = chop->pitchSemitones + chop->fineCents / 100.0f + p.pitch->load();
    const auto seconds = (double) chop->length() / info.sample->sampleRate / std::pow (2.0, semitones / 12.0);
    renderAsync (target, 24, 1, chop->note, seconds * 1.1 + 0.05, std::move (onDone));
}

void KerfAudioProcessor::exportKitAsync (const juce::File& parent, bool processed, std::function<void (juce::File, juce::String)> onDone)
{
    const auto list = chops.getChops();
    if (info.sample == nullptr || list.empty())
    {
        if (onDone) onDone ({}, "No chops to export");
        return;
    }

    const auto base = juce::File::createLegalFileName (info.sample->name);
    const auto folder = parent.getNonexistentChildFile (base + (processed ? " kit" : " kit (dry)"), {}, false);
    if (! folder.createDirectory())
    {
        if (onDone) onDone ({}, "Cannot create " + folder.getFullPathName());
        return;
    }
    auto fileFor = [folder, base] (size_t index, int note)
    {
        return folder.getChildFile (base + " " + juce::String ((int) index + 1).paddedLeft ('0', 2) + " "
                                    + juce::MidiMessage::getMidiNoteName (note, true, true, middleCOctave) + ".wav");
    };

    juce::WeakReference<KerfAudioProcessor> weak (this);
    auto finish = [weak, folder, onDone, count = (int) list.size()] (juce::String error)
    {
        if (auto* self = weak.get())
            self->setStatusMessage (error.isEmpty() ? "Kit: " + juce::String (count) + " chops in " + folder.getFileName() : error);
        if (onDone) onDone (folder, error);
    };

    if (! processed)
    {
        // Dry: the slices exactly as they are in the sample.
        auto sample = info.sample;
        std::vector<std::pair<juce::File, ChopParams>> jobsToWrite;
        for (size_t i = 0; i < list.size(); ++i)
            jobsToWrite.push_back ({ fileFor (i, list[i].note), list[i] });
        jobs.addJob ([sample, jobsToWrite, finish]
        {
            juce::String error;
            for (const auto& [file, c] : jobsToWrite)
            {
                const auto start = (int) juce::jlimit<int64_t> (0, sample->getNumSamples(), c.start);
                const auto length = (int) juce::jlimit<int64_t> (0, sample->getNumSamples() - start, c.length());
                juce::AudioBuffer<float> slice (sample->getNumChannels(), std::max (1, length));
                for (int ch = 0; ch < sample->getNumChannels(); ++ch)
                    slice.copyFrom (ch, 0, sample->audio, ch, start, length);
                if (const auto e = writeWav (file, slice, sample->sampleRate, 24); e.isNotEmpty() && error.isEmpty())
                    error = e;
            }
            juce::MessageManager::callAsync ([finish, error] { finish (error); });
        });
        return;
    }

    // Processed: each chop rendered through the whole engine (two at a time in the background).
    struct Progress { int remaining; juce::String error; };
    auto progress = std::make_shared<Progress> (Progress { (int) list.size(), {} });
    for (size_t i = 0; i < list.size(); ++i)
        exportChopAsync (list[i].id, fileFor (i, list[i].note), [progress, finish] (juce::String error)
        {
            if (error.isNotEmpty() && progress->error.isEmpty())
                progress->error = error;
            if (--progress->remaining == 0)
                finish (progress->error);
        });
}

void KerfAudioProcessor::resamplePatternAsync (std::function<void (juce::String)> onDone)
{
    if (pattern.getEvents().empty() || info.sample == nullptr)
    {
        setStatusMessage ("Resample: the pattern is empty - build one in SEQ first");
        if (onDone) onDone ("The pattern is empty");
        return;
    }

    // Two passes of the pattern; the second starts with the first one's echoes and tails,
    // so on its own it loops without a seam.
    const auto bpm = getEffectiveTempo();
    const auto loopSeconds = pattern.getNumSteps() * 60.0 / bpm / 4.0;
    setStatusMessage ("Resampling the pattern ...");
    juce::WeakReference<KerfAudioProcessor> weak (this);
    renderAsync ({}, 24, 2, -1, 0.0, nullptr, [weak, bpm, loopSeconds, onDone] (juce::AudioBuffer<float>&& audio, double rate)
    {
        auto* self = weak.get();
        if (self == nullptr)
            return;

        const auto loopFrames = (int) std::llround (loopSeconds * rate);
        juce::AudioBuffer<float> loop (2, std::max (1, loopFrames));
        loop.clear();
        for (int ch = 0; ch < 2; ++ch)
            loop.copyFrom (ch, 0, audio, ch, std::min (loopFrames, audio.getNumSamples()),
                           juce::jlimit (0, loopFrames, audio.getNumSamples() - loopFrames));

        const auto name = "RESAMPLE " + juce::Time::getCurrentTime().formatted ("%Y-%m-%d %H%M%S");
        const auto file = getRecordingsFolder().getChildFile (name + ".wav");
        auto toWrite = std::make_shared<juce::AudioBuffer<float>> (loop);
        auto result = self->loader.fromBuffer (std::move (loop), rate, name, file);
        if (result.sample == nullptr)
        {
            self->setStatusMessage (result.error);
            if (onDone) onDone (result.error);
            return;
        }

        ++self->loadGeneration;
        self->beginNewSample (result.sample, nullptr, "Resample", bpm);
        self->setStatusMessage ("Resampled " + juce::String (result.sample->getLengthSeconds(), 2) + " s at "
                                + juce::String (bpm, bpm == std::round (bpm) ? 0 : 2) + " BPM  -  saved to Documents/KERF/Recordings");
        self->jobs.addJob ([toWrite, file, rate]
        {
            file.getParentDirectory().createDirectory();
            writeWav (file, *toWrite, rate, 24);
        });
        if (onDone) onDone ({});
    });
}

void KerfAudioProcessor::renderAsync (const juce::File& target, int bitDepth, int loops, int singleNote, double noteSeconds,
                                      std::function<void (juce::String)> onDone,
                                      std::function<void (juce::AudioBuffer<float>&&, double)> onAudio)
{
    auto snapshot = handoff.getLatest();
    if (snapshot == nullptr || snapshot->sample == nullptr || (singleNote < 0 && snapshot->pattern.numEvents == 0))
    {
        if (onDone) onDone ("The pattern is empty");
        return;
    }

    // Everything the offline render needs, captured now (parameters read atomically).
    struct Job
    {
        EngineSnapshot::Ptr snapshot;
        double rate, bpm;
        EngineSettings engine;
        PingPongDelay::Settings space;
        ToneProcessor::Settings tone;
        DirtProcessor::Settings dirt;
        WobbleProcessor::Settings wobble;
        EqProcessor::Settings eq;
        PumpProcessor::Settings pump;
        PatternPlayer::Groove groove;
        TimeSlot timeSlot;        // TIME (a copy: the job outlives nothing)
        bool timeOn = false;
        float timeMix = 1.0f;
        double timeCycle = 4.0;
        PadTable pads;
        float masterGain;
        int loops;
        int singleNote = -1;     // >= 0: render this one note (a chop) instead of the pattern
        double noteSeconds = 0.0;
    };

    auto job = std::make_shared<Job>();
    job->snapshot = snapshot;
    job->singleNote = singleNote;
    job->noteSeconds = noteSeconds;
    job->rate = preparedRate.load() > 0.0 ? preparedRate.load() : 48000.0;
    job->bpm = getEffectiveTempo();
    job->loops = juce::jlimit (1, 16, loops);
    job->engine.maxVoices = (int) p.voices->load();
    job->engine.retrigger = (RetriggerMode) (int) p.retrigger->load();
    job->engine.voice.hostSampleRate = job->rate;
    job->engine.voice.globalSemitones = p.pitch->load();
    job->engine.voice.timeRatio = (p.stretchMode->load() < 0.5f && p.tempoSync->load() > 0.5f && info.sourceBpm > 0.0)
                                      ? juce::jlimit (0.25, 4.0, job->bpm / info.sourceBpm) : 1.0;
    auto& gs = job->engine.voice.grain;
    gs.mix = p.grainMix->load(); gs.sizeMs = p.grainSize->load(); gs.density = p.grainDensity->load();
    gs.spray = p.grainSpray->load(); gs.pitchRandom = p.grainPitch->load(); gs.spread = p.grainSpread->load();
    gs.scan = p.grainScan->load(); gs.position = p.grainPosition->load();
    gs.reverse = p.grainReverse->load();
    gs.pitch = p.grainPitch2->load();
    gs.drift = p.grainDrift->load();     gs.shimmer = p.grainShimmer->load();
    fillGrainExtras (gs, job->bpm);
    const auto fxOn = p.fxOff->load() < 0.5f; // tabs switched off render as they sound
    job->space = PingPongDelay::fromMacro (fxOn ? p.space->load() : 0.0f, job->bpm);
    job->tone = { fxOn ? p.filter->load() : 0.0f, p.resonance->load() };
    job->dirt = { fxOn ? p.drive->load() : 0.0f, (DirtProcessor::Mode) (int) p.dirtMode->load() };
    job->wobble = { fxOn ? p.wobble->load() : 0.0f, (WobbleProcessor::Mode) (int) p.wobbleMode->load() };
    job->eq = p.eqOff->load() < 0.5f ? eqSettings().heard() : EqProcessor::Settings {};
    job->pump = fxOn ? pumpSettings() : PumpProcessor::Settings { 0.0f, pumpSettings().mode };
    job->groove = groove();
    if (const auto slot = (int) p.timeSlot->load(); slot > 0 && p.timeOff->load() < 0.5f)
    {
        job->timeOn = true;
        job->timeSlot = getTimeSlot (slot - 1);
        job->timeMix = p.timeMix->load();
        job->timeCycle = getTimeCycleBeats();
    }
    job->pads.rootNote = chops.getRootNote();
    for (int slot = 0; slot < numPads; ++slot)
        job->pads.pads[(size_t) slot] = readPad (slot);
    job->engine.pads = &job->pads;
    job->masterGain = juce::Decibels::decibelsToGain (p.master->load(), -59.9f);

    juce::WeakReference<KerfAudioProcessor> weak (this);
    jobs.addJob ([job, target, bitDepth, onDone, onAudio, weak]
    {
        constexpr int block = 512;
        const auto stepsSeconds = 60.0 / job->bpm / 4.0;
        const auto patternSeconds = job->singleNote >= 0 ? job->noteSeconds : job->snapshot->pattern.numSteps * stepsSeconds * job->loops;
        const auto tailSeconds = job->space.enabled && job->space.amount > 0.001f ? 3.0 : 0.5;
        const auto totalFrames = (int) std::ceil ((patternSeconds + tailSeconds) * job->rate);
        const auto patternFrames = (int) std::ceil (patternSeconds * job->rate);

        // On the heap: the engine (40 voices with their grain pools) is too big for a worker
        // thread's stack.
        auto engineStorage = std::make_unique<SamplerEngine>();
        auto& engine = *engineStorage;
        engine.prepare (job->rate);
        engine.setSnapshot (job->snapshot.get());
        PatternPlayer player;
        player.prepare (job->rate);
        PingPongDelay delay;
        delay.prepare (job->rate, block);
        ToneProcessor toneFx;
        toneFx.prepare (job->rate, block);
        DirtProcessor dirtFx;
        dirtFx.prepare (job->rate, block);
        WobbleProcessor wobbleFx;
        wobbleFx.prepare (job->rate, block);
        EqProcessor eqFx;
        eqFx.prepare (job->rate, block);
        PumpProcessor pumpFx;
        pumpFx.prepare (job->rate, block);
        TimeShaper timeFx;
        timeFx.prepare (job->rate);

        juce::AudioBuffer<float> out (2, totalFrames), mix (2, block), send (2, block), scratch (2, block);
        out.clear();
        juce::MidiBuffer midi;
        midi.ensureSize (4096);

        for (int pos = 0; pos < totalFrames; pos += block)
        {
            const auto n = std::min (block, totalFrames - pos);
            mix.clear();
            send.clear();
            midi.clear();

            const auto ppq = pos / job->rate * job->bpm / 60.0;
            if (job->singleNote >= 0)
            {
                // One chop: on at the start, off when it has played (gate / loop chops).
                if (pos == 0)
                    midi.addEvent (juce::MidiMessage::noteOn (1, job->singleNote, 0.9f), 0);
                if (patternFrames >= pos && patternFrames < pos + n)
                    midi.addEvent (juce::MidiMessage::noteOff (1, job->singleNote), patternFrames - pos);
            }
            else
            {
                player.process (job->snapshot->pattern, pos < patternFrames, ppq, job->bpm, n, midi, job->groove);
            }

            const VoiceBusses busses { mix.getWritePointer (0), mix.getWritePointer (1), send.getWritePointer (0), send.getWritePointer (1),
                                       scratch.getWritePointer (0), scratch.getWritePointer (1) };
            TransportInfo t { true, job->bpm, ppq, 4.0 };
            engine.process (busses, n, midi, job->engine, t);
            delay.process (send.getReadPointer (0), send.getReadPointer (1), mix.getWritePointer (0), mix.getWritePointer (1), n, job->space);
            juce::AudioBuffer<float> view (mix.getArrayOfWritePointers(), 2, 0, n);
            dirtFx.process (view, n, job->dirt);
            toneFx.process (view, n, job->tone);
            eqFx.process (view, n, job->eq);
            wobbleFx.process (view, n, job->wobble);
            pumpFx.process (view, n, job->pump, ppq, job->bpm);
            if (job->timeOn && job->singleNote < 0) // the pattern through TIME (a single chop stays as it is)
                timeFx.process (view, n, { &job->timeSlot, job->timeMix, job->timeCycle }, ppq, job->bpm);

            for (int ch = 0; ch < 2; ++ch)
                out.copyFrom (ch, pos, mix.getReadPointer (ch), n, job->masterGain);
        }

        // A single chop ends where its sound ends (no silent tail in the DAW).
        if (job->singleNote >= 0)
        {
            int last = 0;
            for (int ch = 0; ch < 2; ++ch)
                for (int i = out.getNumSamples(); --i > last;)
                    if (std::abs (out.getSample (ch, i)) > 0.0003f) { last = i; break; }
            out.setSize (2, std::min (out.getNumSamples(), last + (int) (0.02 * job->rate) + 1), true);
        }

        if (onAudio != nullptr)
        {
            auto audio = std::make_shared<juce::AudioBuffer<float>> (std::move (out));
            const auto rate = job->rate;
            juce::MessageManager::callAsync ([onAudio, onDone, audio, rate]
            {
                onAudio (std::move (*audio), rate);
                if (onDone) onDone ({});
            });
            return;
        }

        const auto error = writeWav (target, out, job->rate, bitDepth);
        juce::MessageManager::callAsync ([onDone, error, weak, target]
        {
            if (auto* self = weak.get())
                self->setStatusMessage (error.isEmpty() ? "Exported " + target.getFileName() : error);
            if (onDone) onDone (error);
        });
    });
}

//==============================================================================
void KerfAudioProcessor::loadSampleAsync (const juce::File& file)
{
    const int generation = ++loadGeneration;
    info.loading = true;
    info.error = {};
    sendChangeMessage();

    juce::WeakReference<KerfAudioProcessor> weak (this);

    jobs.addJob ([this, weak, file, generation]
    {
        auto result = loader.loadFile (file);
        std::shared_ptr<const WaveformCache> waveform;
        if (result.sample != nullptr)
            waveform = std::make_shared<const WaveformCache> (result.sample->audio, result.sample->sampleRate);

        juce::MessageManager::callAsync ([weak, result, waveform, generation]
        {
            auto* self = weak.get();
            if (self == nullptr || generation != self->loadGeneration.load())
                return;

            self->info.loading = false;

            if (result.sample == nullptr)
            {
                self->info.error = result.error;
                self->sendChangeMessage();
                return;
            }

            self->beginNewSample (result.sample, waveform, "Load " + result.sample->name);
        });
    });
}

void KerfAudioProcessor::installSample (SampleData::Ptr sample, bool resetChopList)
{
    info.sample = sample;
    info.fingerprint = sample != nullptr ? fingerprint (*sample) : juce::String();
    lastSavedMemory = {};
    info.fileMissing = false;
    info.missingFile = juce::File();
    info.onsets = nullptr;
    info.tempo = {};
    info.key = {};
    embeddedAudio.reset();
    unresolvedSample = juce::ValueTree();
    stretchCache.clear();
    stretchInFlight.clear();

    if (resetChopList)
    {
        // Not undo steps of their own: loading the sample is one (beginNewSample).
        chops.resetForSample (sample != nullptr ? sample->getNumSamples() : 0, false);
        resetPads(); // a new sample starts with neutral pads
        settings.removeProperty ("keyMinor", nullptr); // and in its own mode
        settings.setProperty ("selectedChop", chops.getNumChops() > 0 ? chops.getChops().front().id : 0, nullptr);
    }

    if (info.waveform == nullptr && sample != nullptr)
        info.waveform = std::make_shared<const WaveformCache> (sample->audio, sample->sampleRate);

    publishSnapshot();
    sendChangeMessage();
    prepareEmbeddedAudio (sample);
}

void KerfAudioProcessor::prepareEmbeddedAudio (SampleData::Ptr sample)
{
    // The lossless copy used when the project is saved.
    if (sample == nullptr || sample->getLengthSeconds() > maxEmbeddedSeconds)
        return;

    const int generation = loadGeneration.load();
    juce::WeakReference<KerfAudioProcessor> weak (this);

    jobs.addJob ([weak, sample, generation]
    {
        auto flac = std::make_shared<juce::MemoryBlock> (SampleLoader::encodeFlac (*sample));
        juce::MessageManager::callAsync ([weak, flac, sample, generation]
        {
            if (auto* self = weak.get())
                if (self->loadGeneration.load() == generation && self->info.sample == sample)
                    self->embeddedAudio = std::move (*flac);
        });
    });
}

void KerfAudioProcessor::startAnalysis (SampleData::Ptr sample, bool keepManualBpm)
{
    if (sample == nullptr)
        return;

    info.analysing = true;
    sendChangeMessage();

    const int generation = loadGeneration.load();
    juce::WeakReference<KerfAudioProcessor> weak (this);

    jobs.addJob ([this, weak, sample, generation, keepManualBpm]
    {
        auto cancelled = [this, generation] { return loadGeneration.load() != generation; };

        auto onsets = std::make_shared<OnsetAnalysis> (TransientDetector::analyse (sample->audio, sample->sampleRate, cancelled));
        if (cancelled()) return;
        const auto tempo = TempoAnalyzer::estimate (*onsets, sample->getNumSamples(), &sample->audio);
        if (cancelled()) return;
        const auto key = KeyAnalyzer::estimate (sample->audio, sample->sampleRate, cancelled);
        if (cancelled()) return;

        juce::MessageManager::callAsync ([weak, onsets, tempo, key, generation, keepManualBpm]
        {
            auto* self = weak.get();
            if (self == nullptr || generation != self->loadGeneration.load())
                return;

            self->info.onsets = onsets;
            self->info.tempo = tempo;
            self->info.key = key;
            self->info.analysing = false;

            if (! (keepManualBpm && self->info.bpmIsManual))
            {
                self->info.sourceBpm = tempo.bpm;
                self->info.bpmIsManual = false;
            }

            self->sourceBpmForAudio.store (self->info.sourceBpm);
            self->publishSnapshot();
            self->sendChangeMessage();
        });
    });
}

void KerfAudioProcessor::setSourceBpm (double bpm, bool manual)
{
    // Undoable; a drag on the BPM field (rapid edits) becomes one step.
    const auto toManual = manual && bpm > 0.0;
    const auto toBpm = manual ? juce::jlimit (0.0, 400.0, bpm) : info.tempo.bpm;
    const auto now = juce::Time::getMillisecondCounterHiRes();
    if (! (now - lastBpmEdit < 700.0 && ! undoManager.canRedo() && undoManager.getCurrentTransactionName() == "Set BPM"))
        undoManager.beginNewTransaction ("Set BPM");
    lastBpmEdit = now;
    undoManager.perform (new SourceBpmAction (*this, info.sourceBpm, info.bpmIsManual, toBpm, toManual));
}

void KerfAudioProcessor::setSampleShift (double seconds)
{
    if (info.sample == nullptr)
        return;
    const auto origin = -juce::jlimit (-maxSampleShift, maxSampleShift, seconds) * info.sample->sampleRate;
    if (std::abs (origin - chops.getBeatOrigin()) < 0.5)
        return;
    // A drag (rapid edits) is one undo step.
    const auto now = juce::Time::getMillisecondCounterHiRes();
    if (! (now - lastShiftEdit < 700.0 && ! undoManager.canRedo() && undoManager.getCurrentTransactionName() == "Shift sample"))
        undoManager.beginNewTransaction ("Shift sample");
    lastShiftEdit = now;
    if (std::abs (origin) < 0.5)
        chops.getState().removeProperty (IDs::beatOrigin, &undoManager);
    else
        chops.getState().setProperty (IDs::beatOrigin, origin, &undoManager);
}

double KerfAudioProcessor::getSampleShift() const
{
    const auto origin = chops.getBeatOrigin();
    return info.sample != nullptr && origin != 0.0 ? -origin / info.sample->sampleRate : 0.0;
}


//==============================================================================
// Key

namespace
{
    /** The detected tonic as seen in the wanted mode (relative major / minor). */
    int baseTonic (const KeyEstimate& detected, bool minor)
    {
        if (minor == detected.minor)
            return detected.tonic;
        return minor ? detected.tonic - 3 : detected.tonic + 3;
    }
}

KeyEstimate KerfAudioProcessor::getSoundingKey() const
{
    auto k = info.key;
    if (k.tonic < 0)
        return k;

    const auto minor = settings.hasProperty ("keyMinor") ? (bool) settings.getProperty ("keyMinor") : k.minor;
    const auto shift = juce::roundToInt (p.pitch->load());
    k.tonic = ((baseTonic (info.key, minor) + shift) % 12 + 12) % 12;
    k.minor = minor;
    return k;
}

void KerfAudioProcessor::setSoundingKey (int tonic, bool minor)
{
    if (info.key.tonic < 0)
        return;

    // Smallest move: -6 .. +5 semitones.
    auto shift = ((tonic - baseTonic (info.key, minor)) % 12 + 12) % 12;
    if (shift > 5)
        shift -= 12;

    // One undo step for the mode and the transposition together.
    undoManager.beginNewTransaction ("Change key");
    if (minor == info.key.minor)
        settings.removeProperty ("keyMinor", &undoManager);
    else
        settings.setProperty ("keyMinor", minor, &undoManager);

    const ParamUndoGroup group (*this, "Change key");
    paramGroupStarted = true; // record into the transaction begun above
    setParameterAsGesture (Params::globalPitch, (float) shift);
    sendChangeMessage();
}
bool KerfAudioProcessor::tempoChanged() const
{
    return info.bpmIsManual;
}

bool KerfAudioProcessor::keyChanged() const
{
    return info.key.tonic >= 0 && (settings.hasProperty ("keyMinor") || std::abs (p.pitch->load()) > 0.01f);
}

void KerfAudioProcessor::resetTempo()
{
    if (! info.bpmIsManual)
        return;
    undoManager.beginNewTransaction ("Auto tempo");
    undoManager.perform (new SourceBpmAction (*this, info.sourceBpm, true, info.tempo.bpm, false));
    undoManager.beginNewTransaction();
    sendChangeMessage();
}

void KerfAudioProcessor::resetKey()
{
    if (! keyChanged())
        return;
    undoManager.beginNewTransaction ("Auto key");
    if (settings.hasProperty ("keyMinor"))
        settings.removeProperty ("keyMinor", &undoManager);
    if (std::abs (p.pitch->load()) > 0.01f)
    {
        const ParamUndoGroup group (*this, "Auto key");
        paramGroupStarted = true; // record into the transaction begun above
        setParameterAsGesture (Params::globalPitch, 0.0f);
    }
    undoManager.beginNewTransaction();
    sendChangeMessage();
}

void KerfAudioProcessor::resetTempoAndKey()
{
    undoManager.beginNewTransaction ("Auto tempo and key");
    if (info.bpmIsManual)
        undoManager.perform (new SourceBpmAction (*this, info.sourceBpm, true, info.tempo.bpm, false));
    if (info.key.tonic >= 0)
    {
        if (settings.hasProperty ("keyMinor"))
            settings.removeProperty ("keyMinor", &undoManager);
        if (std::abs (p.pitch->load()) > 0.01f)
        {
            const ParamUndoGroup group (*this, "Auto tempo and key");
            paramGroupStarted = true; // record into the transaction begun above
            setParameterAsGesture (Params::globalPitch, 0.0f);
        }
    }
    undoManager.beginNewTransaction();
    sendChangeMessage();
}

void KerfAudioProcessor::applySourceBpm (double bpm, bool manual)
{
    info.sourceBpm = juce::jlimit (0.0, 400.0, bpm);
    info.bpmIsManual = manual && bpm > 0.0;

    if (! manual)
        info.sourceBpm = info.tempo.bpm;

    sourceBpmForAudio.store (info.sourceBpm);
    publishSnapshot();
    sendChangeMessage();
}

void KerfAudioProcessor::clearError()
{
    info.error = {};
    sendChangeMessage();
}

//==============================================================================
int64_t KerfAudioProcessor::regionForChopping (int64_t& start) const
{
    start = 0;
    auto end = info.sample != nullptr ? info.sample->getNumSamples() : 0;

    if ((int) settings.getProperty ("chopScope", 0) == 1)
        if (const auto c = chops.getChop ((int) settings.getProperty ("selectedChop", 0)))
        {
            start = c->start;
            end = c->end;
        }

    return end;
}

void KerfAudioProcessor::replaceChopsUndoable (const std::vector<int64_t>& boundaries, const juce::String& actionName)
{
    if (info.sample == nullptr || boundaries.size() < 2)
        return;

    // The same cuts as already there (APPLY pressed again without a change): nothing to do -
    // no undo step, no new chops, no animation.
    {
        std::vector<int64_t> existing;
        for (const auto& c : chops.getChops())
            for (const auto edge : { c.start, c.end })
                if (edge >= boundaries.front() && edge <= boundaries.back())
                    existing.push_back (edge);
        std::sort (existing.begin(), existing.end());
        existing.erase (std::unique (existing.begin(), existing.end()), existing.end());
        auto wanted = boundaries;
        std::sort (wanted.begin(), wanted.end());
        wanted.erase (std::unique (wanted.begin(), wanted.end()), wanted.end());
        // (More cuts than keys: compared as they will be made, see below.)
        if ((int) settings.getProperty ("chopScope", 0) != 1 && (int) wanted.size() - 1 > chops.chopLimit())
        {
            const auto last = wanted.back();
            wanted.resize ((size_t) chops.chopLimit());
            wanted.push_back (last);
        }
        if (existing == wanted)
            return;
    }

    undoManager.beginNewTransaction (actionName);

    const auto selected = (int) settings.getProperty ("selectedChop", 0);
    bool changed = false;

    if ((int) settings.getProperty ("chopScope", 0) == 1 && chops.getChop (selected))
        changed = chops.subdivide (selected, boundaries);
    else
        changed = chops.replaceWithBoundaries (boundaries);

    if (changed && ! chops.getChop (selected))
        settings.setProperty ("selectedChop", chops.getNumChops() > 0 ? chops.getChops().front().id : 0, nullptr);

    // More cuts than keys: say why there are fewer chops.
    if (changed && (int) boundaries.size() - 1 > chops.getNumChops() && chops.getNumChops() >= chops.chopLimit())
        setStatusMessage ("At most " + juce::String (chops.chopLimit()) + " chops: one per key from the root up (the last one runs to the end)");
}

void KerfAudioProcessor::chopByGrid (NoteValue value, NoteModifier modifier)
{
    if (info.sample == nullptr) return;
    int64_t start; const auto end = regionForChopping (start);
    const auto bpm = info.sourceBpm > 0.0 ? info.sourceBpm : getEffectiveTempo();
    // The whole sample with SHIFT: on the beat lines' grid (the chop scope: from the chop's start).
    if (const auto origin = chops.getBeatOrigin(); origin != 0.0 && start == 0)
        replaceChopsUndoable (ChopTools::byBeats (bpm, info.sample->sampleRate, start, end, gridStepBeats (value, modifier), origin), "Grid chop");
    else
        replaceChopsUndoable (rhythmicGrid (start, end, bpm, info.sample->sampleRate, value, modifier), "Grid chop");
}

void KerfAudioProcessor::deleteAllChops()
{
    if (info.sample == nullptr || (chops.getNumChops() == 1 && chops.getChops().front().length() >= info.sample->getNumSamples()))
        return; // (already one chop over the whole sample)
    undoManager.beginNewTransaction ("Delete all chops");
    chops.resetForSample (info.sample->getNumSamples(), true);
    settings.setProperty ("selectedChop", chops.getNumChops() > 0 ? chops.getChops().front().id : 0, nullptr);
    undoManager.beginNewTransaction();
    setStatusMessage ("All chops deleted (Ctrl+Z brings them back)");
}

//==============================================================================
// Selection

namespace
{
    const juce::Identifier selectedChopsId ("selectedChops"); // the others (Ctrl), comma separated
}

void KerfAudioProcessor::valueTreePropertyChanged (juce::ValueTree& t, const juce::Identifier& property)
{
    // A chop chosen any way but with Ctrl: it alone is selected.
    if (t == settings && property.toString() == "selectedChop" && ! keepSelection && settings.hasProperty (selectedChopsId))
        settings.removeProperty (selectedChopsId, nullptr);
    if (affectsEngine (t))
        triggerAsyncUpdate();
}

std::vector<int> KerfAudioProcessor::getSelectedChops() const
{
    std::vector<int> ids;
    const auto primary = (int) settings.getProperty ("selectedChop", 0);
    if (chops.getChop (primary).has_value())
        ids.push_back (primary);
    for (const auto& token : juce::StringArray::fromTokens (settings.getProperty (selectedChopsId).toString(), ",", {}))
        if (const auto id = token.getIntValue(); id != primary && chops.getChop (id).has_value()
                                                  && std::find (ids.begin(), ids.end(), id) == ids.end())
            ids.push_back (id);
    return ids;
}

bool KerfAudioProcessor::isChopSelected (int chopId) const
{
    const auto ids = getSelectedChops();
    return std::find (ids.begin(), ids.end(), chopId) != ids.end();
}

void KerfAudioProcessor::toggleChopSelection (int chopId)
{
    if (! chops.getChop (chopId).has_value())
        return;

    auto ids = getSelectedChops();
    if (const auto it = std::find (ids.begin(), ids.end(), chopId); it != ids.end())
    {
        if (ids.size() == 1)
            return; // the last one stays
        ids.erase (it); // out of the selection; the one shown is the first left
    }
    else
    {
        ids.insert (ids.begin(), chopId); // in, and shown
    }

    juce::StringArray others;
    for (size_t i = 1; i < ids.size(); ++i)
        others.add (juce::String (ids[i]));
    const juce::ScopedValueSetter<bool> keep (keepSelection, true);
    settings.setProperty (selectedChopsId, others.joinIntoString (","), nullptr);
    settings.setProperty ("selectedChop", ids.front(), nullptr);
}

void KerfAudioProcessor::deleteSelectedChops()
{
    if (info.sample == nullptr)
        return;
    const auto ids = getSelectedChops();
    if (ids.empty())
        return;
    if (ids.size() >= (size_t) chops.getNumChops())
    {
        deleteAllChops(); // all of them: one chop over the whole sample again
        return;
    }

    // What stays selected: the chop after the first deleted one (else the one before it).
    const auto all = chops.getChops();
    int next = 0;
    for (size_t i = 0; i < all.size() && next == 0; ++i)
        if (all[i].id == ids.front())
        {
            for (auto k = i + 1; k < all.size() && next == 0; ++k)
                if (std::find (ids.begin(), ids.end(), all[k].id) == ids.end()) next = all[k].id;
            for (auto k = (int) i - 1; k >= 0 && next == 0; --k)
                if (std::find (ids.begin(), ids.end(), all[(size_t) k].id) == ids.end()) next = all[(size_t) k].id;
        }

    undoManager.beginNewTransaction (ids.size() == 1 ? "Delete chop" : "Delete " + juce::String ((int) ids.size()) + " chops");
    for (const auto id : ids)
        chops.removeChop (id);
    settings.setProperty ("selectedChop", next, nullptr);
    undoManager.beginNewTransaction();
    setStatusMessage (ids.size() == 1 ? "Chop deleted (Ctrl+Z brings it back)"
                                      : juce::String ((int) ids.size()) + " chops deleted (Ctrl+Z brings them back)");
}

void KerfAudioProcessor::setSelectedChopsSound (const juce::Identifier& field, float value, const juce::String& undoName)
{
    undoManager.beginNewTransaction (undoName);
    for (const auto id : getSelectedChops())
    {
        if (auto* param = padParameter (id, field))
        {
            const auto from = param->getValue(), to = param->convertTo0to1 (value);
            if (std::abs (from - to) >= 1.0e-6f)
                undoManager.perform (new ParameterChangeAction (*param, from, to, applyingParamUndo));
        }
        else
        {
            chops.setParam (id, field, value);
        }
    }
    undoManager.beginNewTransaction();
}

void KerfAudioProcessor::chopEqual (int count)
{
    if (info.sample == nullptr) return;
    int64_t start; const auto end = regionForChopping (start);
    replaceChopsUndoable (equalSlices (start, end, count), "Equal slices");
}

void KerfAudioProcessor::chopByTransients (float sensitivity, double minDistanceSeconds)
{
    if (info.sample == nullptr || info.onsets == nullptr) return;
    int64_t start; const auto end = regionForChopping (start);
    replaceChopsUndoable (ChopTools::byTransients (*info.onsets, start, end, sensitivity, minDistanceSeconds, chops.chopLimit()), "Transient chop");
}

void KerfAudioProcessor::chopByBeats (double beatsPerChop)
{
    if (info.sample == nullptr) return;
    int64_t start; const auto end = regionForChopping (start);
    // The BPM shown in the header (yours or the detected one), on the beat lines of the waveform.
    const auto bpm = info.sourceBpm > 0.0 ? info.sourceBpm : info.tempo.bpm;
    if (bpm <= 0.0) return;
    replaceChopsUndoable (ChopTools::byBeats (bpm, info.sample->sampleRate, start, end, beatsPerChop, chops.getBeatOrigin()), "Beat chop");
}

void KerfAudioProcessor::chopRandom (int count, uint32_t seed)
{
    if (info.sample == nullptr) return;
    int64_t start; const auto end = regionForChopping (start);
    replaceChopsUndoable (ChopTools::random (start, end, count, seed, 0.35f, info.onsets.get(), info.sample->sampleRate), "Random chop");
}

void KerfAudioProcessor::chopSmart (int targetCount)
{
    if (info.sample == nullptr || info.onsets == nullptr) return;
    int64_t start; const auto end = regionForChopping (start);
    ChopTools::RuleBasedSuggester suggester;
    auto tempo = info.tempo;
    if (info.sourceBpm > 0.0) tempo.bpm = info.sourceBpm;
    replaceChopsUndoable (suggester.suggest (info.sample->audio, info.sample->sampleRate, *info.onsets, tempo, start, end, targetCount),
                          "Smart chop");
}

void KerfAudioProcessor::resetChops()
{
    if (info.sample == nullptr) return;
    undoManager.beginNewTransaction ("Reset chops");
    chops.replaceWithBoundaries ({ 0, info.sample->getNumSamples() });
    settings.setProperty ("selectedChop", chops.getNumChops() > 0 ? chops.getChops().front().id : 0, nullptr);
}

//==============================================================================
juce::ValueTree KerfAudioProcessor::createStateTree()
{
    juce::ValueTree root (idKERF);
    root.setProperty (idVersion, stateVersion, nullptr);
    root.setProperty (idPads, 1, nullptr); // per-chop sound settings live in pad parameters
    root.appendChild (parameters.copyState(), nullptr);
    root.appendChild (chops.getState().createCopy(), nullptr);
    root.appendChild (settings.createCopy(), nullptr);
    root.appendChild (pattern.getState().createCopy(), nullptr);

    if (info.sample != nullptr)
    {
        juce::ValueTree s (idSAMPLE);
        s.setProperty (idName, info.sample->name, nullptr);
        s.setProperty (idPath, info.sample->file.getFullPathName(), nullptr);
        s.setProperty (idRate, info.sample->sampleRate, nullptr);
        s.setProperty (idFrames, (juce::int64) info.sample->getNumSamples(), nullptr);
        s.setProperty (idSourceBpm, info.sourceBpm, nullptr);
        s.setProperty (idBpmManual, info.bpmIsManual, nullptr);

        if (info.sample->getLengthSeconds() <= maxEmbeddedSeconds)
        {
            if (embeddedAudio.isEmpty()) // background encode not finished yet: do it now
                embeddedAudio = SampleLoader::encodeFlac (*info.sample);
            s.setProperty (idAudio, juce::var (embeddedAudio), nullptr);
        }

        root.appendChild (s, nullptr);
    }
    else if (unresolvedSample.isValid())
    {
        // The project referenced a sample we could not find. Keep the reference so saving
        // again does not lose it.
        root.appendChild (unresolvedSample.createCopy(), nullptr);
    }

    return root;
}

void KerfAudioProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    juce::MemoryOutputStream out (destData, false);
    out.writeInt ((int) stateMagic);
    createStateTree().writeToStream (out);
}

void KerfAudioProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    if (data == nullptr || sizeInBytes < 8)
        return;

    juce::MemoryInputStream in (data, (size_t) sizeInBytes, false);
    if ((juce::uint32) in.readInt() != stateMagic)
        return; // not our data: ignore rather than wipe the current state

    auto tree = juce::ValueTree::readFromStream (in);
    if (! tree.hasType (idKERF))
        return;

    if (juce::MessageManager::getInstance()->isThisTheMessageThread())
    {
        applyStateTree (tree);
    }
    else
    {
        juce::WeakReference<KerfAudioProcessor> weak (this);
        juce::MessageManager::callAsync ([weak, tree]
        {
            if (auto* self = weak.get())
                self->applyStateTree (tree);
        });
    }
}

//==============================================================================
namespace
{
    /** Projects from before v0.4.5 had a fixed three-band EQ. Its bands that did something
        become points of the new EQ (low shelf, bell, high shelf). */
    juce::ValueTree migrateLegacyEq (juce::ValueTree params)
    {
        auto find = [&params] (const juce::String& id) { return params.getChildWithProperty ("id", id); };
        auto value = [&find] (const char* id, float fallback)
        {
            const auto child = find (id);
            return child.isValid() ? (float) child.getProperty ("value") : fallback;
        };
        auto set = [&params, &find] (const juce::String& id, float v)
        {
            auto child = find (id);
            if (! child.isValid())
            {
                child = juce::ValueTree ("PARAM");
                child.setProperty ("id", id, nullptr);
                params.appendChild (child, nullptr);
            }
            child.setProperty ("value", v, nullptr);
        };

        if (! find (Params::LegacyEq::lowGain).isValid() || find (Params::eqParamID (0, Params::EqField::on)).isValid())
            return params; // not an old project (or already migrated)

        struct Old { float gain, freq, q; int type; };
        const Old old[] = {
            { value (Params::LegacyEq::lowGain, 0.0f),  value (Params::LegacyEq::lowFreq, 120.0f),   0.9f,                                   (int) EqProcessor::Type::lowShelf },
            { value (Params::LegacyEq::midGain, 0.0f),  value (Params::LegacyEq::midFreq, 1000.0f),  value (Params::LegacyEq::midQ, 0.9f),   (int) EqProcessor::Type::bell },
            { value (Params::LegacyEq::highGain, 0.0f), value (Params::LegacyEq::highFreq, 6000.0f), 0.9f,                                   (int) EqProcessor::Type::highShelf },
        };

        int band = 0;
        for (const auto& o : old)
        {
            if (std::abs (o.gain) < 0.05f)
                continue;
            set (Params::eqParamID (band, Params::EqField::on), 1.0f);
            set (Params::eqParamID (band, Params::EqField::type), (float) o.type);
            set (Params::eqParamID (band, Params::EqField::freq), o.freq);
            set (Params::eqParamID (band, Params::EqField::gain), o.gain);
            set (Params::eqParamID (band, Params::EqField::q), o.q);
            ++band;
        }
        return params;
    }
}

void KerfAudioProcessor::applyStateTree (const juce::ValueTree& tree)
{
    ++loadGeneration; // cancel any running load/analysis

    if (auto params = tree.getChildWithName (idPARAMS); params.isValid())
        parameters.replaceState (migrateLegacyEq (params.createCopy()));

    if (auto s = tree.getChildWithName (idSETTINGS); s.isValid())
        settings.copyPropertiesAndChildrenFrom (s, nullptr);
    // LOOP and the METRONOME (toolbar) work without the editor being open.
    setPreviewLoop ((bool) settings.getProperty ("previewLoop", false));
    setMetronome ((bool) settings.getProperty ("metronome", false));

    const auto sampleTree = tree.getChildWithName (idSAMPLE);
    SampleData::Ptr sample;
    juce::String error;

    if (sampleTree.isValid())
    {
        const auto name = sampleTree.getProperty (idName).toString();
        const juce::File file (sampleTree.getProperty (idPath).toString());

        if (const auto* block = sampleTree.getProperty (idAudio).getBinaryData(); block != nullptr && block->getSize() > 0)
        {
            auto r = loader.loadFromMemory (block->getData(), block->getSize(), name, file);
            sample = r.sample;
            error = r.error;
        }
        else if (file.existsAsFile())
        {
            auto r = loader.loadFile (file);
            sample = r.sample;
            error = r.error;
        }

        if (sample == nullptr)
        {
            info = {};
            info.fileMissing = true;
            info.missingFile = file;
            info.error = error.isNotEmpty() ? error : "Sample not found: " + file.getFullPathName();
            unresolvedSample = sampleTree.createCopy();
        }
    }

    info.waveform = nullptr;
    const auto missing = info.fileMissing;
    const auto missingFile = info.missingFile;
    const auto missingError = info.error;
    const auto keepUnresolved = unresolvedSample;

    installSample (sample, false);

    if (missing)
    {
        info.fileMissing = true;
        info.missingFile = missingFile;
        info.error = missingError;
        unresolvedSample = keepUnresolved;
    }

    // Chops: restore after the sample so lengths can be validated against it.
    if (auto c = tree.getChildWithName (IDs::CHOPS); c.isValid())
    {
        auto copy = c.createCopy();
        if (sample != nullptr)
            copy.setProperty (IDs::sampleLength, (juce::int64) sample->getNumSamples(), nullptr);
        chops.replaceState (copy);
    }

    if (sample != nullptr && chops.getNumChops() == 0)
        chops.resetForSample (sample->getNumSamples());

    if (auto pt = tree.getChildWithName (PatternModel::PATTERN); pt.isValid())
        pattern.replaceState (pt);
    else
        pattern.replaceState (juce::ValueTree (PatternModel::PATTERN));

    // Projects saved before pads existed: move each chop's settings onto its pad.
    if (! tree.hasProperty (idPads))
        for (const auto& c : chops.getChops())
            if (const auto slot = padSlotFor (c); slot >= 0)
                writePad (slot, PadSound::from (c));

    // A project from before preset sounds were kept: its sound is the preset as it stands.
    if (! settings.getChildWithName (presetSoundsId).getChildWithProperty ("name", getPresetName()).isValid())
        rememberPresetSound (getPresetName());

    undoManager.clearUndoHistory();

    info.sourceBpm = sampleTree.getProperty (idSourceBpm, 0.0);
    info.bpmIsManual = sampleTree.getProperty (idBpmManual, false);
    sourceBpmForAudio.store (info.sourceBpm);

    publishSnapshot();
    sendChangeMessage();

    if (sample != nullptr)
        startAnalysis (sample, true);
}

//==============================================================================
juce::AudioProcessorEditor* KerfAudioProcessor::createEditor()
{
    return new KerfAudioProcessorEditor (*this);
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new KerfAudioProcessor();
}

EqProcessor::Settings KerfAudioProcessor::eqSettings() const noexcept
{
    EqProcessor::Settings s;
    for (size_t b = 0; b < s.bands.size(); ++b)
    {
        auto& band = s.bands[b];
        const auto& ptr = p.eq[b];
        band.on = ptr[0]->load() > 0.5f;
        band.type = (EqProcessor::Type) juce::jlimit (0, 5, (int) ptr[1]->load());
        band.freq = ptr[2]->load();
        band.gainDb = ptr[3]->load();
        band.q = ptr[4]->load();
        band.bypass = ptr[5]->load() > 0.5f;
    }
    return s;
}

PumpProcessor::Settings KerfAudioProcessor::pumpSettings() const noexcept
{
    return { p.pump->load(), (PumpProcessor::Mode) juce::jlimit (0, 2, (int) p.pumpMode->load()) };
}

//==============================================================================
// Parameter helpers (EQ points)

void KerfAudioProcessor::setParameterAsGesture (const juce::String& paramId, float value)
{
    if (auto* param = parameters.getParameter (paramId))
    {
        param->beginChangeGesture();
        param->setValueNotifyingHost (param->convertTo0to1 (value));
        param->endChangeGesture();
    }
}

int KerfAudioProcessor::addEqPoint (float freq, float gainDb, EqProcessor::Type type)
{
    for (int band = 0; band < Params::eqBands; ++band)
    {
        if (p.eq[(size_t) band][0]->load() > 0.5f)
            continue;

        const ParamUndoGroup group (*this, "Add EQ point");
        setParameterAsGesture (Params::eqParamID (band, Params::EqField::type), (float) (int) type);
        setParameterAsGesture (Params::eqParamID (band, Params::EqField::freq), freq);
        setParameterAsGesture (Params::eqParamID (band, Params::EqField::gain), gainDb);
        setParameterAsGesture (Params::eqParamID (band, Params::EqField::q), type == EqProcessor::Type::lowCut || type == EqProcessor::Type::highCut ? 0.707f : 0.9f);
        setParameterAsGesture (Params::eqParamID (band, Params::EqField::bypass), 0.0f); // a new point is heard
        setParameterAsGesture (Params::eqParamID (band, Params::EqField::on), 1.0f);
        return band;
    }
    return -1;
}

int KerfAudioProcessor::addEqPointInGap (EqProcessor::Type type)
{
    // The middle (in octaves) of the widest gap between existing points.
    std::vector<float> used { 20.0f, 20000.0f };
    for (const auto& b : eqSettings().bands)
        if (b.on)
            used.push_back (b.freq);
    std::sort (used.begin(), used.end());

    float best = 1000.0f, widest = 0.0f;
    for (size_t i = 1; i < used.size(); ++i)
        if (const auto gap = std::log (used[i] / used[i - 1]); gap > widest)
        {
            widest = gap;
            best = std::sqrt (used[i] * used[i - 1]);
        }

    const auto band = addEqPoint (best, 0.0f, type);
    if (band >= 0)
        settings.setProperty ("eqSelected", band, nullptr);
    else
        setStatusMessage ("All " + juce::String (Params::eqBands) + " EQ points are in use");
    return band;
}

void KerfAudioProcessor::toggleEqBypass (int band)
{
    if (! juce::isPositiveAndBelow (band, Params::eqBands) || p.eq[(size_t) band][0]->load() < 0.5f)
        return;
    const auto bypassed = p.eq[(size_t) band][5]->load() > 0.5f;
    const ParamUndoGroup group (*this, bypassed ? "EQ point on" : "Bypass EQ point");
    setParameterAsGesture (Params::eqParamID (band, Params::EqField::bypass), bypassed ? 0.0f : 1.0f);
}

void KerfAudioProcessor::removeEqPoint (int band)
{
    if (! juce::isPositiveAndBelow (band, Params::eqBands))
        return;
    const ParamUndoGroup group (*this, "Remove EQ point");
    setParameterAsGesture (Params::eqParamID (band, Params::EqField::on), 0.0f);
    setParameterAsGesture (Params::eqParamID (band, Params::EqField::bypass), 0.0f);
}

void KerfAudioProcessor::clearEq()
{
    const ParamUndoGroup group (*this, "Clear EQ");
    for (int band = 0; band < Params::eqBands; ++band)
        if (p.eq[(size_t) band][0]->load() > 0.5f)
            setParameterAsGesture (Params::eqParamID (band, Params::EqField::on), 0.0f);
}

//==============================================================================
// Pads: automatable per-chop sound settings

namespace
{
    // Order matches padPtrs[slot][index].
    const char* const padFields[] = { Params::PadField::tune, Params::PadField::fine, Params::PadField::gain, Params::PadField::pan,
                                      Params::PadField::reverse, Params::PadField::mode, Params::PadField::fadeIn, Params::PadField::fadeOut,
                                      Params::PadField::spaceSend, Params::PadField::grainSend, Params::PadField::decay, Params::PadField::sustain };

    const char* padFieldFor (const juce::Identifier& field)
    {
        if (field == IDs::pitch)     return Params::PadField::tune;
        if (field == IDs::fine)      return Params::PadField::fine;
        if (field == IDs::gain)      return Params::PadField::gain;
        if (field == IDs::pan)       return Params::PadField::pan;
        if (field == IDs::reverse)   return Params::PadField::reverse;
        if (field == IDs::mode)      return Params::PadField::mode;
        if (field == IDs::fadeIn)    return Params::PadField::fadeIn;
        if (field == IDs::fadeOut)   return Params::PadField::fadeOut;
        if (field == IDs::spaceSend) return Params::PadField::spaceSend;
        if (field == IDs::grainSend) return Params::PadField::grainSend;
        if (field == IDs::decay)     return Params::PadField::decay;
        if (field == IDs::sustain)   return Params::PadField::sustain;
        return nullptr;
    }

}


void KerfAudioProcessor::initialisePads()
{
    for (int slot = 0; slot < numPads; ++slot)
        for (size_t f = 0; f < std::size (padFields); ++f)
        {
            padPtrs[(size_t) slot][f] = parameters.getRawParameterValue (Params::padParamID (slot, padFields[f]));
            jassert (padPtrs[(size_t) slot][f] != nullptr);
        }

    // Mode / reverse show up in the waveform and pads: refresh the UI when they change.
    for (int slot = 0; slot < numPads; ++slot)
    {
        parameters.addParameterListener (Params::padParamID (slot, Params::PadField::mode), this);
        parameters.addParameterListener (Params::padParamID (slot, Params::PadField::reverse), this);
    }
}

void KerfAudioProcessor::parameterChanged (const juce::String&, float)
{
    triggerAsyncUpdate(); // rebuild the snapshot + notify the UI on the message thread
}

int KerfAudioProcessor::padSlotFor (const ChopParams& c) const
{
    if (c.note < 0)
        return -1;
    const auto slot = c.note - chops.getRootNote();
    return slot >= 0 && slot < numPads ? slot : -1;
}

PadSound KerfAudioProcessor::readPad (int slot) const
{
    PadSound pad;
    if (slot < 0 || slot >= numPads)
        return pad;

    const auto& v = padPtrs[(size_t) slot];
    pad.pitchSemitones = v[0]->load();
    pad.fineCents = v[1]->load();
    pad.gainDb = v[2]->load();
    pad.pan = v[3]->load();
    pad.reverse = v[4]->load() > 0.5f;
    pad.mode = (PlayMode) juce::jlimit (0, 2, (int) v[5]->load());
    pad.fadeInMs = v[6]->load();
    pad.fadeOutMs = v[7]->load();
    pad.spaceSend = v[8]->load();
    pad.grainSend = v[9]->load();
    pad.decayMs = v[10]->load();
    pad.sustain = v[11]->load();
    return pad;
}

void KerfAudioProcessor::refreshPadTable()
{
    padTable.rootNote = rootNoteForAudio.load (std::memory_order_relaxed);
    for (int slot = 0; slot < numPads; ++slot)
        padTable.pads[(size_t) slot] = readPad (slot);
}

void KerfAudioProcessor::writePad (int slot, const PadSound& pad)
{
    auto set = [this, slot] (const char* field, float plainValue)
    {
        if (auto* param = parameters.getParameter (Params::padParamID (slot, field)))
            param->setValueNotifyingHost (param->convertTo0to1 (plainValue));
    };

    set (Params::PadField::tune, pad.pitchSemitones);
    set (Params::PadField::fine, pad.fineCents);
    set (Params::PadField::gain, pad.gainDb);
    set (Params::PadField::pan, pad.pan);
    set (Params::PadField::reverse, pad.reverse ? 1.0f : 0.0f);
    set (Params::PadField::mode, (float) (int) pad.mode);
    set (Params::PadField::fadeIn, pad.fadeInMs);
    set (Params::PadField::fadeOut, pad.fadeOutMs);
    set (Params::PadField::spaceSend, pad.spaceSend);
    set (Params::PadField::grainSend, pad.grainSend);
    set (Params::PadField::decay, pad.decayMs);
    set (Params::PadField::sustain, pad.sustain);
}

void KerfAudioProcessor::resetPads()
{
    for (int slot = 0; slot < numPads; ++slot)
        for (auto* field : padFields)
            if (auto* param = parameters.getParameter (Params::padParamID (slot, field)))
                if (std::abs (param->getValue() - param->getDefaultValue()) > 1.0e-7f) // (the host hears only real changes)
                    param->setValueNotifyingHost (param->getDefaultValue());
}

//==============================================================================
// Loading a sample as one undo step

/** Everything that belongs to one loaded sample. The chops are kept as the very tree
    objects they were, so the undo steps recorded before the swap still find them. */
struct KerfAudioProcessor::SampleSession
{
    SampleInfo info;
    juce::MemoryBlock embeddedAudio;
    juce::ValueTree unresolvedSample;
    juce::NamedValueSet chopProperties;
    juce::Array<juce::ValueTree> chopTrees;
    std::vector<float> pads;            // normalised, slot by slot in padFields order
    juce::var selectedChop, keyMinor;   // void = not set

    int64_t bytes() const
    {
        const auto audio = info.sample != nullptr ? (int64_t) info.sample->getNumChannels() * info.sample->getNumSamples() * (int64_t) sizeof (float) : 0;
        int64_t overview = 0;
        if (info.waveform != nullptr)
            for (const auto& level : info.waveform->getLevels())
                overview += (int64_t) (level.bins.size() * sizeof (WaveformCache::Bin));
        return audio + overview + (int64_t) embeddedAudio.getSize();
    }
};

/** Swaps the current sample's session with the stored one (both ways: undo and redo). */
struct KerfAudioProcessor::SampleSwapAction final : public juce::UndoableAction
{
    SampleSwapAction (KerfAudioProcessor& p, std::unique_ptr<SampleSession> previous)
        : owner (p), other (std::move (previous))
    {
        // Fixed when recorded (the undo manager adds and subtracts the same number): one unit
        // per 4 KB this step holds on its own, the other sample (the current one is held by
        // the processor anyway, and each replaced sample is "the other" of exactly one step).
        units = (int) std::min<int64_t> (1 << 20, 16 + other->bytes() / 4096);
    }

    bool perform() override
    {
        if (std::exchange (alreadyApplied, false))
            return true; // the load itself installed the new sample
        swap();
        return true;
    }

    bool undo() override { swap(); return true; }
    int getSizeInUnits() override { return units; }

    void swap()
    {
        auto current = owner.takeSession();
        owner.putSession (std::move (*other));
        *other = std::move (current);
    }

    KerfAudioProcessor& owner;
    std::unique_ptr<SampleSession> other;
    bool alreadyApplied = true;
    int units = 16;
};

KerfAudioProcessor::SampleSession KerfAudioProcessor::takeSession()
{
    SampleSession s;
    s.info = info;
    s.embeddedAudio = std::move (embeddedAudio);
    embeddedAudio.reset();
    s.unresolvedSample = unresolvedSample;

    auto& tree = chops.getState();
    for (int i = 0; i < tree.getNumProperties(); ++i)
        s.chopProperties.set (tree.getPropertyName (i), tree.getProperty (tree.getPropertyName (i)));
    while (tree.getNumChildren() > 0)
    {
        s.chopTrees.add (tree.getChild (0));
        tree.removeChild (0, nullptr);
    }

    for (int slot = 0; slot < numPads; ++slot)
        for (auto* field : padFields)
            if (auto* param = parameters.getParameter (Params::padParamID (slot, field)))
                s.pads.push_back (param->getValue());

    s.selectedChop = settings.getProperty ("selectedChop");
    s.keyMinor = settings.getProperty ("keyMinor");
    return s;
}

void KerfAudioProcessor::putSession (SampleSession&& s)
{
    lastSavedMemory = {}; // written again for whichever sample comes back
    ++loadGeneration; // whatever still runs for the other sample (analysis, FLAC) is dropped

    info = s.info;
    info.loading = false;
    embeddedAudio = std::move (s.embeddedAudio);
    unresolvedSample = s.unresolvedSample;

    // Chop ids are never reused: keep the higher "next id" of the two samples.
    auto& tree = chops.getState();
    const auto nextId = std::max ((int) tree.getProperty (IDs::nextId, 1), (int) s.chopProperties[IDs::nextId]);
    tree.removeAllChildren (nullptr);
    tree.removeProperty (IDs::beatOrigin, nullptr); // the session's own SHIFT (or none)
    for (int i = 0; i < s.chopProperties.size(); ++i)
        tree.setProperty (s.chopProperties.getName (i), s.chopProperties.getValueAt (i), nullptr);
    tree.setProperty (IDs::nextId, nextId, nullptr);
    for (auto& t : s.chopTrees)
        tree.appendChild (t, nullptr);

    size_t i = 0;
    for (int slot = 0; slot < numPads; ++slot)
        for (auto* field : padFields)
            if (auto* param = parameters.getParameter (Params::padParamID (slot, field)); param != nullptr && i < s.pads.size())
                if (const auto v = s.pads[i++]; std::abs (param->getValue() - v) > 1.0e-7f)
                    param->setValueNotifyingHost (v);

    for (const auto& [name, value] : { std::pair<const char*, const juce::var&> { "selectedChop", s.selectedChop }, { "keyMinor", s.keyMinor } })
        if (value.isVoid()) settings.removeProperty (name, nullptr);
        else                settings.setProperty (name, value, nullptr);

    stretchCache.clear();
    stretchInFlight.clear();
    sourceBpmForAudio.store (info.sourceBpm);
    publishSnapshot();
    sendChangeMessage();

    // Work that was cut short when this sample was swapped out runs again.
    if (info.sample != nullptr && (info.analysing || info.onsets == nullptr))
        startAnalysis (info.sample, true);
    if (embeddedAudio.isEmpty())
        prepareEmbeddedAudio (info.sample);
}

//==============================================================================
// Chop memory

juce::String KerfAudioProcessor::fingerprint (const SampleData& s)
{
    // FNV-1a over the length, channels, rate and the audio itself (16-bit, every few frames):
    // the same file is recognised after renaming or moving it.
    uint64_t h = 1469598103934665603ull;
    auto mix = [&h] (uint64_t v)
    {
        for (int b = 0; b < 8; ++b)
        {
            h ^= (v >> (b * 8)) & 0xff;
            h *= 1099511628211ull;
        }
    };
    const auto n = (int64_t) s.getNumSamples();
    mix ((uint64_t) n);
    mix ((uint64_t) s.getNumChannels());
    mix ((uint64_t) std::llround (s.sampleRate));
    const auto stride = std::max<int64_t> (1, n / 65536);
    for (int ch = 0; ch < s.getNumChannels(); ++ch)
    {
        const auto* data = s.audio.getReadPointer (ch);
        for (int64_t i = 0; i < n; i += stride)
            mix ((uint16_t) (int16_t) juce::jlimit (-32768, 32767, juce::roundToInt (data[i] * 32767.0f)));
    }
    return juce::String::toHexString ((juce::int64) h);
}

juce::File KerfAudioProcessor::memoryFileFor (const juce::String& print) const
{
    return getRecordingsFolder().getParentDirectory().getChildFile ("Memory").getChildFile (print + ".kerfchops");
}

void KerfAudioProcessor::saveChopMemory()
{
    if (info.sample == nullptr || info.fingerprint.isEmpty() || info.loading)
        return;

    // Nothing worth keeping (the untouched single chop, no tempo of your own): forget it.
    const auto list = chops.getChops();
    const auto untouched = list.size() <= 1 && ! info.bpmIsManual && chops.getBeatOrigin() == 0.0;

    juce::String text;
    if (! untouched)
    {
        juce::ValueTree memory ("KERFCHOPS");
        memory.setProperty ("name", info.sample->name, nullptr);
        if (info.bpmIsManual)
            memory.setProperty ("bpm", info.sourceBpm, nullptr);
        auto tree = chops.getState().createCopy();
        for (auto child : tree) // the pad sounds baked in, so the memory stands alone
            if (const auto c = getEffectiveChop ((int) child.getProperty (IDs::id)))
                ChopModel::writeToTree (child, *c, nullptr);
        memory.appendChild (tree, nullptr);
        text = memory.toXmlString();
    }

    if (text == lastSavedMemory)
        return;
    const auto file = memoryFileFor (info.fingerprint);
    if (text.isEmpty())
        file.deleteFile();
    else if (! (file.getParentDirectory().createDirectory() && file.replaceWithText (text)))
        return; // try again next time
    lastSavedMemory = text;
}

bool KerfAudioProcessor::restoreChopMemory()
{
    lastSavedMemory = {};
    const auto file = memoryFileFor (info.fingerprint);
    if (info.sample == nullptr || info.fingerprint.isEmpty() || ! file.existsAsFile())
        return false;

    const auto xml = juce::XmlDocument::parse (file);
    if (xml == nullptr || ! xml->hasTagName ("KERFCHOPS"))
        return false;
    const auto memory = juce::ValueTree::fromXml (*xml);
    auto tree = memory.getChildWithName (IDs::CHOPS).createCopy();
    if (! tree.isValid() || tree.getNumChildren() == 0)
        return false;

    // The chops keep their ids, so a pattern built with them plays again; new chops still
    // get ids above everything seen so far.
    tree.setProperty (IDs::nextId, std::max ((int) tree.getProperty (IDs::nextId, 1), (int) chops.getState().getProperty (IDs::nextId, 1)), nullptr);
    tree.setProperty (IDs::sampleLength, (juce::int64) info.sample->getNumSamples(), nullptr);

    chops.replaceState (tree, false); // part of the load's undo step
    if (chops.getNumChops() == 0)
    {
        chops.resetForSample (info.sample->getNumSamples(), false);
        return false;
    }
    for (const auto& c : chops.getChops())
        if (const auto slot = padSlotFor (c); slot >= 0)
            writePad (slot, PadSound::from (c));
    settings.setProperty ("selectedChop", chops.getChops().front().id, nullptr);

    if (memory.hasProperty ("bpm") && (double) memory.getProperty ("bpm") > 0.0)
    {
        info.sourceBpm = memory.getProperty ("bpm");
        info.bpmIsManual = true;
        sourceBpmForAudio.store (info.sourceBpm);
    }
    publishSnapshot();
    return true;
}

void KerfAudioProcessor::beginNewSample (SampleData::Ptr sample, std::shared_ptr<const WaveformCache> waveform, const juce::String& undoName,
                                         double knownBpm)
{
    saveChopMemory(); // the sample going out: its latest chops

    // The sample that was there (with its chops, pads and analysis) is kept for undo. Its
    // lossless copy for saving is not: it is made again if the sample comes back (putSession).
    auto previous = std::make_unique<SampleSession> (takeSession());
    if (previous->info.sample != nullptr)
        previous->embeddedAudio.reset();

    info.waveform = std::move (waveform);
    info.bpmIsManual = false;
    info.sourceBpm = 0.0;
    sourceBpmForAudio.store (0.0);
    installSample (sample, true);
    chops.getState().removeProperty (IDs::beatOrigin, nullptr); // a new sample starts unshifted

    // Chopped before? Its chops (and pad sounds, and your tempo) come back.
    const auto remembered = restoreChopMemory();
    if (knownBpm > 0.0)
    {
        info.sourceBpm = knownBpm;
        info.bpmIsManual = true;
        sourceBpmForAudio.store (knownBpm);
    }
    startAnalysis (sample, info.bpmIsManual);
    if (remembered)
        setStatusMessage ("Your chops for " + sample->name + " are back (" + juce::String (chops.getNumChops()) + " chops)");

    undoManager.beginNewTransaction (undoName);
    undoManager.perform (new SampleSwapAction (*this, std::move (previous)));
    undoManager.beginNewTransaction(); // the next edit is a step of its own
}

//==============================================================================
// TIME slots: settings child TIME_SLOTS, one SLOT per edited slot (index, time, volume).

namespace
{
    const juce::Identifier timeSlotsId ("TIME_SLOTS"), timeSlotId ("SLOT");
}

bool KerfAudioProcessor::affectsEngine (const juce::ValueTree& tree) const
{
    if (tree.hasType (timeSlotsId) || tree.hasType (timeSlotId))
        return true;
    return ! (tree == settings || tree.isAChildOf (settings)); // chops, pattern
}

kerf::TimeSlot KerfAudioProcessor::getTimeSlot (int index) const
{
    index = juce::jlimit (0, TimeFactory::numSlots - 1, index);
    const auto edited = settings.getChildWithName (timeSlotsId).getChildWithProperty ("index", index);
    if (! edited.isValid())
        return TimeFactory::slots()[(size_t) index];
    TimeSlot slot;
    slot.time = TimeCurve::fromString (edited.getProperty ("time").toString());
    slot.volume = TimeCurve::fromString (edited.getProperty ("volume").toString());
    if (slot.time.numPoints < 2) slot.time = TimeCurve::flat (0.0f);
    if (slot.volume.numPoints < 2) slot.volume = TimeCurve::flat (1.0f);
    return slot;
}

bool KerfAudioProcessor::isTimeSlotEdited (int index) const
{
    return settings.getChildWithName (timeSlotsId).getChildWithProperty ("index", index).isValid();
}

void KerfAudioProcessor::setTimeSlot (int index, const kerf::TimeSlot& slot)
{
    index = juce::jlimit (0, TimeFactory::numSlots - 1, index);
    auto slots = settings.getChildWithName (timeSlotsId);
    if (! slots.isValid())
    {
        slots = juce::ValueTree (timeSlotsId);
        settings.appendChild (slots, &undoManager);
    }
    auto edited = slots.getChildWithProperty ("index", index);
    if (! edited.isValid())
    {
        edited = juce::ValueTree (timeSlotId);
        edited.setProperty ("index", index, nullptr);
        slots.appendChild (edited, &undoManager);
    }
    edited.setProperty ("time", slot.time.toString(), &undoManager);
    edited.setProperty ("volume", slot.volume.toString(), &undoManager);
}

void KerfAudioProcessor::resetTimeSlot (int index)
{
    auto slots = settings.getChildWithName (timeSlotsId);
    if (const auto edited = slots.getChildWithProperty ("index", index); edited.isValid())
    {
        undoManager.beginNewTransaction ("Reset TIME slot");
        slots.removeChild (edited, &undoManager);
        undoManager.beginNewTransaction();
    }
}

double KerfAudioProcessor::getTimeCycleBeats() const noexcept
{
    static constexpr double beats[] = { 2.0, 4.0, 8.0 };
    return beats[juce::jlimit (0, 2, (int) p.timeLength->load())];
}

ChopParams KerfAudioProcessor::effectiveChop (const ChopParams& c) const
{
    auto result = c;
    if (const auto slot = padSlotFor (c); slot >= 0)
        readPad (slot).applyTo (result);
    return result;
}

std::optional<ChopParams> KerfAudioProcessor::getEffectiveChop (int chopId) const
{
    if (const auto c = chops.getChop (chopId))
        return effectiveChop (*c);
    return std::nullopt;
}

juce::RangedAudioParameter* KerfAudioProcessor::padParameter (int chopId, const juce::Identifier& field)
{
    const auto c = chops.getChop (chopId);
    const auto* name = padFieldFor (field);
    if (! c.has_value() || name == nullptr)
        return nullptr;

    const auto slot = padSlotFor (*c);
    return slot >= 0 ? parameters.getParameter (Params::padParamID (slot, name)) : nullptr;
}

void KerfAudioProcessor::setChopSound (int chopId, const juce::Identifier& field, float value)
{
    if (auto* param = padParameter (chopId, field))
    {
        const auto from = param->getValue();
        const auto to = param->convertTo0to1 (value);
        if (std::abs (from - to) < 1.0e-6f)
            return;
        undoManager.beginNewTransaction ("Edit " + field.toString());
        undoManager.perform (new ParameterChangeAction (*param, from, to, applyingParamUndo));
        return;
    }

    undoManager.beginNewTransaction ("Edit " + field.toString());
    chops.setParam (chopId, field, value);
}

//==============================================================================
// Presets and A / B

juce::File KerfAudioProcessor::getPresetFolder()
{
    return getRecordingsFolder().getParentDirectory().getChildFile ("Presets");
}

juce::ValueTree KerfAudioProcessor::captureSound() const
{
    // Every parameter as id / value (real values), except the transport.
    juce::ValueTree state ("SOUND");
    for (auto* parameter : AudioProcessor::getParameters())
        if (auto* ranged = dynamic_cast<juce::RangedAudioParameter*> (parameter); ranged != nullptr && ranged->getParameterID() != Params::patternPlay)
        {
            juce::ValueTree param ("PARAM");
            param.setProperty ("id", ranged->getParameterID(), nullptr);
            param.setProperty ("value", ranged->convertFrom0to1 (ranged->getValue()), nullptr);
            state.appendChild (param, nullptr);
        }
    return state;
}

void KerfAudioProcessor::applySound (const juce::ValueTree& state, const juce::String& undoName,
                                     const std::function<bool (const juce::String&)>& scope, bool intoCurrentTransaction)
{
    // Parameters missing from the state (an older preset) go back to their defaults.
    const ParamUndoGroup group (*this, undoName);
    if (intoCurrentTransaction)
        paramGroupStarted = true; // record into the transaction the caller began
    for (auto* parameter : AudioProcessor::getParameters())
        if (auto* ranged = dynamic_cast<juce::RangedAudioParameter*> (parameter); ranged != nullptr && ranged->getParameterID() != Params::patternPlay)
        {
            if (scope && ! scope (ranged->getParameterID()))
                continue;
            const auto child = state.getChildWithProperty ("id", ranged->getParameterID());
            const auto value = child.isValid() ? (float) child.getProperty ("value")
                                               : ranged->convertFrom0to1 (ranged->getDefaultValue());
            if (std::abs (ranged->convertTo0to1 (value) - ranged->getValue()) > 1.0e-6f)
                setParameterAsGesture (ranged->getParameterID(), value);
        }
}

void KerfAudioProcessor::rememberPresetSound (const juce::String& name)
{
    // The sound each recent preset had when it was loaded or saved, by name (not undoable:
    // after Ctrl+Z brings back an earlier preset's name, its sound is still here).
    auto sounds = settings.getChildWithName (presetSoundsId);
    if (! sounds.isValid())
    {
        sounds = juce::ValueTree (presetSoundsId);
        settings.appendChild (sounds, nullptr);
    }
    sounds.removeChild (sounds.getChildWithProperty ("name", name), nullptr);
    juce::ValueTree entry (presetSoundId);
    entry.setProperty ("name", name, nullptr);
    entry.appendChild (captureSound(), nullptr);
    sounds.appendChild (entry, nullptr);
    while (sounds.getNumChildren() > 16)
        sounds.removeChild (0, nullptr);
}

bool KerfAudioProcessor::isPresetModified() const
{
    // Any parameter away from the preset's sound. Default is every parameter at its default;
    // an unknown preset sound (an older project) counts as unchanged.
    const auto name = getPresetName();
    const auto entry = settings.getChildWithName (presetSoundsId).getChildWithProperty ("name", name);
    if (! entry.isValid() && name != "Default")
        return false;
    const auto sound = entry.getChild (0);
    std::unordered_map<juce::String, float> stored; // one pass over the stored sound, not one search per parameter
    for (const auto& child : sound)
        stored[child.getProperty ("id").toString()] = (float) child.getProperty ("value");
    for (auto* parameter : AudioProcessor::getParameters())
        if (auto* ranged = dynamic_cast<juce::RangedAudioParameter*> (parameter); ranged != nullptr && ranged->getParameterID() != Params::patternPlay)
        {
            const auto it = stored.find (ranged->getParameterID());
            const auto expected = it != stored.end() ? ranged->convertTo0to1 (it->second) : ranged->getDefaultValue();
            if (std::abs (expected - ranged->getValue()) > 1.0e-4f)
                return true;
        }
    return false;
}

void KerfAudioProcessor::applyPreset (const juce::ValueTree& state, const juce::String& presetName, const juce::String& undoName,
                                      const std::function<bool (const juce::String&)>& scope)
{
    // One undo step for the sound and the name shown in the header (Ctrl+Z brings back both).
    undoManager.beginNewTransaction (undoName);
    settings.setProperty ("presetName", presetName, &undoManager);
    applySound (state, undoName, scope, true);
    rememberPresetSound (presetName);
    undoManager.beginNewTransaction(); // the next edit is a step of its own
}

bool KerfAudioProcessor::savePreset (const juce::File& file)
{
    auto state = captureSound();
    state.setProperty ("name", file.getFileNameWithoutExtension(), nullptr);
    state.setProperty ("version", KERF_VERSION, nullptr);
    const auto xml = state.createXml();
    file.getParentDirectory().createDirectory();
    if (xml == nullptr || ! xml->writeTo (file))
        return false;
    settings.setProperty ("presetName", file.getFileNameWithoutExtension(), nullptr);
    rememberPresetSound (file.getFileNameWithoutExtension());
    setStatusMessage ("Saved preset " + file.getFileNameWithoutExtension());
    return true;
}

bool KerfAudioProcessor::loadPreset (const juce::File& file)
{
    const auto xml = juce::XmlDocument::parse (file);
    if (xml == nullptr || ! xml->hasTagName ("SOUND"))
    {
        setStatusMessage ("Could not read " + file.getFileName());
        return false;
    }
    applyPreset (juce::ValueTree::fromXml (*xml), file.getFileNameWithoutExtension(), "Load preset " + file.getFileNameWithoutExtension());
    setStatusMessage ("Loaded preset " + file.getFileNameWithoutExtension());
    return true;
}

void KerfAudioProcessor::loadFactoryPreset (int index)
{
    const auto& list = Factory::presets();
    if (! juce::isPositiveAndBelow (index, (int) list.size()))
        return;
    const auto& preset = list[(size_t) index];
    applyPreset (Factory::toSound (preset), preset.name, "Load preset " + juce::String (preset.name), Factory::inScope);
    setStatusMessage (juce::String (preset.name) + ": " + preset.description);
}

void KerfAudioProcessor::stepPreset (int direction)
{
    // Menu order: Default, the factory presets, then your own (sorted by name).
    const auto& factory = Factory::presets();
    auto files = getPresetFolder().findChildFiles (juce::File::findFiles, false, juce::String ("*") + presetExtension);
    files.sort();
    const auto total = 1 + (int) factory.size() + files.size();

    const auto current = getPresetName();
    int index = current == "Default" ? 0 : -1;
    for (size_t i = 0; i < factory.size() && index < 0; ++i)
        if (current == factory[i].name) index = 1 + (int) i;
    for (int i = 0; i < files.size() && index < 0; ++i)
        if (current == files[i].getFileNameWithoutExtension()) index = 1 + (int) factory.size() + i;

    const auto next = index < 0 ? (direction > 0 ? 1 : total - 1) : (index + (direction > 0 ? 1 : -1) + total) % total;
    if (next == 0)
        initPreset();
    else if (next <= (int) factory.size())
        loadFactoryPreset (next - 1);
    else
        loadPreset (files[next - 1 - (int) factory.size()]);
}

void KerfAudioProcessor::initPreset()
{
    applyPreset (juce::ValueTree ("SOUND"), "Default", "Default preset");
    setStatusMessage ("Default: every setting back to its start value");
}

void KerfAudioProcessor::switchAb (int slot)
{
    slot = juce::jlimit (0, 1, slot);
    const auto current = getAbSlot();
    if (slot == current)
        return;

    // Each slot keeps its sound and the name of its preset: switching brings back both.
    const juce::Identifier ids[] = { "AB_A", "AB_B" };
    auto store = [this] (const juce::Identifier& id, const juce::ValueTree& sound)
    {
        settings.removeChild (settings.getChildWithName (id), nullptr);
        auto copy = sound.createCopy();
        juce::ValueTree holder (id);
        holder.setProperty ("presetName", getPresetName(), nullptr);
        holder.appendChild (copy, nullptr);
        settings.appendChild (holder, nullptr);
    };

    store (ids[current], captureSound());
    const auto target = settings.getChildWithName (ids[slot]);
    if (target.isValid() && target.getNumChildren() > 0)
    {
        const auto undoName = slot == 0 ? "Switch to A" : "Switch to B";
        undoManager.beginNewTransaction (undoName);
        if (const auto name = target.getProperty ("presetName", getPresetName()).toString(); name != getPresetName())
            settings.setProperty ("presetName", name, &undoManager);
        applySound (target.getChild (0), undoName, {}, true);
        undoManager.beginNewTransaction();
    }
    else
    {
        store (ids[slot], captureSound()); // first visit: B starts as a copy of A
    }

    settings.setProperty ("abSlot", slot, nullptr);
}

#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_formats/juce_audio_formats.h>

#include "Parameters.h"
#include "AudioEngine/ChopModel.h"
#include "AudioEngine/ChopTools.h"
#include "AudioEngine/ReleasePool.h"
#include "AudioEngine/SampleLoader.h"
#include "AudioEngine/SamplerEngine.h"
#include "AudioEngine/TimeStretch.h"
#include "Effects/PingPongDelay.h"
#include "Arrangement/PatternModel.h"
#include "Arrangement/PatternPlayer.h"
#include "Arrangement/AudioRecorder.h"
#include "Effects/ToneProcessor.h"
#include "Effects/DirtProcessor.h"
#include "Effects/WobbleProcessor.h"
#include "Effects/EqProcessor.h"
#include "Effects/PumpProcessor.h"
#include "Effects/GrainHaze.h"
#include "Effects/Metronome.h"
#include "Analysis/WaveformCache.h"
#include "Analysis/TransientDetector.h"
#include "Analysis/MusicalAnalysis.h"
#include "Analysis/SpectrumAnalyzer.h"
#include "Analysis/OutputTap.h"

/** Everything the UI needs to know about the loaded sample. Message thread only. */
struct SampleInfo
{
    kerf::SampleData::Ptr sample;
    std::shared_ptr<const kerf::WaveformCache> waveform;
    std::shared_ptr<const kerf::OnsetAnalysis> onsets;
    kerf::TempoEstimate tempo;
    kerf::KeyEstimate key;
    double sourceBpm = 0.0;     // what the engine uses (detected, or the user's override)
    bool bpmIsManual = false;
    bool analysing = false;
    bool loading = false;
    bool fileMissing = false;   // project referenced a file that no longer exists
    juce::File missingFile;
    juce::String error;         // last load error (shown in the UI, cleared on next load)
    juce::String fingerprint;   // of the audio (chop memory key), empty if none
};

class KerfAudioProcessor final : public juce::AudioProcessor,
                                 public juce::ChangeBroadcaster,
                                 private juce::ValueTree::Listener,
                                 private juce::AudioProcessorValueTreeState::Listener,
                                 private juce::AsyncUpdater
{
public:
    KerfAudioProcessor();
    ~KerfAudioProcessor() override;

    //==========================================================================
    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    void processBlockBypassed (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return JucePlugin_Name; }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 2.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    //==========================================================================
    // Message-thread API used by the editor.

    juce::AudioProcessorValueTreeState& getParameters() noexcept { return parameters; }
    kerf::ChopModel& getChops() noexcept { return chops; }
    juce::UndoManager& getUndoManager() noexcept { return undoManager; }
    const SampleInfo& getSampleInfo() const noexcept { return info; }
    juce::ValueTree& getSettings() noexcept { return settings; }
    const kerf::SampleLoader& getLoader() const noexcept { return loader; }

    void loadSampleAsync (const juce::File& file);
    void setSourceBpm (double bpm, bool manual);
    /** SHIFT: the sample moved against the beat grid, in seconds (+: the sample later, the
        lines earlier). The beat lines, BEAT and GRID chops, grid snapping and the metronome
        follow. Kept with the chops (the project, the chop memory); undoable, a drag is one step. */
    void setSampleShift (double seconds);
    double getSampleShift() const;
    static constexpr double maxSampleShift = 0.5; // seconds, either way
    /** The key the sample sounds in: the detected key moved by Global Pitch, as major or
        minor (switching mode goes to the relative key: C minor = E flat major). */
    kerf::KeyEstimate getSoundingKey() const;
    /** Transposes (Global Pitch, smallest move) so the sample sounds in this key. Undoable. */
    void setSoundingKey (int tonic, bool minor);
    /** AUTO next to BPM: back to the detected tempo; AUTO next to KEY: back to the detected
        key (no transposition, the detected mode). Each one undo step. */
    void resetTempo();
    void resetKey();
    void resetTempoAndKey(); // both, one undo step
    bool tempoChanged() const; // a manual BPM (its AUTO shows)
    bool keyChanged() const;   // transposed or another mode (its AUTO shows)
    bool tempoOrKeyChanged() const { return tempoChanged() || keyChanged(); }
    void clearError();

    /** Plays a chop note from the UI. velocity 0 = note off. playOnce: plays the whole chop
        to its end (no note-off needed), whatever its play mode - used by the Space bar. */
    void previewNote (int note, float velocity, bool playOnce = false);
    /** Plays a chop from the UI by its id - also chops beyond the 128 MIDI notes (pads, Space). */
    /** `from`: a frame of the sample to start at (inside the chop; < 0: its start). */
    void previewChop (int chopId, float velocity, bool playOnce = false, double from = -1.0);
    /** CHOP: `nextId` plays once the chop `afterId` (playing) reaches its end; 0: nothing queued. */
    void queuePreview (int afterId, int nextId);

    // Chopping (all undoable as a single step)
    void chopByGrid (kerf::NoteValue value, kerf::NoteModifier modifier);
    void chopEqual (int count);
    /** Every cut gone - one chop over the whole sample again. One undo step. */
    void deleteAllChops();

    // Selection: the selected chop ("selectedChop": the one the controls show) and, chosen with
    // Ctrl, more chops besides it ("selectedChops"). Choosing a chop any other way chooses it alone.
    /** The selected chops, the selected one first. */
    std::vector<int> getSelectedChops() const;
    void setChopSelection (const std::vector<int>& ids); // the first is shown
    bool isChopSelected (int chopId) const;
    /** Ctrl + click: the chop joins the selection (and is the one shown), or leaves it. */
    void toggleChopSelection (int chopId);
    /** Shift + click: every chop from the one shown to `chopId` (in the timeline) selected. */
    void selectChopRange (int chopId);
    /** Ctrl+A: every chop selected (the one shown stays shown). */
    void selectAllChops();
    /** The sample out of KERF (back to the start screen). One undo step: Ctrl+Z brings it back. */
    void unloadSample();
    /** The bin in the toolbar: the selected chops gone (their stretch of the sample is no chop
        any more; all of them: one chop over the whole sample again). One undo step. */
    void deleteSelectedChops();
    /** A chop setting (pad parameter or chop model) for every selected chop, as one undo step. */
    void setSelectedChopsSound (const juce::Identifier& field, float value, const juce::String& undoName);
    void chopByTransients (float sensitivity, double minDistanceSeconds);
    void chopByBeats (double beatsPerChop);
    void chopRandom (int count, uint32_t seed);
    void chopSmart (int targetCount);
    void resetChops();

    // Pattern (Milestone 7)
    kerf::PatternModel& getPattern() noexcept { return pattern; }
    void setPatternPlaying (bool shouldPlay);
    /** PAUSE / PLAY: the pattern stops where it is and goes on from there (STOP and a fresh
        PLAY start at step 1). */
    void pausePattern() { patternResume.store (true); setPatternPlaying (false); }
    void resumePattern() { patternResume.store (true); setPatternPlaying (true); }
    void stopPattern() { patternResume.store (false); setPatternPlaying (false); }
    /** The pattern playing here (not following the DAW) starts again from bar 1. */
    void restartPattern() { patternRestart.store (true); }
    /** PAUSE for chops played from the UI (the top-right PLAY): they hold where they are. */
    void setPreviewsPaused (bool paused) noexcept { previewsPaused.store (paused); }
    bool arePreviewsPaused() const noexcept { return previewsPaused.load(); }
    /** LOOP (toolbar): a chop played from the UI repeats until stopped. */
    void setPreviewLoop (bool on) noexcept { previewLoop.store (on); }
    bool isPreviewLoop() const noexcept { return previewLoop.load(); }
    /** SAMPLE next to PLAY: where the whole sample starts playing (its playhead at rest). */
    void setSampleCursor (int64_t frame) noexcept { sampleCursor.store (std::max<int64_t> (0, frame)); }
    int64_t getSampleCursor() const noexcept { return sampleCursor.load(); }
    /** The host started playing since the last call (and whether the pattern was already
        running in KERF before it): KERF's own playback then stops. Message thread. */
    bool takeHostStart (bool& patternWasRunning) noexcept
    {
        if (! hostStarted.exchange (false, std::memory_order_acquire))
            return false;
        patternWasRunning = patternRanBeforeHost.load (std::memory_order_relaxed);
        return true;
    }
    /** The metronome (toolbar): clicks while the song or the pattern plays; not recorded. */
    void setMetronome (bool on) noexcept { metronomeOn.store (on); }
    bool isMetronomeOn() const noexcept { return metronomeOn.load(); }
    bool isPatternPlaying() const noexcept { return p.patternPlay->load() > 0.5f; }
    kerf::PatternPlayer::Groove groove() const noexcept { return { p.swing->load(), p.humanize->load() }; }

    // Pads (automatable per-chop settings) ---------------------------------------------
    /** Pad index 0..31 for a chop (note - root), or -1 if the chop is not on a pad. */
    int padSlotFor (const kerf::ChopParams& c) const;
    /** The pad parameter controlling `field` (an IDs:: chop field) of a chop, or nullptr. */
    juce::RangedAudioParameter* padParameter (int chopId, const juce::Identifier& field);
    /** The chop with its pad settings applied (what you actually hear). */
    kerf::ChopParams effectiveChop (const kerf::ChopParams& c) const;
    std::optional<kerf::ChopParams> getEffectiveChop (int chopId) const;
    /** Changes a chop sound setting: its pad parameter if it has one, else the chop model. Undoable. */
    void setChopSound (int chopId, const juce::Identifier& field, float value);

    /** Groups several parameter changes into one named undo step while it exists. */
    struct ParamUndoGroup
    {
        ParamUndoGroup (KerfAudioProcessor& p, const juce::String& name) : owner (p) { owner.paramGroupName = name; owner.paramGroupStarted = false; }
        ~ParamUndoGroup() { owner.paramGroupName = {}; }
        KerfAudioProcessor& owner;
    };
    /** Sets a parameter (real value) as a complete gesture: undoable, and hosts record it. */
    void setParameterAsGesture (const juce::String& paramId, float value);

    /** EQ points (one undo step each). addEqPoint returns the band, or -1 when all are in use. */
    int addEqPoint (float freq, float gainDb, kerf::EqProcessor::Type type = kerf::EqProcessor::Type::bell);
    /** Adds a bell (0 dB) in the middle of the widest free gap; selects it. -1 when all are in use. */
    int addEqPointInGap (kerf::EqProcessor::Type type = kerf::EqProcessor::Type::bell);
    void removeEqPoint (int band);
    /** Right-click on a point: it stays where it is but is not heard (again: heard). Undoable. */
    void toggleEqBypass (int band);
    void clearEq();
    kerf::PadSound readPad (int slot) const;

    /** Current EQ settings from the parameters (for the audio and the EQ display). */
    kerf::EqProcessor::Settings eqSettings() const noexcept;
    kerf::PumpProcessor::Settings pumpSettings() const noexcept;
    /** The beat position the PUMP follows (host, pattern or its own clock), for the UI. */
    double getPumpPpq() const noexcept     { return pumpPpqForUi.load (std::memory_order_relaxed); }
    float getHazeLevel() const noexcept    { return hazeLevelForUi.load (std::memory_order_relaxed); } // HAZE's tail, for the galaxy
    int getPatternStep() const noexcept { return patternPlayer.getCurrentStep(); }

    // Recording / capture (Milestone 7)
    kerf::AudioRecorder& getRecorder() noexcept { return recorder; }
    bool startRecording (kerf::AudioRecorder::Source source, double lengthBars);
    static juce::File getRecordingsFolder();
    /** Where files dragged into the DAW are written (they must stay: the DAW refers to them). */
    static juce::File getDragFolder();
    /** The pattern as a MIDI file (16ths at the current tempo; notes trigger the same chops). */
    bool writePatternMidi (const juce::File& target) const;

    // Export (Milestone 7). Runs in the background; `onDone` gets an error (empty = ok).
    void exportPatternAsync (const juce::File& target, int bitDepth, int loops, std::function<void (juce::String)> onDone);
    void exportSampleAsync (const juce::File& target, int bitDepth, std::function<void (juce::String)> onDone);
    //==========================================================================
    // Presets: the whole sound (every parameter: pads, FX, granulizer, EQ, voices, groove).
    static juce::File getPresetFolder();   // Documents/KERF/Presets
    static constexpr const char* presetExtension = ".kerfpreset";
    bool savePreset (const juce::File& file);
    bool loadPreset (const juce::File& file);   // undoable
    /** A sound from kerf::Factory (effects, granulizer, EQ, groove, master; the chops and
        pads stay). Undoable. */
    void loadFactoryPreset (int index);
    void initPreset();                         // "Default": every parameter to its default (undoable)
    /** The previous / next preset in menu order (Default, factory, then yours), round the list. */
    void stepPreset (int direction);
    /** "Default" until a preset is loaded (projects from before 0.5 say "Init" for it). */
    juce::String getPresetName() const { const auto n = settings.getProperty ("presetName", "Default").toString(); return n == "Init" ? juce::String ("Default") : n; }
    // TIME (the page after EQ): 60 presets of curves, kept with the project; edits are undoable.
    // Index ownTimeCurves (-1): your own curves, drawn with no preset selected (they play then).
    static constexpr int ownTimeCurves = -1;
    kerf::TimeSlot getTimeSlot (int index) const;                 // edited, or the factory shape
    void setTimeSlot (int index, const kerf::TimeSlot& slot);     // one undo step per gesture (the caller begins it)
    void resetTimeSlot (int index);                               // back to the factory shape (undoable)
    /** The active slot clicked again: TIME off (no slot), and that slot's TIME and VOLUME curves
        back to how they came. One undo step. */
    void switchTimeSlotOff (int slot);
    bool isTimeSlotEdited (int index) const;
    /** TIME plays: a preset selected, or (none selected) your own curves drawn; not with the tab off. */
    bool isTimeOn() const;
    double getTimeCycleBeats() const noexcept;                    // 2, 4 or 8
    const kerf::TimeShaper& getTimeShaper() const noexcept { return timeShaper; }
    /** Counts every parameter change (automation too): views rest while it stands still. */
    uint32_t getParameterChangeCount() const noexcept { return parameterChanges.load (std::memory_order_relaxed); }

    /** True when any parameter differs from the current preset as loaded or saved (the
        header shows a * then). */
    bool isPresetModified() const;
    /** A / B: two versions of the sound to compare. Switching keeps the current sound in its
        slot and recalls the other one (the first visit to a slot copies the current sound). */
    int getAbSlot() const { return (int) settings.getProperty ("abSlot", 0); }
    void switchAb (int slot);

    /** One chop as you hear it (pads, pitch, every effect), trimmed at the end of its sound. */
    void exportChopAsync (int chopId, const juce::File& target, std::function<void (juce::String)> onDone);
    /** One chop as a MIDI note (its length at the current tempo). */
    bool writeChopMidi (int chopId, const juce::File& target) const;

    /** KIT: every chop as its own WAV in a new folder inside `parent` ("<sample> 01 C4.wav"
        ...). processed: as you hear it (pads, envelope, pitch, every effect, trimmed);
        otherwise the dry slice from the sample. `onDone` gets the folder and an error. */
    void exportKitAsync (const juce::File& parent, bool processed, std::function<void (juce::File folder, juce::String error)> onDone);

    /** RESAMPLE: the pattern with every effect becomes a new sample - one seamless loop (the
        effect tails wrapped round to the start), at the current tempo. Undoable like
        loading a sample; the WAV is kept in Documents/KERF/Recordings. */
    void resamplePatternAsync (std::function<void (juce::String error)> onDone);

    /** Short status line for the UI (export results etc.). */
    juce::String getStatusMessage() const { return statusMessage; }
    void setStatusMessage (const juce::String& m) { statusMessage = m; sendChangeMessage(); }

    // Real-time readouts for the UI (lock-free)
    const kerf::SamplerEngine& getEngine() const noexcept { return engine; }
    kerf::SpectrumAnalyzer& getSpectrum() noexcept { return spectrum; }
    kerf::OutputTap& getOutputTap() noexcept { return outputTap; }
    double getHostBpm() const noexcept     { return hostBpm.load (std::memory_order_relaxed); }
    double getHostPpq() const noexcept     { return hostPpq.load (std::memory_order_relaxed); }
    bool isHostPlaying() const noexcept    { return hostPlaying.load (std::memory_order_relaxed); }
    bool hasHostTempo() const noexcept     { return hostHasTempo.load (std::memory_order_relaxed); }
    float getOutputLevel() const noexcept  { return outputLevel.load (std::memory_order_relaxed); }
    /** Sound is coming out right now (the host is still processing: a paused host leaves
        the last level standing). Views use it to know when to rest. */
    bool isSounding() const noexcept { return getOutputLevel() > 1.0e-4f && juce::Time::getMillisecondCounter() - lastProcessMs.load (std::memory_order_relaxed) < 250; }
    /** Audio was processed in the last quarter second (a paused host may stop calling us). */
    bool isProcessing() const noexcept { return juce::Time::getMillisecondCounter() - lastProcessMs.load (std::memory_order_relaxed) < 250; }
    int getMidiNoteCounter() const noexcept { return midiNoteCounter.load (std::memory_order_relaxed); }

    /** The tempo the engine syncs to: the host's if available, otherwise the sample's. */
    double getEffectiveTempo() const;

    /** Stretch mode status for the UI: how many chops still need rendering. */
    bool isStretchMode() const { return p.stretchMode->load() > 0.5f; }
    int getStretchPending() const noexcept { return stretchPending; }
    int getStretchNeeded() const noexcept { return stretchNeeded; }

    static constexpr double maxEmbeddedSeconds = 180.0;

private:
    //==========================================================================
    // Chops and pattern always; of the settings only the TIME curves (see affectsEngine).
    void valueTreePropertyChanged (juce::ValueTree& t, const juce::Identifier& property) override;
    bool keepSelection = false; // toggleChopSelection is choosing: the others stay selected
    void valueTreeChildAdded (juce::ValueTree& parent, juce::ValueTree& child) override  { if (affectsEngine (parent) || affectsEngine (child)) triggerAsyncUpdate(); }
    void valueTreeChildRemoved (juce::ValueTree& parent, juce::ValueTree& child, int) override { if (affectsEngine (parent) || affectsEngine (child)) triggerAsyncUpdate(); }
    bool affectsEngine (const juce::ValueTree& tree) const;
    void valueTreeChildOrderChanged (juce::ValueTree&, int, int) override              {}
    void valueTreeRedirected (juce::ValueTree&) override                               { triggerAsyncUpdate(); }
    void handleAsyncUpdate() override;

    void publishSnapshot();
    void fillPatternData (kerf::PatternData& out) const; // the pattern with notes resolved (snapshot, MIDI file)
    void applySourceBpm (double bpm, bool manual);
    juce::ValueTree captureSound() const;                                    // parameters (no transport)
    /** One undo step. With `scope`, only the parameters it accepts are touched. */
    void applySound (const juce::ValueTree& state, const juce::String& undoName,
                     const std::function<bool (const juce::String&)>& scope = {}, bool intoCurrentTransaction = false);
    /** A preset: its sound and its name, one undo step. */
    void applyPreset (const juce::ValueTree& state, const juce::String& presetName, const juce::String& undoName,
                      const std::function<bool (const juce::String&)>& scope = {});
    /** Renders the pattern (or one chop) offline in the background. Writes a WAV to `target`,
        or, with `onAudio`, hands the audio over on the message thread instead. */
    void renderAsync (const juce::File& target, int bitDepth, int loops, int singleNote, double noteSeconds,
                      std::function<void (juce::String)> onDone,
                      std::function<void (juce::AudioBuffer<float>&&, double rate)> onAudio = {});
    void parameterChanged (const juce::String& parameterID, float newValue) override;
    void writePad (int slot, const kerf::PadSound& pad);
    void resetPads();
    void refreshPadTable(); // audio thread
    void initialisePads();
    kerf::PadTable padTable;
    std::array<std::array<std::atomic<float>*, 12>, kerf::numPads> padPtrs {};
    std::atomic<int> rootNoteForAudio { kerf::defaultRootNote };
    void installSample (kerf::SampleData::Ptr sample, bool resetChops);
    void startAnalysis (kerf::SampleData::Ptr sample, bool keepManualBpm);
    void prepareEmbeddedAudio (kerf::SampleData::Ptr sample); // FLAC copy for project saving (background)

    // Chop memory: the chops (with their pad sounds) and a manual BPM of every sample you
    // chop, kept in Documents/KERF/Memory by an audio fingerprint, and brought back when
    // that sample is loaded again.
    static juce::String fingerprint (const kerf::SampleData& sample);
    juce::File memoryFileFor (const juce::String& print) const;
    bool restoreChopMemory();   // into the current (just loaded) sample; true if there was one
    void saveChopMemory();      // when it changed (called every 2 s, on a new sample, on exit)
    juce::String lastSavedMemory; // what is on disk for the current sample

    // Loading a sample is one undo step: everything that belongs to the sample (audio,
    // analysis, chops, pads, selection) is swapped as a whole, and undo swaps it back.
    struct SampleSession;
    struct SampleSwapAction;
    SampleSession takeSession();            // moves the current sample's state out (leaves it empty)
    void putSession (SampleSession&& session);
    /** `knownBpm` > 0: the tempo is known exactly (a resample) and kept, not detected. */
    void beginNewSample (kerf::SampleData::Ptr sample, std::shared_ptr<const kerf::WaveformCache> waveform, const juce::String& undoName,
                         double knownBpm = 0.0);
    void processChunk (juce::AudioBuffer<float>& buffer, int start, int num, const juce::MidiBuffer& midi);
    juce::ValueTree createStateTree();
    void applyStateTree (const juce::ValueTree& tree);
    int64_t regionForChopping (int64_t& start) const;
    void replaceChopsUndoable (const std::vector<int64_t>& boundaries, const juce::String& actionName);

    // Stretch mode (Milestone 6): background pre-rendering of chops.
    double currentStretchRatio() const;
    kerf::TimeStretch::Request desiredStretch (const kerf::ChopParams& chop) const;
    void updateStretchRenders();

    struct StretchEntry { kerf::TimeStretch::Request request; kerf::SampleData::Ptr rendered; };
    std::map<int, StretchEntry> stretchCache;                // by chop id
    std::map<int, kerf::TimeStretch::Request> stretchInFlight; // by chop id
    int stretchPending = 0, stretchNeeded = 0;

    struct StretchTimer final : juce::Timer
    {
        explicit StretchTimer (KerfAudioProcessor& o) : owner (o) {}
        void timerCallback() override { owner.updateStretchRenders(); owner.pollRecorder(); if (++ticks % 13 == 0) owner.saveChopMemory(); }
        int ticks = 0;
        KerfAudioProcessor& owner;
    } stretchTimer { *this };

    //==========================================================================
    juce::UndoManager undoManager;
    juce::AudioProcessorValueTreeState parameters;
    kerf::ChopModel chops { &undoManager };
    kerf::PatternModel pattern { &undoManager };
    juce::ValueTree settings { "SETTINGS" };

    // Undo for every parameter: each control gesture (knob drag, button, menu) becomes one
    // undo step; quick repeats of the same control merge into one.
    struct ParamUndoRecorder;
    friend struct ParamUndoRecorder;
    struct SourceBpmAction;
    std::unique_ptr<ParamUndoRecorder> paramUndo;
    bool applyingParamUndo = false;
    juce::String paramGroupName;   // set by ParamUndoGroup
    bool paramGroupStarted = false;
    double lastBpmEdit = 0.0;

    kerf::SampleLoader loader;
    juce::ThreadPool jobs { 2 };
    std::atomic<int> loadGeneration { 0 };
    SampleInfo info;
    juce::MemoryBlock embeddedAudio; // FLAC cache of the current sample for project saving

    kerf::ReleasePool releasePool;
    kerf::RealtimeHandoff<kerf::EngineSnapshot> handoff { releasePool };

    // Audio-thread state
    kerf::EngineSnapshot::Ptr audioSnapshot;
    uint32_t audioSnapshotVersion = 0;
    kerf::SamplerEngine engine;
    kerf::PingPongDelay space;
    kerf::ToneProcessor tone;
    kerf::DirtProcessor dirt;
    kerf::WobbleProcessor wobble;
    kerf::EqProcessor eq;
    kerf::PumpProcessor pump;
    kerf::TimeShaper timeShaper;
    kerf::Metronome metronome;                 // the toolbar's click (added last: heard, never resampled)
    double pumpFreePpq = 0.0; // audio thread: the PUMP clock while nothing else runs
    bool wasPlayingAnything = false; // audio thread: something played in the last chunk
    kerf::SpectrumAnalyzer spectrum;
    kerf::OutputTap outputTap;
    kerf::PatternPlayer patternPlayer;
    kerf::AudioRecorder recorder;
    bool lastPatternRun = false; // audio thread
    std::atomic<bool> patternRestart { false };
    bool hostWasPlaying = false; // audio thread
    std::atomic<bool> hostStarted { false }, patternRanBeforeHost { false };
    double internalPpq = 0.0;
    juce::String statusMessage;
    void pollRecorder();
    void installRecording (juce::AudioBuffer<float>&& audio, kerf::AudioRecorder::Source source);
    juce::AudioBuffer<float> spaceSend, mixBuffer, grainScratch, hazeSend;
    kerf::GrainHaze haze; // HAZE: the grains' own reverb
    juce::MidiBuffer chunkMidi;
    kerf::TransportInfo transport;
    bool firstChunk = true;
    juce::ValueTree unresolvedSample; // sample reference from a project whose file is missing
    juce::SmoothedValue<float> masterGain;
    int maxChunk = 512;

    // UI -> audio note FIFO
    struct UiNote { int note; float velocity; bool once; int chopId = 0; double from = -1.0; int after = 0; bool queue = false; }; // chopId > 0: a chop preview (from: a frame to start at; queue: after the chop `after`)
    std::array<UiNote, 32> chopPreviews {}; // audio thread: this block's chop previews
    int numChopPreviews = 0;
    juce::AbstractFifo uiNoteFifo { 256 };
    std::array<UiNote, 256> uiNotes {};

    // Audio -> UI readouts
    std::atomic<double> hostBpm { 120.0 }, hostPpq { 0.0 }, pumpPpqForUi { 0.0 };
    std::atomic<float> hazeLevelForUi { 0.0f };
    std::atomic<bool> hostPlaying { false }, hostHasTempo { false };
    std::atomic<float> outputLevel { 0.0f };
    std::atomic<int> midiNoteCounter { 0 };

    // Cached parameter pointers (no string lookups on the audio thread)
    struct ParamPtrs
    {
        std::atomic<float>* master = nullptr; std::atomic<float>* pitch = nullptr;
        std::atomic<float>* stretchMode = nullptr; std::atomic<float>* tempoSync = nullptr;
        std::atomic<float>* space = nullptr;
        std::atomic<float>* filter = nullptr; std::atomic<float>* resonance = nullptr; std::atomic<float>* drive = nullptr;
        std::atomic<float>* dirtMode = nullptr; std::atomic<float>* wobble = nullptr; std::atomic<float>* wobbleMode = nullptr;
        std::atomic<float>* voices = nullptr; std::atomic<float>* retrigger = nullptr; std::atomic<float>* quantize = nullptr;
        std::atomic<float>* grainMix = nullptr; std::atomic<float>* grainSize = nullptr; std::atomic<float>* grainDensity = nullptr;
        std::atomic<float>* grainSpray = nullptr; std::atomic<float>* grainPitch = nullptr; std::atomic<float>* grainSpread = nullptr;
        std::atomic<float>* grainScan = nullptr; std::atomic<float>* grainPosition = nullptr;
        std::atomic<float>* grainReverse = nullptr;
        std::atomic<float>* grainPitch2 = nullptr;
        std::atomic<float>* grainDrift = nullptr; std::atomic<float>* grainShimmer = nullptr;
        std::atomic<float>* grainShape = nullptr; std::atomic<float>* grainScale = nullptr;
        std::atomic<float>* grainFreeze = nullptr; std::atomic<float>* grainSync = nullptr;
        std::atomic<float>* grainGlide = nullptr; std::atomic<float>* grainDetune = nullptr; std::atomic<float>* grainHaze = nullptr;
        std::atomic<float>* timeSlot = nullptr; std::atomic<float>* timeMix = nullptr; std::atomic<float>* timeLength = nullptr;
        std::atomic<float>* grainOff = nullptr; std::atomic<float>* fxOff = nullptr; std::atomic<float>* eqOff = nullptr; std::atomic<float>* timeOff = nullptr;
        std::array<std::array<std::atomic<float>*, 6>, kerf::Params::eqBands> eq {}; // on, type, freq, gain, q, bypass
        std::atomic<float>* pump = nullptr; std::atomic<float>* pumpMode = nullptr;
        std::atomic<float>* patternPlay = nullptr;
        std::atomic<float>* swing = nullptr; std::atomic<float>* humanize = nullptr;
    } p;

    std::atomic<double> sourceBpmForAudio { 0.0 };
    double lastShiftEdit = 0.0;
    std::atomic<bool> chordMinorForAudio { false };
    std::atomic<uint32_t> parameterChanges { 0 };
    std::atomic<juce::uint32> lastProcessMs { 0 };
    std::atomic<bool> previewsPaused { false }, patternResume { false };
    std::atomic<bool> previewLoop { false }, metronomeOn { false };
    std::atomic<int64_t> sampleCursor { 0 };
    std::atomic<double> preparedRate { 48000.0 }; // from prepareToPlay: what the engine and effects run at
    void rememberPresetSound (const juce::String& name);
    void fillGrainSettings (kerf::GrainSettings& gs, double bpm) const; // every GRANULIZER setting for a block (audio thread, export)
    float hazeAmount() const; // HAZE as heard (0 with the GRANULIZER tab off)

    JUCE_DECLARE_WEAK_REFERENCEABLE (KerfAudioProcessor)
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (KerfAudioProcessor)
};

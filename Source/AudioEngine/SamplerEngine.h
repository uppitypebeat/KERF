#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <array>
#include <atomic>
#include "SampleVoice.h"

namespace kerf
{

/** What a new note does to voices that are already sounding. */
enum class RetriggerMode : int
{
    poly = 0,         // notes overlap freely
    sameNoteCuts = 1, // re-pressing a note cuts its previous voice (default, MPC-like)
    mono = 2          // any new note cuts every other voice
};

/** Optional delay of note-ons to the next musical grid line (only while the host plays). */
enum class TriggerQuantize : int { off = 0, sixteenth, eighth, quarter, bar };

double quantizeBeats (TriggerQuantize q) noexcept;

struct EngineSettings
{
    int maxVoices = 16;
    RetriggerMode retrigger = RetriggerMode::sameNoteCuts;
    TriggerQuantize quantize = TriggerQuantize::off;
    VoiceGlobals voice;
    const PadTable* pads = nullptr; // live, automatable pad settings (may be null)
    bool previewsPaused = false;    // PAUSE: chops played from the UI (once) hold where they are, silent
    bool previewLoop = false;       // LOOP: chops played from the UI repeat until stopped
};

/** Host transport information for trigger quantisation. */
struct TransportInfo
{
    bool isPlaying = false;
    double bpm = 120.0;
    double ppqAtBlockStart = 0.0;
    double beatsPerBar = 4.0;
};

namespace engine_detail
{
    // (Outside the class: their default member initializers are used inside it, for default
    // arguments and `{}`. Clang only allows that for classes already complete there.)

    /** What the pattern adds to one of its notes: the step's pitch and a slide into it. */
    struct StepPitch { int transpose = 0; bool glide = false; int glideFrom = 0; float glideSeconds = 0.0f; };

    /** A note-on waiting for the next quantise grid line. */
    struct PendingNote { int note = -1; float velocity = 0.0f; int64_t samplesUntilStart = 0; bool released = false; StepPitch step; };
}

/** Turns MIDI into voices. Runs entirely on the audio thread except for prepare(). */
class SamplerEngine
{
public:
    static constexpr int maxPolyphony = 32;
    static constexpr int numPhysicalVoices = maxPolyphony + 8; // extra voices let stolen voices fade out

    /** Note-ons on this MIDI channel play the chop once to its end (UI preview). */
    static constexpr int previewOnceChannel = 15;

    /** The pattern's channel. A pitch-wheel message on it sets the pitch (semitones, see
        PatternPlayer::pitchToWheel) of the next note-on on it only. */
    static constexpr int patternChannel = 16;

    void prepare (double sampleRate);

    /** Called on the audio thread when a new snapshot arrives. Sounding voices pick up
        the new chop settings; voices whose chop was deleted fade out. */
    void setSnapshot (const EngineSnapshot* snapshot);

    void process (const VoiceBusses& out, int numSamples, const juce::MidiBuffer& midi,
                  const EngineSettings& settings, const TransportInfo& transport);

    void allNotesOff (bool immediate);

    /** UI preview of a chop by its id - also chops beyond the 128 MIDI notes. velocity 0
        releases it (playOnce: stops a play-once preview with its fade). Audio thread.
        `from`: a frame of the sample inside the chop to start at (< 0: from its start). */
    void previewChop (int chopId, float velocity, bool playOnce, const EngineSettings& settings, double from = -1.0);
    /** CHOP: a chop clicked while another plays - `nextId` plays as soon as `afterId` reaches
        its end (to the sample; a looping one ends its turn first). `nextId` 0: nothing queued.
        If `afterId` no longer plays, `nextId` starts now. Audio thread. */
    void queuePreview (int afterId, int nextId, float velocity, const EngineSettings& settings);

    /** Voices of previewed chops without a MIDI note use this + their chop index as note. */
    static constexpr int previewNoteBase = 1000;
    /** previewChop with this id plays the whole sample (PLAY with FULL selected). */
    static constexpr int wholeSampleId = 0x7ffffff0;
    static constexpr int wholeSampleNote = 2000;
    /** A chop played from the UI (once) is sounding and not paused: the metronome runs with it. */
    bool isPreviewPlaying (bool paused) const noexcept;

    int getNumActiveVoices() const noexcept;

    /** Lock-free per-voice readout for the UI playheads. */
    struct VoiceDisplay
    {
        std::atomic<int> chopId { 0 };
        std::atomic<double> position { 0.0 };
        std::atomic<float> level { 0.0f };
        std::atomic<uint32_t> triggers { 0 };            // counts note-ons (drives UI hit animations)

        // GRAIN: where this voice's grains read from (positions in the original sample).
        static constexpr int maxGrains = GrainCloud::maxGrains;
        std::atomic<float> grainAmount { 0.0f };          // 0 = no grains shown
        std::atomic<int> numGrains { 0 };
        std::array<std::atomic<double>, maxGrains> grainPosition {};
        std::array<std::atomic<float>, maxGrains> grainLevel {};
        std::array<std::atomic<float>, maxGrains> grainPitch {}; // semitones
        std::array<std::atomic<float>, maxGrains> grainCents {}; // DETUNE
        std::atomic<double> sprayCentre { 0.0 }, sprayHalfWidth { 0.0 };
    };

    const std::array<VoiceDisplay, numPhysicalVoices>& getVoiceDisplay() const noexcept { return display; }

private:
    void handleMidi (const juce::MidiMessage& m, int samplePos, const EngineSettings& settings, const TransportInfo& transport);
    using StepPitch = engine_detail::StepPitch;
    void noteOn (int note, float velocity, const EngineSettings& settings, bool playOnce = false, StepPitch step = {});
    void startChop (const ChopParams& chop, int chopIndex, int note, float velocity, const EngineSettings& settings,
                    bool playOnce, StepPitch step, double from = -1.0);
    void noteOff (int note);
    SampleVoice* findVoiceToUse (const EngineSettings& settings);
    void renderVoices (const VoiceBusses& out, int start, int num, const EngineSettings& settings);
    int queueDueAt (int position, const EngineSettings& settings) const; // where the queued chop starts in this block
    int queuedAfter = 0, queuedNext = 0;
    float queuedVelocity = 0.9f;

    std::array<SampleVoice, numPhysicalVoices> voices;
    std::array<VoiceDisplay, numPhysicalVoices> display;
    const EngineSnapshot* snapshot = nullptr; // owned by the processor for the block's duration
    uint64_t ageCounter = 0;
    double sampleRate = 44100.0;

    bool sustainPedal = false;
    std::array<bool, 128> heldBySustain {};

    // Note-ons waiting for the next quantise grid line.
    using PendingNote = engine_detail::PendingNote;
    std::array<PendingNote, 64> pending {};
    int numPending = 0;
    StepPitch patternStep; // pitch and slide for the next pattern note
};

} // namespace kerf

#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include "ChopTypes.h"
#include "GrainCloud.h"

namespace kerf
{

/** Settings shared by all voices for one processing block. */
struct VoiceGlobals
{
    double hostSampleRate = 44100.0;
    double globalSemitones = 0.0;
    double timeRatio = 1.0; // > 1 plays faster (repitch mode only)
    GrainSettings grain;

    /** The metronome with a chop or the sample played from PLAY: the sample's beat lines (one every
        `beatFrames` frames of the original sample, from its start, at KERF's BPM). A voice
        played from the UI notes each line it passes in `beatClicks` (0: off). */
    struct BeatClicks { std::array<int, 16> offset {}; std::array<bool, 16> downbeat {}; int count = 0; };
    double beatFrames = 0.0;
    double beatOrigin = 0.0; // SHIFT: the frame of the original sample where the lines start
    int beatsPerBar = 4;
    BeatClicks* beatClicks = nullptr;
};

/** Output busses a voice renders into. */
struct VoiceBusses
{
    float* dryL = nullptr;
    float* dryR = nullptr;
    float* spaceL = nullptr; // pre-fader send into SPACE
    float* spaceR = nullptr;
    float* scratchL = nullptr; // per-voice work space (at least numSamples long)
    float* scratchR = nullptr;
    float* hazeL = nullptr;    // the grains alone (after gain and pan), into HAZE
    float* hazeR = nullptr;
};

/** Plays one chop. Real-time safe: no allocation, no locks. */
class SampleVoice
{
public:
    enum class Stage { idle, playing, releasing, killing };

    void prepare (double hostSampleRate);

    /** Starts the voice. `velocity` is 0..1. `startDelay` delays the first output sample
        within the current block (sample-accurate note-on). */
    void start (const SampleData::Ptr& sample, const ChopParams& chop, int midiNote, float velocity, uint64_t age,
                const SampleData::Ptr& prerendered = nullptr, bool playOnce = false, float transposeSemitones = 0.0f,
                double from = -1.0); // from: a frame of the sample inside the chop to start at (< 0: its start)

    /** Slide (pattern): right after start(), the pitch begins at `fromSemitones` (a step
        pitch) and glides to the note's own step pitch in `seconds`. */
    void glideFrom (float fromSemitones, float seconds) noexcept
    {
        glideTarget = stepSemitones;
        stepSemitones = fromSemitones;
        glidePerSample = (glideTarget - fromSemitones) / (float) std::max (1.0, (double) seconds * sampleRate);
    }

    /** Note-off: gate/loop chops fade out; one-shots ignore it. */
    void noteOff();

    /** Fast fade-out used for voice stealing, choke groups and retrigger. */
    void kill();

    /** Stops a play-once preview with its normal fade-out. */
    void stopPreview() noexcept { if (forcedOneShot && stage == Stage::playing) stage = Stage::releasing; }
    bool isPlayOnce() const noexcept { return forcedOneShot; }
    /** LOOP in the toolbar: a chop played from the UI repeats until it is stopped (live). */
    void setPreviewLoop (bool on) noexcept
    {
        if (! forcedOneShot || forcedLoop == on) return;
        forcedLoop = on;
        chop.mode = forcedMode();
    }
    /** Output samples until the read position reaches the chop's end (at the current speed). */
    double framesUntilEnd (const VoiceGlobals& g) const;

    /** Live-updates the sound settings while playing (keeps position). Region changes are
        applied safely: the read position is clamped into the new region. */
    void updateChop (const ChopParams& chop);

    /** Applies live (automated) pad settings while the voice plays. Cheap when unchanged. */
    void applyPad (const PadSound& pad) noexcept;

    /** Adds this voice's output into the busses. */
    void render (const VoiceBusses& out, int startSample, int numSamples, const VoiceGlobals& g);

    bool isActive() const noexcept     { return stage != Stage::idle; }
    bool isReleasing() const noexcept  { return stage == Stage::releasing || stage == Stage::killing; }
    Stage getStage() const noexcept    { return stage; }
    int getNote() const noexcept       { return note; }
    int getChopId() const noexcept     { return chop.id; }
    int getChokeGroup() const noexcept { return chop.chokeGroup; }
    uint64_t getAge() const noexcept   { return age; }
    double getPosition() const noexcept { return position; }

    /** Maps a position in what this voice plays to the original sample (pre-rendered
        STRETCH playback is a separate buffer). Used for playheads and grain display. */
    double toDisplay (double pos) const noexcept
    {
        if (! isPrerendered) return pos;
        return (double) originalStart + pos * (double) originalLength / std::max (1.0, (double) chop.length());
    }

    double getDisplayPosition() const noexcept { return toDisplay (position); }

    /** UI: grain amount for this voice (0 = no grains). */
    float getGrainAmount() const noexcept
    {
        return stage == Stage::idle ? 0.0f : std::max (0.0f, smoothGrain.getCurrentValue());
    }

    /** UI: calls fn (positionInOriginalSample, level 0..1, semitones, detune cents) for every sounding grain. */
    template <typename Fn>
    void forEachGrainDisplay (Fn&& fn) const
    {
        cloud.forEachGrain ([this, &fn] (double pos, float level, float semis, float cents) { fn (toDisplay (pos), level, semis, cents); });
    }

    double getSprayCentreDisplay() const noexcept { return toDisplay (cloud.getSprayCentre()); }
    double getSprayHalfWidthDisplay() const noexcept
    {
        const auto scale = isPrerendered ? (double) originalLength / std::max (1.0, (double) chop.length()) : 1.0;
        return cloud.getSprayHalfWidth() * scale;
    }
    float getCurrentLevel() const noexcept { return envelope * gainLinear; }

    /** Releases the sample reference. Call from the audio thread only when idle. */
    void clearSampleIfIdle() noexcept { if (stage == Stage::idle) sample = nullptr; }

private:
    double readRate (const VoiceGlobals& g) const; // source frames per output sample
    void noteBeatLine (int offset, double stepFrames, const VoiceGlobals& g); // the metronome
    static constexpr int64_t noBeatLine = std::numeric_limits<int64_t>::min();
    int64_t beatLine = noBeatLine; // the beat line the voice was last in (the metronome)
    float readInterpolated (const float* data, double pos) const noexcept;
    void computePanGains (float panValue) noexcept;
    void computeDecay() noexcept; // from chop.decayMs

    SampleData::Ptr sample;
    ChopParams chop;
    Stage stage = Stage::idle;
    int note = -1;
    uint64_t age = 0;

    double sampleRate = 44100.0;
    double position = 0.0;      // in source frames
    bool isPrerendered = false; // playing a time-stretched render of the chop
    bool forcedOneShot = false; // preview: plays to the end whatever the chop's mode
    bool forcedLoop = false;    // preview with LOOP on: repeats instead (until stopped)
    PlayMode forcedMode() const noexcept { return forcedLoop ? PlayMode::loop : PlayMode::oneShot; }
    float stepSemitones = 0.0f; // a pattern step's pitch, on top of everything else
    float glideTarget = 0.0f, glidePerSample = 0.0f; // slide: where stepSemitones is heading
    int64_t originalStart = 0, originalLength = 0;
    float velocityGain = 1.0f;
    float gainLinear = 1.0f;

    float envelope = 0.0f;      // overall amplitude envelope 0..1
    float attackStep = 1.0f;    // per-sample increments
    float releaseStep = 1.0f;
    float decayCoeff = 1.0f;    // per sample: the distance to the sustain level shrinks by this
    bool attackDone = false;    // the attack reached the top: decaying / sustaining now
    float killStep = 1.0f;

    juce::SmoothedValue<float> smoothGain, smoothPanL, smoothPanR, smoothSpace, smoothGrain;
    GrainCloud cloud;
};

} // namespace kerf

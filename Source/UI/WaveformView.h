#pragma once

#include "Controls.h"
#include <set>
#include "../PluginProcessor.h"
#include "DropVortex.h"

namespace kerf::ui
{

/** The main waveform editor: displays the sample, its chops, playheads and the beat grid,
    and lets the user create, select, resize and split chops by direct manipulation. */
class WaveformView final : public juce::Component,
                           private AnimationClock::Client,
                           private juce::ChangeListener,
                           private juce::ValueTree::Listener
{
public:
    WaveformView (KerfAudioProcessor& processor, AnimationClock& clock);
    ~WaveformView() override;

    void setKnifeMode (bool on) { knifeMode = on; updateMouseCursor(); repaint(); }
    bool isKnifeMode() const noexcept { return knifeMode; }
    void setDragHighlight (bool on) { if (dragHighlight != on) { dragHighlight = on; repaint(); } }
    /** A file being dragged over the view: where the mouse is (view coordinates). */
    void setDragPoint (juce::Point<float> p) { dragPoint = p; hasDragPoint = true; }
    /** The file was dropped: the singularity goes supernova. */
    void dropHere() { vortex.drop(); }
    bool isDropAnimating() const noexcept { return vortex.isActive(); }
    /** Steps every animation of the view by hand (the screenshot tool). */
    void advanceAnimation (double seconds) { animationTick (seconds); }
    /** The furthest any star is from the place it had when the last drag began (0..1 of the
        view), and whether it is rewinding. For the screenshot tool. */
    float getStarDisplacement() const noexcept
    {
        float worst = 0.0f;
        for (const auto& s : stars) worst = std::max (worst, std::hypot (s.hx - s.sx, s.hy - s.sy));
        return worst;
    }
    bool isRewinding() const noexcept { return rewind >= 0.0f; }
    /** Where a frame of the sample is in the big waveform (the screenshot tool clicks there). */
    juce::Point<float> pointAtFrame (double frame) const { return { (float) frameToX (frame), waveArea.getCentreY() }; }
    juce::Rectangle<float> getWaveArea() const noexcept { return waveArea; }

    void zoomToFit (bool animate = true);
    /** Opening animation: the waveform grows out of the centre line again. */
    void playIntro() { reveal = 0.0f; repaint(); }
    void zoomBy (double factor, double anchorFrame);
    void zoomToSelection();

    /** Called by the editor when the selection changes so the view can follow. */
    void ensureVisible (int64_t start, int64_t end);

    std::function<void()> onLoadRequested;
    /** The playhead was put elsewhere while it played: play `target` from `from` (the editor
        keeps track of what plays). Unset: the view starts it itself. */
    std::function<void (int target, double from)> onPlayFrom;
    /** A click chose a chop (on its body; the editor may queue it after the one playing). */
    std::function<void (int chopId)> onChopClicked;

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;
    void mouseMagnify (const juce::MouseEvent&, float scaleFactor) override;

private:
    enum class DragMode { none, edge, pan, overview, overviewLeft, overviewRight, playhead };

    struct EdgeHit { int chopId = 0; ChopEdge edge = ChopEdge::start; };

    void animationTick (double dt) override;
    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    void valueTreePropertyChanged (juce::ValueTree&, const juce::Identifier&) override { refreshChops(); }
    void valueTreeChildAdded (juce::ValueTree&, juce::ValueTree&) override             { refreshChops(); }
    void valueTreeChildRemoved (juce::ValueTree&, juce::ValueTree&, int) override      { refreshChops(); }
    void valueTreeChildOrderChanged (juce::ValueTree&, int, int) override              {}

    void refreshChops();
    void clampRange (double& start, double& end) const;
    void setViewNow (double start, double end);      // direct manipulation (pan, drag)
    void animateViewTo (double start, double end);   // zoom commands glide
    void tickAnimations (double dt, bool& needsRepaint);
    int selectedChopId() const;
    juce::Colour colourOf (int chopId) const { return chopColour (chopId, chops.empty() ? 0 : chops.front().id); }
    void selectChop (int id);

    // Geometry
    double frameToX (double frame) const noexcept;
    double xToFrame (double x) const noexcept;
    int64_t numFrames() const noexcept;
    std::optional<EdgeHit> hitTestEdge (juce::Point<float> p) const;
    int64_t snapFrame (int64_t frame, bool allowSnap, juce::String& snapKind) const;
    double beatLengthFrames() const;

    // Painting
    void paintEmptyState (juce::Graphics&);
    void updateStars (double dt);
    void paintStars (juce::Graphics&, juce::Rectangle<float> area);
    juce::Rectangle<float> screenArea() const;
    void paintWaveform (juce::Graphics&);
    void paintEnvelope (juce::Graphics&); // the selected chop's envelope over its waveform
    void paintBeatGrid (juce::Graphics&);
    void paintChops (juce::Graphics&);
    void paintPlayheads (juce::Graphics&);
    void paintGrains (juce::Graphics&);
    void paintRuler (juce::Graphics&);
    void paintOverview (juce::Graphics&);
    /** In the overview strip: -1 near the visible window's left edge, 1 near its right edge
        (drag them to zoom), 0 elsewhere. */
    int overviewEdgeAt (float x) const;
    /** Splits the chop at `at`; says why when every key already has a chop. */
    int splitAtFrame (int64_t at);
    void paintDragReadout (juce::Graphics&);
    void showContextMenu (int64_t frame);

    juce::String formatTime (double frames) const;

    KerfAudioProcessor& processor;
    AnimationClock& clock;

    juce::Rectangle<float> labelLane, waveArea, rulerArea, overviewArea;

    std::vector<ChopParams> chops;
    double viewStart = 0.0, viewEnd = 1.0;
    SampleData::Ptr shownSample;

    DragMode dragMode = DragMode::none;
    int overviewHover = 0; // the mouse near an end of the overview's window (-1 left, 1 right)
    // PLAY starts at the playhead: at rest at the start of the selected chop; a second click
    // into that chop (or dragging the playhead) puts it anywhere (with CHOP: in the chop), and
    // while it plays it jumps there. With SAMPLE it always shows, with CHOP once put somewhere.
    bool sampleScope = false;
    juce::String selectionSig; // the chops chosen with Ctrl, as last painted
    int cursorChop = 0;            // the selected chop the playhead belongs to
    bool cursorPlaced = false;     // put somewhere by hand (not the chop's start)
    bool clickOnSelected = false;  // this click landed on the chop already selected
    int64_t clickFrame = 0, playheadDragFrame = 0;
    int playTarget() const;        // what PLAY plays: the whole sample (SAMPLE) or the selected chop
    bool targetPlaying() const;
    bool cursorShown() const;
    double samplePlayheadFrame() const; // where the SAMPLE playhead is drawn (playing or at rest)
    void placeSampleCursor (int64_t frame);
    int64_t playheadFrameAt (float x, bool snapToChops) const; // CHOP: catches on chop starts
    EdgeHit dragEdge;
    bool dragLinked = true;
    int64_t dragOriginalFrame = 0, dragCurrentFrame = 0;
    juce::String dragSnapKind;
    double panStartViewStart = 0.0;
    float panStartX = 0.0f;
    juce::Point<float> mousePos;

    std::optional<EdgeHit> hoverEdge;
    bool knifeMode = false;
    bool dragHighlight = false;
    float loadingPhase = 0.0f;

    // Animated per-chop activity (0..1) for the playback glow, keyed by chop id.
    std::map<int, float> activity;
    std::array<double, SamplerEngine::numPhysicalVoices> playheadFrames {};
    std::array<int, SamplerEngine::numPhysicalVoices> playheadChops {};

    // Motion
    static constexpr size_t numVoices = SamplerEngine::numPhysicalVoices;
    double time = 0.0;
    double targetStart = 0.0, targetEnd = 1.0;   // the view glides towards these
    float reveal = 1.0f;                         // new sample: the waveform grows out of the centre line
    std::map<int, float> kick;                   // per chop: 1 on a hit, decays (flash + swell)
    std::map<int, double> born;                  // per chop: when its boundary appeared (drop-in)
    std::set<int> knownChops;
    bool chopsSeeded = false;
    double selStart = -1.0, selEnd = -1.0;       // selection tint glides between chops
    std::array<uint32_t, numVoices> lastTriggers {};
    std::array<double, numVoices> trailFrames {};  // lagging tail of each playhead (phosphor trail)
    std::array<double, numVoices> trailHead {};
    std::array<float, numVoices> trailLevel {};
    std::array<int, numVoices> trailChop {};

    // Playhead "read head" (styled like the GRAIN beam): envelope where the beam crosses
    // the waveform (normalised 0..1 above / below the centre line) and sparks shed behind it.
    std::array<float, numVoices> headTop {}, headBottom {};
    struct Spark { float x, y, vx, vy, life, maxLife, size; };
    std::vector<Spark> sparks;
    std::array<float, numVoices> sparkDebt {};
    juce::Random sparkRandom { 0x4b455246 };

    // Start screen: a field of stars drifting through space. They part around the mouse and
    // spring back; dragging a file in sends them into warp.
    // When the drag ends (taken away or dropped) they rewind: every star flies back to where
    // it was when the file came in (`sx, sy`), from where it was then (`fx, fy`).
    struct Star { float hx, hy, ox, oy, vx, vy, z, phase; bool tinted; float sx = 0, sy = 0, fx = 0, fy = 0; };
    float rewind = -1.0f;          // 0..1 while rewinding, -1 otherwise
    double starFrameClock = 0.0;   // the start screen alone: a frame every 1/30 s
    bool starsWarping = false;
    std::vector<Star> stars;
    juce::Random starRandom { 0x53544152 };
    bool mouseInside = false;
    float warp = 0.0f;
    DropVortex vortex;             // a file dragged over the view (see DropVortex)
    void updateDrop (double dt);
    juce::Point<float> dragPoint;
    bool hasDragPoint = false;
    juce::Image nebula;            // cached, dithered (no banding); rebuilt for a new size or theme
    juce::Colour nebulaAccent;
};

} // namespace kerf::ui

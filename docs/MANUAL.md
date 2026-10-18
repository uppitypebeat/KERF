# Using KERF

The first start opens a short guide. You can open it again any time from the **menu**
symbol, top right.

| Action | How |
|---|---|
| Load a sample | Drag a WAV / AIFF / FLAC / OGG onto the waveform, click the empty waveform, or the folder symbol (Ctrl+O) |
| Play | PLAY / PAUSE and STOP at the top left. **CHOP** plays the selected chop, **SAMPLE** the whole sample. **Space** plays or stops what the last used play button plays (the chop, or the pattern after using SEQ) |
| Playhead | PLAY starts at the selected chop's start. Click into the selected chop a second time to put the playhead there, or drag it (with CHOP it catches on each chop's start; Shift: not). Switching CHOP / SAMPLE while it plays goes on from the same place |
| Loop, metronome | LOOP repeats the chop until stopped. The METRONOME clicks on the beats at KERF's BPM (the header's): with a chop or the sample on the sample's beat lines (where you start does not move them), with the song from its start, with the pattern on its beat. Never recorded. Both are on the CHOP page |
| Pads | Every chop is a pad on its own key from the root up (at most one chop per key). Click a pad, play your keyboard, or drag a pad into the DAW (audio; Ctrl: MIDI) |
| Select several chops | **Ctrl + click** (Cmd on a Mac) chops in the waveform or pads, **Ctrl + drag** a box over chops in the waveform, **Shift + click** for every chop in between, **Ctrl+A** for all: every knob, mode, REVERSE, CHOKE and send you change then goes to all of them (one undo step). A plain click selects one again |
| Delete chops | The bin in the toolbar deletes the selected chops (all of them: one chop over the whole sample again). Ctrl+Z brings them back |
| Queue a chop | While a chop plays (CHOP), click another: it plays as soon as the first one ends |
| Split | Double-click the waveform, or use the KNIFE (K) and click |
| Move a boundary | Drag it. SNAP (magnet) snaps to transients and the grid; **Shift** = no snap; **Alt** = move only this chop's edge |
| Auto-chop | GRID / EQUAL / TRANS / BEAT / RANDOM / SMART, adjust, then **APPLY** (scope ALL or one CHOP). BEAT cuts on the BPM's beat lines; its **SHIFT** slider moves the sample against those lines (up to 500 ms either way; drag, Shift: fine, wheel: 1 ms, double-click: 0) - line the lines up with the hits, then BEAT, GRID, snapping and the metronome follow. Kept with the chops, undoable |
| Zoom | Mouse wheel over the waveform, +/−, or **F** to fit. In the overview strip, drag the window's ends to zoom and its middle to scroll |
| Pages | Tabs at the top right: **CHOP**, **GRANULIZER**, **FX**, **TIME**, **EQ**. A quick double-click switches a page off (it is veiled and its controls rest); again switches it on |
| Remove the sample | Right-click the waveform: *Remove the sample from KERF* (Ctrl+Z brings it back) |
| SEQ | PADS / SEQ on the left of the pads: a step pattern of chops, 1–8 bars, with swing, humanize, repeats, chance and slides. Drag a step to the right to merge the next steps into one long note. The die next to CLEAR writes a pattern from your chops like a producer would: it listens to them (kick, snare, hat, tones and their pitch), gives the bars a form (AABA, ABAB, AAAB ...) with a motif that comes back changed, a density curve, transitions at phrase ends and an ending - using a few of pitch, ratchets, ghost notes, held notes, slides and scattered chops per roll, never all at once. Melodies stay in the key; every roll is new. Right-click it for a MODE (Balanced, Groove, Choppy, Melodic, Texture, Minimal, Chaos) and a STYLE (or Auto). The result is an ordinary pattern: edit it, Ctrl+Z brings the old one back. Export as WAV, or drag as audio / MIDI |
| TIME | 60 slots in three banks (A tempo and tape, B repeats and stutters, C gates and volume). No slot is selected at first (factory setting). Click a slot to select it and see its curves; draw your own (click, drag, double-click, Shift: free); RESET is next to the slot's name. Clicking the selected slot again deselects it and resets its TIME and VOLUME curves |
| EQ | Double-click to add the point shown or remove one (Delete also removes the selected one). The point types are the buttons at the top left; right-click a point to bypass it |
| Record | REC records the input, CAPTURE records KERF's own output, and RESAMPLE renders the chops and the pattern into a new sample (CHOP page) |
| Presets | In the header: 66 factory presets in 8 categories, your own (Save as...), and A / B to compare two versions |
| BPM / KEY | Detected automatically. Drag, type or use the wheel on BPM, or pick a key; AUTO brings back the detected one |
| Undo / redo | Ctrl+Z / Ctrl+Shift+Z or Ctrl+Y. Covers every knob, button and menu, chop edits, pads, pattern, presets and BPM |
| Theme | Click the **KERF** logo for the next theme: Nebula (the default), Aurora, Ember (night skies), Daylight, Glacier, Bloom (white mode) |
| Right-click a chop | Play mode, reverse, choke group, MIDI note, merge, delete |
| Knobs | Drag (Shift: fine) or use the wheel. A knob rests briefly on its default and its marks as you drag past (Shift: it doesn't). Double-click the value to type it; double-click the knob for its default |
| Updates | KERF checks for a new version when it opens. If there is one, the menu button (top right) pulses until you open the menu, and "Check for updates" has a dot; DOWNLOAD there fetches the installer |

KERF's own playback (PLAY, the pattern) stops when the DAW starts playing.

## Pitch and tempo

* **REPITCH** (default, zero latency): speed and pitch move together, like a turntable.
* **STRETCH**: pitch and tempo are independent. Each chop is pre-rendered in the
  background with Signalsmith Stretch, so playback stays latency-free. While a chop is
  rendering, it briefly plays in repitch mode. Automating Global Pitch in this mode
  re-renders in steps, so use REPITCH for smooth pitch sweeps.
* **SYNC** follows the host tempo using the sample's BPM (detected or typed in the header).

## Getting DAW audio into KERF (REC)

An instrument plugin only receives audio from other tracks if the DAW routes it there.
In FL Studio, that is the mixer's sidechain routing into KERF's input. CAPTURE and
RESAMPLE need no routing.

# Piano roll for Furnace

A fork of [Furnace](https://github.com/tildearrow/furnace) v0.6.8.3 that adds a per-channel piano roll window. The roll reads and writes the tracker's patterns directly. There is no new song format: everything you do in the roll becomes ordinary notes, instruments, volumes and effects that stock Furnace opens as usual.

![demo](piano-roll-demo.gif)

## Opening it

`window` > `piano roll`, or **Ctrl+Shift+R** (rebindable in Settings > Keyboard).

## The window, top to bottom

- **Toolbar, row 1:** channel, tools (Select **S**, Draw **D**, Erase **E**), Snap, how drawn notes end (OFF or REL), Fold, Ghost notes, Follow playback.
- **Toolbar, row 2:** zoom, what to do with repeated patterns, Lanes, Clear transport, and hints or warnings.
- **Note panel:** start, end, length, pitch, instrument and effects of the selected note (or notes).
- **Arrangement bar:** one block per order position. Repeated patterns are amber with "xN". Click a block to scroll there.
- **Ruler:** rows and bars. Click to set the start marker.
- **Roll:** keyboard on the left and the channel's notes; the other channels of the same chip are shown in gray (Ghost notes).
- **Lanes:** volume and one effect of your choice, row by row.

## Editing notes

| Action | How |
|---|---|
| Create | Draw: click (uses the last length) or click and drag |
| Move | drag the body of a note |
| Change length | drag the right edge |
| Duplicate | Shift + drag (the copy lands next to the original, never on top of it) |
| Select several | Select: drag a rectangle on empty space; Ctrl+click adds or removes |
| Delete | Delete key, or the Erase tool (click or drag over notes) |
| Select all / clear | Ctrl+A / Esc |
| Undo / redo | Ctrl+Z / Ctrl+Y (the tracker's own history) |

Each gesture is a single undo step. A tracker channel is monophonic: a note starts where the previous one ends. The roll therefore never deletes a note that is not part of the edit; the edited note is the one that gets shorter.

Moving or duplicating a note takes along the effects on its start row, that row's volume and an `ECxx` cut inside the note. Song-wide effects (speed, tempo, order jumps, stop) stay where they are.

**Repeated patterns:** when an edit falls in a pattern used at more than one order position, the roll asks whether to edit all of them, make the pattern unique at that position, or cancel. The answer is remembered until Furnace is closed and can be changed in the toolbar.

**Instrument:** with notes selected, clicking an instrument in the Instruments list applies it to them.

## Note panel

Shows the selected note, or several ("..." where values differ). Start and end use order:row in hex, as in the tracker. Length, transpose (-12, -1, +1, +12) and instrument apply to every selected note.

**Effects** are grouped by code. Editing a value changes the rows that already have that effect; **+N** adds it to the rows that do not; **x** removes it. **Add** writes a new effect on all of them: type the code, Enter, the value, Enter. Double-clicking a note jumps to the Add field. Rows without a free effect column are skipped and the toolbar says so. The roll never adds effect columns by itself; use the `++` button in the tracker's corner for that.

## Volume and effect lanes

- **Volume:** steps showing the volume in force (it holds until the next write). Drag to draw; right button or Erase clears.
- **Effect:** the selector on the left picks the effect; `*` means "the most used in this channel". Bar height is the value (0 to FF). Clicking a row shows it in the note panel even when no note starts there, which is how loose effects such as `EC04` are edited. With Draw, dragging writes the effect; right button or Erase removes it.

## Portamento

A note with `03xx` gets a yellow line that leaves the previous note, ramps to the new pitch and runs through the note. The ramp length is the real slide time, computed the way Furnace does it in linear pitch mode (the default). Drag the dot at the end of the ramp to change the `03` value. A hollow circle means the note has `03` but nothing is sounding before it, so it will not slide.

## Snap and sub-row timing

- **row:** notes on rows.
- **1/2 and tick:** starts and ends inside a row, written as `EDxx` (delay) and `ECxx` (cut). If there is no free effect column for them, the whole edit is refused and nothing changes.

## Row names

Right-click a key on the roll's keyboard to name that pitch in this channel ("kick", "snare", a sample name). The name shows on the key and on the notes.

## Sync with the tracker

Clicking a note moves the tracker cursor to it. Moving the tracker cursor brings the roll to that channel and row. During playback the roll leaves the tracker cursor alone.

## Transport ("piano roll" button next to Poly)

With the button on:
- **Space** is play/stop in any window (overriding Furnace's usual binding), except while typing in a field.
- Play starts at the **marker** (click the ruler). Clicking the ruler while playing jumps there.
- **I** and **O** set the loop start and end at the marker. With **Repeat pattern** on, playback loops inside the selection and cuts notes at the loop point.
- **Clear transport** removes the marker and the loop. F5 still plays from the start of the song.

With the button off, Furnace behaves like the original.

## Files

Per-channel preferences (OFF/REL ending, Fold, row names) live next to the song in `<song>.fur.roll.json`. The `.fur` format is unchanged. Zoom, channel, tool, Snap, Ghost, Follow and Lanes are kept in Furnace's configuration.

## Language

The window's texts are translated to Brazilian Portuguese. Pick the language in Settings > General > Language and restart Furnace. The `locale` folder must sit next to `furnace.exe`.

## Known limits

- One channel is edited at a time.
- The marker and loop work in rows, not ticks, and last until Furnace is closed.
- In songs that do not use linear pitch, the portamento ramp is approximate (drawn dashed).
- The `.exe` is not code-signed, so Windows shows an "unknown publisher" warning.

## Source code and license

GPL-2.0-or-later, like Furnace. The complete source of this version ships with the program (`furnace-piano-roll-src.tar.gz`), and the changes over v0.6.8.3 are in `piano-roll.patch`. Build instructions are in `BUILD.md`.

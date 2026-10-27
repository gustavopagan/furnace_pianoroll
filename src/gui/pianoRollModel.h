/**
 * Furnace Tracker - multi-system chiptune tracker
 * Copyright (C) 2021-2025 tildearrow and contributors
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

// piano roll data model (fork piano-roll).
// converts tracker rows of one channel into a list of notes and back.
// this module must NOT include ImGui or the GUI; it only sees pattern cells
// through PRChannelSource, so it can be unit-tested on its own.
//
// cell layout in v0.6.8.3 (see "Decisões tomadas durante a implementação"):
//   col 0 = note (1..12, 12 = C of the next octave; 100 OFF, 101 REL, 102 macro release)
//   col 1 = octave (signed char stored in a short)
//   col 2 = instrument, col 3 = volume, col 4+2n = effect n, col 5+2n = effect value n
//   empty note = 0/0, any other empty cell = -1
// inside the model a pitch is 0..179 (60 = C-0) and OFF/REL/macro release are 253/254/255.

#ifndef _PIANO_ROLL_MODEL_H
#define _PIANO_ROLL_MODEL_H

#include <vector>
#include <functional>
#include "../engine/defines.h"

#define PR_PITCH_EMPTY -1
#define PR_PITCH_OFF 253
#define PR_PITCH_REL 254
#define PR_PITCH_MACRO_REL 255

#define PR_COL_NOTE 0
#define PR_COL_OCTAVE 1
#define PR_COL_INS 2
#define PR_COL_VOL 3
#define PR_COL_FX(n) (4+((n)<<1))
#define PR_COL_FXVAL(n) (5+((n)<<1))

enum PREndType {
  PR_END_SONG=0, // song ended while the note was sounding (no marker)
  PR_END_NEXT,   // cut by the next note
  PR_END_OFF,
  PR_END_REL
};

struct PRNote {
  int start, end;        // absolute rows, end is exclusive
  int pitch;             // 0..179
  int ins;               // effective instrument (-1 if none seen yet)
  bool insExplicit;      // instrument written on the start row
  PREndType endType;
  int macroRelRow;       // absolute row of a macro release inside the note, or -1
  int delayTicks;        // EDxx on the start row (0 if none)
  int cutRow, cutTicks;  // first ECxx inside the note (cutRow=-1 if none)
  PRNote():
    start(0), end(0), pitch(0), ins(-1), insExplicit(false), endType(PR_END_SONG),
    macroRelRow(-1), delayTicks(0), cutRow(-1), cutTicks(0) {}
};

// read-only view of one channel of one subsong.
struct PRChannelSource {
  int patLen, ordersLen, effectCols;
  // pattern index at an order position
  std::function<int(int orderPos)> patAt;
  // DIV_MAX_COLS cells of a row of a pattern (never NULL)
  std::function<const short*(int patIndex, int row)> rowAt;
  PRChannelSource(): patLen(64), ordersLen(1), effectCols(1) {}
};

// cell <-> pitch conversion
int prDecodePitch(short note, short octave);
void prEncodePitch(int pitch, short& note, short& octave);

// absolute row helpers
inline int prAbsRow(int patLen, int orderPos, int row) { return orderPos*patLen+row; }
inline int prOrderOf(int patLen, int absRow) { return absRow/patLen; }
inline int prRowOf(int patLen, int absRow) { return absRow%patLen; }

// value of effect `fx` on a row, or -1 if absent (first column that has it)
int prFindEffect(const short* row, int effectCols, int fx);

// 3.2: rows -> notes
std::vector<PRNote> prReadChannel(const PRChannelSource& src);
std::vector<PRNote> prReadRows(int total, int effectCols, const std::function<const short*(int absRow)>& rowAbs);

// how many order positions of this channel use the same pattern as orderPos
int prPatternUseCount(const PRChannelSource& src, int orderPos);

// ---------------- 3.3: notes -> rows ----------------

// mutable view of one channel by absolute row.
// getMut may be called for any row 0..total-1 and must return the row that
// get() will return afterwards (writes are visible to later reads).
struct PREditCtx {
  int total, effectCols;
  std::function<const short*(int absRow)> get;
  std::function<short*(int absRow)> getMut;
  PREditCtx(): total(0), effectCols(1) {}
};

// a note to be written. end is exclusive and clamped to the song length.
struct PRNoteSpec {
  int start, end, pitch, ins;
  PRNoteSpec(int s, int e, int p, int i): start(s), end(e), pitch(p), ins(i) {}
};

// one edit = remove some notes (identified by their start row), then add some.
// covers create, delete, move, resize, duplicate, and multi-note versions.
// endMarker: PR_PITCH_OFF or PR_PITCH_REL (channel preference, 3.5).
// only note/octave/instrument cells are written; volume and effects are never touched.
// notes that are not part of the edit keep their length and effective instrument
// (if removing a note would extend or re-instrument a neighbour, a marker or an
// explicit instrument is written to keep the sound the same).
// returns false if nothing was changed.
bool prApplyEdit(PREditCtx& ctx, const std::vector<int>& removeStarts, const std::vector<PRNoteSpec>& add, int endMarker);

// E5: give the notes that start at `starts` the instrument `ins` (written explicitly on the
// start row). Nothing else is touched: end markers, macro releases, volume and effects stay,
// and notes that are not in `starts` keep the instrument they were sounding with.
// returns false if nothing was changed (ins<0, no such note, or already that instrument).
bool prSetInstrument(PREditCtx& ctx, const std::vector<int>& starts, int ins);

// ---------------- E6: effects and volume (3.6, 3.7) ----------------
// effects belong to the row, not to the note. these never touch note/octave/instrument.

struct PRFx {
  int col, fx, val; // col 0..effectCols-1; val -1 = no value written
  PRFx(int c, int f, int v): col(c), fx(f), val(v) {}
};

// non-empty effect columns of a row (a column with only a value and no effect is listed with fx -1)
std::vector<PRFx> prRowEffects(const short* row, int effectCols);

// write effect fx with value val (0..255) on each row:
// - if the row already has fx in a column, that column's value changes (first such column);
// - otherwise the first empty column (no effect and no value) gets it;
// - otherwise the row is left alone and added to *refused (if not NULL).
// returns true if anything changed.
bool prSetEffect(PREditCtx& ctx, const std::vector<int>& rows, int fx, int val, std::vector<int>* refused);

// only rows that already have fx get the new value. returns true if anything changed.
bool prSetEffectValueWherePresent(PREditCtx& ctx, const std::vector<int>& rows, int fx, int val);

// remove every column holding fx on each row (effect and value). returns true if anything changed.
bool prRemoveEffect(PREditCtx& ctx, const std::vector<int>& rows, int fx);

// volume column: rowVal maps absolute row -> value (-1 clears). returns true if anything changed.
bool prSetVolume(PREditCtx& ctx, const std::vector<std::pair<int,int>>& rowVal);

// ---------------- E6 (revised): notes carry their effects when moved ----------------

// effects that belong to the song, not to a note (speed, tempo, jumps, stop):
// they never travel with a note.
bool prIsSongEffect(int fx);

// one note being moved, duplicated or resized.
// from: start row of the existing note. to: where it goes (already clipped by the caller).
// copy: duplicate (the original and its effects stay).
struct PRMove {
  int from;       // start row of the existing note; -1: a new note
  PRNoteSpec to;
  bool copy;
  // E8 sub-row (tick snap). -1: keep whatever the note had (moves carry it).
  // delay >= 0: the note starts `delay` ticks into its first row (EDxx; 0 = no EDxx).
  // cut >= 0: the note is cut `cut` ticks into its last row, to.end-1 (ECxx; 0 = no ECxx).
  int delay, cut;
  PRMove(int f, PRNoteSpec t, bool c): from(f), to(t), copy(c), delay(-1), cut(-1) {}
  PRMove(int f, PRNoteSpec t, bool c, int d, int ct): from(f), to(t), copy(c), delay(d), cut(ct) {}
};

// like prApplyEdit for moves, plus:
// - when the start row changes, the note's start-row effects (except song effects) and its
//   start-row volume go with it; a note cut (ECxx) later inside the note keeps its distance
//   from the end of the note.
// - when only the length changes (resize), the note cut keeps its distance from the end.
// E8: with delay/cut set, the note's EDxx/ECxx are replaced by the given ones; a value that does
// not fit (no free effect column) goes to *refused (the caller refuses the whole edit).
// at the destination, effects already there stay; carried ones take free columns (same effect:
// the carried value wins). what does not fit stays where it was and its row goes to *refused.
bool prMoveNotes(PREditCtx& ctx, const std::vector<PRMove>& moves, int endMarker, std::vector<int>* refused);

// ---------------- copy / paste ----------------

// one copied note, relative to the first copied note.
struct PRClipNote {
  int rel, len, pitch, ins;  // ins -1: none known
  int vol;                   // start-row volume, -1: none
  std::vector<std::pair<int,int>> fx; // start-row effects (song effects left out), val -1 = none
  int cutRel, cutVal;        // ECxx inside the note: row relative to the note start (-1: none)
  PRClipNote(): rel(0), len(1), pitch(0), ins(-1), vol(-1), cutRel(-1), cutVal(0) {}
};

// builds the clipboard from notes of a channel (rows read with rowAt(absRow)).
std::vector<PRClipNote> prCopyNotes(const std::vector<const PRNote*>& notes, int effectCols, const std::function<const short*(int absRow)>& rowAt);

// pastes clip at absolute row `at`. notes of `existing` that start inside the pasted span
// [at, at+span) are replaced (with their start-row volume and note effects); a note playing
// into the span is cut by the first pasted note, as in any tracker edit. pasted notes get an
// explicit instrument. notes past the end of the song are dropped. effects that find no free
// column go to *refused (rows). *newStarts gets the start rows of the pasted notes.
// returns false if nothing was changed.
bool prPasteNotes(PREditCtx& ctx, const std::vector<PRNote>& existing, const std::vector<PRClipNote>& clip, int at, int endMarker, std::vector<int>* refused, std::vector<int>* newStarts);

#endif

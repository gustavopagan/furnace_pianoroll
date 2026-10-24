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

// how many order positions of this channel use the same pattern as orderPos
int prPatternUseCount(const PRChannelSource& src, int orderPos);

#endif

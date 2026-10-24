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

#include "pianoRollModel.h"

int prDecodePitch(short note, short octave) {
  if (note==0 && octave==0) return PR_PITCH_EMPTY;
  if (note==100) return PR_PITCH_OFF;
  if (note==101) return PR_PITCH_REL;
  if (note==102) return PR_PITCH_MACRO_REL;
  if (note<1 || note>12) return PR_PITCH_EMPTY;
  int p=note+(signed char)octave*12+60;
  if (p<0 || p>179) return PR_PITCH_EMPTY;
  return p;
}

void prEncodePitch(int pitch, short& note, short& octave) {
  if (pitch==PR_PITCH_OFF) { note=100; octave=0; return; }
  if (pitch==PR_PITCH_REL) { note=101; octave=0; return; }
  if (pitch==PR_PITCH_MACRO_REL) { note=102; octave=0; return; }
  if (pitch<0 || pitch>179) { note=0; octave=0; return; }
  // same convention as FurnaceGUI::decodeNote(): C is note 12 of the previous octave
  int seek=pitch;
  if ((seek%12)==0) {
    note=12;
    octave=(unsigned char)((seek/12)-6);
  } else {
    note=seek%12;
    octave=(unsigned char)((seek/12)-5);
  }
}

int prFindEffect(const short* row, int effectCols, int fx) {
  if (effectCols>DIV_MAX_EFFECTS) effectCols=DIV_MAX_EFFECTS;
  for (int i=0; i<effectCols; i++) {
    if (row[PR_COL_FX(i)]==fx) {
      short v=row[PR_COL_FXVAL(i)];
      return (v<0)?0:v;
    }
  }
  return -1;
}

std::vector<PRNote> prReadChannel(const PRChannelSource& src) {
  std::vector<PRNote> ret;
  bool open=false;
  PRNote cur;
  int lastIns=-1;
  int total=src.ordersLen*src.patLen;

  for (int r=0; r<total; r++) {
    int orderPos=prOrderOf(src.patLen,r);
    int patIndex=src.patAt(orderPos);
    const short* row=src.rowAt(patIndex,prRowOf(src.patLen,r));
    int pitch=prDecodePitch(row[PR_COL_NOTE],row[PR_COL_OCTAVE]);
    short ins=row[PR_COL_INS];

    if (pitch>=0 && pitch<=179) {
      if (open) {
        cur.end=r;
        cur.endType=PR_END_NEXT;
        ret.push_back(cur);
      }
      cur=PRNote();
      cur.start=r;
      cur.pitch=pitch;
      if (ins>=0) {
        cur.ins=ins;
        cur.insExplicit=true;
      } else {
        cur.ins=lastIns;
        cur.insExplicit=false;
      }
      int d=prFindEffect(row,src.effectCols,0xed);
      if (d>0) cur.delayTicks=d;
      open=true;
    } else if (pitch==PR_PITCH_OFF || pitch==PR_PITCH_REL) {
      if (open) {
        cur.end=r;
        cur.endType=(pitch==PR_PITCH_OFF)?PR_END_OFF:PR_END_REL;
        ret.push_back(cur);
        open=false;
      }
    } else if (pitch==PR_PITCH_MACRO_REL) {
      if (open && cur.macroRelRow<0) cur.macroRelRow=r;
    }

    // instrument column also updates the inherited instrument on rows without a note
    if (ins>=0) lastIns=ins;

    // first ECxx while the note is sounding (including its start row)
    if (open && cur.cutRow<0) {
      int c=prFindEffect(row,src.effectCols,0xec);
      if (c>0) {
        cur.cutRow=r;
        cur.cutTicks=c;
      }
    }
  }
  if (open) {
    cur.end=total;
    cur.endType=PR_END_SONG;
    ret.push_back(cur);
  }
  return ret;
}

int prPatternUseCount(const PRChannelSource& src, int orderPos) {
  int p=src.patAt(orderPos);
  int n=0;
  for (int i=0; i<src.ordersLen; i++) {
    if (src.patAt(i)==p) n++;
  }
  return n;
}

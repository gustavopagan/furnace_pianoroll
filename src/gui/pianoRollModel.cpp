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
#include <stddef.h>

#define MIN_PR(a,b) (((a)<(b))?(a):(b))

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
  int patLen=src.patLen;
  return prReadRows(src.ordersLen*src.patLen,src.effectCols,[&src,patLen](int r) -> const short* {
    return src.rowAt(src.patAt(prOrderOf(patLen,r)),prRowOf(patLen,r));
  });
}

std::vector<PRNote> prReadRows(int total, int effectCols, const std::function<const short*(int absRow)>& rowAbs) {
  std::vector<PRNote> ret;
  bool open=false;
  PRNote cur;
  int lastIns=-1;

  for (int r=0; r<total; r++) {
    const short* row=rowAbs(r);
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
      int d=prFindEffect(row,effectCols,0xed);
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
      int c=prFindEffect(row,effectCols,0xec);
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

// ---------------- writing ----------------

static int prCellPitch(const short* row) {
  return prDecodePitch(row[PR_COL_NOTE],row[PR_COL_OCTAVE]);
}

static void prSetPitchCell(PREditCtx& ctx, int r, int pitch) {
  short* row=ctx.getMut(r);
  short n,o;
  prEncodePitch(pitch,n,o);
  row[PR_COL_NOTE]=n;
  row[PR_COL_OCTAVE]=o;
}

static void prClearPitchCell(PREditCtx& ctx, int r) {
  short* row=ctx.getMut(r);
  row[PR_COL_NOTE]=0;
  row[PR_COL_OCTAVE]=0;
}

static bool prIsMarker(int pitch) {
  return pitch==PR_PITCH_OFF || pitch==PR_PITCH_REL || pitch==PR_PITCH_MACRO_REL;
}

static const PRNote* prFindStart(const std::vector<PRNote>& notes, int start) {
  for (const PRNote& n: notes) if (n.start==start) return &n;
  return NULL;
}

// clear the cells that belong to note n (start note+ins, macro release, own end marker)
static void prRemoveCells(PREditCtx& ctx, const PRNote& n) {
  prClearPitchCell(ctx,n.start);
  ctx.getMut(n.start)[PR_COL_INS]=-1;
  if (n.macroRelRow>=0 && prCellPitch(ctx.get(n.macroRelRow))==PR_PITCH_MACRO_REL) {
    prClearPitchCell(ctx,n.macroRelRow);
  }
  if ((n.endType==PR_END_OFF || n.endType==PR_END_REL) && n.end<ctx.total) {
    int p=prCellPitch(ctx.get(n.end));
    if (p==PR_PITCH_OFF || p==PR_PITCH_REL) prClearPitchCell(ctx,n.end);
  }
}

static void prAddOne(PREditCtx& ctx, PRNoteSpec spec, int endMarker, const std::vector<PRNote>& before) {
  int a=spec.start;
  int b=MIN_PR(spec.end,ctx.total);
  if (a<0 || a>=ctx.total || b<=a) return;
  std::vector<PRNote> notes=prReadRows(ctx.total,ctx.effectCols,ctx.get);

  for (const PRNote& m: notes) {
    if (m.start>=a && m.start<b) {
      // overlap is judged by the length the note had before this edit, so a note that
      // only got longer because a neighbour was removed in the same edit is not resurrected
      int mEnd=m.end;
      const PRNote* mb=prFindStart(before,m.start);
      if (mb!=NULL && mb->pitch==m.pitch && mb->end<mEnd) mEnd=mb->end;
      if (mEnd<=b) {
        // fully covered: delete
        prRemoveCells(ctx,m);
      } else {
        // starts inside, ends after: trim its start to b (end marker stays)
        prClearPitchCell(ctx,m.start);
        ctx.getMut(m.start)[PR_COL_INS]=-1;
        if (m.macroRelRow>=0 && m.macroRelRow<b && prCellPitch(ctx.get(m.macroRelRow))==PR_PITCH_MACRO_REL) {
          prClearPitchCell(ctx,m.macroRelRow);
        }
        prSetPitchCell(ctx,b,m.pitch);
        if (m.ins>=0) ctx.getMut(b)[PR_COL_INS]=m.ins;
      }
    } else if (m.start<a && m.end>a) {
      // previous note: will be cut at a by the new note
      if (m.macroRelRow>=a && prCellPitch(ctx.get(m.macroRelRow))==PR_PITCH_MACRO_REL) {
        prClearPitchCell(ctx,m.macroRelRow);
      }
      if ((m.endType==PR_END_OFF || m.endType==PR_END_REL) && m.end!=b && m.end<ctx.total) {
        int p=prCellPitch(ctx.get(m.end));
        if (p==PR_PITCH_OFF || p==PR_PITCH_REL) prClearPitchCell(ctx,m.end);
      }
    }
  }
  // stray markers inside the new note would cut it
  for (int r=a+1; r<b; r++) {
    if (prIsMarker(prCellPitch(ctx.get(r)))) prClearPitchCell(ctx,r);
  }
  // the note itself, instrument always explicit
  prSetPitchCell(ctx,a,spec.pitch);
  if (spec.ins>=0) ctx.getMut(a)[PR_COL_INS]=spec.ins;
  // end marker unless another note starts there
  if (b<ctx.total) {
    int p=prCellPitch(ctx.get(b));
    if (!(p>=0 && p<=179)) prSetPitchCell(ctx,b,endMarker);
  }
}

bool prApplyEdit(PREditCtx& ctx, const std::vector<int>& removeStarts, const std::vector<PRNoteSpec>& add, int endMarker) {
  if (endMarker!=PR_PITCH_OFF && endMarker!=PR_PITCH_REL) endMarker=PR_PITCH_OFF;
  std::vector<PRNote> before=prReadRows(ctx.total,ctx.effectCols,ctx.get);

  // remove
  for (int s: removeStarts) {
    std::vector<PRNote> cur=prReadRows(ctx.total,ctx.effectCols,ctx.get);
    const PRNote* n=prFindStart(cur,s);
    if (n!=NULL) prRemoveCells(ctx,*n);
  }
  // add
  for (const PRNoteSpec& sp: add) prAddOne(ctx,sp,endMarker,before);

  // keep untouched notes sounding the same
  auto isEdited=[&](const PRNote& n) -> bool {
    for (int s: removeStarts) if (s==n.start) return true;
    for (const PRNoteSpec& sp: add) {
      if (n.start>=sp.start && n.start<sp.end) return true; // replaced or trimmed by an added note
    }
    return false;
  };
  for (int pass=0; pass<4; pass++) {
    bool changed=false;
    std::vector<PRNote> after=prReadRows(ctx.total,ctx.effectCols,ctx.get);
    for (const PRNote& b: before) {
      if (isEdited(b)) continue;
      const PRNote* a=prFindStart(after,b.start);
      if (a==NULL || a->pitch!=b.pitch) continue;
      // instrument changed because an explicit instrument before it was removed
      if (a->ins!=b.ins && b.ins>=0) {
        ctx.getMut(b.start)[PR_COL_INS]=b.ins;
        changed=true;
      }
      // got longer because the note that cut it was removed
      if (a->end>b.end && b.end<ctx.total) {
        int m=(b.endType==PR_END_REL)?PR_PITCH_REL:((b.endType==PR_END_OFF)?PR_PITCH_OFF:endMarker);
        prSetPitchCell(ctx,b.end,m);
        changed=true;
      }
    }
    if (!changed) break;
  }

  std::vector<PRNote> fin=prReadRows(ctx.total,ctx.effectCols,ctx.get);
  if (fin.size()!=before.size()) return true;
  for (size_t i=0; i<fin.size(); i++) {
    if (fin[i].start!=before[i].start || fin[i].end!=before[i].end || fin[i].pitch!=before[i].pitch || fin[i].ins!=before[i].ins || fin[i].endType!=before[i].endType) return true;
  }
  return false;
}

bool prSetInstrument(PREditCtx& ctx, const std::vector<int>& starts, int ins) {
  if (ins<0) return false;
  std::vector<PRNote> before=prReadRows(ctx.total,ctx.effectCols,ctx.get);
  bool changed=false;
  for (int s: starts) {
    const PRNote* n=prFindStart(before,s);
    if (n==NULL) continue;
    if (n->ins==ins && n->insExplicit) continue;
    ctx.getMut(s)[PR_COL_INS]=ins;
    changed=true;
  }
  if (!changed) return false;
  // a note that inherited from a changed note keeps its own sound
  for (int pass=0; pass<4; pass++) {
    bool fixed=false;
    std::vector<PRNote> after=prReadRows(ctx.total,ctx.effectCols,ctx.get);
    for (const PRNote& b: before) {
      bool sel=false;
      for (int s: starts) if (s==b.start) sel=true;
      if (sel) continue;
      const PRNote* a=prFindStart(after,b.start);
      if (a==NULL || a->pitch!=b.pitch) continue;
      // same cell as a changed note (repeated pattern): it changes with it, by design
      if (a->insExplicit && a->ins==ins) continue;
      if (a->ins!=b.ins && b.ins>=0) {
        ctx.getMut(b.start)[PR_COL_INS]=b.ins;
        fixed=true;
      }
    }
    if (!fixed) break;
  }
  return true;
}

// ---------------- E6: effects and volume ----------------

std::vector<PRFx> prRowEffects(const short* row, int effectCols) {
  std::vector<PRFx> r;
  if (effectCols>DIV_MAX_EFFECTS) effectCols=DIV_MAX_EFFECTS;
  for (int i=0; i<effectCols; i++) {
    short f=row[PR_COL_FX(i)];
    short v=row[PR_COL_FXVAL(i)];
    if (f<0 && v<0) continue;
    r.push_back(PRFx(i,(f<0)?-1:f,(v<0)?-1:v));
  }
  return r;
}

static int prClampCols(int effectCols) {
  if (effectCols<1) return 1;
  if (effectCols>DIV_MAX_EFFECTS) return DIV_MAX_EFFECTS;
  return effectCols;
}

bool prSetEffect(PREditCtx& ctx, const std::vector<int>& rows, int fx, int val, std::vector<int>* refused) {
  if (fx<0 || fx>255) return false;
  if (val<0) val=0;
  if (val>255) val=255;
  int cols=prClampCols(ctx.effectCols);
  bool changed=false;
  std::vector<int> done;
  for (int r: rows) {
    if (r<0 || r>=ctx.total) continue;
    bool dup=false;
    for (int d: done) if (d==r) dup=true;
    if (dup) continue;
    done.push_back(r);
    const short* row=ctx.get(r);
    int col=-1;
    for (int i=0; i<cols; i++) {
      if (row[PR_COL_FX(i)]==fx) { col=i; break; }
    }
    if (col<0) {
      for (int i=0; i<cols; i++) {
        if (row[PR_COL_FX(i)]<0 && row[PR_COL_FXVAL(i)]<0) { col=i; break; }
      }
    }
    if (col<0) {
      if (refused!=NULL) refused->push_back(r);
      continue;
    }
    if (row[PR_COL_FX(col)]==fx && row[PR_COL_FXVAL(col)]==val) continue;
    short* m=ctx.getMut(r);
    m[PR_COL_FX(col)]=fx;
    m[PR_COL_FXVAL(col)]=val;
    changed=true;
  }
  return changed;
}

bool prSetEffectValueWherePresent(PREditCtx& ctx, const std::vector<int>& rows, int fx, int val) {
  if (fx<0 || fx>255) return false;
  if (val<0) val=0;
  if (val>255) val=255;
  int cols=prClampCols(ctx.effectCols);
  bool changed=false;
  for (int r: rows) {
    if (r<0 || r>=ctx.total) continue;
    const short* row=ctx.get(r);
    for (int i=0; i<cols; i++) {
      if (row[PR_COL_FX(i)]!=fx) continue;
      if (row[PR_COL_FXVAL(i)]!=val) {
        ctx.getMut(r)[PR_COL_FXVAL(i)]=val;
        changed=true;
      }
      break;
    }
  }
  return changed;
}

bool prRemoveEffect(PREditCtx& ctx, const std::vector<int>& rows, int fx) {
  if (fx<0 || fx>255) return false;
  int cols=prClampCols(ctx.effectCols);
  bool changed=false;
  for (int r: rows) {
    if (r<0 || r>=ctx.total) continue;
    for (int i=0; i<cols; i++) {
      if (ctx.get(r)[PR_COL_FX(i)]!=fx) continue;
      short* m=ctx.getMut(r);
      m[PR_COL_FX(i)]=-1;
      m[PR_COL_FXVAL(i)]=-1;
      changed=true;
    }
  }
  return changed;
}

bool prSetVolume(PREditCtx& ctx, const std::vector<std::pair<int,int>>& rowVal) {
  bool changed=false;
  for (auto& rv: rowVal) {
    int r=rv.first;
    if (r<0 || r>=ctx.total) continue;
    int v=rv.second;
    if (v<-1) v=-1;
    if (ctx.get(r)[PR_COL_VOL]==v) continue;
    ctx.getMut(r)[PR_COL_VOL]=v;
    changed=true;
  }
  return changed;
}

// ---------------- E6 (revised): moving notes with their effects ----------------

bool prIsSongEffect(int fx) {
  if (fx>=0xc0 && fx<=0xcf) return true; // Cxxx tick rate
  switch (fx) {
    case 0x09: case 0x0f: // speed
    case 0xf0: case 0xfd: case 0xfe: // tempo
    case 0x0b: case 0x0d: // jumps
    case 0xff: // stop
      return true;
  }
  return false;
}

bool prMoveNotes(PREditCtx& ctx, const std::vector<PRMove>& moves, int endMarker, std::vector<int>* refused) {
  std::vector<PRNote> before=prReadRows(ctx.total,ctx.effectCols,ctx.get);
  int cols=prClampCols(ctx.effectCols);

  struct Job {
    PRMove m;
    int oldEnd;
    bool sameStart;
    int vol;
    std::vector<std::pair<int,int>> fx; // carried start-row effects
    int cutRow, cutVal;                 // carried note cut (-1: none)
    Job(const PRMove& mv): m(mv), oldEnd(0), sameStart(false), vol(-1), cutRow(-1), cutVal(0) {}
  };
  std::vector<Job> jobs;
  for (const PRMove& mv: moves) {
    const PRNote* n=prFindStart(before,mv.from);
    if (n==NULL) continue;
    Job j(mv);
    j.oldEnd=n->end;
    j.sameStart=(mv.to.start==mv.from);
    const short* row=ctx.get(mv.from);
    if (!j.sameStart) {
      j.vol=row[PR_COL_VOL];
      for (int i=0; i<cols; i++) {
        int f=row[PR_COL_FX(i)];
        if (f<0 || prIsSongEffect(f)) continue;
        bool dup=false;
        for (auto& x: j.fx) if (x.first==f) dup=true;
        if (dup) continue;
        int v=row[PR_COL_FXVAL(i)];
        j.fx.push_back(std::make_pair(f,(v<0)?0:v));
      }
    }
    // the cut travels on its own only if it is not already part of the start-row effects
    if (n->cutRow>=0 && (n->cutRow>n->start || j.sameStart)) {
      j.cutRow=n->cutRow;
      j.cutVal=n->cutTicks;
    }
    jobs.push_back(j);
  }

  bool changed=false;
  // 1. lift what travels from the old rows (moves only, not copies)
  for (Job& j: jobs) {
    if (j.m.copy) continue;
    if (!j.sameStart) {
      if (j.vol>=0) {
        ctx.getMut(j.m.from)[PR_COL_VOL]=-1;
        changed=true;
      }
      for (auto& x: j.fx) {
        if (prRemoveEffect(ctx,{j.m.from},x.first)) changed=true;
      }
    }
    if (j.cutRow>=0) {
      if (prRemoveEffect(ctx,{j.cutRow},0xec)) changed=true;
    }
  }

  // 2. the notes themselves
  std::vector<int> rem;
  std::vector<PRNoteSpec> add;
  for (Job& j: jobs) {
    if (!j.m.copy) rem.push_back(j.m.from);
    add.push_back(j.m.to);
  }
  if (prApplyEdit(ctx,rem,add,endMarker)) changed=true;

  // 3. put down what travels
  for (Job& j: jobs) {
    int to=j.m.to.start;
    if (to<0 || to>=ctx.total) continue;
    if (!j.sameStart) {
      if (j.vol>=0 && ctx.get(to)[PR_COL_VOL]!=j.vol) {
        ctx.getMut(to)[PR_COL_VOL]=j.vol;
        changed=true;
      }
      for (auto& x: j.fx) {
        std::vector<int> ref;
        if (prSetEffect(ctx,{to},x.first,x.second,&ref)) changed=true;
        if (!ref.empty()) {
          // did not fit: leave it where it was
          std::vector<int> ref2;
          if (!j.m.copy) prSetEffect(ctx,{j.m.from},x.first,x.second,&ref2);
          if (refused!=NULL) refused->push_back(to);
        }
      }
    }
    if (j.cutRow>=0) {
      int newEnd=MIN_PR(j.m.to.end,ctx.total);
      int nc=newEnd-(j.oldEnd-j.cutRow);
      if (nc<to) nc=to;
      if (nc>=newEnd) nc=newEnd-1;
      if (nc>=0 && nc<ctx.total) {
        std::vector<int> ref;
        if (prSetEffect(ctx,{nc},0xec,j.cutVal,&ref)) changed=true;
        if (!ref.empty()) {
          std::vector<int> ref2;
          if (!j.m.copy) prSetEffect(ctx,{j.cutRow},0xec,j.cutVal,&ref2);
          if (refused!=NULL) refused->push_back(nc);
        }
      }
    }
  }
  return changed;
}

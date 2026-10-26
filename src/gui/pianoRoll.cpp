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

// piano roll window (fork piano-roll)
// time runs left to right, pitch bottom to top.
// E3: drawing. E4: editing (create/move/resize/delete/duplicate) with undo.

#include "gui.h"
#include "guiConst.h"
#include "pianoRollModel.h"
#include "pianoRollPrefs.h"
#include <imgui_internal.h>
#include <math.h>
#include <algorithm>
#include <set>
#include <array>
#include <map>
#include <functional>

enum PRTool {
  PR_TOOL_SELECT=0,
  PR_TOOL_DRAW,
  PR_TOOL_ERASE
};

enum PRDrag {
  PR_DRAG_NONE=0,
  PR_DRAG_MOVE,
  PR_DRAG_DUPLICATE,
  PR_DRAG_RESIZE,
  PR_DRAG_CREATE,
  PR_DRAG_RECT,
  PR_DRAG_ERASE,
  PR_DRAG_PORTA
};

enum PRSnap {
  PR_SNAP_ROW=0,
  PR_SNAP_HALF,
  PR_SNAP_TICK
};

enum PRLaneDrag {
  PR_LANE_NONE=0,
  PR_LANE_VOL_PAINT,
  PR_LANE_VOL_ERASE,
  PR_LANE_FX_PAINT,
  PR_LANE_FX_ERASE,
  PR_LANE_FX_PICK
};

enum PRRepeatMode {
  PR_REPEAT_ASK=0,
  PR_REPEAT_EDIT_ALL,
  PR_REPEAT_MAKE_UNIQUE
};

struct FurnacePianoRoll {
  int chan;
  bool follow, ghost;
  float zoomX, zoomY;     // pixels per row, pixels per semitone (before dpiScale)
  float scrollX, scrollY; // grid scroll in pixels, read back from the grid child
  bool centerPending;     // scroll vertically to the channel's notes on next frame
  float pendingScrollX;   // <0: none
  float pendingScrollY;
  bool firstFrame;

  // editing
  int tool;
  std::set<int> sel;      // selected notes of the current channel, by start row
  int drag;
  int dragGrabStart;      // start row of the note under the mouse when the drag began
  int dragGrabPitch;
  float dragMouseRow0;    // mouse position (in rows / axis rows) when the drag began
  float dragMouseAx0;
  int dragDRow, dragDPitch, dragDLen;
  int createRow, createPitch;
  std::set<int> eraseSet;
  ImVec2 rectA, rectB;    // rubber band in content coordinates
  int lastLen;
  // E7b: transport (DAW style). absolute rows, -1: unset. kept for the session only.
  int markerRow, loopIn, loopOut;
  bool loopApplied;
  int loopAppliedIn, loopAppliedOut;
  int seenOrd, seenRow, seenChan; // E7: last tracker cursor seen
  int revealRow;          // E7: scroll the roll to show this row (-1: none)
  int snap;               // E8: PR_SNAP_*
  int nameEditPitch;      // E8: row being named (4.5)
  char nameBuf[64];
  bool pendStrict;        // E8: refuse the whole edit if an effect does not fit
  int portaNote;          // start row of the note whose portamento anchor is being dragged
  int portaNewSpeed;

  // pending edit (prApplyEdit arguments), committed by pianoRollCommit()
  std::vector<int> pendRem;
  std::vector<PRNoteSpec> pendAdd;
  std::vector<int> pendSelect; // starts to select after the commit
  // E5/E6: any other edit (instrument, effects, volume) as a function on the channel.
  // it runs twice (dry run, then for real), so it must only depend on what it captured.
  std::function<bool(PREditCtx&)> pendOp;
  std::vector<int> pendRefused; // rows an effect edit could not write (no free column)

  // E6: note panel and lanes
  int focusRow;           // row shown in the note panel when no note is selected (-1: none)
  int laneFx;             // effect shown in the effect lane (-1: most used in the channel)
  bool lanes;             // show the volume and effect lanes
  bool focusFxField;      // give keyboard focus to the "add effect" field on the next frame
  bool focusValField;
  int laneDrag;           // PR_LANE_*
  std::map<int,int> lanePaint; // row -> value being painted (-1: erase)
  int laneLastRow;
  float laneLastVal;
  char addFxCode[8], addFxVal[8];
  int repeatMode;
  bool repeatPopup;
  int repeatPopupCount;

  // preview
  int previewNote;        // engine note (pitch-60) currently sounding, or -999

  // status line
  String status;
  double statusTime;

  // per-channel preferences (.fur.roll.json)
  PRPrefs prefs;
  String prefsSong;
  bool prefsLoaded;

  FurnacePianoRoll():
    chan(0), follow(true), ghost(true),
    zoomX(14.0f), zoomY(18.0f), scrollX(0), scrollY(0),
    centerPending(true), pendingScrollX(-1), pendingScrollY(-1), firstFrame(true),
    tool(PR_TOOL_DRAW), drag(PR_DRAG_NONE), dragGrabStart(-1), dragGrabPitch(0),
    dragMouseRow0(0), dragMouseAx0(0), dragDRow(0), dragDPitch(0), dragDLen(0),
    createRow(0), createPitch(0), lastLen(1),
    repeatMode(PR_REPEAT_ASK), repeatPopup(false), repeatPopupCount(0),
    previewNote(-999), statusTime(-100.0), prefsLoaded(false) {
    portaNote=-1; portaNewSpeed=0;
    snap=PR_SNAP_ROW; pendStrict=false;
    nameEditPitch=-1; nameBuf[0]=0;
    seenOrd=-1; seenRow=-1; seenChan=-1; revealRow=-1;
    markerRow=-1; loopIn=-1; loopOut=-1; loopApplied=false; loopAppliedIn=-1; loopAppliedOut=-1;
    focusRow=-1; laneFx=-1; lanes=true; focusFxField=false; focusValField=false;
    laneDrag=0; laneLastRow=-1; laneLastVal=0;
    addFxCode[0]=0;
    addFxVal[0]=0;
  }
};

static PRChannelSource prSourceFor(DivEngine* e, int ch) {
  PRChannelSource src;
  DivSubSong* s=e->curSubSong;
  src.patLen=s->patLen;
  src.ordersLen=s->ordersLen;
  src.effectCols=s->pat[ch].effectCols;
  src.patAt=[s,ch](int p) -> int { return s->orders.ord[ch][p]; };
  src.rowAt=[s,ch](int pat, int r) -> const short* { return s->pat[ch].getPattern(pat,false)->data[r]; };
  return src;
}

// an edited note never runs over a note that is not part of the edit:
// it ends where the next such note (or the next edited note) starts
static void prClipAdds(const std::vector<PRNote>& notes, const std::vector<int>& rem, std::vector<PRNoteSpec>& add) {
  for (PRNoteSpec& sp: add) {
    int lim=sp.end;
    for (const PRNote& n: notes) {
      bool removed=std::find(rem.begin(),rem.end(),n.start)!=rem.end();
      if (removed) continue;
      if (n.start>sp.start && n.start<lim) lim=n.start;
    }
    for (PRNoteSpec& o: add) {
      if (o.start>sp.start && o.start<lim) lim=o.start;
    }
    sp.end=lim;
  }
}

static bool prParseOrdRow(const char* s, int patLen, int totalRows, int& out) {
  unsigned int o=0, r=0;
  if (sscanf(s,"%x:%x",&o,&r)!=2) return false;
  if ((int)r>=patLen) return false;
  int a=(int)o*patLen+(int)r;
  if (a<0 || a>=totalRows) return false;
  out=a;
  return true;
}

static bool prIsBlackKey(int pitch) {
  static const bool black[12]={false,true,false,true,false,false,true,false,true,false,true,false};
  return black[((pitch%12)+12)%12];
}

static void prSetStatus(FurnacePianoRoll& pr, const String& s) {
  pr.status=s;
  pr.statusTime=ImGui::GetTime();
}

// ---------------- preferences file hooks ----------------

void FurnaceGUI::pianoRollSongLoaded(const String& path) {
  if (pianoRoll==NULL) return; // loaded lazily when the window is created
  pianoRoll->prefs=PRPrefs();
  pianoRoll->prefsSong=path;
  pianoRoll->prefsLoaded=true;
  prPrefsLoad(pianoRoll->prefs,path);
  pianoRoll->sel.clear();
  pianoRoll->centerPending=true;
}

void FurnaceGUI::pianoRollSongSaved(const String& path) {
  if (pianoRoll==NULL) return;
  if (!backupPath.empty() && path.find(backupPath)==0) return;
  pianoRoll->prefsSong=path;
  bool any=false;
  for (auto& i: pianoRoll->prefs.chans) if (!i.second.isDefault()) any=true;
  if (any) prPrefsSave(pianoRoll->prefs,path);
}

static void prSavePrefs(FurnacePianoRoll& pr, const String& curFileName) {
  if (curFileName.empty()) return; // kept in memory until the song is saved
  pr.prefsSong=curFileName;
  if (!prPrefsSave(pr.prefs,curFileName)) {
    prSetStatus(pr,_("could not write the .roll.json file next to the song"));
  }
}

// ---------------- committing an edit ----------------

void FurnaceGUI::pianoRollCommit() {
  if (pianoRoll==NULL) return;
  FurnacePianoRoll& pr=*pianoRoll;
  int ch=pr.chan;
  DivSubSong* sub=e->curSubSong;
  int patLen=sub->patLen;
  int ordersLen=sub->ordersLen;
  int total=patLen*ordersLen;
  int effectCols=sub->pat[ch].effectCols;
  int endMarker=pr.prefs.get(ch).endRel?PR_PITCH_REL:PR_PITCH_OFF;

  // 1. dry run on a copy to find which order positions would change
  std::map<int,std::array<short,DIV_MAX_COLS>> overlay;
  PREditCtx dry;
  dry.total=total;
  dry.effectCols=effectCols;
  dry.get=[&](int r) -> const short* {
    auto it=overlay.find(r);
    if (it!=overlay.end()) return it->second.data();
    return e->curPat[ch].getPattern(e->curOrders->ord[ch][r/patLen],false)->data[r%patLen];
  };
  dry.getMut=[&](int r) -> short* {
    auto it=overlay.find(r);
    if (it!=overlay.end()) return it->second.data();
    std::array<short,DIV_MAX_COLS>& a=overlay[r];
    memcpy(a.data(),e->curPat[ch].getPattern(e->curOrders->ord[ch][r/patLen],false)->data[r%patLen],DIV_MAX_COLS*sizeof(short));
    return a.data();
  };
  auto applyPending=[&](PREditCtx& c) -> bool {
    pr.pendRefused.clear();
    if (pr.pendOp) return pr.pendOp(c);
    return prApplyEdit(c,pr.pendRem,pr.pendAdd,endMarker);
  };
  bool dryChanged=applyPending(dry);
  if (pr.pendStrict && !pr.pendRefused.empty()) {
    prSetStatus(pr,fmt::sprintf(_("no free effect column for EDxx/ECxx on %d row(s): nothing was changed (add effect columns to this channel, or use Row snap)"),(int)pr.pendRefused.size()));
    pr.pendRem.clear(); pr.pendAdd.clear(); pr.pendSelect.clear(); pr.pendOp=nullptr; pr.pendStrict=false;
    return;
  }
  if (!dryChanged) {
    pr.pendRem.clear(); pr.pendAdd.clear(); pr.pendSelect.clear(); pr.pendOp=nullptr; pr.pendStrict=false;
    return;
  }
  std::set<int> touched;
  for (auto& i: overlay) {
    const short* orig=e->curPat[ch].getPattern(e->curOrders->ord[ch][i.first/patLen],false)->data[i.first%patLen];
    if (memcmp(orig,i.second.data(),DIV_MAX_COLS*sizeof(short))!=0) touched.insert(i.first/patLen);
  }

  // 2. repeated patterns
  PRChannelSource src=prSourceFor(e,ch);
  std::vector<int> repeated;
  int maxUse=0;
  for (int o: touched) {
    int u=prPatternUseCount(src,o);
    if (u>1) {
      repeated.push_back(o);
      if (u>maxUse) maxUse=u;
    }
  }
  if (!repeated.empty() && pr.repeatMode==PR_REPEAT_ASK) {
    pr.repeatPopup=true;
    pr.repeatPopupCount=maxUse;
    return; // pending edit is kept until the user answers
  }

  // free pattern indices for "make unique"
  std::vector<std::pair<int,int>> uniq; // order position, new pattern index
  if (!repeated.empty() && pr.repeatMode==PR_REPEAT_MAKE_UNIQUE) {
    bool used[DIV_MAX_PATTERNS];
    memset(used,0,sizeof(used));
    for (int i=0; i<ordersLen; i++) used[e->curOrders->ord[ch][i]]=true;
    for (int o: repeated) {
      int found=-1;
      // never-allocated patterns first, then allocated but empty and unused ones
      for (int pass=0; pass<2 && found<0; pass++) {
        for (int i=0; i<DIV_MAX_PATTERNS; i++) {
          if (used[i]) continue;
          DivPattern* p=e->curPat[ch].data[i];
          if (pass==0 && p!=NULL) continue;
          if (pass==1) {
            if (p==NULL) continue;
            bool empty=true;
            for (int r=0; r<DIV_MAX_ROWS && empty; r++) {
              if (p->data[r][0]!=0 || p->data[r][1]!=0) empty=false;
              for (int c=2; c<DIV_MAX_COLS && empty; c++) if (p->data[r][c]!=-1) empty=false;
            }
            if (!empty) continue;
          }
          found=i;
          break;
        }
      }
      if (found<0) {
        prSetStatus(pr,_("no free pattern left in this channel to make it unique; nothing was changed"));
        pr.pendRem.clear(); pr.pendAdd.clear(); pr.pendSelect.clear(); pr.pendOp=nullptr; pr.pendStrict=false;
        return;
      }
      used[found]=true;
      uniq.push_back(std::make_pair(o,found));
    }
  }

  // 3. apply for real, as one undo step
  UndoRegion region(0,ch,0,ordersLen-1,ch,patLen-1);
  prepareUndo(GUI_UNDO_PATTERN_EDIT,region);
  std::vector<UndoOrderData> ordChanges;
  size_t subSong=e->getCurrentSubSong();
  for (auto& u: uniq) {
    int oldPat=e->curOrders->ord[ch][u.first];
    e->curPat[ch].getPattern(oldPat,false)->copyOn(e->curPat[ch].getPattern(u.second,true));
    ordChanges.push_back(UndoOrderData(subSong,ch,u.first,oldPat,u.second));
    e->curOrders->ord[ch][u.first]=u.second;
  }
  PREditCtx real;
  real.total=total;
  real.effectCols=effectCols;
  real.get=[&](int r) -> const short* {
    return e->curPat[ch].getPattern(e->curOrders->ord[ch][r/patLen],false)->data[r%patLen];
  };
  real.getMut=[&](int r) -> short* {
    return e->curPat[ch].getPattern(e->curOrders->ord[ch][r/patLen],true)->data[r%patLen];
  };
  applyPending(real);
  size_t histBefore=undoHist.size();
  bool redoWasEmpty=redoHist.empty();
  makeUndo(GUI_UNDO_PATTERN_EDIT,region);
  (void)histBefore; (void)redoWasEmpty;
  if (!ordChanges.empty() && !undoHist.empty()) {
    for (auto& oc: ordChanges) undoHist.back().ord.push_back(oc);
  }
  if (!uniq.empty()) {
    prSetStatus(pr,fmt::sprintf(_("made %d pattern(s) unique"),(int)uniq.size()));
  }
  if (!pr.pendRefused.empty()) {
    prSetStatus(pr,fmt::sprintf(_("%d effect(s) did not fit (no free effect column) and stayed where they were; add effect columns to this channel in the pattern view"),(int)pr.pendRefused.size()));
  }

  pr.sel.clear();
  for (int s: pr.pendSelect) pr.sel.insert(s);
  pr.pendRem.clear(); pr.pendAdd.clear(); pr.pendSelect.clear(); pr.pendOp=nullptr; pr.pendStrict=false;
}

// E5: called by the instrument list when an instrument is clicked.
// returns true if the click was used on the roll's selected notes.
bool FurnaceGUI::pianoRollApplyInstrument(int ins) {
  if (!pianoRollOpen || pianoRoll==NULL || ins<0) return false;
  FurnacePianoRoll& pr=*pianoRoll;
  if (pr.sel.empty() || pr.drag!=PR_DRAG_NONE) return false;
  if (pr.chan>=e->getTotalChannelCount()) return false;
  std::vector<int> starts(pr.sel.begin(),pr.sel.end());
  pr.pendRem.clear(); pr.pendAdd.clear();
  pr.pendSelect=starts;
  pr.pendOp=[starts,ins](PREditCtx& c) -> bool { return prSetInstrument(c,starts,ins); };
  size_t n=starts.size();
  pianoRollCommit();
  if (!pr.repeatPopup) {
    prSetStatus(pr,fmt::sprintf(_("instrument %02X applied to %d note(s)"),ins,(int)n));
  }
  return true;
}

// ---------------- the window ----------------

// E9: remember the view options (called from commitState)
void FurnaceGUI::pianoRollSaveConf(DivConfig& conf) {
  if (pianoRoll==NULL) return;
  FurnacePianoRoll& pr=*pianoRoll;
  conf.set("pianoRollZoomX",pr.zoomX);
  conf.set("pianoRollZoomY",pr.zoomY);
  conf.set("pianoRollGhost",pr.ghost);
  conf.set("pianoRollFollow",pr.follow);
  conf.set("pianoRollLanes",pr.lanes);
  conf.set("pianoRollSnap",pr.snap);
  conf.set("pianoRollTool",pr.tool);
  conf.set("pianoRollChan",pr.chan);
}

// E7b: the "piano roll" toggle next to Poly in the play controls
void FurnaceGUI::drawPianoRollTransportToggle() {
  pushToggleColors(pianoRollTransport);
  if (ImGui::Button(_("piano roll##PRTransport"))) {
    pianoRollTransport=!pianoRollTransport;
    pianoRollHasPause=false;
  }
  if (ImGui::IsItemHovered()) {
    ImGui::SetTooltip("%s",_("piano roll transport\non: Space is play/stop; play starts at the piano roll's marker;\nwith Repeat pattern on, playback loops inside the I/O selection"));
  }
  popToggleColors();
}

// E7b: Space with the "piano roll" toggle on. stop; or play from the marker / loop,
// or (no marker) from where the last stop happened
void FurnaceGUI::pianoRollPlayPause() {
  if (e->isPlaying() && !e->isStepping()) {
    int o=0, r=0;
    e->getPlayPos(o,r);
    stop();
    pianoRollHasPause=true;
    pianoRollPauseOrd=o;
    pianoRollPauseRow=r;
    return;
  }
  if (pianoRollMarker()>=0) {
    play();
    return;
  }
  if (pianoRollHasPause && pianoRollPauseOrd<e->curSubSong->ordersLen) {
    curOrder=pianoRollPauseOrd;
    e->setOrder(pianoRollPauseOrd);
    play(pianoRollPauseRow);
  } else {
    play();
  }
}

// E7b: Play / Play-Stop start from here when the "piano roll" toggle is on (see FurnaceGUI::play)
int FurnaceGUI::pianoRollMarker() {
  if (!pianoRollTransport || pianoRoll==NULL) return -1;
  int total=e->curSubSong->patLen*e->curSubSong->ordersLen;
  FurnacePianoRoll& pr=*pianoRoll;
  bool loop=(pr.loopIn>=0 && pr.loopOut>pr.loopIn && pr.loopIn<total);
  // with a loop, play starts inside it: at the marker if the marker is in the loop, else at its start
  if (loop && (pr.markerRow<pr.loopIn || pr.markerRow>=pr.loopOut)) return pr.loopIn;
  if (pr.markerRow<0 || pr.markerRow>=total) return -1;
  return pr.markerRow;
}

void FurnaceGUI::drawPianoRoll() {
  if (!pianoRollOpen) {
    // the loop belongs to the roll: closing the window turns it off
    if (pianoRoll!=NULL && pianoRoll->loopApplied) {
      e->setRollLoop(false,0,0);
      pianoRoll->loopApplied=false;
    }
    return;
  }
  if (pianoRoll==NULL) {
    pianoRoll=new FurnacePianoRoll;
    // E9: view options remembered between sessions (Furnace config, not the song)
    FurnacePianoRoll& p0=*pianoRoll;
    p0.zoomX=CLAMP(e->getConfFloat("pianoRollZoomX",p0.zoomX),2.0f,64.0f);
    p0.zoomY=CLAMP(e->getConfFloat("pianoRollZoomY",p0.zoomY),4.0f,40.0f);
    p0.ghost=e->getConfBool("pianoRollGhost",p0.ghost);
    p0.follow=e->getConfBool("pianoRollFollow",p0.follow);
    p0.lanes=e->getConfBool("pianoRollLanes",p0.lanes);
    p0.snap=CLAMP(e->getConfInt("pianoRollSnap",p0.snap),0,2);
    p0.tool=CLAMP(e->getConfInt("pianoRollTool",p0.tool),0,2);
    int ch=e->getConfInt("pianoRollChan",-1);
    if (ch>=0) {
      p0.chan=ch;
      p0.firstFrame=false;
    }
  }
  FurnacePianoRoll& pr=*pianoRoll;
  if (!pr.prefsLoaded) {
    pr.prefsLoaded=true;
    pr.prefsSong=curFileName;
    prPrefsLoad(pr.prefs,curFileName);
  }
  // new song (file name cleared): forget the previous song's preferences
  if (curFileName.empty() && !pr.prefsSong.empty()) {
    pr.prefs=PRPrefs();
    pr.prefsSong="";
    pr.sel.clear();
  }

  if (nextWindow==GUI_WINDOW_PIANO_ROLL) {
    ImGui::SetNextWindowFocus();
    nextWindow=GUI_WINDOW_NOTHING;
  }
  ImGui::SetNextWindowSize(ImVec2(900.0f*dpiScale,560.0f*dpiScale),ImGuiCond_FirstUseEver);
  if (!ImGui::Begin("Piano Roll",&pianoRollOpen,globalWinFlags,_("Piano Roll"))) {
    ImGui::End();
    return;
  }
  bool winFocused=ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows);
  if (winFocused) curWindow=GUI_WINDOW_PIANO_ROLL;

  DivSubSong* sub=e->curSubSong;
  int chanCount=e->getTotalChannelCount();
  if (pr.firstFrame) {
    pr.chan=cursor.xCoarse;
    pr.firstFrame=false;
  }
  if (pr.chan<0 || pr.chan>=chanCount) pr.chan=0;
  if (chanCount<1 || sub==NULL) {
    ImGui::End();
    return;
  }
  // E7: tracker cursor -> roll. a cursor move made in the tracker (or anywhere else in Furnace)
  // brings the roll to that channel and row. moves made by the roll itself update seen* first.
  {
    int co=curOrder, cr=cursor.y, cc=cursor.xCoarse;
    if (co!=pr.seenOrd || cr!=pr.seenRow || cc!=pr.seenChan) {
      bool first=(pr.seenOrd<0);
      pr.seenOrd=co;
      pr.seenRow=cr;
      pr.seenChan=cc;
      if (!first) {
        if (cc!=pr.chan && cc>=0 && cc<chanCount) {
          pr.chan=cc;
          pr.sel.clear();
          pr.focusRow=-1;
          pr.centerPending=true;
        }
        if (!e->isPlaying()) pr.revealRow=co*sub->patLen+cr;
      }
    }
  }
  PRChanPrefs& cp=pr.prefs.get(pr.chan);

  // ---------------- toolbar ----------------
  ImGui::AlignTextToFramePadding();
  ImGui::TextUnformatted(_("Channel"));
  ImGui::SameLine();
  ImGui::SetNextItemWidth(150.0f*dpiScale);
  if (ImGui::BeginCombo("##PRChan",e->getChannelName(pr.chan))) {
    for (int i=0; i<chanCount; i++) {
      ImGui::PushID(i);
      if (ImGui::Selectable(e->getChannelName(i),i==pr.chan)) {
        if (i!=pr.chan) pr.sel.clear();
        pr.chan=i;
        pr.centerPending=true;
      }
      ImGui::PopID();
    }
    ImGui::EndCombo();
  }
  ImGui::SameLine();
  ImGui::Dummy(ImVec2(6.0f*dpiScale,1));
  ImGui::SameLine();
  {
    const char* toolNames[3]={_("Select"),_("Draw"),_("Erase")};
    const char* toolKeys[3]={"S","D","E"};
    for (int t=0; t<3; t++) {
      bool on=(pr.tool==t);
      if (on) ImGui::PushStyleColor(ImGuiCol_Button,ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
      if (ImGui::Button(fmt::sprintf("%s##PRTool%d",toolNames[t],t).c_str())) pr.tool=t;
      if (on) ImGui::PopStyleColor();
      if (ImGui::IsItemHovered()) ImGui::SetTooltip(_("tool (%s)"),toolKeys[t]);
      ImGui::SameLine();
    }
  }
  {
    const char* snapNames[3]={_("Snap: row"),_("Snap: 1/2"),_("Snap: tick")};
    ImGui::SetNextItemWidth(ImGui::CalcTextSize(snapNames[2]).x+ImGui::GetFrameHeight()+10.0f*dpiScale);
    if (ImGui::BeginCombo("##PRSnap",snapNames[CLAMP(pr.snap,0,2)])) {
      for (int i=0; i<3; i++) if (ImGui::Selectable(snapNames[i],pr.snap==i)) pr.snap=i;
      ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s",_("where note starts and ends can fall:\nrow: on rows (no effects written)\n1/2 and tick: inside a row, written as EDxx (start delay) and ECxx (cut)"));
  }
  ImGui::SameLine();
  ImGui::Dummy(ImVec2(6.0f*dpiScale,1));
  ImGui::SameLine();
  if (ImGui::Button(cp.endRel?_("End: REL##PREnd"):_("End: OFF##PREnd"))) {
    cp.endRel=!cp.endRel;
    prSavePrefs(pr,curFileName);
  }
  if (ImGui::IsItemHovered()) ImGui::SetTooltip(_("how notes drawn in this channel end: OFF (note off) or REL (note release)"));
  ImGui::SameLine();
  if (ImGui::Checkbox(_("Fold"),&cp.fold)) {
    pr.centerPending=true;
    prSavePrefs(pr,curFileName);
  }
  if (ImGui::IsItemHovered()) ImGui::SetTooltip(_("show only the pitches used in this channel (saved per channel)"));
  ImGui::SameLine();
  ImGui::Checkbox(_("Ghost notes"),&pr.ghost);
  if (ImGui::IsItemHovered()) ImGui::SetTooltip(_("show the other channels of the same chip in gray"));
  ImGui::SameLine();
  ImGui::Checkbox(_("Follow playback"),&pr.follow);

  // second toolbar row: zoom, repeated patterns, status
  ImGui::AlignTextToFramePadding();
  ImGui::TextUnformatted(_("Zoom"));
  ImGui::SameLine();
  if (ImGui::SmallButton("-##PRZX")) pr.zoomX=MAX(2.0f,pr.zoomX/1.25f);
  ImGui::SameLine();
  if (ImGui::SmallButton("+##PRZX")) pr.zoomX=MIN(64.0f,pr.zoomX*1.25f);
  if (ImGui::IsItemHovered()) ImGui::SetTooltip(_("horizontal zoom (Ctrl+wheel)"));
  ImGui::SameLine();
  if (ImGui::SmallButton("-##PRZY")) pr.zoomY=MAX(4.0f,pr.zoomY/1.25f);
  ImGui::SameLine();
  if (ImGui::SmallButton("+##PRZY")) pr.zoomY=MIN(40.0f,pr.zoomY*1.25f);
  if (ImGui::IsItemHovered()) ImGui::SetTooltip(_("vertical zoom (Ctrl+Shift+wheel)"));
  ImGui::SameLine();
  {
    const char* modes[3]={_("Repeated: ask"),_("Repeated: edit all"),_("Repeated: make unique")};
    ImGui::SetNextItemWidth(ImGui::CalcTextSize(modes[2]).x+ImGui::GetFrameHeight()+12.0f*dpiScale);
    if (ImGui::BeginCombo("##PRRepeat",modes[pr.repeatMode])) {
      for (int i=0; i<3; i++) if (ImGui::Selectable(modes[i],pr.repeatMode==i)) pr.repeatMode=i;
      ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip(_("what to do when an edit falls in a pattern used in more than one order position"));
  }

  ImGui::SameLine();
  ImGui::Checkbox(_("Lanes"),&pr.lanes);
  if (ImGui::IsItemHovered()) ImGui::SetTooltip(_("show the volume and effect lanes under the roll"));

  // E7b: transport state and its clear button
  ImGui::SameLine();
  {
    bool any=(pr.markerRow>=0 || pr.loopIn>=0 || pr.loopOut>=0);
    int pl=MAX(1,(int)sub->patLen);
    if (!any) ImGui::BeginDisabled();
    if (ImGui::Button(_("Clear transport"))) {
      pr.markerRow=-1;
      pr.loopIn=-1;
      pr.loopOut=-1;
    }
    if (!any) ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s",_("remove the start marker and the loop\n(click the ruler above the roll to set the marker; I and O set the loop)"));
    if (pr.markerRow>=0) {
      ImGui::SameLine();
      ImGui::Text(_("start %.2X:%.2X"),pr.markerRow/pl,pr.markerRow%pl);
    }
    if (pr.loopIn>=0) {
      ImGui::SameLine();
      if (pr.loopOut>pr.loopIn) {
        ImGui::Text(_("loop %.2X:%.2X - %.2X:%.2X"),pr.loopIn/pl,pr.loopIn%pl,pr.loopOut/pl,pr.loopOut%pl);
      } else {
        ImGui::Text(_("loop %.2X:%.2X - (press O)"),pr.loopIn/pl,pr.loopIn%pl);
      }
    }
    if (any) {
      ImGui::SameLine();
      if (!pianoRollTransport) {
        ImGui::TextColored(uiColors[GUI_COLOR_WARNING],"%s",_("(off: turn on \"piano roll\" next to Poly)"));
      } else if (pr.loopIn>=0 && pr.loopOut>pr.loopIn && !e->getRepeatPattern()) {
        ImGui::TextDisabled("%s",_("(the loop plays with Repeat pattern on)"));
      }
    }
  }

  // status message (only while there is something to say)
  ImGui::SameLine();
  bool showStatus=(ImGui::GetTime()-pr.statusTime)<6.0;
  if (showStatus) {
    ImGui::PushStyleColor(ImGuiCol_Text,uiColors[GUI_COLOR_WARNING]);
    ImGui::TextUnformatted(pr.status.c_str());
    ImGui::PopStyleColor();
  } else {
    ImGui::TextDisabled("%s",_("S/D/E: tools, Shift+drag: duplicate, double-click: add effect, Del: delete, Ctrl+A: select all, I/O: loop"));
  }

  // ---------------- data ----------------
  int patLen=sub->patLen;
  int ordersLen=sub->ordersLen;
  int totalRows=patLen*ordersLen;
  PRChannelSource src=prSourceFor(e,pr.chan);
  std::vector<PRNote> notes=prReadChannel(src);

  // E7b: keep the transport inside the song and hand the loop to the engine
  if (pr.markerRow>=totalRows) pr.markerRow=-1;
  if (pr.loopIn>=totalRows) pr.loopIn=-1;
  if (pr.loopOut>totalRows) pr.loopOut=totalRows;
  {
    bool want=pianoRollTransport && (pr.loopIn>=0 && pr.loopOut>pr.loopIn);
    if (want!=pr.loopApplied || (want && (pr.loopIn!=pr.loopAppliedIn || pr.loopOut!=pr.loopAppliedOut))) {
      e->setRollLoop(want,pr.loopIn,pr.loopOut);
      pr.loopApplied=want;
      pr.loopAppliedIn=pr.loopIn;
      pr.loopAppliedOut=pr.loopOut;
    }
  }

  // E7: roll -> tracker. selecting a note puts the tracker cursor on its first row and selects
  // its rows in the pattern (within one order). not while playing: moving the order would jump.
  auto syncTracker=[&](int startRow, int endRow) {
    if (e->isPlaying() || startRow<0 || startRow>=totalRows) return;
    int o=startRow/patLen, r=startRow%patLen;
    setOrder((unsigned char)o);
    cursor.xCoarse=pr.chan;
    cursor.xFine=0;
    cursor.y=r;
    selStart=cursor;
    selEnd=cursor;
    int last=endRow-1;
    if (last>startRow && last/patLen==o) selEnd.y=last%patLen;
    updateScroll(r);
    demandScrollX=true;
    pr.seenOrd=o;
    pr.seenRow=r;
    pr.seenChan=pr.chan;
  };
  int trackerRow=(cursor.xCoarse==pr.chan)?(curOrder*patLen+cursor.y):-1;

  // drop selected starts that no longer exist (undo, tracker edits)
  for (auto it=pr.sel.begin(); it!=pr.sel.end();) {
    bool ok=false;
    for (PRNote& n: notes) if (n.start==*it) { ok=true; break; }
    if (ok) ++it; else it=pr.sel.erase(it);
  }

  // diagnostic dump for automated checks (env FURNACE_PIANO_ROLL_DUMP=<file>):
  // written on the first frame and again whenever the song data changes (edit, undo, redo)
  {
    static size_t lastHist=(size_t)-1, lastRedo=(size_t)-1;
    const char* dumpPath=getenv("FURNACE_PIANO_ROLL_DUMP");
    if (dumpPath!=NULL && dumpPath[0] && (undoHist.size()!=lastHist || redoHist.size()!=lastRedo)) {
      lastHist=undoHist.size();
      lastRedo=redoHist.size();
      FILE* f=fopen(dumpPath,"w");
      if (f!=NULL) {
        for (int i=0; i<chanCount; i++) {
          std::vector<PRNote> cn=prReadChannel(prSourceFor(e,i));
          for (PRNote& n: cn) {
            fprintf(f,"%d %d %d %d %d %d %d %d %d %d %d\n",i,n.start,n.end,n.pitch,n.ins,n.insExplicit?1:0,(int)n.endType,n.macroRelRow,n.delayTicks,n.cutRow,n.cutTicks);
          }
        }
        fclose(f);
      }
    }
  }

  // ghost channels: same chip (dispatch) as the current channel
  std::vector<std::vector<PRNote>> ghosts;
  if (pr.ghost) {
    for (int i=0; i<chanCount; i++) {
      if (i==pr.chan) continue;
      if (e->dispatchOfChan[i]!=e->dispatchOfChan[pr.chan]) continue;
      ghosts.push_back(prReadChannel(prSourceFor(e,i)));
    }
  }

  // vertical axis: list of pitches from top to bottom (negative = empty fold row, -1-pitch)
  std::vector<int> axis;
  if (cp.fold) {
    bool used[180];
    memset(used,0,sizeof(used));
    for (PRNote& n: notes) used[n.pitch]=true;
    int lo=-1, hi=-1;
    for (int p=179; p>=0; p--) if (used[p]) { if (hi<0) hi=p; lo=p; }
    if (hi<0) {
      for (int p=72+12*4; p>=60+12*3; p--) axis.push_back(p);
    } else {
      if (hi<179) axis.push_back(-1-(hi+1));
      for (int p=179; p>=0; p--) if (used[p]) axis.push_back(p);
      if (lo>0) axis.push_back(-1-(lo-1));
    }
  } else {
    for (int p=179; p>=0; p--) axis.push_back(p);
  }
  int axisIndex[180];
  for (int i=0; i<180; i++) axisIndex[i]=-1;
  for (size_t i=0; i<axis.size(); i++) {
    int p=axis[i];
    if (p<0) p=-1-p;
    if (p>=0 && p<180) axisIndex[p]=(int)i;
  }
  auto axisPitch=[&](int a) -> int {
    if (a<0) a=0;
    if (a>=(int)axis.size()) a=(int)axis.size()-1;
    int p=axis[a];
    return (p<0)?(-1-p):p;
  };

  float zX=pr.zoomX*dpiScale;
  float zY=pr.zoomY*dpiScale;
  float kbW=56.0f*dpiScale;
  float arrH=ImGui::GetFontSize()+6.0f*dpiScale;
  float contentW=totalRows*zX;
  float contentH=axis.size()*zY;

  // playback position
  bool playing=e->isPlaying();
  float playX=-1;
  if (playing) {
    int pOrd=0, pRow=0, pTick=0, pSpeed=1;
    e->getPlayPosTick(pOrd,pRow,pTick,pSpeed);
    float rowTicks=(float)MAX(1,pSpeed*(sub->timeBase+1));
    float frac=1.0f-((float)pTick/rowTicks);
    if (frac<0) frac=0;
    if (frac>1) frac=1;
    playX=((float)(pOrd*patLen+pRow)+frac)*zX;
  }

  // keyboard shortcuts (only while this window has focus and no text field is active)
  if (winFocused && !ImGui::GetIO().WantTextInput && pr.drag==PR_DRAG_NONE) {
    ImGuiIO& io=ImGui::GetIO();
    if (!io.KeyCtrl && !io.KeyAlt) {
      if (ImGui::IsKeyPressed(ImGuiKey_S,false)) pr.tool=PR_TOOL_SELECT;
      if (ImGui::IsKeyPressed(ImGuiKey_D,false)) pr.tool=PR_TOOL_DRAW;
      if (ImGui::IsKeyPressed(ImGuiKey_E,false)) pr.tool=PR_TOOL_ERASE;
      if (ImGui::IsKeyPressed(ImGuiKey_Escape,false)) pr.sel.clear();
      // E7b: loop in / out at the marker
      if (ImGui::IsKeyPressed(ImGuiKey_I,false) && pr.markerRow>=0) {
        pr.loopIn=pr.markerRow;
        if (pr.loopOut>=0 && pr.loopOut<=pr.loopIn) pr.loopOut=-1;
      }
      if (ImGui::IsKeyPressed(ImGuiKey_O,false) && pr.markerRow>=0) {
        if (pr.loopIn<0) {
          if (pr.markerRow>0) { pr.loopIn=0; pr.loopOut=pr.markerRow; }
        } else if (pr.markerRow>pr.loopIn) {
          pr.loopOut=pr.markerRow;
        } else if (pr.markerRow<pr.loopIn) {
          pr.loopOut=pr.loopIn;
          pr.loopIn=pr.markerRow;
        }
      }
      if ((ImGui::IsKeyPressed(ImGuiKey_Delete,false) || ImGui::IsKeyPressed(ImGuiKey_Backspace,false)) && !pr.sel.empty()) {
        pr.pendRem.assign(pr.sel.begin(),pr.sel.end());
        pr.pendOp=nullptr;
        pr.pendAdd.clear();
        pr.pendSelect.clear();
        pianoRollCommit();
      }
    }
    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_A,false)) {
      pr.sel.clear();
      for (PRNote& n: notes) pr.sel.insert(n.start);
    }
  }

  // ---------------- note panel (4.1 item 2, 3.7) ----------------
  // shows the selected notes, or the row picked in the effect lane when no note is selected.
  // every edit here is one undo step and goes through pianoRollCommit (repeated patterns included).
  std::vector<const PRNote*> selNotes;
  for (const PRNote& n: notes) if (pr.sel.count(n.start)) selNotes.push_back(&n);
  if (!selNotes.empty()) pr.focusRow=-1;
  if (pr.focusRow>=totalRows) pr.focusRow=-1;
  std::vector<int> tRows;
  if (!selNotes.empty()) {
    for (const PRNote* n: selNotes) tRows.push_back(n->start);
  } else if (pr.focusRow>=0) {
    tRows.push_back(pr.focusRow);
  }
  auto rowPtr=[&](int r) -> const short* {
    return src.rowAt(src.patAt(r/patLen),r%patLen);
  };
  FurnacePianoRoll* prp=&pr;
  int effectCols=MIN(DIV_MAX_EFFECTS,MAX(1,(int)sub->pat[pr.chan].effectCols));
  auto queueOp=[&](std::function<bool(PREditCtx&)> op) {
    pr.pendRem.clear();
    pr.pendAdd.clear();
    pr.pendSelect.assign(pr.sel.begin(),pr.sel.end());
    pr.pendOp=op;
    pianoRollCommit();
  };
  int endMk=cp.endRel?PR_PITCH_REL:PR_PITCH_OFF;
  // rem[i] becomes add[i]: the note keeps its effects (prMoveNotes)
  auto queueNotes=[&](std::vector<int> rem, std::vector<PRNoteSpec> add, std::vector<int> selAfter) {
    prClipAdds(notes,rem,add);
    std::vector<PRMove> moves;
    for (size_t i=0; i<rem.size() && i<add.size(); i++) moves.push_back(PRMove(rem[i],add[i],false));
    pr.pendRem.clear();
    pr.pendAdd.clear();
    pr.pendSelect=selAfter;
    pr.pendOp=[moves,endMk,prp](PREditCtx& c) -> bool { return prMoveNotes(c,moves,endMk,&prp->pendRefused); };
    pianoRollCommit();
  };
  auto fxDesc=[&](int fx) -> const char* {
    const char* d=e->getEffectDesc((unsigned char)fx,pr.chan);
    if (d==NULL) return _("(not valid on this chip)");
    if (strlen(d)>6) return d+6;
    return d;
  };

  ImGui::Separator();
  float panelY0=ImGui::GetCursorPosY();
  float fieldW2=ImGui::CalcTextSize("00:00").x+ImGui::GetStyle().FramePadding.x*2.0f+4.0f*dpiScale;
  float fieldHex=ImGui::CalcTextSize("000").x+ImGui::GetStyle().FramePadding.x*2.0f;
  ImGuiInputTextFlags enterFlags=ImGuiInputTextFlags_EnterReturnsTrue|ImGuiInputTextFlags_AutoSelectAll;

  // line 1: note fields
  ImGui::AlignTextToFramePadding();
  if (!selNotes.empty()) {
    const PRNote& f=*selNotes[0];
    bool one=(selNotes.size()==1);
    if (one) {
      ImGui::TextUnformatted(_("Note"));
    } else {
      ImGui::Text(_("%d notes"),(int)selNotes.size());
    }

    // start
    ImGui::SameLine();
    ImGui::TextUnformatted(_("Start"));
    ImGui::SameLine();
    if (one) {
      char buf[16];
      snprintf(buf,15,"%.2X:%.2X",f.start/patLen,f.start%patLen);
      ImGui::SetNextItemWidth(fieldW2);
      if (ImGui::InputText("##PRStart",buf,15,enterFlags)) {
        int ns=-1;
        if (prParseOrdRow(buf,patLen,totalRows,ns) && ns!=f.start) {
          int len=f.end-f.start;
          queueNotes({f.start},{PRNoteSpec(ns,MIN(totalRows,ns+len),f.pitch,f.ins)},{ns});
        } else if (ns<0) {
          prSetStatus(pr,_("type the position as order:row in hex, e.g. 01:0C"));
        }
      }
      if (ImGui::IsItemHovered()) ImGui::SetTooltip(_("order:row (hex), Enter to move the note"));
    } else {
      ImGui::TextDisabled("...");
    }

    // end
    ImGui::SameLine();
    ImGui::TextUnformatted(_("End"));
    ImGui::SameLine();
    if (one) {
      char buf[16];
      snprintf(buf,15,"%.2X:%.2X",f.end/patLen,f.end%patLen);
      if (f.end>=totalRows) snprintf(buf,15,"--:--");
      ImGui::SetNextItemWidth(fieldW2);
      if (ImGui::InputText("##PREnd",buf,15,enterFlags)) {
        int ne=-1;
        unsigned int o=0, r=0;
        // the end may be the row right after the last one (end of song)
        if (sscanf(buf,"%x:%x",&o,&r)==2 && (int)r<patLen) ne=(int)o*patLen+(int)r;
        if (ne==totalRows || (ne>f.start && ne<totalRows)) {
          if (ne!=f.end) queueNotes({f.start},{PRNoteSpec(f.start,ne,f.pitch,f.ins)},{f.start});
        } else {
          prSetStatus(pr,_("the end must come after the start (order:row in hex)"));
        }
      }
      if (ImGui::IsItemHovered()) ImGui::SetTooltip(_("first row after the note (order:row, hex), Enter to apply"));
    } else {
      ImGui::TextDisabled("...");
    }

    // length (rows, decimal), applies to all
    ImGui::SameLine();
    ImGui::TextUnformatted(_("Length"));
    ImGui::SameLine();
    {
      bool same=true;
      for (const PRNote* n: selNotes) if (n->end-n->start!=f.end-f.start) same=false;
      char buf[16];
      if (same) snprintf(buf,15,"%d",f.end-f.start); else snprintf(buf,15,"...");
      ImGui::SetNextItemWidth(ImGui::CalcTextSize("00000").x+ImGui::GetStyle().FramePadding.x*2.0f);
      if (ImGui::InputText("##PRLen",buf,15,enterFlags|ImGuiInputTextFlags_CharsDecimal)) {
        int nl=atoi(buf);
        if (nl>=1) {
          std::vector<int> rem;
          std::vector<PRNoteSpec> add;
          std::vector<int> after;
          for (const PRNote* n: selNotes) {
            rem.push_back(n->start);
            add.push_back(PRNoteSpec(n->start,MIN(totalRows,n->start+nl),n->pitch,n->ins));
            after.push_back(n->start);
          }
          queueNotes(rem,add,after);
          pr.lastLen=nl;
        }
      }
      if (ImGui::IsItemHovered()) ImGui::SetTooltip(_("length in rows; Enter applies it to every selected note\n(a note never runs over the next note that is not selected)"));
    }

    // pitch: name + transpose buttons, applies to all
    ImGui::SameLine();
    ImGui::TextUnformatted(_("Pitch"));
    ImGui::SameLine();
    {
      bool same=true;
      for (const PRNote* n: selNotes) if (n->pitch!=f.pitch) same=false;
      if (same) {
        short nn,oo;
        prEncodePitch(f.pitch,nn,oo);
        ImGui::TextUnformatted(noteName(nn,oo));
      } else {
        ImGui::TextUnformatted("...");
      }
      const int steps[4]={-12,-1,1,12};
      const char* labels[4]={"-12##PRTr0","-1##PRTr1","+1##PRTr2","+12##PRTr3"};
      for (int i=0; i<4; i++) {
        ImGui::SameLine();
        if (ImGui::SmallButton(labels[i])) {
          int d=steps[i];
          bool ok=true;
          for (const PRNote* n: selNotes) if (n->pitch+d<0 || n->pitch+d>179) ok=false;
          if (ok) {
            std::vector<int> rem;
            std::vector<PRNoteSpec> add;
            std::vector<int> after;
            for (const PRNote* n: selNotes) {
              rem.push_back(n->start);
              add.push_back(PRNoteSpec(n->start,n->end,n->pitch+d,n->ins));
              after.push_back(n->start);
            }
            queueNotes(rem,add,after);
          } else {
            prSetStatus(pr,_("a selected note would leave the pitch range; nothing was changed"));
          }
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip(_("transpose the selected notes"));
      }
    }

    // instrument (E5 operation), applies to all
    ImGui::SameLine();
    ImGui::TextUnformatted(_("Instrument"));
    ImGui::SameLine();
    {
      bool same=true;
      for (const PRNote* n: selNotes) if (n->ins!=f.ins) same=false;
      String prev="...";
      if (same) {
        if (f.ins>=0 && f.ins<(int)e->song.ins.size()) {
          prev=fmt::sprintf("%.2X: %s",f.ins,e->song.ins[f.ins]->name);
        } else if (f.ins>=0) {
          prev=fmt::sprintf("%.2X: %s",f.ins,_("(missing)"));
        } else {
          prev=_("none");
        }
        if (one && !f.insExplicit) prev+=_(" (inherited)");
      }
      ImGui::SetNextItemWidth(220.0f*dpiScale);
      if (ImGui::BeginCombo("##PRIns",prev.c_str(),ImGuiComboFlags_HeightLarge)) {
        for (int i=0; i<(int)e->song.ins.size(); i++) {
          ImGui::PushID(i);
          bool cur=same && f.ins==i;
          if (ImGui::Selectable(fmt::sprintf("%.2X: %s",i,e->song.ins[i]->name).c_str(),cur)) {
            std::vector<int> starts=tRows;
            int ins=i;
            queueOp([starts,ins](PREditCtx& c) -> bool { return prSetInstrument(c,starts,ins); });
          }
          ImGui::PopID();
        }
        ImGui::EndCombo();
      }
    }
  } else if (pr.focusRow>=0) {
    ImGui::Text(_("Row %.2X:%.2X (no note starts here)"),pr.focusRow/patLen,pr.focusRow%patLen);
    short v=rowPtr(pr.focusRow)[PR_COL_VOL];
    if (v>=0) {
      ImGui::SameLine();
      ImGui::TextDisabled(_("volume %.2X"),v);
    }
  } else {
    ImGui::TextDisabled("%s",_("Select a note to see and edit it here, or click a row in the effect lane below."));
  }

  // line 2: effects of the rows (start row of each selected note, or the picked row)
  ImGui::AlignTextToFramePadding();
  ImGui::TextUnformatted(_("Effects"));
  if (!tRows.empty()) {
    struct FxSum {
      int count, val;
      bool same;
      FxSum(): count(0), val(-1), same(true) {}
    };
    std::map<int,FxSum> fxs;
    for (int r: tRows) {
      std::set<int> seen;
      for (const PRFx& x: prRowEffects(rowPtr(r),effectCols)) {
        if (x.fx<0 || seen.count(x.fx)) continue;
        seen.insert(x.fx);
        FxSum& fsum=fxs[x.fx];
        if (fsum.count==0) {
          fsum.val=x.val;
        } else if (fsum.val!=x.val) {
          fsum.same=false;
        }
        fsum.count++;
      }
    }
    for (auto& it: fxs) {
      int fx=it.first;
      FxSum& fsum=it.second;
      ImGui::PushID(fx+1000);
      ImGui::SameLine();
      ImGui::PushStyleColor(ImGuiCol_Text,uiColors[fxColors[fx]]);
      ImGui::Text("%.2X",fx);
      ImGui::PopStyleColor();
      if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%.2X: %s\n%s",fx,fxDesc(fx),(fsum.count<(int)tRows.size())?fmt::sprintf(_("present on %d of %d rows"),fsum.count,(int)tRows.size()).c_str():"");
      }
      ImGui::SameLine(0,2.0f*dpiScale);
      char vbuf[16];
      if (fsum.same && fsum.val>=0) snprintf(vbuf,15,"%.2X",fsum.val);
      else if (fsum.same) snprintf(vbuf,7,"..");
      else snprintf(vbuf,7,"..");
      ImGui::SetNextItemWidth(fieldHex);
      if (ImGui::InputText("##PRFxV",vbuf,7,enterFlags|ImGuiInputTextFlags_CharsHexadecimal|ImGuiInputTextFlags_CharsUppercase)) {
        unsigned int v=0;
        if (sscanf(vbuf,"%x",&v)==1 && v<=255) {
          std::vector<int> rows=tRows;
          int ival=(int)v;
          queueOp([rows,fx,ival](PREditCtx& c) -> bool { return prSetEffectValueWherePresent(c,rows,fx,ival); });
        }
      }
      if (ImGui::IsItemHovered()) ImGui::SetTooltip(_("value (hex); Enter changes it on every row that has this effect"));
      if (fsum.count<(int)tRows.size()) {
        ImGui::SameLine(0,2.0f*dpiScale);
        if (ImGui::SmallButton(fmt::sprintf("+%d",(int)tRows.size()-fsum.count).c_str())) {
          std::vector<int> missing;
          for (int r: tRows) {
            bool has=false;
            for (const PRFx& x: prRowEffects(rowPtr(r),effectCols)) if (x.fx==fx) has=true;
            if (!has) missing.push_back(r);
          }
          int ival=(fsum.val>=0)?fsum.val:0;
          queueOp([missing,fx,ival,prp](PREditCtx& c) -> bool { return prSetEffect(c,missing,fx,ival,&prp->pendRefused); });
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip(_("add this effect to the rows that don't have it"));
      }
      ImGui::SameLine(0,2.0f*dpiScale);
      if (ImGui::SmallButton("x")) {
        std::vector<int> rows=tRows;
        queueOp([rows,fx](PREditCtx& c) -> bool { return prRemoveEffect(c,rows,fx); });
      }
      if (ImGui::IsItemHovered()) ImGui::SetTooltip(_("remove this effect from these rows"));
      ImGui::PopID();
    }
    if (fxs.empty()) {
      ImGui::SameLine();
      ImGui::TextDisabled("%s",_("none"));
    }

    // add an effect to all the rows
    ImGui::SameLine();
    ImGui::Dummy(ImVec2(8.0f*dpiScale,1));
    ImGui::SameLine();
    ImGui::TextUnformatted(_("Add"));
    ImGui::SameLine();
    if (pr.focusFxField && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
      ImGui::SetKeyboardFocusHere();
      pr.focusFxField=false;
    }
    ImGui::SetNextItemWidth(fieldHex);
    bool addEnter=ImGui::InputText("##PRAddFx",pr.addFxCode,3,enterFlags|ImGuiInputTextFlags_CharsHexadecimal|ImGuiInputTextFlags_CharsUppercase);
    if (ImGui::IsItemHovered()) {
      unsigned int c=0;
      if (sscanf(pr.addFxCode,"%x",&c)==1 && c<=255) {
        ImGui::SetTooltip("%.2X: %s",c,fxDesc((int)c));
      } else {
        ImGui::SetTooltip("%s",_("effect (hex)"));
      }
    }
    ImGui::SameLine(0,1.0f*dpiScale);
    if (ImGui::ArrowButton("##PRAddFxList",ImGuiDir_Down)) ImGui::OpenPopup("PRAddFxPopup");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s",_("list of effects for this chip"));
    ImGui::SetNextWindowSizeConstraints(ImVec2(360.0f*dpiScale,0),ImVec2(720.0f*dpiScale,420.0f*dpiScale));
    if (ImGui::BeginPopup("PRAddFxPopup")) {
      for (int i=0; i<256; i++) {
        const char* d=e->getEffectDesc((unsigned char)i,pr.chan);
        if (d==NULL) continue;
        ImGui::PushID(i);
        ImGui::PushStyleColor(ImGuiCol_Text,uiColors[fxColors[i]]);
        if (ImGui::Selectable(fmt::sprintf("%.2X  %s",i,(strlen(d)>6)?(d+6):d).c_str())) {
          snprintf(pr.addFxCode,3,"%.2X",i);
          pr.focusFxField=false;
        }
        ImGui::PopStyleColor();
        ImGui::PopID();
      }
      ImGui::EndPopup();
    }
    ImGui::SameLine();
    if (pr.focusValField) {
      ImGui::SetKeyboardFocusHere();
      pr.focusValField=false;
    }
    ImGui::SetNextItemWidth(fieldHex);
    bool valEnter=ImGui::InputText("##PRAddVal",pr.addFxVal,3,enterFlags|ImGuiInputTextFlags_CharsHexadecimal|ImGuiInputTextFlags_CharsUppercase);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s",_("value (hex)"));
    ImGui::SameLine();
    bool apply=ImGui::SmallButton(_("Apply##PRAddApply"));
    if (ImGui::IsItemHovered()) ImGui::SetTooltip(_("write this effect on the %d row(s) above\n(same effect already there: only its value changes; no free column: the row is skipped)"),(int)tRows.size());
    if (apply || valEnter || addEnter) {
      unsigned int c=0, v=0;
      bool okC=(sscanf(pr.addFxCode,"%x",&c)==1 && c<=255);
      bool okV=(sscanf(pr.addFxVal,"%x",&v)==1 && v<=255);
      if (addEnter && !apply && !valEnter) {
        // Enter on the effect field: move on to the value
        pr.focusValField=true;
      } else if (okC && okV) {
        std::vector<int> rows=tRows;
        int fx=(int)c, ival=(int)v;
        queueOp([rows,fx,ival,prp](PREditCtx& cc) -> bool { return prSetEffect(cc,rows,fx,ival,&prp->pendRefused); });
      } else {
        prSetStatus(pr,_("type the effect and the value in hex, e.g. 08 and F0"));
      }
    }
  } else {
    ImGui::SameLine();
    ImGui::TextDisabled("-");
  }
  // keep the panel height stable whatever it shows
  {
    float want=panelY0+ImGui::GetFrameHeightWithSpacing()*2.0f;
    if (ImGui::GetCursorPosY()<want) ImGui::SetCursorPosY(want);
  }

  // layout
  ImVec2 areaPos=ImGui::GetCursorScreenPos();
  ImVec2 avail=ImGui::GetContentRegionAvail();
  float rulerH=ImGui::GetFontSize()+4.0f*dpiScale;
  if (avail.x<kbW+32 || avail.y<arrH+rulerH+32) {
    ImGui::End();
    return;
  }
  ImVec2 gridPos=ImVec2(areaPos.x+kbW,areaPos.y+arrH+rulerH);
  float laneGap=3.0f*dpiScale;
  float laneVolH=pr.lanes?52.0f*dpiScale:0.0f;
  float laneFxH=pr.lanes?60.0f*dpiScale:0.0f;
  float lanesH=pr.lanes?(laneVolH+laneFxH+laneGap*2.0f):0.0f;
  if (avail.y-arrH-rulerH-lanesH<48.0f*dpiScale) {
    // too short for the lanes: hide them this frame
    laneVolH=laneFxH=lanesH=0.0f;
  }
  bool lanesShown=(lanesH>0.0f);
  ImVec2 gridSize=ImVec2(avail.x-kbW,avail.y-arrH-rulerH-lanesH);

  // ---------------- grid (scrolling child) ----------------
  ImGui::SetCursorScreenPos(gridPos);
  ImGui::SetNextWindowContentSize(ImVec2(contentW,contentH));
  if (pr.pendingScrollX>=0) {
    ImGui::SetNextWindowScroll(ImVec2(pr.pendingScrollX,(pr.pendingScrollY>=0)?pr.pendingScrollY:pr.scrollY));
    pr.pendingScrollX=-1;
    pr.pendingScrollY=-1;
  } else if (pr.pendingScrollY>=0) {
    ImGui::SetNextWindowScroll(ImVec2(pr.scrollX,pr.pendingScrollY));
    pr.pendingScrollY=-1;
  }
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,ImVec2(0,0));
  ImGui::BeginChild("PRGrid",gridSize,false,ImGuiWindowFlags_HorizontalScrollbar|ImGuiWindowFlags_AlwaysHorizontalScrollbar|ImGuiWindowFlags_AlwaysVerticalScrollbar|ImGuiWindowFlags_NoScrollWithMouse|ImGuiWindowFlags_NoMove);
  ImGui::PopStyleVar();
  {
    ImGuiWindow* win=ImGui::GetCurrentWindow();
    ImDrawList* dl=ImGui::GetWindowDrawList();
    pr.scrollX=ImGui::GetScrollX();
    pr.scrollY=ImGui::GetScrollY();
    ImVec2 viewMin=win->InnerRect.Min;
    ImVec2 viewMax=win->InnerRect.Max;
    float viewW=viewMax.x-viewMin.x;
    float viewH=viewMax.y-viewMin.y;
    ImVec2 org=ImVec2(viewMin.x-pr.scrollX,viewMin.y-pr.scrollY);
    ImGuiIO& io=ImGui::GetIO();
    bool gridHovered=ImGui::IsWindowHovered() && ImGui::IsMouseHoveringRect(viewMin,viewMax);

    // mouse wheel: scroll / zoom
    if (ImGui::IsWindowHovered()) {
      float wheel=io.MouseWheel;
      float wheelH=io.MouseWheelH;
      if (wheel!=0 || wheelH!=0) {
        if (io.KeyCtrl && io.KeyShift) {
          float mouseY=io.MousePos.y-org.y;
          float old=pr.zoomY;
          pr.zoomY=CLAMP(pr.zoomY*powf(1.15f,wheel),4.0f,40.0f);
          float newY=mouseY*(pr.zoomY/old);
          pr.pendingScrollY=MAX(0.0f,newY-(io.MousePos.y-viewMin.y));
        } else if (io.KeyCtrl) {
          float mouseX=io.MousePos.x-org.x;
          float old=pr.zoomX;
          pr.zoomX=CLAMP(pr.zoomX*powf(1.15f,wheel),2.0f,64.0f);
          float newX=mouseX*(pr.zoomX/old);
          pr.pendingScrollX=MAX(0.0f,newX-(io.MousePos.x-viewMin.x));
        } else if (io.KeyShift || wheelH!=0) {
          float d=(wheelH!=0)?-wheelH:wheel;
          ImGui::SetScrollX(pr.scrollX-d*zX*4.0f);
          pr.follow=false;
        } else {
          ImGui::SetScrollY(pr.scrollY-wheel*zY*3.0f);
        }
      }
    }

    // follow playback: keep the cursor at ~1/3 of the view (not while dragging)
    if (playing && pr.follow && playX>=0 && pr.drag==PR_DRAG_NONE) {
      float target=playX-viewW*0.33f;
      if (target<0) target=0;
      if (fabsf(target-pr.scrollX)>0.5f) ImGui::SetScrollX(target);
    }

    // E7: show the row the tracker cursor went to
    if (pr.revealRow>=0) {
      float rx=pr.revealRow*zX;
      if (rx<pr.scrollX || rx>pr.scrollX+viewW-zX) ImGui::SetScrollX(MAX(0.0f,rx-viewW*0.33f));
      pr.revealRow=-1;
    }

    // center vertically on the channel's notes
    if (pr.centerPending && !notes.empty()) {
      int sum=0;
      for (PRNote& n: notes) sum+=axisIndex[n.pitch];
      float mid=((float)sum/notes.size()+0.5f)*zY;
      ImGui::SetScrollY(MAX(0.0f,mid-viewH*0.5f));
      pr.centerPending=false;
    } else if (pr.centerPending) {
      pr.centerPending=false;
    }

    auto ticksPerRow=[&](int r) -> float {
      int len=MAX(1,(int)sub->speeds.len);
      return (float)MAX(1,sub->speeds.val[r%len]*(sub->timeBase+1));
    };
    auto noteX0=[&](const PRNote& n) -> float {
      float x=n.start*zX;
      if (n.delayTicks>0) x+=zX*MIN(1.0f,(float)n.delayTicks/ticksPerRow(n.start));
      return x;
    };
    auto noteX1=[&](const PRNote& n) -> float {
      float x=n.end*zX;
      if (n.cutRow>=0) {
        float cx=n.cutRow*zX+zX*MIN(1.0f,(float)n.cutTicks/ticksPerRow(n.cutRow));
        if (cx<x) x=cx;
      }
      return x;
    };

    // portamento (03xx on the start row of a note): how the pitch travels from the previous note.
    // in linear pitch mode (the default), the engine moves the pitch by speed*pitchSlideSpeed
    // units per tick, with 128 units per semitone (DivEngine::calcBaseFreq, playback.cpp case 0x03),
    // so the length of the slide is exact. other pitch modes depend on the chip: drawn dashed.
    struct PRPorta {
      int speed;        // 03xx value (0: none)
      int prev;         // index of the note it slides from (-1: nothing sounding -> no slide)
      int ticks;        // ticks to reach the target
      bool exact;
      PRPorta(): speed(0), prev(-1), ticks(0), exact(true) {}
    };
    std::vector<PRPorta> portas(notes.size());
    int effColsGrid=MIN(DIV_MAX_EFFECTS,MAX(1,(int)sub->pat[pr.chan].effectCols));
    for (size_t i=0; i<notes.size(); i++) {
      const PRNote& n=notes[i];
      int sp=prFindEffect(src.rowAt(src.patAt(n.start/patLen),n.start%patLen),effColsGrid,0x03);
      if (sp<=0) continue;
      PRPorta& pt=portas[i];
      pt.speed=sp;
      if (i>0 && notes[i-1].end==n.start && notes[i-1].endType==PR_END_NEXT) {
        pt.prev=(int)i-1;
        int units=abs(n.pitch-notes[i-1].pitch)*128;
        int rate=sp*((e->song.linearPitch==2)?MAX(1,(int)e->song.pitchSlideSpeed):1);
        pt.ticks=(units+rate-1)/rate+(e->song.noSlidesOnFirstTick?1:0);
        pt.exact=(e->song.linearPitch==2);
      }
    }
    // x (content coordinates) of a point `ticks` ticks after the start of row r
    auto tickToX=[&](int r, float ticks) -> float {
      while (r<totalRows && ticks>=ticksPerRow(r)) {
        ticks-=ticksPerRow(r);
        r++;
      }
      if (r>=totalRows) return totalRows*zX;
      return r*zX+zX*(ticks/ticksPerRow(r));
    };

    // inverse of tickToX: ticks after the start of row r for content x
    auto xToTicks=[&](int r, float x) -> float {
      float t=0;
      while (r<totalRows && x>=(r+1)*zX) {
        t+=ticksPerRow(r);
        r++;
      }
      if (r>=totalRows) return t;
      return t+MAX(0.0f,(x-r*zX)/zX)*ticksPerRow(r);
    };
    // the speed that makes a slide of `semis` semitones last about `ticks` ticks
    auto portaSpeedFor=[&](int semis, float ticks) -> int {
      int pss=(e->song.linearPitch==2)?MAX(1,(int)e->song.pitchSlideSpeed):1;
      int t=(int)roundf(ticks)-(e->song.noSlidesOnFirstTick?1:0);
      if (t<1) t=1;
      int units=abs(semis)*128;
      int sp=(units+t*pss-1)/(t*pss);
      return CLAMP(sp,1,255);
    };
    auto portaTicksFor=[&](int semis, int sp) -> int {
      int pss=(e->song.linearPitch==2)?MAX(1,(int)e->song.pitchSlideSpeed):1;
      int units=abs(semis)*128;
      int rate=MAX(1,sp*pss);
      return (units+rate-1)/rate+(e->song.noSlidesOnFirstTick?1:0);
    };

    // portamento anchors: the end of each ramp can be dragged to change the 03xx value
    int anchorHot=-1;
    auto anchorPos=[&](size_t i, int ticks) -> ImVec2 {
      const PRNote& n=notes[i];
      int a=axisIndex[CLAMP(n.pitch,0,179)];
      return ImVec2(org.x+tickToX(n.start,(float)(n.delayTicks+ticks)),org.y+a*zY+zY*0.5f);
    };
    if (gridHovered && pr.drag==PR_DRAG_NONE) {
      float best=7.0f*dpiScale;
      for (size_t i=0; i<notes.size(); i++) {
        if (portas[i].speed<=0 || portas[i].prev<0) continue;
        if (axisIndex[CLAMP(notes[i].pitch,0,179)]<0) continue;
        ImVec2 ap=anchorPos(i,portas[i].ticks);
        float d=sqrtf((io.MousePos.x-ap.x)*(io.MousePos.x-ap.x)+(io.MousePos.y-ap.y)*(io.MousePos.y-ap.y));
        if (d<best) { best=d; anchorHot=(int)i; }
      }
    }

    // ---------- mouse editing ----------
    float mRowF=(io.MousePos.x-org.x)/zX;
    float mAxF=(io.MousePos.y-org.y)/zY;
    int mRow=CLAMP((int)floorf(mRowF),0,MAX(0,totalRows-1));
    int mAx=CLAMP((int)floorf(mAxF),0,MAX(0,(int)axis.size()-1));
    float edgeW=6.0f*dpiScale;

    // note under the mouse (body or right edge)
    int hitIdx=-1;
    bool hitEdge=false;
    if (gridHovered) {
      for (size_t i=0; i<notes.size(); i++) {
        const PRNote& n=notes[i];
        int a=axisIndex[n.pitch];
        if (a!=mAx) continue;
        float x0=org.x+noteX0(n), x1=org.x+MAX(noteX0(n)+2.0f,noteX1(n));
        if (io.MousePos.x>=x0 && io.MousePos.x<x1) {
          hitIdx=(int)i;
          // resize zone: at most a quarter of the note, so short notes can still be moved
          float ew=MIN(edgeW,(x1-x0)*0.25f);
          hitEdge=(io.MousePos.x>=x1-ew) && (x1-x0)>=8.0f*dpiScale;
          break;
        }
      }
    }
    if (pr.drag==PR_DRAG_NONE && hitIdx>=0 && pr.tool!=PR_TOOL_ERASE) {
      ImGui::SetMouseCursor(hitEdge?ImGuiMouseCursor_ResizeEW:ImGuiMouseCursor_Hand);
    }
    if (pr.drag==PR_DRAG_RESIZE) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);

    auto previewOn=[&](int pitch, int ins) {
      int note=pitch-60;
      if (pr.previewNote!=-999) {
        int old=pr.previewNote;
        e->synchronized([this,old]() { e->autoNoteOff(-1,old); });
      }
      e->setMidiBaseChan(pr.chan);
      int useIns=(ins>=0)?ins:curIns;
      e->synchronized([this,useIns,note]() {
        if (!e->autoNoteOn(-1,useIns,note)) failedNoteOn=true;
      });
      pr.previewNote=note;
    };
    auto previewOff=[&]() {
      if (pr.previewNote==-999) return;
      int old=pr.previewNote;
      e->synchronized([this,old]() {
        e->autoNoteOff(-1,old);
        failedNoteOn=false;
      });
      pr.previewNote=-999;
    };

    if (anchorHot>=0 && pr.drag==PR_DRAG_NONE) {
      ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
      if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        ImGui::SetWindowFocus();
        pr.drag=PR_DRAG_PORTA;
        pr.portaNote=notes[anchorHot].start;
        pr.portaNewSpeed=portas[anchorHot].speed;
        pr.sel.clear();
        pr.sel.insert(notes[anchorHot].start);
        pr.focusRow=-1;
      } else {
        ImGui::SetTooltip(_("portamento 03%.2X: %d tick(s)\ndrag left or right to change the speed"),portas[anchorHot].speed,portas[anchorHot].ticks);
      }
    }
    // double click on a note: go to the "add effect" field (checked apart from the drag state,
    // which may still be finishing the first click on a slow frame)
    if (gridHovered && hitIdx>=0 && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
      pr.focusFxField=true;
      if (!pr.sel.count(notes[hitIdx].start)) {
        pr.sel.clear();
        pr.sel.insert(notes[hitIdx].start);
      }
    }
    // mouse down
    if (gridHovered && pr.drag==PR_DRAG_NONE && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
      ImGui::SetWindowFocus();
      pr.focusRow=-1;
      pr.dragMouseRow0=mRowF;
      pr.dragMouseAx0=mAxF;
      pr.dragDRow=0; pr.dragDPitch=0; pr.dragDLen=0;
      if (pr.tool==PR_TOOL_ERASE) {
        pr.drag=PR_DRAG_ERASE;
        pr.eraseSet.clear();
        if (hitIdx>=0) pr.eraseSet.insert(notes[hitIdx].start);
      } else if (hitIdx>=0) {
        const PRNote& n=notes[hitIdx];
        bool inSel=pr.sel.count(n.start)>0;
        if (io.KeyCtrl) {
          if (inSel) pr.sel.erase(n.start); else pr.sel.insert(n.start);
        } else if (!inSel) {
          pr.sel.clear();
          pr.sel.insert(n.start);
        }
        if (!io.KeyCtrl) syncTracker(n.start,n.end);
        pr.dragGrabStart=n.start;
        pr.dragGrabPitch=n.pitch;
        if (!io.KeyCtrl || pr.sel.count(n.start)) {
          pr.drag=hitEdge?PR_DRAG_RESIZE:(io.KeyShift?PR_DRAG_DUPLICATE:PR_DRAG_MOVE);
        }
        previewOn(n.pitch,n.ins);
      } else if (pr.tool==PR_TOOL_DRAW) {
        pr.drag=PR_DRAG_CREATE;
        pr.createRow=mRow;
        pr.createPitch=axisPitch(mAx);
        pr.dragDLen=0;
        pr.sel.clear();
        previewOn(pr.createPitch,curIns);
      } else {
        pr.drag=PR_DRAG_RECT;
        pr.rectA=ImVec2(io.MousePos.x-org.x,io.MousePos.y-org.y);
        pr.rectB=pr.rectA;
        if (!io.KeyCtrl) pr.sel.clear();
      }
    }

    // dragging
    if (pr.drag!=PR_DRAG_NONE && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
      int dRow=(int)roundf(mRowF-pr.dragMouseRow0);
      switch (pr.drag) {
        case PR_DRAG_MOVE:
        case PR_DRAG_DUPLICATE: {
          pr.dragDRow=dRow;
          int newPitch=axisPitch((int)floorf(mAxF));
          int dp=newPitch-pr.dragGrabPitch;
          if (dp!=pr.dragDPitch) {
            pr.dragDPitch=dp;
            int gi=-1;
            for (size_t i=0; i<notes.size(); i++) if (notes[i].start==pr.dragGrabStart) gi=(int)i;
            if (gi>=0) previewOn(CLAMP(notes[gi].pitch+dp,0,179),notes[gi].ins);
          }
          break;
        }
        case PR_DRAG_RESIZE:
          pr.dragDLen=dRow;
          break;
        case PR_DRAG_CREATE:
          pr.dragDLen=MAX(0,(int)floorf(mRowF)-pr.createRow);
          break;
        case PR_DRAG_RECT:
          pr.rectB=ImVec2(io.MousePos.x-org.x,io.MousePos.y-org.y);
          break;
        case PR_DRAG_ERASE:
          if (hitIdx>=0) pr.eraseSet.insert(notes[hitIdx].start);
          break;
        case PR_DRAG_PORTA: {
          ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
          for (size_t i=0; i<notes.size(); i++) {
            if (notes[i].start!=pr.portaNote || portas[i].prev<0) continue;
            int semis=notes[i].pitch-notes[portas[i].prev].pitch;
            float t=xToTicks(notes[i].start,io.MousePos.x-org.x)-(float)notes[i].delayTicks;
            pr.portaNewSpeed=portaSpeedFor(semis,t);
            ImGui::SetTooltip(_("portamento 03%.2X: %d tick(s)"),pr.portaNewSpeed,portaTicksFor(semis,pr.portaNewSpeed));
            break;
          }
          break;
        }
      }
      // autoscroll near the edges while dragging
      if (io.MousePos.x>viewMax.x-8.0f*dpiScale) ImGui::SetScrollX(pr.scrollX+zX);
      if (io.MousePos.x<viewMin.x+8.0f*dpiScale) ImGui::SetScrollX(pr.scrollX-zX);
    }

    // E8: absolute tick at the start of each row, and snapping inside a row
    std::vector<long long> tickStart(totalRows+1,0);
    for (int r=0; r<totalRows; r++) tickStart[r+1]=tickStart[r]+(long long)ticksPerRow(r);
    long long totalTicks=tickStart[totalRows];
    auto snapTick=[&](float cx) -> long long {
      int r=CLAMP((int)floorf(cx/zX),0,MAX(0,totalRows-1));
      float frac=CLAMP((cx-r*zX)/zX,0.0f,1.0f);
      int tpr=(int)ticksPerRow(r);
      int t=0;
      if (pr.snap==PR_SNAP_TICK) {
        t=(int)roundf(frac*tpr);
      } else {
        int half=tpr/2;
        float ft=frac*tpr;
        t=0;
        if (half>0 && fabsf(ft-half)<ft && fabsf(ft-half)<=fabsf(tpr-ft)) t=half;
        else if (fabsf(tpr-ft)<ft) t=tpr;
      }
      return CLAMP(tickStart[r]+t,0LL,totalTicks);
    };
    auto tickToRT=[&](long long T, int& row, int& t) {
      int lo=0, hi=totalRows; // largest r with tickStart[r]<=T
      while (lo<hi) {
        int mid=(lo+hi+1)/2;
        if (tickStart[mid]<=T) lo=mid; else hi=mid-1;
      }
      row=lo;
      t=(int)(T-tickStart[lo]);
    };
    auto noteStartTick=[&](const PRNote& n) -> long long { return tickStart[n.start]+n.delayTicks; };
    auto noteEndTick=[&](const PRNote& n) -> long long {
      if (n.cutRow>=0 && n.cutRow<n.end) return tickStart[n.cutRow]+n.cutTicks;
      return tickStart[MIN(n.end,totalRows)];
    };

    // build the edit a drag would make (also used to draw the preview)
    std::vector<int> dRem;
    std::vector<PRNoteSpec> dAdd;
    std::vector<int> dSel;
    std::vector<int> dFrom; // E6b: existing note each dAdd comes from (move, duplicate, resize)
    std::vector<int> dDelay, dCut; // E8: sub-row start/end of each dAdd (tick and 1/2 snap)
    bool dTick=false;
    auto buildDragEdit=[&]() {
      dRem.clear(); dAdd.clear(); dSel.clear(); dFrom.clear(); dDelay.clear(); dCut.clear();
      dTick=(pr.snap!=PR_SNAP_ROW) && (pr.drag==PR_DRAG_MOVE || pr.drag==PR_DRAG_DUPLICATE || pr.drag==PR_DRAG_RESIZE || pr.drag==PR_DRAG_CREATE);
      if (dTick) {
        long long tDown=snapTick(pr.dragMouseRow0*zX);
        long long tCur=snapTick(mRowF*zX);
        struct Ed { int from; long long S, E; int pitch, ins; };
        std::vector<Ed> eds;
        if (pr.drag==PR_DRAG_CREATE) {
          long long S=tDown, E=tCur;
          if (E<=S) {
            int rS, tS;
            tickToRT(S,rS,tS);
            E=MIN(totalTicks,tickStart[MIN(totalRows,rS+MAX(1,pr.lastLen))]+tS);
          }
          if (E<=S) return;
          eds.push_back({-1,S,E,pr.createPitch,curIns});
        } else {
          long long d=tCur-tDown;
          int dp=pr.dragDPitch;
          long long minS=totalTicks, maxE=0;
          for (PRNote& n: notes) {
            if (!pr.sel.count(n.start)) continue;
            minS=MIN(minS,noteStartTick(n));
            maxE=MAX(maxE,noteEndTick(n));
            if (pr.drag!=PR_DRAG_RESIZE) {
              if (n.pitch+dp<0) dp=-n.pitch;
              if (n.pitch+dp>179) dp=179-n.pitch;
            }
          }
          if (minS>=maxE) return;
          if (pr.drag==PR_DRAG_RESIZE) {
            if (d==0) return;
          } else {
            if (pr.drag==PR_DRAG_DUPLICATE) {
              long long span=maxE-minS;
              if (d>-span && d<span) d=(d<0)?-span:span;
            }
            if (minS+d<0) d=-minS;
            if (maxE+d>totalTicks) d=totalTicks-maxE;
            if (d==0 && dp==0) return;
            if (pr.drag==PR_DRAG_DUPLICATE && d==0) return;
          }
          for (PRNote& n: notes) {
            if (!pr.sel.count(n.start)) continue;
            long long S=noteStartTick(n), E=noteEndTick(n);
            if (pr.drag==PR_DRAG_RESIZE) {
              E=MAX(S+1,E+d);
              eds.push_back({n.start,S,MIN(E,totalTicks),n.pitch,n.ins});
            } else {
              eds.push_back({n.start,S+d,E+d,n.pitch+dp,n.ins});
            }
          }
        }
        for (Ed& x: eds) {
          int rS, tS, rE, uE;
          tickToRT(x.S,rS,tS);
          tickToRT(x.E,rE,uE);
          if (rS>=totalRows) continue;
          int endRow=(uE>0)?(rE+1):rE;
          if (endRow<=rS) endRow=rS+1;
          if (endRow>totalRows) { endRow=totalRows; uE=0; }
          if (x.from>=0 && pr.drag!=PR_DRAG_DUPLICATE) dRem.push_back(x.from);
          dFrom.push_back(x.from);
          dAdd.push_back(PRNoteSpec(rS,endRow,x.pitch,x.ins));
          dDelay.push_back(tS);
          dCut.push_back(uE);
          dSel.push_back(rS);
        }
        std::vector<int> ends;
        for (PRNoteSpec& a: dAdd) ends.push_back(a.end);
        prClipAdds(notes,dRem,dAdd);
        for (size_t i=0; i<dAdd.size(); i++) if (dAdd[i].end!=ends[i]) dCut[i]=0; // cut short by the next note
        return;
      }
      if (pr.drag==PR_DRAG_MOVE || pr.drag==PR_DRAG_DUPLICATE || pr.drag==PR_DRAG_RESIZE) {
        // clamp the deltas so no selected note leaves the song or the pitch range
        int dr=pr.dragDRow, dp=pr.dragDPitch, dl=pr.dragDLen;
        for (PRNote& n: notes) {
          if (!pr.sel.count(n.start)) continue;
          if (pr.drag==PR_DRAG_RESIZE) {
            if (n.end-n.start+dl<1) dl=1-(n.end-n.start);
            if (n.end+dl>totalRows) dl=totalRows-n.end;
          } else {
            if (n.start+dr<0) dr=-n.start;
            if (n.start+dr>totalRows-1) dr=totalRows-1-n.start;
            if (n.pitch+dp<0) dp=-n.pitch;
            if (n.pitch+dp>179) dp=179-n.pitch;
          }
        }
        if (pr.drag==PR_DRAG_DUPLICATE) {
          // a channel is monophonic: a copy can't sit on top of its source.
          // if it would overlap, put the copy right after (or before) the copied block
          int lo=totalRows, hi=0;
          for (PRNote& n: notes) {
            if (!pr.sel.count(n.start)) continue;
            lo=MIN(lo,n.start);
            hi=MAX(hi,n.end);
          }
          int span=hi-lo;
          if (span>0 && dr>-span && dr<span) dr=(dr<0)?-span:span;
          for (PRNote& n: notes) {
            if (!pr.sel.count(n.start)) continue;
            if (n.start+dr<0 || n.start+dr>totalRows-1) { dr=0; break; }
          }
          if (dr==0) return;
        }
        bool noop=(pr.drag==PR_DRAG_RESIZE)?(dl==0):(dr==0 && dp==0);
        if (noop) return;
        for (PRNote& n: notes) {
          if (!pr.sel.count(n.start)) continue;
          if (pr.drag!=PR_DRAG_DUPLICATE) dRem.push_back(n.start);
          dFrom.push_back(n.start);
          if (pr.drag==PR_DRAG_RESIZE) {
            dAdd.push_back(PRNoteSpec(n.start,n.end+dl,n.pitch,n.ins));
            dSel.push_back(n.start);
          } else {
            dAdd.push_back(PRNoteSpec(n.start+dr,MIN(totalRows,n.end+dr),n.pitch+dp,n.ins));
            dSel.push_back(n.start+dr);
          }
        }
      } else if (pr.drag==PR_DRAG_CREATE) {
        int len=(pr.dragDLen>0)?(pr.dragDLen+1):pr.lastLen;
        dAdd.push_back(PRNoteSpec(pr.createRow,MIN(totalRows,pr.createRow+len),pr.createPitch,curIns));
        dSel.push_back(pr.createRow);
      }
      prClipAdds(notes,dRem,dAdd);
    };
    buildDragEdit();

    // mouse up: commit
    if (pr.drag!=PR_DRAG_NONE && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
      previewOff();
      switch (pr.drag) {
        case PR_DRAG_RECT: {
          float rx0=MIN(pr.rectA.x,pr.rectB.x), rx1=MAX(pr.rectA.x,pr.rectB.x);
          float ry0=MIN(pr.rectA.y,pr.rectB.y), ry1=MAX(pr.rectA.y,pr.rectB.y);
          for (PRNote& n: notes) {
            float nx0=n.start*zX, nx1=n.end*zX;
            int a=axisIndex[n.pitch];
            if (a<0) continue;
            float ny0=a*zY, ny1=ny0+zY;
            if (nx1>rx0 && nx0<rx1 && ny1>ry0 && ny0<ry1) pr.sel.insert(n.start);
          }
          break;
        }
        case PR_DRAG_PORTA: {
          int row=pr.portaNote, sp=pr.portaNewSpeed;
          bool changed=false;
          for (size_t i=0; i<notes.size(); i++) {
            if (notes[i].start==row && portas[i].speed!=sp) changed=true;
          }
          if (changed && sp>0) {
            queueOp([row,sp](PREditCtx& c) -> bool { return prSetEffectValueWherePresent(c,{row},0x03,sp); });
          }
          pr.portaNote=-1;
          break;
        }
        case PR_DRAG_ERASE:
          if (!pr.eraseSet.empty()) {
            pr.pendRem.assign(pr.eraseSet.begin(),pr.eraseSet.end());
            pr.pendOp=nullptr;
            pr.pendAdd.clear();
            pr.pendSelect.clear();
            pianoRollCommit();
          }
          break;
        default:
          if (!dAdd.empty() || !dRem.empty()) {
            if (pr.drag==PR_DRAG_CREATE) {
              pr.lastLen=dAdd[0].end-dAdd[0].start;
            } else if (pr.drag==PR_DRAG_RESIZE && dAdd.size()==1) {
              pr.lastLen=dAdd[0].end-dAdd[0].start;
            }
            if (dTick && dFrom.size()==dAdd.size()) {
              // E8: sub-row positions, written as EDxx/ECxx; refused as a whole if they do not fit
              std::vector<PRMove> moves;
              bool copy=(pr.drag==PR_DRAG_DUPLICATE);
              for (size_t i=0; i<dAdd.size(); i++) {
                int dl2=(pr.drag==PR_DRAG_RESIZE)?-1:dDelay[i];
                moves.push_back(PRMove(dFrom[i],dAdd[i],copy,dl2,dCut[i]));
              }
              int endMk2=cp.endRel?PR_PITCH_REL:PR_PITCH_OFF;
              FurnacePianoRoll* prp2=&pr;
              pr.pendRem.clear();
              pr.pendAdd.clear();
              pr.pendStrict=true;
              pr.pendOp=[moves,endMk2,prp2](PREditCtx& c) -> bool { return prMoveNotes(c,moves,endMk2,&prp2->pendRefused); };
            } else if ((pr.drag==PR_DRAG_MOVE || pr.drag==PR_DRAG_DUPLICATE || pr.drag==PR_DRAG_RESIZE) && dFrom.size()==dAdd.size()) {
              // E6b: notes take their effects along
              std::vector<PRMove> moves;
              bool copy=(pr.drag==PR_DRAG_DUPLICATE);
              for (size_t i=0; i<dAdd.size(); i++) moves.push_back(PRMove(dFrom[i],dAdd[i],copy));
              int endMk2=cp.endRel?PR_PITCH_REL:PR_PITCH_OFF;
              FurnacePianoRoll* prp2=&pr;
              pr.pendRem.clear();
              pr.pendAdd.clear();
              pr.pendOp=[moves,endMk2,prp2](PREditCtx& c) -> bool { return prMoveNotes(c,moves,endMk2,&prp2->pendRefused); };
            } else {
              pr.pendRem=dRem;
              pr.pendAdd=dAdd;
              pr.pendOp=nullptr;
            }
            pr.pendSelect=dSel;
            pianoRollCommit();
          }
          break;
      }
      pr.drag=PR_DRAG_NONE;
      pr.eraseSet.clear();
    }

    // ---------- drawing ----------
    int firstRow=MAX(0,(int)floorf(pr.scrollX/zX));
    int lastRow=MIN(totalRows,(int)ceilf((pr.scrollX+viewW)/zX)+1);
    int firstAx=MAX(0,(int)floorf(pr.scrollY/zY));
    int lastAx=MIN((int)axis.size(),(int)ceilf((pr.scrollY+viewH)/zY)+1);

    ImU32 colBg=ImGui::GetColorU32(ImGuiCol_WindowBg);
    ImU32 colBlackRow=ImGui::GetColorU32(ImVec4(0.0f,0.0f,0.0f,0.28f));
    ImU32 colFoldGap=ImGui::GetColorU32(ImVec4(1.0f,1.0f,1.0f,0.04f));
    ImU32 colRepeat=ImGui::GetColorU32(ImVec4(1.0f,0.75f,0.2f,0.06f));
    ImU32 colHatch=ImGui::GetColorU32(ImVec4(1.0f,0.75f,0.2f,0.10f));
    ImU32 colLineRow=ImGui::GetColorU32(ImVec4(1.0f,1.0f,1.0f,0.05f));
    ImU32 colLineA=ImGui::GetColorU32(ImVec4(1.0f,1.0f,1.0f,0.12f));
    ImU32 colLineB=ImGui::GetColorU32(ImVec4(1.0f,1.0f,1.0f,0.22f));
    ImU32 colLinePat=ImGui::GetColorU32(ImVec4(1.0f,1.0f,1.0f,0.50f));
    ImU32 colCtoC=ImGui::GetColorU32(ImVec4(1.0f,1.0f,1.0f,0.10f));

    dl->AddRectFilled(viewMin,viewMax,colBg);

    for (int a=firstAx; a<lastAx; a++) {
      int p=axis[a];
      float y0=org.y+a*zY;
      if (p<0) {
        dl->AddRectFilled(ImVec2(viewMin.x,y0),ImVec2(viewMax.x,y0+zY),colFoldGap);
      } else if (prIsBlackKey(p)) {
        dl->AddRectFilled(ImVec2(viewMin.x,y0),ImVec2(viewMax.x,y0+zY),colBlackRow);
      }
      int pp=(p<0)?(-1-p):p;
      dl->AddLine(ImVec2(viewMin.x,y0+zY),ImVec2(viewMax.x,y0+zY),(!cp.fold && (pp%12)==0)?colCtoC:colLineRow);
    }

    for (int o=firstRow/patLen; o<=MIN(ordersLen-1,(lastRow-1)/patLen); o++) {
      if (prPatternUseCount(src,o)<2) continue;
      float x0=org.x+o*patLen*zX;
      float x1=x0+patLen*zX;
      float cx0=MAX(x0,viewMin.x), cx1=MIN(x1,viewMax.x);
      dl->AddRectFilled(ImVec2(cx0,viewMin.y),ImVec2(cx1,viewMax.y),colRepeat);
      dl->PushClipRect(ImVec2(cx0,viewMin.y),ImVec2(cx1,viewMax.y),true);
      float step=14.0f*dpiScale;
      float h=viewH;
      float start=x0-h;
      if (cx0-h>start) start+=floorf((cx0-h-start)/step)*step;
      for (float x=start; x<cx1; x+=step) {
        dl->AddLine(ImVec2(x,viewMax.y),ImVec2(x+h,viewMin.y),colHatch);
      }
      dl->PopClipRect();
    }

    int hlA=MAX(1,(int)sub->hilightA);
    int hlB=MAX(1,(int)sub->hilightB);
    for (int r=firstRow; r<=lastRow; r++) {
      float x=org.x+r*zX;
      int rr=r%patLen;
      ImU32 c;
      if (rr==0) c=colLinePat;
      else if ((rr%hlB)==0) c=colLineB;
      else if ((rr%hlA)==0) c=colLineA;
      else if (zX>=5.0f*dpiScale) c=colLineRow;
      else continue;
      dl->AddLine(ImVec2(x,viewMin.y),ImVec2(x,viewMax.y),c,(rr==0)?2.0f*dpiScale:1.0f);
    }

    if (pr.ghost) {
      ImU32 colGhost=ImGui::GetColorU32(ImVec4(0.75f,0.75f,0.75f,0.22f));
      for (auto& g: ghosts) {
        for (const PRNote& n: g) {
          if (n.end<firstRow || n.start>lastRow) continue;
          int a=axisIndex[n.pitch];
          if (a<firstAx || a>=lastAx) continue;
          float y0=org.y+a*zY;
          dl->AddRectFilled(ImVec2(org.x+noteX0(n)+1,y0+1),ImVec2(org.x+noteX1(n)-1,y0+zY-1),colGhost,2.0f*dpiScale);
        }
      }
    }

    ImVec4 cc=channelColor(pr.chan);
    if (cc.w<0.5f) cc.w=1.0f;
    bool dragging=(pr.drag==PR_DRAG_MOVE || pr.drag==PR_DRAG_RESIZE) && (!dAdd.empty());
    ImU32 colNote=ImGui::GetColorU32(ImVec4(cc.x*0.85f+0.15f,cc.y*0.85f+0.15f,cc.z*0.85f+0.15f,dragging?0.35f:0.92f));
    ImU32 colNoteSel=ImGui::GetColorU32(ImVec4(cc.x*0.5f+0.5f,cc.y*0.5f+0.5f,cc.z*0.5f+0.5f,dragging?0.35f:1.0f));
    ImU32 colNoteBorder=ImGui::GetColorU32(ImVec4(cc.x*0.5f,cc.y*0.5f,cc.z*0.5f,1.0f));
    ImU32 colSelBorder=ImGui::GetColorU32(ImVec4(1.0f,1.0f,1.0f,1.0f));
    ImU32 colNoteText=ImGui::GetColorU32(ImVec4(0.0f,0.0f,0.0f,0.9f));
    ImU32 colOff=ImGui::GetColorU32(ImVec4(1.0f,1.0f,1.0f,0.9f));
    ImU32 colRel=ImGui::GetColorU32(ImVec4(1.0f,0.85f,0.3f,0.95f));
    ImU32 colDiamond=ImGui::GetColorU32(ImVec4(1.0f,1.0f,1.0f,0.95f));
    ImU32 colErase=ImGui::GetColorU32(ImVec4(1.0f,0.25f,0.25f,0.9f));
    for (size_t ni=0; ni<notes.size(); ni++) {
      const PRNote& n=notes[ni];
      if (n.end<firstRow || n.start>lastRow) continue;
      int a=axisIndex[n.pitch];
      if (a<firstAx || a>=lastAx) continue;
      bool isSel=pr.sel.count(n.start)>0;
      float y0=org.y+a*zY;
      float x0=org.x+noteX0(n);
      float x1=org.x+noteX1(n);
      if (x1<x0+2.0f) x1=x0+2.0f;
      ImVec2 p0=ImVec2(x0+1,y0+1);
      ImVec2 p1=ImVec2(x1-1,y0+zY-1);
      dl->AddRectFilled(p0,p1,isSel?colNoteSel:colNote,2.0f*dpiScale);
      dl->AddRect(p0,p1,isSel?colSelBorder:colNoteBorder,2.0f*dpiScale,0,isSel?2.0f*dpiScale:1.0f);
      if (pr.eraseSet.count(n.start)) dl->AddRect(p0,p1,colErase,2.0f*dpiScale,0,2.0f*dpiScale);
      if (n.endType==PR_END_OFF || n.endType==PR_END_REL) {
        float ex=org.x+n.end*zX;
        dl->AddLine(ImVec2(ex-1,y0+1),ImVec2(ex-1,y0+zY-1),(n.endType==PR_END_OFF)?colOff:colRel,2.0f*dpiScale);
      }
      if (n.macroRelRow>=0) {
        float mx=org.x+(n.macroRelRow+0.5f)*zX;
        float my=y0+zY*0.5f;
        float r=MIN(zX,zY)*0.3f;
        dl->AddQuadFilled(ImVec2(mx,my-r),ImVec2(mx+r,my),ImVec2(mx,my+r),ImVec2(mx-r,my),colDiamond);
      }
      if (zY>=ImGui::GetFontSize()*0.85f && (x1-x0)>ImGui::GetFontSize()*1.8f) {
        short nn,oo;
        prEncodePitch(n.pitch,nn,oo);
        dl->PushClipRect(p0,p1,true);
        auto nmIt2=cp.rowNames.find(n.pitch);
        const char* lbl=(nmIt2!=cp.rowNames.end() && !nmIt2->second.empty())?nmIt2->second.c_str():noteName(nn,oo);
        dl->AddText(ImVec2(x0+3.0f*dpiScale,y0+(zY-ImGui::GetFontSize())*0.5f),colNoteText,lbl);
        dl->PopClipRect();
      }
      if (pr.drag==PR_DRAG_NONE && gridHovered && ImGui::IsMouseHoveringRect(p0,p1)) {
        short nn,oo;
        prEncodePitch(n.pitch,nn,oo);
        const char* endName=_("end of song");
        if (n.endType==PR_END_NEXT) endName=_("next note");
        if (n.endType==PR_END_OFF) endName="OFF";
        if (n.endType==PR_END_REL) endName="REL";
        String portaTip;
        const PRPorta& pt=portas[ni];
        if (pt.speed>0) {
          if (pt.prev<0) {
            portaTip=fmt::sprintf(_("\nportamento 03%.2X: no note is sounding right before this one, so there is nothing to slide from"),pt.speed);
          } else {
            portaTip=fmt::sprintf(_("\nportamento 03%.2X: slides from the previous note in %d tick(s)%s"),pt.speed,pt.ticks,pt.exact?"":_(" (approximate: this song does not use linear pitch)"));
          }
        }
        ImGui::SetTooltip("%s  %.2X:%.2X - %.2X:%.2X  (%d)\n%s %.2X%s\n%s: %s%s",
          noteName(nn,oo),
          n.start/patLen,n.start%patLen,n.end/patLen,n.end%patLen,n.end-n.start,
          _("instrument"),(n.ins<0)?0:n.ins,n.insExplicit?"":_(" (inherited)"),
          _("ends by"),endName,portaTip.c_str());
      }
    }

    // E7: tracker cursor in the roll: a thin line on its row and an outline on the note it is in
    if (trackerRow>=0 && trackerRow>=firstRow && trackerRow<=lastRow) {
      ImVec4 cv=uiColors[GUI_COLOR_PATTERN_CURSOR];
      ImU32 colCur=ImGui::GetColorU32(ImVec4(cv.x,cv.y,cv.z,0.55f));
      ImU32 colCurNote=ImGui::GetColorU32(ImVec4(cv.x*0.5f+0.5f,cv.y*0.5f+0.5f,cv.z*0.5f+0.5f,1.0f));
      float cx=org.x+(trackerRow+0.5f)*zX;
      dl->AddLine(ImVec2(cx,viewMin.y),ImVec2(cx,viewMax.y),colCur,1.0f*dpiScale);
      for (const PRNote& n: notes) {
        if (n.start>trackerRow || n.end<=trackerRow) continue;
        int a=axisIndex[n.pitch];
        if (a<0) break;
        float y0=org.y+a*zY;
        dl->AddRect(ImVec2(org.x+noteX0(n)-1,y0-1),ImVec2(org.x+noteX1(n)+1,y0+zY+1),colCurNote,2.0f*dpiScale,0,2.0f*dpiScale);
        break;
      }
    }

    // portamento line: runs through the previous note at its pitch, ramps to the new pitch
    // over the length of the slide, then runs through the note with the 03xx
    {
      ImU32 colPorta=ImGui::GetColorU32(uiColors[fxColors[0x03]]);
      float th=2.0f*dpiScale;
      auto pitchY=[&](int p) -> float {
        int a=axisIndex[CLAMP(p,0,179)];
        if (a<0) return -1.0f;
        return org.y+a*zY+zY*0.5f;
      };
      auto dashed=[&](ImVec2 a, ImVec2 b) {
        float len=sqrtf((b.x-a.x)*(b.x-a.x)+(b.y-a.y)*(b.y-a.y));
        float seg=4.0f*dpiScale;
        int nseg=MAX(1,(int)(len/seg));
        for (int k=0; k<nseg; k+=2) {
          float t0=(float)k/nseg, t1=(float)MIN(k+1,nseg)/nseg;
          dl->AddLine(ImVec2(a.x+(b.x-a.x)*t0,a.y+(b.y-a.y)*t0),ImVec2(a.x+(b.x-a.x)*t1,a.y+(b.y-a.y)*t1),colPorta,th);
        }
      };
      for (size_t i=0; i<notes.size(); i++) {
        const PRPorta& pt=portas[i];
        if (pt.speed<=0) continue;
        const PRNote& n=notes[i];
        if (n.end<firstRow || (pt.prev>=0?notes[pt.prev].start:n.start)>lastRow) continue;
        float yT=pitchY(n.pitch);
        if (yT<0) continue;
        float xs=org.x+noteX0(n);
        float xe=org.x+noteX1(n);
        if (pt.prev<0) {
          // 03xx with nothing to slide from: a hollow marker at the start of the note
          float r=MIN(zY*0.35f,6.0f*dpiScale);
          dl->AddCircle(ImVec2(xs+r+2.0f*dpiScale,yT),r,colPorta,0,1.5f*dpiScale);
          continue;
        }
        const PRNote& p=notes[pt.prev];
        float yP=pitchY(p.pitch);
        if (yP<0) continue;
        float xp=org.x+noteX0(p);
        int ticksDraw=pt.ticks;
        bool dragging=(pr.drag==PR_DRAG_PORTA && pr.portaNote==n.start);
        if (dragging) ticksDraw=portaTicksFor(n.pitch-p.pitch,pr.portaNewSpeed);
        float x0=org.x+tickToX(n.start,(float)n.delayTicks);
        float x1=org.x+tickToX(n.start,(float)(n.delayTicks+ticksDraw));
        dl->AddLine(ImVec2(xp,yP),ImVec2(x0,yP),colPorta,th);
        // the note may end before the slide is over
        ImVec2 rampEnd=ImVec2(x1,yT);
        if (xe<x1 && x1>x0) {
          float t=(xe-x0)/(x1-x0);
          rampEnd=ImVec2(xe,yP+(yT-yP)*t);
        }
        if (pt.exact) dl->AddLine(ImVec2(x0,yP),rampEnd,colPorta,th);
        else dashed(ImVec2(x0,yP),rampEnd);
        if (rampEnd.x<xe) dl->AddLine(rampEnd,ImVec2(xe,yT),colPorta,th);
        // the anchor: where the slide reaches the new pitch (may be past the end of the note)
        if (x1>xe) dashed(rampEnd,ImVec2(x1,yT));
        float ar=((int)i==anchorHot || dragging)?5.0f*dpiScale:3.5f*dpiScale;
        dl->AddCircleFilled(ImVec2(x1,yT),ar,colPorta);
        dl->AddCircle(ImVec2(x1,yT),ar,ImGui::GetColorU32(ImVec4(0,0,0,0.8f)),0,1.0f*dpiScale);
        if (dragging) {
          char buf[16];
          snprintf(buf,15,"03%.2X",pr.portaNewSpeed);
          dl->AddText(ImVec2(x1+6.0f*dpiScale,yT-ImGui::GetFontSize()-2.0f*dpiScale),colPorta,buf);
        }
      }
    }

    // drag preview (where the notes will go)
    if (!dAdd.empty() && pr.drag!=PR_DRAG_NONE) {
      ImU32 colPrev=ImGui::GetColorU32(ImVec4(cc.x*0.5f+0.5f,cc.y*0.5f+0.5f,cc.z*0.5f+0.5f,0.85f));
      for (size_t di=0; di<dAdd.size(); di++) {
        const PRNoteSpec& s=dAdd[di];
        int a=axisIndex[CLAMP(s.pitch,0,179)];
        if (a<0) continue;
        float y0=org.y+a*zY;
        float px0=s.start*zX, px1=s.end*zX;
        if (dTick && di<dDelay.size()) {
          if (dDelay[di]>0) px0=tickToX(s.start,(float)dDelay[di]);
          if (dCut[di]>0) px1=tickToX(s.end-1,(float)dCut[di]);
        }
        ImVec2 p0=ImVec2(org.x+px0+1,y0+1);
        ImVec2 p1=ImVec2(org.x+MAX(px0+3.0f,px1)-1,y0+zY-1);
        dl->AddRectFilled(p0,p1,colPrev,2.0f*dpiScale);
        dl->AddRect(p0,p1,colSelBorder,2.0f*dpiScale,0,2.0f*dpiScale);
        short nn,oo;
        prEncodePitch(s.pitch,nn,oo);
        if (zY>=ImGui::GetFontSize()*0.85f) dl->AddText(ImVec2(p0.x+2.0f*dpiScale,y0+(zY-ImGui::GetFontSize())*0.5f),colNoteText,noteName(nn,oo));
      }
    }
    // rubber band
    if (pr.drag==PR_DRAG_RECT) {
      ImVec2 r0=ImVec2(org.x+MIN(pr.rectA.x,pr.rectB.x),org.y+MIN(pr.rectA.y,pr.rectB.y));
      ImVec2 r1=ImVec2(org.x+MAX(pr.rectA.x,pr.rectB.x),org.y+MAX(pr.rectA.y,pr.rectB.y));
      dl->AddRectFilled(r0,r1,ImGui::GetColorU32(ImVec4(0.4f,0.6f,1.0f,0.15f)));
      dl->AddRect(r0,r1,ImGui::GetColorU32(ImVec4(0.4f,0.6f,1.0f,0.8f)));
    }

    // E7b: loop band and start marker
    {
      ImU32 colLoop=ImGui::GetColorU32(ImVec4(0.35f,0.75f,1.0f,0.08f));
      ImU32 colLoopEdge=ImGui::GetColorU32(ImVec4(0.35f,0.75f,1.0f,0.70f));
      if (pr.loopIn>=0) {
        int lo=(pr.loopOut>pr.loopIn)?pr.loopOut:totalRows;
        float lx0=org.x+pr.loopIn*zX, lx1=org.x+lo*zX;
        dl->AddRectFilled(ImVec2(MAX(lx0,viewMin.x),viewMin.y),ImVec2(MIN(lx1,viewMax.x),viewMax.y),colLoop);
        dl->AddLine(ImVec2(lx0,viewMin.y),ImVec2(lx0,viewMax.y),colLoopEdge,1.5f*dpiScale);
        if (pr.loopOut>pr.loopIn) dl->AddLine(ImVec2(lx1,viewMin.y),ImVec2(lx1,viewMax.y),colLoopEdge,1.5f*dpiScale);
      }
      if (pr.markerRow>=0) {
        float mx=org.x+pr.markerRow*zX;
        dl->AddLine(ImVec2(mx,viewMin.y),ImVec2(mx,viewMax.y),ImGui::GetColorU32(ImVec4(1.0f,1.0f,1.0f,0.55f)),1.0f*dpiScale);
      }
    }

    if (playX>=0) {
      float x=org.x+playX;
      ImVec4 ph=uiColors[GUI_COLOR_PATTERN_PLAY_HEAD];
      dl->AddLine(ImVec2(x,viewMin.y),ImVec2(x,viewMax.y),ImGui::GetColorU32(ImVec4(ph.x*0.4f+0.6f,ph.y*0.4f+0.6f,ph.z*0.4f+0.6f,0.95f)),2.0f*dpiScale);
    }
  }
  ImGui::EndChild();

  // ---------------- ruler (E7b): click to set the start marker ----------------
  {
    ImDrawList* dl=ImGui::GetWindowDrawList();
    ImVec2 r0=ImVec2(gridPos.x,gridPos.y-rulerH);
    ImVec2 r1=ImVec2(gridPos.x+gridSize.x-ImGui::GetStyle().ScrollbarSize,gridPos.y-1.0f);
    ImGui::SetCursorScreenPos(r0);
    ImGui::InvisibleButton("##PRRuler",ImVec2(MAX(1.0f,r1.x-r0.x),MAX(1.0f,r1.y-r0.y)));
    bool rHov=ImGui::IsItemHovered();
    bool rClick=ImGui::IsItemClicked(ImGuiMouseButton_Left);
    float zXl=pr.zoomX*dpiScale;
    float orgX=r0.x-pr.scrollX;
    int ra=MAX(0,(int)floorf(pr.scrollX/zXl));
    int rb=MIN(totalRows,(int)ceilf((pr.scrollX+(r1.x-r0.x))/zXl)+1);
    int hlAr=MAX(1,(int)sub->hilightA), hlBr=MAX(1,(int)sub->hilightB);
    dl->PushClipRect(r0,r1,true);
    dl->AddRectFilled(r0,r1,ImGui::GetColorU32(ImGuiCol_FrameBgHovered,0.55f));
    // loop band
    if (pr.loopIn>=0) {
      int lo=(pr.loopOut>pr.loopIn)?pr.loopOut:totalRows;
      ImU32 c=ImGui::GetColorU32(ImVec4(0.35f,0.75f,1.0f,(pr.loopOut>pr.loopIn)?0.45f:0.20f));
      dl->AddRectFilled(ImVec2(orgX+pr.loopIn*zXl,r0.y),ImVec2(orgX+lo*zXl,r1.y),c);
    }
    ImU32 colTick=ImGui::GetColorU32(ImVec4(1,1,1,0.35f));
    ImU32 colTickB=ImGui::GetColorU32(ImVec4(1,1,1,0.75f));
    float lastLabel=-1000.0f;
    for (int r=ra; r<=rb; r++) {
      float x=orgX+r*zXl;
      int rr=r%patLen;
      float h=(r1.y-r0.y);
      if (rr==0 || (rr%hlBr)==0) {
        dl->AddLine(ImVec2(x,r0.y),ImVec2(x,r1.y),colTickB);
        char buf[16];
        snprintf(buf,15,"%.2X:%.2X",r/patLen,rr);
        float tw=ImGui::CalcTextSize(buf).x;
        if (x-lastLabel>tw+8.0f*dpiScale) {
          dl->AddText(ImVec2(x+2.0f*dpiScale,r0.y+1.0f*dpiScale),ImGui::GetColorU32(ImGuiCol_Text),buf);
          lastLabel=x;
        }
      } else if ((rr%hlAr)==0) {
        dl->AddLine(ImVec2(x,r1.y-h*0.5f),ImVec2(x,r1.y),colTick);
      } else if (zXl>=6.0f*dpiScale) {
        dl->AddLine(ImVec2(x,r1.y-h*0.25f),ImVec2(x,r1.y),colTick);
      }
    }
    // marker: a downward triangle
    if (pr.markerRow>=0) {
      float mx=orgX+pr.markerRow*zXl;
      float t=6.0f*dpiScale;
      dl->AddTriangleFilled(ImVec2(mx-t,r0.y),ImVec2(mx+t,r0.y),ImVec2(mx,r1.y),ImGui::GetColorU32(ImVec4(1,1,1,0.95f)));
    }
    dl->PopClipRect();
    if (rHov) {
      int r=CLAMP((int)floorf((ImGui::GetIO().MousePos.x-orgX)/zXl),0,MAX(0,totalRows-1));
      ImGui::SetTooltip(_("%.2X:%.2X\nclick: put the start marker here (Play starts from it)\nI / O: loop from / to the marker"),r/patLen,r%patLen);
      if (rClick) {
        pr.markerRow=r;
        ImGui::SetWindowFocus();
        if (e->isPlaying()) {
          // like a DAW: clicking the ruler while playing jumps there
          play();
        } else {
          syncTracker(r,r+1);
        }
      }
    }
  }

  // ---------------- arrangement bar ----------------
  {
    ImDrawList* dl=ImGui::GetWindowDrawList();
    ImVec2 a0=ImVec2(gridPos.x,areaPos.y);
    ImVec2 a1=ImVec2(gridPos.x+gridSize.x-ImGui::GetStyle().ScrollbarSize,areaPos.y+arrH-2.0f*dpiScale);
    ImGui::SetCursorScreenPos(a0);
    ImGui::InvisibleButton("##PRArr",ImVec2(MAX(1.0f,a1.x-a0.x),MAX(1.0f,a1.y-a0.y)));
    bool arrClicked=ImGui::IsItemClicked();
    bool arrHovered=ImGui::IsItemHovered();
    dl->PushClipRect(a0,a1,true);
    dl->AddRectFilled(a0,a1,ImGui::GetColorU32(ImGuiCol_FrameBg));
    float zXl=pr.zoomX*dpiScale;
    float blockW=patLen*zXl;
    int o0=MAX(0,(int)(pr.scrollX/blockW));
    int o1=MIN(ordersLen,(int)((pr.scrollX+(a1.x-a0.x))/blockW)+1);
    for (int o=o0; o<o1; o++) {
      float x0=a0.x+o*blockW-pr.scrollX;
      float x1=x0+blockW;
      int use=prPatternUseCount(src,o);
      ImU32 fill=(use>1)?ImGui::GetColorU32(ImVec4(0.55f,0.42f,0.12f,0.85f)):ImGui::GetColorU32(ImGuiCol_Button);
      if (playing && playX>=o*blockW && playX<(o+1)*blockW) fill=ImGui::GetColorU32(ImGuiCol_ButtonActive);
      dl->AddRectFilled(ImVec2(x0+1,a0.y+1),ImVec2(x1-1,a1.y-1),fill,2.0f*dpiScale);
      char buf[32];
      if (use>1) {
        snprintf(buf,31,"%.2X  x%d",src.patAt(o),use);
      } else {
        snprintf(buf,31,"%.2X",src.patAt(o));
      }
      if (blockW>ImGui::CalcTextSize(buf).x+6.0f*dpiScale) {
        float tx=MAX(x0,a0.x)+4.0f*dpiScale;
        if (tx+ImGui::CalcTextSize(buf).x>x1-2.0f*dpiScale) tx=x1-2.0f*dpiScale-ImGui::CalcTextSize(buf).x;
        dl->AddText(ImVec2(tx,a0.y+3.0f*dpiScale),ImGui::GetColorU32(ImGuiCol_Text),buf);
      }
      if (arrHovered && ImGui::IsMouseHoveringRect(ImVec2(x0,a0.y),ImVec2(x1,a1.y))) {
        ImGui::SetTooltip(_("order %.2X: pattern %.2X (used %d times in this channel)"),o,src.patAt(o),use);
        if (arrClicked) {
          pr.pendingScrollX=o*blockW;
          pr.follow=false;
        }
      }
    }
    dl->PopClipRect();
  }

  // ---------------- keyboard ----------------
  {
    ImDrawList* dl=ImGui::GetWindowDrawList();
    ImVec2 k0=ImVec2(areaPos.x,gridPos.y);
    ImVec2 k1=ImVec2(gridPos.x-1.0f,gridPos.y+gridSize.y-ImGui::GetStyle().ScrollbarSize);
    dl->PushClipRect(k0,k1,true);
    dl->AddRectFilled(k0,k1,ImGui::GetColorU32(uiColors[GUI_COLOR_PIANO_BACKGROUND]));
    float zYl=pr.zoomY*dpiScale;
    int a0=MAX(0,(int)(pr.scrollY/zYl));
    int a1=MIN((int)axis.size(),(int)((pr.scrollY+(k1.y-k0.y))/zYl)+1);
    ImU32 colWhite=ImGui::GetColorU32(ImVec4(0.86f,0.86f,0.86f,1.0f));
    ImU32 colBlack=ImGui::GetColorU32(ImVec4(0.10f,0.10f,0.12f,1.0f));
    ImU32 colSep=ImGui::GetColorU32(ImVec4(0.0f,0.0f,0.0f,0.45f));
    ImU32 colGapKey=ImGui::GetColorU32(ImVec4(0.35f,0.35f,0.38f,1.0f));
    float kw=k1.x-k0.x;
    for (int a=a0; a<a1; a++) {
      int p=axis[a];
      bool gap=(p<0);
      if (gap) p=-1-p;
      float y0=k0.y+a*zYl-pr.scrollY;
      bool black=prIsBlackKey(p);
      if (gap) {
        dl->AddRectFilled(ImVec2(k0.x,y0),ImVec2(k1.x,y0+zYl),colGapKey);
      } else {
        dl->AddRectFilled(ImVec2(k0.x,y0),ImVec2(k1.x,y0+zYl),colWhite);
        if (black) dl->AddRectFilled(ImVec2(k0.x,y0),ImVec2(k0.x+(cp.fold?kw:kw*0.6f),y0+zYl),colBlack);
      }
      int pc=p%12;
      if (cp.fold || pc==0 || pc==5) dl->AddLine(ImVec2(k0.x,y0+zYl),ImVec2(k1.x,y0+zYl),colSep);
      // E8 (4.5): a named row shows its name instead of the note
      auto nmIt=cp.rowNames.find(p);
      if (nmIt!=cp.rowNames.end() && !nmIt->second.empty() && zYl>=ImGui::GetFontSize()*0.7f) {
        ImU32 tc=(black && !gap)?ImGui::GetColorU32(ImVec4(1,1,1,0.95f)):ImGui::GetColorU32(ImVec4(0,0,0,0.95f));
        if (black && !gap) dl->AddRectFilled(ImVec2(k0.x,y0),ImVec2(k1.x,y0+zYl),colBlack);
        dl->PushClipRect(ImVec2(k0.x,y0),ImVec2(k1.x,y0+zYl),true);
        dl->AddText(ImVec2(k0.x+3.0f*dpiScale,y0+(zYl-ImGui::GetFontSize())*0.5f),tc,nmIt->second.c_str());
        dl->PopClipRect();
        continue;
      }
      bool label=(pc==0) || cp.fold;
      if (label && zYl>=ImGui::GetFontSize()*0.7f) {
        short nn,oo;
        prEncodePitch(p,nn,oo);
        const char* nm=noteName(nn,oo);
        ImU32 tc=ImGui::GetColorU32(ImVec4(0,0,0,0.9f));
        if (black && !gap && cp.fold) tc=ImGui::GetColorU32(ImVec4(1,1,1,0.9f));
        if (gap) tc=ImGui::GetColorU32(ImVec4(0.8f,0.8f,0.8f,0.9f));
        dl->AddText(ImVec2(k1.x-ImGui::CalcTextSize(nm).x-3.0f*dpiScale,y0+(zYl-ImGui::GetFontSize())*0.5f),tc,nm);
      }
    }
    dl->PopClipRect();

    // E8 (4.5): right-click a key to name that row (sample, drum, noise mode...)
    ImGui::SetCursorScreenPos(k0);
    ImGui::InvisibleButton("##PRKeys",ImVec2(MAX(1.0f,k1.x-k0.x),MAX(1.0f,k1.y-k0.y)),ImGuiButtonFlags_MouseButtonRight);
    if (ImGui::IsItemHovered()) {
      int a=(int)floorf((ImGui::GetIO().MousePos.y-k0.y+pr.scrollY)/zYl);
      if (a>=0 && a<(int)axis.size()) {
        int p=axis[a];
        if (p<0) p=-1-p;
        short nn,oo;
        prEncodePitch(p,nn,oo);
        auto it=cp.rowNames.find(p);
        if (it!=cp.rowNames.end()) {
          ImGui::SetTooltip(_("%s: %s\nright-click: rename this row"),noteName(nn,oo),it->second.c_str());
        } else {
          ImGui::SetTooltip(_("%s\nright-click: give this row a name (e.g. kick, snare)"),noteName(nn,oo));
        }
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
          pr.nameEditPitch=p;
          snprintf(pr.nameBuf,63,"%s",(it!=cp.rowNames.end())?it->second.c_str():"");
          ImGui::OpenPopup("PRRowName");
        }
      }
    }
    if (ImGui::BeginPopup("PRRowName")) {
      short nn,oo;
      prEncodePitch(CLAMP(pr.nameEditPitch,0,179),nn,oo);
      ImGui::Text(_("Name for row %s (this channel)"),noteName(nn,oo));
      if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
      ImGui::SetNextItemWidth(220.0f*dpiScale);
      bool enter=ImGui::InputText("##PRRowNameIn",pr.nameBuf,63,ImGuiInputTextFlags_EnterReturnsTrue);
      ImGui::SameLine();
      bool ok=ImGui::Button(_("OK##PRRowNameOK"));
      ImGui::SameLine();
      bool clear=ImGui::Button(_("Remove##PRRowNameDel"));
      if (enter || ok || clear) {
        if (clear || pr.nameBuf[0]==0) {
          cp.rowNames.erase(pr.nameEditPitch);
        } else {
          cp.rowNames[pr.nameEditPitch]=pr.nameBuf;
        }
        prSavePrefs(pr,curFileName);
        ImGui::CloseCurrentPopup();
      }
      ImGui::EndPopup();
    }
  }

  // ---------------- lanes: volume (3.6) and effect (3.7) ----------------
  if (lanesShown) {
    ImDrawList* dl=ImGui::GetWindowDrawList();
    ImGuiIO& io=ImGui::GetIO();
    float sbW=ImGui::GetStyle().ScrollbarSize;
    float lx0=gridPos.x, lx1=gridPos.x+gridSize.x-sbW;
    float zXl=pr.zoomX*dpiScale;
    float orgX=lx0-pr.scrollX;
    int r0=MAX(0,(int)floorf(pr.scrollX/zXl));
    int r1=MIN(totalRows,(int)ceilf((pr.scrollX+(lx1-lx0))/zXl)+1);
    ImU32 colLaneBg=ImGui::GetColorU32(ImGuiCol_FrameBg);
    ImU32 colPatLine=ImGui::GetColorU32(ImVec4(1.0f,1.0f,1.0f,0.35f));
    ImU32 colBeat=ImGui::GetColorU32(ImVec4(1.0f,1.0f,1.0f,0.10f));
    ImU32 colTarget=ImGui::GetColorU32(ImVec4(1.0f,1.0f,1.0f,0.22f));
    ImU32 colLabel=ImGui::GetColorU32(ImGuiCol_Text);
    int hlBl=MAX(1,(int)sub->hilightB);
    bool released=!ImGui::IsMouseDown(ImGuiMouseButton_Left) && !ImGui::IsMouseDown(ImGuiMouseButton_Right);
    auto rowAtX=[&](float x) -> int {
      return CLAMP((int)floorf((x-orgX)/zXl),0,MAX(0,totalRows-1));
    };
    auto laneGrid=[&](float y0, float y1) {
      dl->AddRectFilled(ImVec2(lx0,y0),ImVec2(lx1,y1),colLaneBg);
      for (int r=r0; r<=r1; r++) {
        int rr=r%patLen;
        float x=orgX+r*zXl;
        if (rr==0) dl->AddLine(ImVec2(x,y0),ImVec2(x,y1),colPatLine,2.0f*dpiScale);
        else if ((rr%hlBl)==0) dl->AddLine(ImVec2(x,y0),ImVec2(x,y1),colBeat);
      }
      // rows edited by the note panel
      for (int t: tRows) {
        if (t<r0 || t>r1) continue;
        float x=orgX+t*zXl;
        dl->AddRectFilled(ImVec2(x,y0),ImVec2(x+MAX(2.0f,zXl),y1),colTarget);
      }
    };
    // paint helper: fills skipped rows when the mouse moves fast
    auto paintTo=[&](int row, float val, bool erase) {
      if (pr.laneLastRow<0) {
        pr.lanePaint[row]=erase?-1:(int)roundf(val);
      } else {
        int a=pr.laneLastRow, b=row;
        int dir=(b>=a)?1:-1;
        for (int r=a; ; r+=dir) {
          float t=(a==b)?1.0f:(float)(r-a)/(float)(b-a);
          float v=pr.laneLastVal+(val-pr.laneLastVal)*t;
          pr.lanePaint[r]=erase?-1:(int)roundf(v);
          if (r==b) break;
        }
      }
      pr.laneLastRow=row;
      pr.laneLastVal=val;
    };

    // ----- volume lane -----
    {
      float y0=gridPos.y+gridSize.y+laneGap;
      float y1=y0+laneVolH;
      int maxVol=MAX(1,e->getMaxVolumeChan(pr.chan));
      // label
      dl->AddText(ImVec2(areaPos.x+4.0f*dpiScale,y0+2.0f*dpiScale),colLabel,_("Volume"));
      dl->AddText(ImVec2(areaPos.x+4.0f*dpiScale,y0+2.0f*dpiScale+ImGui::GetFontSize()),ImGui::GetColorU32(ImGuiCol_TextDisabled),fmt::sprintf(_("max %.2X"),maxVol).c_str());

      ImGui::SetCursorScreenPos(ImVec2(lx0,y0));
      ImGui::InvisibleButton("##PRVolLane",ImVec2(MAX(1.0f,lx1-lx0),laneVolH),ImGuiButtonFlags_MouseButtonLeft|ImGuiButtonFlags_MouseButtonRight);
      bool hov=ImGui::IsItemHovered();
      dl->PushClipRect(ImVec2(lx0,y0),ImVec2(lx1,y1),true);
      laneGrid(y0,y1);
      float h=y1-y0-4.0f*dpiScale;
      auto volY=[&](int v) -> float { return y1-2.0f*dpiScale-h*((float)v/(float)maxVol); };
      ImVec4 cc=channelColor(pr.chan);
      ImU32 colStep=ImGui::GetColorU32(ImVec4(cc.x*0.6f+0.2f,cc.y*0.6f+0.2f,cc.z*0.6f+0.2f,0.35f));
      ImU32 colStepLine=ImGui::GetColorU32(ImVec4(cc.x*0.6f+0.4f,cc.y*0.6f+0.4f,cc.z*0.6f+0.4f,0.95f));
      ImU32 colWrite=ImGui::GetColorU32(ImVec4(1.0f,1.0f,1.0f,0.95f));
      ImU32 colPaint=ImGui::GetColorU32(ImVec4(1.0f,0.85f,0.3f,0.95f));
      // value in force before the first visible row
      int held=-1;
      for (int r=r0-1; r>=0; r--) {
        short v=rowPtr(r)[PR_COL_VOL];
        if (v>=0) { held=v; break; }
      }
      bool painting=(pr.laneDrag==PR_LANE_VOL_PAINT || pr.laneDrag==PR_LANE_VOL_ERASE);
      for (int r=r0; r<r1; r++) {
        int v=rowPtr(r)[PR_COL_VOL];
        bool written=(v>=0);
        bool painted=false;
        if (painting) {
          auto it=pr.lanePaint.find(r);
          if (it!=pr.lanePaint.end()) {
            painted=true;
            v=it->second;
            written=(v>=0);
          }
        }
        if (written) held=MIN(v,maxVol);
        if (held<0) continue;
        float x0=orgX+r*zXl, x1=x0+zXl;
        float y=volY(held);
        dl->AddRectFilled(ImVec2(x0,y),ImVec2(x1,y1),colStep);
        dl->AddLine(ImVec2(x0,y),ImVec2(x1,y),colStepLine,1.5f*dpiScale);
        if (written) {
          float w=MAX(2.0f*dpiScale,MIN(zXl-1.0f,5.0f*dpiScale));
          dl->AddRectFilled(ImVec2(x0,y-2.0f*dpiScale),ImVec2(x0+w,y+2.0f*dpiScale),painted?colPaint:colWrite);
        } else if (painted) {
          dl->AddLine(ImVec2(x0,y0),ImVec2(x0,y1),colPaint);
        }
      }
      dl->PopClipRect();

      if (hov && pr.laneDrag==PR_LANE_NONE && pr.drag==PR_DRAG_NONE) {
        int r=rowAtX(io.MousePos.x);
        short v=rowPtr(r)[PR_COL_VOL];
        int hv=-1, from=-1;
        for (int k=r; k>=0; k--) {
          short kv=rowPtr(k)[PR_COL_VOL];
          if (kv>=0) { hv=kv; from=k; break; }
        }
        if (v>=0) {
          ImGui::SetTooltip(_("%.2X:%.2X  volume %.2X\ndrag: draw volume, right button or Erase tool: clear"),r/patLen,r%patLen,v);
        } else if (hv>=0) {
          ImGui::SetTooltip(_("%.2X:%.2X  volume %.2X (held from %.2X:%.2X)\ndrag: draw volume, right button or Erase tool: clear"),r/patLen,r%patLen,hv,from/patLen,from%patLen);
        } else {
          ImGui::SetTooltip(_("%.2X:%.2X  no volume written yet (instrument default)\ndrag: draw volume"),r/patLen,r%patLen);
        }
        bool left=ImGui::IsMouseClicked(ImGuiMouseButton_Left);
        bool right=ImGui::IsMouseClicked(ImGuiMouseButton_Right);
        if (left || right) {
          pr.laneDrag=(right || pr.tool==PR_TOOL_ERASE)?PR_LANE_VOL_ERASE:PR_LANE_VOL_PAINT;
          pr.lanePaint.clear();
          pr.laneLastRow=-1;
          pr.focusRow=-1;
        }
      }
      if (pr.laneDrag==PR_LANE_VOL_PAINT || pr.laneDrag==PR_LANE_VOL_ERASE) {
        bool erase=(pr.laneDrag==PR_LANE_VOL_ERASE);
        int r=rowAtX(io.MousePos.x);
        float val=CLAMP((y1-2.0f*dpiScale-io.MousePos.y)/h*(float)maxVol,0.0f,(float)maxVol);
        if (!released) paintTo(r,val,erase);
        if (io.MousePos.x>lx1-8.0f*dpiScale) pr.pendingScrollX=pr.scrollX+zXl;
        if (io.MousePos.x<lx0+8.0f*dpiScale && pr.scrollX>0) pr.pendingScrollX=MAX(0.0f,pr.scrollX-zXl);
        if (released) {
          std::vector<std::pair<int,int>> vals(pr.lanePaint.begin(),pr.lanePaint.end());
          pr.laneDrag=PR_LANE_NONE;
          pr.lanePaint.clear();
          pr.laneLastRow=-1;
          if (!vals.empty()) queueOp([vals](PREditCtx& c) -> bool { return prSetVolume(c,vals); });
        }
      }
    }

    // ----- effect lane -----
    {
      float y0=gridPos.y+gridSize.y+laneGap*2.0f+laneVolH;
      float y1=y0+laneFxH;
      int effectColsL=MIN(DIV_MAX_EFFECTS,MAX(1,(int)sub->pat[pr.chan].effectCols));
      // which effects the channel uses (and how often)
      std::map<int,int> used;
      for (int r=0; r<totalRows; r++) {
        const short* row=rowPtr(r);
        for (int i=0; i<effectColsL; i++) {
          short f=row[PR_COL_FX(i)];
          if (f>=0) used[f]++;
        }
      }
      int fx=pr.laneFx;
      if (fx<0) {
        int best=0;
        for (auto& u: used) if (u.second>best) { best=u.second; fx=u.first; }
      }

      // label + selector
      ImGui::SetCursorScreenPos(ImVec2(areaPos.x+2.0f*dpiScale,y0+1.0f*dpiScale));
      ImGui::SetNextItemWidth(kbW-4.0f*dpiScale);
      String prevFx=(fx>=0)?fmt::sprintf("%.2X",fx):String("--");
      if (pr.laneFx<0) prevFx+="*";
      ImGui::SetNextWindowSizeConstraints(ImVec2(380.0f*dpiScale,0),ImVec2(760.0f*dpiScale,440.0f*dpiScale));
      if (ImGui::BeginCombo("##PRLaneFx",prevFx.c_str(),ImGuiComboFlags_NoArrowButton)) {
        if (ImGui::Selectable(_("auto: most used in this channel"),pr.laneFx<0)) pr.laneFx=-1;
        if (!used.empty()) {
          ImGui::SeparatorText(_("used in this channel"));
          for (auto& u: used) {
            ImGui::PushID(u.first);
            ImGui::PushStyleColor(ImGuiCol_Text,uiColors[fxColors[u.first]]);
            if (ImGui::Selectable(fmt::sprintf(_("%.2X  %s  (%d rows)"),u.first,fxDesc(u.first),u.second).c_str(),pr.laneFx==u.first)) pr.laneFx=u.first;
            ImGui::PopStyleColor();
            ImGui::PopID();
          }
        }
        ImGui::SeparatorText(_("all effects of this chip"));
        for (int i=0; i<256; i++) {
          const char* d=e->getEffectDesc((unsigned char)i,pr.chan);
          if (d==NULL) continue;
          ImGui::PushID(i+256);
          ImGui::PushStyleColor(ImGuiCol_Text,uiColors[fxColors[i]]);
          if (ImGui::Selectable(fmt::sprintf("%.2X  %s",i,(strlen(d)>6)?(d+6):d).c_str(),pr.laneFx==i)) pr.laneFx=i;
          ImGui::PopStyleColor();
          ImGui::PopID();
        }
        ImGui::EndCombo();
      }
      if (ImGui::IsItemHovered()) {
        if (fx>=0) {
          ImGui::SetTooltip(_("effect shown in this lane: %.2X %s%s"),fx,fxDesc(fx),(pr.laneFx<0)?_("\n(* = picked automatically: the most used)"):"");
        } else {
          ImGui::SetTooltip("%s",_("choose the effect to show in this lane"));
        }
      }
      dl->AddText(ImVec2(areaPos.x+4.0f*dpiScale,y0+ImGui::GetFrameHeight()+2.0f*dpiScale),ImGui::GetColorU32(ImGuiCol_TextDisabled),_("effect"));

      ImGui::SetCursorScreenPos(ImVec2(lx0,y0));
      ImGui::InvisibleButton("##PRFxLane",ImVec2(MAX(1.0f,lx1-lx0),laneFxH),ImGuiButtonFlags_MouseButtonLeft|ImGuiButtonFlags_MouseButtonRight);
      bool hov=ImGui::IsItemHovered();
      dl->PushClipRect(ImVec2(lx0,y0),ImVec2(lx1,y1),true);
      laneGrid(y0,y1);
      float h=y1-y0-4.0f*dpiScale;
      auto fxY=[&](int v) -> float { return y1-2.0f*dpiScale-h*((float)MAX(0,v)/255.0f); };
      bool painting=(pr.laneDrag==PR_LANE_FX_PAINT || pr.laneDrag==PR_LANE_FX_ERASE);
      ImU32 colPaint=ImGui::GetColorU32(ImVec4(1.0f,0.85f,0.3f,0.95f));
      ImU32 colFocus=ImGui::GetColorU32(ImVec4(1.0f,1.0f,1.0f,0.95f));
      if (fx>=0) {
        ImU32 colBar=ImGui::GetColorU32(uiColors[fxColors[fx]]);
        // other effects on the row: a small tick at the bottom, so loose effects are visible too
        ImU32 colOther=ImGui::GetColorU32(ImVec4(1.0f,1.0f,1.0f,0.25f));
        for (int r=r0; r<r1; r++) {
          const short* row=rowPtr(r);
          int v=-2;
          bool other=false;
          for (int i=0; i<effectColsL; i++) {
            short f=row[PR_COL_FX(i)];
            if (f==fx && v==-2) v=row[PR_COL_FXVAL(i)];
            else if (f>=0) other=true;
          }
          bool painted=false;
          if (painting) {
            auto it=pr.lanePaint.find(r);
            if (it!=pr.lanePaint.end()) {
              painted=true;
              v=(it->second<0)?-2:it->second;
            }
          }
          float x0=orgX+r*zXl;
          float bw=MAX(2.0f*dpiScale,zXl-1.0f);
          if (other) dl->AddRectFilled(ImVec2(x0,y1-3.0f*dpiScale),ImVec2(x0+bw,y1),colOther);
          if (v==-2) {
            if (painted) dl->AddLine(ImVec2(x0,y0),ImVec2(x0,y1),colPaint);
            continue;
          }
          // small values still get a visible marker
          float y=MIN(fxY(v),y1-4.0f*dpiScale);
          dl->AddRectFilled(ImVec2(x0,y),ImVec2(x0+bw,y1),painted?colPaint:colBar);
          if (zXl>=ImGui::CalcTextSize("00").x+2.0f*dpiScale) {
            char vb[16];
            snprintf(vb,15,"%.2X",MAX(0,v));
            float ty=MAX(y0+1.0f,y-ImGui::GetFontSize()-1.0f);
            dl->AddText(ImVec2(x0+1.0f,ty),ImGui::GetColorU32(ImGuiCol_Text),vb);
          }
          if (r==pr.focusRow) dl->AddRect(ImVec2(x0-1,y0+1),ImVec2(x0+bw+1,y1-1),colFocus,0,0,1.5f*dpiScale);
        }
      } else {
        dl->AddText(ImVec2(lx0+6.0f*dpiScale,y0+4.0f*dpiScale),ImGui::GetColorU32(ImGuiCol_TextDisabled),_("no effects in this channel: choose one in the selector on the left to draw it"));
      }
      if (pr.focusRow>=r0 && pr.focusRow<r1) {
        float x0=orgX+pr.focusRow*zXl;
        dl->AddRect(ImVec2(x0-1,y0+1),ImVec2(x0+MAX(2.0f,zXl)+1,y1-1),colFocus,0,0,1.5f*dpiScale);
      }
      dl->PopClipRect();

      if (hov && pr.laneDrag==PR_LANE_NONE && pr.drag==PR_DRAG_NONE) {
        int r=rowAtX(io.MousePos.x);
        String tip=fmt::sprintf("%.2X:%.2X",r/patLen,r%patLen);
        std::vector<PRFx> rf=prRowEffects(rowPtr(r),effectColsL);
        if (rf.empty()) tip+=String("  ")+_("no effects");
        for (const PRFx& x: rf) {
          if (x.fx<0) continue;
          tip+=fmt::sprintf("\n%.2X%.2X  %s",x.fx,MAX(0,x.val),fxDesc(x.fx));
        }
        tip+=String("\n\n")+_("click: show this row in the note panel");
        if (fx>=0) tip+=String("\n")+fmt::sprintf(_("Draw tool + drag: write %.2X with the value set by the height\nright button or Erase tool: remove %.2X"),fx,fx);
        ImGui::SetTooltip("%s",tip.c_str());
        bool left=ImGui::IsMouseClicked(ImGuiMouseButton_Left);
        bool right=ImGui::IsMouseClicked(ImGuiMouseButton_Right);
        if (left || right) {
          pr.lanePaint.clear();
          pr.laneLastRow=-1;
          if (fx>=0 && (right || pr.tool==PR_TOOL_ERASE)) {
            pr.laneDrag=PR_LANE_FX_ERASE;
          } else if (fx>=0 && pr.tool==PR_TOOL_DRAW) {
            pr.laneDrag=PR_LANE_FX_PAINT;
          } else if (left) {
            pr.laneDrag=PR_LANE_FX_PICK;
          }
          // the clicked row goes to the note panel: its note if one starts there, the row otherwise
          bool noteThere=false;
          for (const PRNote& n: notes) if (n.start==r) noteThere=true;
          pr.sel.clear();
          if (noteThere) {
            pr.sel.insert(r);
            pr.focusRow=-1;
            for (const PRNote& n: notes) if (n.start==r) syncTracker(n.start,n.end);
          } else {
            pr.focusRow=r;
            syncTracker(r,r+1);
          }
        }
      }
      if (pr.laneDrag==PR_LANE_FX_PICK && released) pr.laneDrag=PR_LANE_NONE;
      if (pr.laneDrag==PR_LANE_FX_PAINT || pr.laneDrag==PR_LANE_FX_ERASE) {
        bool erase=(pr.laneDrag==PR_LANE_FX_ERASE);
        int r=rowAtX(io.MousePos.x);
        float val=CLAMP((y1-2.0f*dpiScale-io.MousePos.y)/h*255.0f,0.0f,255.0f);
        if (!released) paintTo(r,val,erase);
        if (io.MousePos.x>lx1-8.0f*dpiScale) pr.pendingScrollX=pr.scrollX+zXl;
        if (io.MousePos.x<lx0+8.0f*dpiScale && pr.scrollX>0) pr.pendingScrollX=MAX(0.0f,pr.scrollX-zXl);
        if (released) {
          std::vector<std::pair<int,int>> vals(pr.lanePaint.begin(),pr.lanePaint.end());
          pr.laneDrag=PR_LANE_NONE;
          pr.lanePaint.clear();
          pr.laneLastRow=-1;
          int lfx=fx;
          if (!vals.empty() && lfx>=0) {
            if (erase) {
              std::vector<int> rows;
              for (auto& v: vals) rows.push_back(v.first);
              queueOp([rows,lfx](PREditCtx& c) -> bool { return prRemoveEffect(c,rows,lfx); });
            } else {
              queueOp([vals,lfx,prp](PREditCtx& c) -> bool {
                bool any=false;
                for (auto& v: vals) {
                  if (prSetEffect(c,{v.first},lfx,v.second,&prp->pendRefused)) any=true;
                }
                return any;
              });
            }
          }
        }
      }
    }
  }

  ImGui::SetCursorScreenPos(ImVec2(areaPos.x,areaPos.y+avail.y));
  ImGui::Dummy(ImVec2(0,0));

  // ---------------- repeated pattern popup ----------------
  if (pr.repeatPopup) {
      ImGui::OpenPopup(_("Repeated pattern##PRRepeatPopup"));
    pr.repeatPopup=false;
  }
  if (ImGui::BeginPopupModal(_("Repeated pattern##PRRepeatPopup"),NULL,ImGuiWindowFlags_AlwaysAutoResize)) {
    ImGui::Text(_("This pattern appears in %d positions of this channel's order."),pr.repeatPopupCount);
    ImGui::TextUnformatted(_("Your answer is remembered until Furnace is closed (change it in the toolbar)."));
    if (ImGui::Button(_("Edit all"))) {
      pr.repeatMode=PR_REPEAT_EDIT_ALL;
      ImGui::CloseCurrentPopup();
      pianoRollCommit();
    }
    ImGui::SameLine();
    if (ImGui::Button(_("Make unique at this position"))) {
      pr.repeatMode=PR_REPEAT_MAKE_UNIQUE;
      ImGui::CloseCurrentPopup();
      pianoRollCommit();
    }
    ImGui::SameLine();
    if (ImGui::Button(_("Cancel"))) {
      pr.pendRem.clear(); pr.pendAdd.clear(); pr.pendSelect.clear(); pr.pendOp=nullptr; pr.pendStrict=false;
      ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
  }

  ImGui::End();
}

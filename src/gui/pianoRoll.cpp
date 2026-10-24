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
  PR_DRAG_ERASE
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

  // pending edit (prApplyEdit arguments), committed by pianoRollCommit()
  std::vector<int> pendRem;
  std::vector<PRNoteSpec> pendAdd;
  std::vector<int> pendSelect; // starts to select after the commit
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
    previewNote(-999), statusTime(-100.0), prefsLoaded(false) {}
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
  if (!prApplyEdit(dry,pr.pendRem,pr.pendAdd,endMarker)) {
    pr.pendRem.clear(); pr.pendAdd.clear(); pr.pendSelect.clear();
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
        pr.pendRem.clear(); pr.pendAdd.clear(); pr.pendSelect.clear();
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
  prApplyEdit(real,pr.pendRem,pr.pendAdd,endMarker);
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

  pr.sel.clear();
  for (int s: pr.pendSelect) pr.sel.insert(s);
  pr.pendRem.clear(); pr.pendAdd.clear(); pr.pendSelect.clear();
}

// ---------------- the window ----------------

void FurnaceGUI::drawPianoRoll() {
  if (!pianoRollOpen) return;
  if (pianoRoll==NULL) {
    pianoRoll=new FurnacePianoRoll;
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

  // status message (only while there is something to say)
  ImGui::SameLine();
  bool showStatus=(ImGui::GetTime()-pr.statusTime)<6.0;
  if (showStatus) {
    ImGui::PushStyleColor(ImGuiCol_Text,uiColors[GUI_COLOR_WARNING]);
    ImGui::TextUnformatted(pr.status.c_str());
    ImGui::PopStyleColor();
  } else {
    ImGui::TextDisabled("%s",_("S/D/E: tools, Shift+drag: duplicate, Del: delete, Ctrl+A: select all"));
  }

  // ---------------- data ----------------
  int patLen=sub->patLen;
  int ordersLen=sub->ordersLen;
  int totalRows=patLen*ordersLen;
  PRChannelSource src=prSourceFor(e,pr.chan);
  std::vector<PRNote> notes=prReadChannel(src);

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
      if ((ImGui::IsKeyPressed(ImGuiKey_Delete,false) || ImGui::IsKeyPressed(ImGuiKey_Backspace,false)) && !pr.sel.empty()) {
        pr.pendRem.assign(pr.sel.begin(),pr.sel.end());
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

  // layout
  ImVec2 areaPos=ImGui::GetCursorScreenPos();
  ImVec2 avail=ImGui::GetContentRegionAvail();
  if (avail.x<kbW+32 || avail.y<arrH+32) {
    ImGui::End();
    return;
  }
  ImVec2 gridPos=ImVec2(areaPos.x+kbW,areaPos.y+arrH);
  ImVec2 gridSize=ImVec2(avail.x-kbW,avail.y-arrH);

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
        float x0=org.x+n.start*zX, x1=org.x+n.end*zX;
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

    // mouse down
    if (gridHovered && pr.drag==PR_DRAG_NONE && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
      ImGui::SetWindowFocus();
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
      }
      // autoscroll near the edges while dragging
      if (io.MousePos.x>viewMax.x-8.0f*dpiScale) ImGui::SetScrollX(pr.scrollX+zX);
      if (io.MousePos.x<viewMin.x+8.0f*dpiScale) ImGui::SetScrollX(pr.scrollX-zX);
    }

    // build the edit a drag would make (also used to draw the preview)
    std::vector<int> dRem;
    std::vector<PRNoteSpec> dAdd;
    std::vector<int> dSel;
    auto buildDragEdit=[&]() {
      dRem.clear(); dAdd.clear(); dSel.clear();
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
      // an edited note never runs over a note that is not part of the edit:
      // it ends where the next such note (or the next edited note) starts
      for (PRNoteSpec& sp: dAdd) {
        int lim=sp.end;
        for (PRNote& n: notes) {
          bool removed=std::find(dRem.begin(),dRem.end(),n.start)!=dRem.end();
          if (removed) continue;
          if (n.start>sp.start && n.start<lim) lim=n.start;
        }
        for (PRNoteSpec& o: dAdd) {
          if (o.start>sp.start && o.start<lim) lim=o.start;
        }
        sp.end=lim;
      }
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
        case PR_DRAG_ERASE:
          if (!pr.eraseSet.empty()) {
            pr.pendRem.assign(pr.eraseSet.begin(),pr.eraseSet.end());
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
            pr.pendRem=dRem;
            pr.pendAdd=dAdd;
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
    for (const PRNote& n: notes) {
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
        dl->AddText(ImVec2(x0+3.0f*dpiScale,y0+(zY-ImGui::GetFontSize())*0.5f),colNoteText,noteName(nn,oo));
        dl->PopClipRect();
      }
      if (pr.drag==PR_DRAG_NONE && gridHovered && ImGui::IsMouseHoveringRect(p0,p1)) {
        short nn,oo;
        prEncodePitch(n.pitch,nn,oo);
        const char* endName=_("end of song");
        if (n.endType==PR_END_NEXT) endName=_("next note");
        if (n.endType==PR_END_OFF) endName="OFF";
        if (n.endType==PR_END_REL) endName="REL";
        ImGui::SetTooltip("%s  %.2X:%.2X - %.2X:%.2X  (%d)\n%s %.2X%s\n%s: %s",
          noteName(nn,oo),
          n.start/patLen,n.start%patLen,n.end/patLen,n.end%patLen,n.end-n.start,
          _("instrument"),(n.ins<0)?0:n.ins,n.insExplicit?"":_(" (inherited)"),
          _("ends by"),endName);
      }
    }

    // drag preview (where the notes will go)
    if (!dAdd.empty() && pr.drag!=PR_DRAG_NONE) {
      ImU32 colPrev=ImGui::GetColorU32(ImVec4(cc.x*0.5f+0.5f,cc.y*0.5f+0.5f,cc.z*0.5f+0.5f,0.85f));
      for (const PRNoteSpec& s: dAdd) {
        int a=axisIndex[CLAMP(s.pitch,0,179)];
        if (a<0) continue;
        float y0=org.y+a*zY;
        ImVec2 p0=ImVec2(org.x+s.start*zX+1,y0+1);
        ImVec2 p1=ImVec2(org.x+s.end*zX-1,y0+zY-1);
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

    if (playX>=0) {
      float x=org.x+playX;
      ImVec4 ph=uiColors[GUI_COLOR_PATTERN_PLAY_HEAD];
      dl->AddLine(ImVec2(x,viewMin.y),ImVec2(x,viewMax.y),ImGui::GetColorU32(ImVec4(ph.x*0.4f+0.6f,ph.y*0.4f+0.6f,ph.z*0.4f+0.6f,0.95f)),2.0f*dpiScale);
    }
  }
  ImGui::EndChild();

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
      pr.pendRem.clear(); pr.pendAdd.clear(); pr.pendSelect.clear();
      ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
  }

  ImGui::End();
}

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
// E3: read-only roll. time runs left to right, pitch bottom to top.

#include "gui.h"
#include "guiConst.h"
#include "pianoRollModel.h"
#include <imgui_internal.h>
#include <math.h>
#include <algorithm>

struct FurnacePianoRoll {
  int chan;
  bool follow, fold, ghost;
  float zoomX, zoomY;     // pixels per row, pixels per semitone (before dpiScale)
  float scrollX, scrollY; // grid scroll in pixels, read back from the grid child
  bool centerPending;     // scroll vertically to the channel's notes on next frame
  float pendingScrollX;   // <0: none
  float pendingScrollY;
  bool firstFrame;
  FurnacePianoRoll():
    chan(0), follow(true), fold(false), ghost(true),
    zoomX(14.0f), zoomY(18.0f), scrollX(0), scrollY(0),
    centerPending(true), pendingScrollX(-1), pendingScrollY(-1), firstFrame(true) {}
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

void FurnaceGUI::drawPianoRoll() {
  if (!pianoRollOpen) return;
  if (pianoRoll==NULL) pianoRoll=new FurnacePianoRoll;
  FurnacePianoRoll& pr=*pianoRoll;

  ImGui::SetNextWindowSize(ImVec2(900.0f*dpiScale,560.0f*dpiScale),ImGuiCond_FirstUseEver);
  if (!ImGui::Begin("Piano Roll",&pianoRollOpen,globalWinFlags,_("Piano Roll"))) {
    ImGui::End();
    return;
  }

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

  // ---------------- toolbar ----------------
  ImGui::AlignTextToFramePadding();
  ImGui::TextUnformatted(_("Channel"));
  ImGui::SameLine();
  ImGui::SetNextItemWidth(160.0f*dpiScale);
  if (ImGui::BeginCombo("##PRChan",e->getChannelName(pr.chan))) {
    for (int i=0; i<chanCount; i++) {
      ImGui::PushID(i);
      if (ImGui::Selectable(e->getChannelName(i),i==pr.chan)) {
        pr.chan=i;
        pr.centerPending=true;
      }
      ImGui::PopID();
    }
    ImGui::EndCombo();
  }
  ImGui::SameLine();
  if (ImGui::Checkbox(_("Fold"),&pr.fold)) pr.centerPending=true;
  if (ImGui::IsItemHovered()) ImGui::SetTooltip(_("show only the pitches used in this channel"));
  ImGui::SameLine();
  ImGui::Checkbox(_("Ghost notes"),&pr.ghost);
  if (ImGui::IsItemHovered()) ImGui::SetTooltip(_("show the other channels of the same chip in gray"));
  ImGui::SameLine();
  ImGui::Checkbox(_("Follow playback"),&pr.follow);
  ImGui::SameLine();
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

  // ---------------- data ----------------
  int patLen=sub->patLen;
  int ordersLen=sub->ordersLen;
  int totalRows=patLen*ordersLen;
  PRChannelSource src=prSourceFor(e,pr.chan);
  std::vector<PRNote> notes=prReadChannel(src);

  // diagnostic dump for automated checks (env FURNACE_PIANO_ROLL_DUMP=<file>), once per run
  static bool dumped=false;
  if (!dumped) {
    dumped=true;
    const char* dumpPath=getenv("FURNACE_PIANO_ROLL_DUMP");
    if (dumpPath!=NULL && dumpPath[0]) {
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

  // vertical axis: list of pitches from top to bottom
  std::vector<int> axis;
  if (pr.fold) {
    bool used[180];
    memset(used,0,sizeof(used));
    for (PRNote& n: notes) used[n.pitch]=true;
    int lo=-1, hi=-1;
    for (int p=179; p>=0; p--) if (used[p]) { if (hi<0) hi=p; lo=p; }
    if (hi<0) {
      for (int p=72+12*4; p>=60+12*3; p--) axis.push_back(p);
    } else {
      // one empty row above and below
      if (hi<179) axis.push_back(-1-(hi+1));
      for (int p=179; p>=0; p--) if (used[p]) axis.push_back(p);
      if (lo>0) axis.push_back(-1-(lo-1));
    }
  } else {
    for (int p=179; p>=0; p--) axis.push_back(p);
  }
  // pitch -> axis index
  int axisIndex[180];
  for (int i=0; i<180; i++) axisIndex[i]=-1;
  for (size_t i=0; i<axis.size(); i++) {
    int p=axis[i];
    if (p<0) p=-1-p;
    if (p>=0 && p<180) axisIndex[p]=(int)i;
  }

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
  ImGui::BeginChild("PRGrid",gridSize,false,ImGuiWindowFlags_HorizontalScrollbar|ImGuiWindowFlags_AlwaysHorizontalScrollbar|ImGuiWindowFlags_AlwaysVerticalScrollbar|ImGuiWindowFlags_NoScrollWithMouse);
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

    // mouse wheel: scroll / zoom
    if (ImGui::IsWindowHovered()) {
      ImGuiIO& io=ImGui::GetIO();
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

    // follow playback: keep the cursor at ~1/3 of the view
    if (playing && pr.follow && playX>=0) {
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

    // pitch rows
    for (int a=firstAx; a<lastAx; a++) {
      int p=axis[a];
      float y0=org.y+a*zY;
      if (p<0) {
        dl->AddRectFilled(ImVec2(viewMin.x,y0),ImVec2(viewMax.x,y0+zY),colFoldGap);
      } else if (prIsBlackKey(p)) {
        dl->AddRectFilled(ImVec2(viewMin.x,y0),ImVec2(viewMax.x,y0+zY),colBlackRow);
      }
      int pp=(p<0)?(-1-p):p;
      dl->AddLine(ImVec2(viewMin.x,y0+zY),ImVec2(viewMax.x,y0+zY),(!pr.fold && (pp%12)==0)?colCtoC:colLineRow);
    }

    // repeated patterns: tint + hatch
    for (int o=firstRow/patLen; o<=MIN(ordersLen-1,(lastRow-1)/patLen); o++) {
      if (prPatternUseCount(src,o)<2) continue;
      float x0=org.x+o*patLen*zX;
      float x1=x0+patLen*zX;
      float cx0=MAX(x0,viewMin.x), cx1=MIN(x1,viewMax.x);
      dl->AddRectFilled(ImVec2(cx0,viewMin.y),ImVec2(cx1,viewMax.y),colRepeat);
      dl->PushClipRect(ImVec2(cx0,viewMin.y),ImVec2(cx1,viewMax.y),true);
      float step=14.0f*dpiScale;
      float h=viewH;
      // lines anchored to the pattern start so they don't swim while scrolling
      float start=x0-h;
      if (cx0-h>start) start+=floorf((cx0-h-start)/step)*step;
      for (float x=start; x<cx1; x+=step) {
        dl->AddLine(ImVec2(x,viewMax.y),ImVec2(x+h,viewMin.y),colHatch);
      }
      dl->PopClipRect();
    }

    // vertical lines
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

    // tick fraction helper (display only; groove indexed by absolute row)
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

    // ghost notes
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

    // channel notes
    ImVec4 cc=channelColor(pr.chan);
    if (cc.w<0.5f) cc.w=1.0f;
    ImU32 colNote=ImGui::GetColorU32(ImVec4(cc.x*0.85f+0.15f,cc.y*0.85f+0.15f,cc.z*0.85f+0.15f,0.92f));
    ImU32 colNoteBorder=ImGui::GetColorU32(ImVec4(cc.x*0.5f,cc.y*0.5f,cc.z*0.5f,1.0f));
    ImU32 colNoteText=ImGui::GetColorU32(ImVec4(0.0f,0.0f,0.0f,0.9f));
    ImU32 colOff=ImGui::GetColorU32(ImVec4(1.0f,1.0f,1.0f,0.9f));
    ImU32 colRel=ImGui::GetColorU32(ImVec4(1.0f,0.85f,0.3f,0.95f));
    ImU32 colDiamond=ImGui::GetColorU32(ImVec4(1.0f,1.0f,1.0f,0.95f));
    for (const PRNote& n: notes) {
      if (n.end<firstRow || n.start>lastRow) continue;
      int a=axisIndex[n.pitch];
      if (a<firstAx || a>=lastAx) continue;
      float y0=org.y+a*zY;
      float x0=org.x+noteX0(n);
      float x1=org.x+noteX1(n);
      if (x1<x0+2.0f) x1=x0+2.0f;
      ImVec2 p0=ImVec2(x0+1,y0+1);
      ImVec2 p1=ImVec2(x1-1,y0+zY-1);
      dl->AddRectFilled(p0,p1,colNote,2.0f*dpiScale);
      dl->AddRect(p0,p1,colNoteBorder,2.0f*dpiScale);
      // end marker: OFF = white bar, REL = yellow bar
      if (n.endType==PR_END_OFF || n.endType==PR_END_REL) {
        float ex=org.x+n.end*zX;
        dl->AddLine(ImVec2(ex-1,y0+1),ImVec2(ex-1,y0+zY-1),(n.endType==PR_END_OFF)?colOff:colRel,2.0f*dpiScale);
      }
      // macro release diamond
      if (n.macroRelRow>=0) {
        float mx=org.x+(n.macroRelRow+0.5f)*zX;
        float my=y0+zY*0.5f;
        float r=MIN(zX,zY)*0.3f;
        dl->AddQuadFilled(ImVec2(mx,my-r),ImVec2(mx+r,my),ImVec2(mx,my+r),ImVec2(mx-r,my),colDiamond);
      }
      // label
      if (zY>=ImGui::GetFontSize()*0.85f && (x1-x0)>ImGui::GetFontSize()*1.8f) {
        short nn,oo;
        prEncodePitch(n.pitch,nn,oo);
        dl->PushClipRect(p0,p1,true);
        dl->AddText(ImVec2(x0+3.0f*dpiScale,y0+(zY-ImGui::GetFontSize())*0.5f),colNoteText,noteName(nn,oo));
        dl->PopClipRect();
      }
      // tooltip
      if (ImGui::IsWindowHovered() && ImGui::IsMouseHoveringRect(p0,p1)) {
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

    // playback cursor
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
        // keep the label visible when the block starts left of the view
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
        // white background on every row, black key drawn over 60% of the width
        dl->AddRectFilled(ImVec2(k0.x,y0),ImVec2(k1.x,y0+zYl),colWhite);
        // folded: rows are not adjacent semitones, so black keys take the full width
        if (black) dl->AddRectFilled(ImVec2(k0.x,y0),ImVec2(k0.x+(pr.fold?kw:kw*0.6f),y0+zYl),colBlack);
      }
      // separator between white keys (B|C and E|F) or under every row when folded
      int pc=p%12;
      if (pr.fold || pc==0 || pc==5) dl->AddLine(ImVec2(k0.x,y0+zYl),ImVec2(k1.x,y0+zYl),colSep);
      bool label=(pc==0) || pr.fold;
      if (label && zYl>=ImGui::GetFontSize()*0.7f) {
        short nn,oo;
        prEncodePitch(p,nn,oo);
        const char* nm=noteName(nn,oo);
        ImU32 tc=ImGui::GetColorU32(ImVec4(0,0,0,0.9f));
        if (black && !gap && pr.fold) tc=ImGui::GetColorU32(ImVec4(1,1,1,0.9f));
        if (gap) tc=ImGui::GetColorU32(ImVec4(0.8f,0.8f,0.8f,0.9f));
        dl->AddText(ImVec2(k1.x-ImGui::CalcTextSize(nm).x-3.0f*dpiScale,y0+(zYl-ImGui::GetFontSize())*0.5f),tc,nm);
      }
    }
    dl->PopClipRect();
  }

  // leave the cursor below the whole area
  ImGui::SetCursorScreenPos(ImVec2(areaPos.x,areaPos.y+avail.y));
  ImGui::Dummy(ImVec2(0,0));

  ImGui::End();
}

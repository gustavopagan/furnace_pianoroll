// unit tests for src/gui/pianoRollModel (fork piano-roll).
// standalone: no ImGui, no engine. build with -DPIANO_ROLL_TESTS=ON
// or by hand: g++ -std=c++14 -I src test/pianoRoll/pianoRollModelTest.cpp src/gui/pianoRollModel.cpp

#include <stdio.h>
#include <string.h>
#include <map>
#include <algorithm>
#include <string>
#include "gui/pianoRollModel.h"
#include "gui/pianoRollPrefs.h"

// ---- fake channel ----
struct FakePat {
  short data[DIV_MAX_ROWS][DIV_MAX_COLS];
  FakePat() {
    memset(data,-1,sizeof(data));
    for (int i=0; i<DIV_MAX_ROWS; i++) { data[i][0]=0; data[i][1]=0; }
  }
};

struct FakeChan {
  int patLen, effectCols;
  std::vector<int> ord;
  std::map<int,FakePat> pats;
  FakePat empty;
  FakeChan(int len, std::vector<int> o, int fxCols=1): patLen(len), effectCols(fxCols), ord(o) {}

  short* row(int pat, int r) { return pats[pat].data[r]; }
  void note(int pat, int r, int pitch, int ins=-1) {
    short n,o; prEncodePitch(pitch,n,o);
    row(pat,r)[0]=n; row(pat,r)[1]=o;
    if (ins>=0) row(pat,r)[2]=ins;
  }
  void fx(int pat, int r, int col, int fx, int val) {
    row(pat,r)[PR_COL_FX(col)]=fx; row(pat,r)[PR_COL_FXVAL(col)]=val;
  }
  PREditCtx ctx() {
    PREditCtx c;
    c.total=patLen*(int)ord.size(); c.effectCols=effectCols;
    c.get=[this](int r) -> const short* {
      int pat=ord[r/patLen];
      auto it=pats.find(pat);
      if (it==pats.end()) return empty.data[r%patLen];
      return it->second.data[r%patLen];
    };
    c.getMut=[this](int r) -> short* { return pats[ord[r/patLen]].data[r%patLen]; };
    return c;
  }
  // flat copy of every cell of every order position, for "nothing else changed" checks
  std::vector<short> snap() {
    std::vector<short> v;
    PREditCtx c=ctx();
    for (int r=0; r<c.total; r++) { const short* row=c.get(r); v.insert(v.end(),row,row+DIV_MAX_COLS); }
    return v;
  }
  PRChannelSource src() {
    PRChannelSource s;
    s.patLen=patLen; s.ordersLen=(int)ord.size(); s.effectCols=effectCols;
    s.patAt=[this](int p) { return ord[p]; };
    s.rowAt=[this](int pat, int r) -> const short* {
      auto it=pats.find(pat);
      if (it==pats.end()) return empty.data[r];
      return it->second.data[r];
    };
    return s;
  }
};

// ---- tiny assert framework ----
static int fails=0, checks=0;
static std::string curTest;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("  FALHOU [%s] linha %d: %s\n",curTest.c_str(),__LINE__,#c); } } while (0)
#define TEST(name) curTest=name; printf("- %s\n",name);

static const int C4=108; // C-4

int main() {
  // 1. nota + OFF
  {
    TEST("nota + OFF");
    FakeChan c(16,{0});
    c.note(0,2,C4,3);
    c.note(0,6,PR_PITCH_OFF);
    auto n=prReadChannel(c.src());
    CHECK(n.size()==1);
    CHECK(n[0].start==2 && n[0].end==6);
    CHECK(n[0].pitch==C4);
    CHECK(n[0].endType==PR_END_OFF);
    CHECK(n[0].ins==3 && n[0].insExplicit);
  }
  // 2. nota + REL
  {
    TEST("nota + REL");
    FakeChan c(16,{0});
    c.note(0,0,C4+7,1);
    c.note(0,4,PR_PITCH_REL);
    auto n=prReadChannel(c.src());
    CHECK(n.size()==1);
    CHECK(n[0].start==0 && n[0].end==4);
    CHECK(n[0].endType==PR_END_REL);
  }
  // 3. nota cortada por nota
  {
    TEST("nota cortada por nota");
    FakeChan c(16,{0});
    c.note(0,0,C4,0);
    c.note(0,3,C4+2,0);
    c.note(0,5,PR_PITCH_OFF);
    auto n=prReadChannel(c.src());
    CHECK(n.size()==2);
    CHECK(n[0].start==0 && n[0].end==3 && n[0].endType==PR_END_NEXT);
    CHECK(n[1].start==3 && n[1].end==5 && n[1].endType==PR_END_OFF);
    CHECK(n[1].pitch==C4+2);
  }
  // 4. instrumento herdado
  {
    TEST("instrumento herdado");
    FakeChan c(16,{0});
    c.note(0,0,C4,5);
    c.note(0,2,C4+1);          // sem instrumento: herda 5
    c.row(0,4)[PR_COL_INS]=9;  // instrumento solto, sem nota
    c.note(0,6,C4+2);          // herda 9
    c.note(0,8,C4+3,2);        // explícito 2
    auto n=prReadChannel(c.src());
    CHECK(n.size()==4);
    CHECK(n[0].ins==5 && n[0].insExplicit);
    CHECK(n[1].ins==5 && !n[1].insExplicit);
    CHECK(n[2].ins==9 && !n[2].insExplicit);
    CHECK(n[3].ins==2 && n[3].insExplicit);
  }
  // 4b. música começa sem instrumento nenhum
  {
    TEST("nota sem instrumento no início da música");
    FakeChan c(16,{0});
    c.note(0,1,C4);
    auto n=prReadChannel(c.src());
    CHECK(n.size()==1);
    CHECK(n[0].ins==-1 && !n[0].insExplicit);
  }
  // 5. macro release
  {
    TEST("macro release não fecha a nota");
    FakeChan c(16,{0});
    c.note(0,0,C4,0);
    c.note(0,3,PR_PITCH_MACRO_REL);
    c.note(0,7,PR_PITCH_OFF);
    auto n=prReadChannel(c.src());
    CHECK(n.size()==1);
    CHECK(n[0].start==0 && n[0].end==7);
    CHECK(n[0].macroRelRow==3);
    CHECK(n[0].endType==PR_END_OFF);
  }
  // 6. EDxx na linha de início
  {
    TEST("EDxx na linha de início");
    FakeChan c(16,{0});
    c.note(0,4,C4,0);
    c.fx(0,4,0,0xed,3);
    c.note(0,8,PR_PITCH_OFF);
    auto n=prReadChannel(c.src());
    CHECK(n.size()==1);
    CHECK(n[0].start==4);
    CHECK(n[0].delayTicks==3);
  }
  // 6b. EDxx numa segunda coluna de efeito, e ECxx dentro da nota
  {
    TEST("EDxx na 2a coluna de efeito + ECxx no meio da nota");
    FakeChan c(16,{0},2);
    c.note(0,0,C4,0);
    c.fx(0,0,0,0x08,0x11);
    c.fx(0,0,1,0xed,2);
    c.fx(0,2,0,0xec,4);
    c.note(0,6,PR_PITCH_OFF);
    auto n=prReadChannel(c.src());
    CHECK(n.size()==1);
    CHECK(n[0].delayTicks==2);
    CHECK(n[0].cutRow==2 && n[0].cutTicks==4);
  }
  // 6c. ECxx solto numa linha sem nota soando não cria nem altera nota
  {
    TEST("ECxx solto sem nota soando");
    FakeChan c(16,{0});
    c.fx(0,1,0,0xec,4);
    c.note(0,4,C4,0);
    c.note(0,6,PR_PITCH_OFF);
    auto n=prReadChannel(c.src());
    CHECK(n.size()==1);
    CHECK(n[0].cutRow==-1);
  }
  // 7. pattern repetida
  {
    TEST("pattern repetida na order");
    FakeChan c(8,{0,1,0});
    c.note(0,2,C4,0);
    c.note(0,4,PR_PITCH_OFF);
    c.note(1,0,C4+5,1);
    c.note(1,1,PR_PITCH_OFF);
    PRChannelSource s=c.src();
    auto n=prReadChannel(s);
    CHECK(n.size()==3);
    CHECK(n[0].start==2 && n[0].end==4);
    CHECK(n[1].start==8 && n[1].end==9);
    CHECK(n[2].start==16+2 && n[2].end==16+4);
    CHECK(n[2].pitch==C4 && n[2].ins==0);
    CHECK(prPatternUseCount(s,0)==2);
    CHECK(prPatternUseCount(s,1)==1);
    CHECK(prPatternUseCount(s,2)==2);
  }
  // 8. nota atravessa fronteira de pattern e chega ao fim da música
  {
    TEST("nota atravessa pattern e termina no fim da música");
    FakeChan c(4,{0,1});
    c.note(0,3,C4,0);
    auto n=prReadChannel(c.src());
    CHECK(n.size()==1);
    CHECK(n[0].start==3 && n[0].end==8);
    CHECK(n[0].endType==PR_END_SONG);
  }
  // 9. OFF sem nota aberta é ignorado
  {
    TEST("OFF sem nota aberta");
    FakeChan c(8,{0});
    c.note(0,0,PR_PITCH_OFF);
    c.note(0,2,C4,0);
    auto n=prReadChannel(c.src());
    CHECK(n.size()==1);
    CHECK(n[0].start==2);
  }
  // 10. ida e volta da codificação da altura, nas 180 alturas
  {
    TEST("codificação de altura ida e volta (0..179, OFF, REL, macro rel)");
    bool ok=true;
    for (int p=0; p<180; p++) {
      short n,o; prEncodePitch(p,n,o);
      if (prDecodePitch(n,o)!=p) { ok=false; printf("    altura %d -> %d/%d\n",p,n,o); }
    }
    CHECK(ok);
    short n,o;
    prEncodePitch(C4,n,o); CHECK(n==12 && o==3);       // C-4 = nota 12, oitava 3
    prEncodePitch(C4+1,n,o); CHECK(n==1 && o==4);      // C#4
    prEncodePitch(60,n,o); CHECK(n==12 && (signed char)o==-1); // C-0
    prEncodePitch(PR_PITCH_OFF,n,o); CHECK(prDecodePitch(n,o)==PR_PITCH_OFF);
    prEncodePitch(PR_PITCH_REL,n,o); CHECK(prDecodePitch(n,o)==PR_PITCH_REL);
    prEncodePitch(PR_PITCH_MACRO_REL,n,o); CHECK(prDecodePitch(n,o)==PR_PITCH_MACRO_REL);
    CHECK(prDecodePitch(0,0)==PR_PITCH_EMPTY);
  }


  // ================= E4: escrita =================
  // rows whose cells differ between two snapshots
  auto diffRows=[](const std::vector<short>& a, const std::vector<short>& b) {
    std::vector<int> r;
    for (size_t i=0; i<a.size(); i+=DIV_MAX_COLS) {
      if (memcmp(&a[i],&b[i],DIV_MAX_COLS*sizeof(short))!=0) r.push_back((int)(i/DIV_MAX_COLS));
    }
    return r;
  };
  auto pitchAt=[](FakeChan& c, int r) { PREditCtx x=c.ctx(); const short* row=x.get(r); return prDecodePitch(row[0],row[1]); };
  auto insAt=[](FakeChan& c, int r) { PREditCtx x=c.ctx(); return (int)x.get(r)[PR_COL_INS]; };

  {
    TEST("E4 criar nota em canal vazio (OFF)");
    FakeChan c(16,{0});
    auto s0=c.snap();
    PREditCtx x=c.ctx();
    CHECK(prApplyEdit(x,{},{PRNoteSpec(2,5,C4,7)},PR_PITCH_OFF));
    auto n=prReadChannel(c.src());
    CHECK(n.size()==1 && n[0].start==2 && n[0].end==5 && n[0].pitch==C4);
    CHECK(n[0].ins==7 && n[0].insExplicit && n[0].endType==PR_END_OFF);
    CHECK((diffRows(s0,c.snap())==std::vector<int>{2,5}));
  }
  {
    TEST("E4 criar nota com preferência REL");
    FakeChan c(16,{0});
    PREditCtx x=c.ctx();
    prApplyEdit(x,{},{PRNoteSpec(0,4,C4,0)},PR_PITCH_REL);
    CHECK(pitchAt(c,4)==PR_PITCH_REL);
  }
  {
    TEST("E4 nova nota corta a anterior e limpa o OFF que ficaria dentro dela");
    FakeChan c(16,{0});
    c.note(0,0,C4,1); c.note(0,8,PR_PITCH_OFF);
    PREditCtx x=c.ctx();
    prApplyEdit(x,{},{PRNoteSpec(4,6,C4+4,2)},PR_PITCH_OFF);
    auto n=prReadChannel(c.src());
    CHECK(n.size()==2);
    CHECK(n[0].start==0 && n[0].end==4 && n[0].endType==PR_END_NEXT);
    CHECK(n[1].start==4 && n[1].end==6 && n[1].endType==PR_END_OFF);
    CHECK(pitchAt(c,8)==PR_PITCH_EMPTY);
  }
  {
    TEST("E4 nova nota que cobre a anterior inteira apaga a anterior");
    FakeChan c(16,{0});
    c.note(0,2,C4,1); c.note(0,4,PR_PITCH_OFF);
    PREditCtx x=c.ctx();
    prApplyEdit(x,{},{PRNoteSpec(0,8,C4+1,3)},PR_PITCH_OFF);
    auto n=prReadChannel(c.src());
    CHECK(n.size()==1 && n[0].start==0 && n[0].end==8);
    CHECK(pitchAt(c,2)==PR_PITCH_EMPTY && pitchAt(c,4)==PR_PITCH_EMPTY);
  }
  {
    TEST("E4 nova nota sobre o começo de uma nota à direita apara o começo dela");
    FakeChan c(16,{0});
    c.note(0,4,C4+7,5); c.note(0,12,PR_PITCH_OFF);
    PREditCtx x=c.ctx();
    prApplyEdit(x,{},{PRNoteSpec(0,6,C4,1)},PR_PITCH_OFF);
    auto n=prReadChannel(c.src());
    CHECK(n.size()==2);
    CHECK(n[0].start==0 && n[0].end==6 && n[0].endType==PR_END_NEXT);
    CHECK(n[1].start==6 && n[1].end==12 && n[1].pitch==C4+7 && n[1].ins==5 && n[1].endType==PR_END_OFF);
  }
  {
    TEST("E4 apagar nota que cortava a anterior: a anterior mantém a duração");
    FakeChan c(16,{0});
    c.note(0,0,C4,1); c.note(0,4,C4+2,1); c.note(0,8,PR_PITCH_OFF);
    PREditCtx x=c.ctx();
    prApplyEdit(x,{4},{},PR_PITCH_OFF);
    auto n=prReadChannel(c.src());
    CHECK(n.size()==1 && n[0].start==0 && n[0].end==4 && n[0].endType==PR_END_OFF);
    CHECK(pitchAt(c,8)==PR_PITCH_EMPTY);
  }
  {
    TEST("E4 apagar nota com instrumento explícito: a seguinte não muda de timbre");
    FakeChan c(16,{0});
    c.note(0,0,C4,1); c.note(0,2,PR_PITCH_OFF);
    c.note(0,4,C4,6); c.note(0,6,PR_PITCH_OFF);
    c.note(0,8,C4+3);  c.note(0,10,PR_PITCH_OFF); // herda 6
    PREditCtx x=c.ctx();
    prApplyEdit(x,{4},{},PR_PITCH_OFF);
    auto n=prReadChannel(c.src());
    CHECK(n.size()==2);
    CHECK(n[1].start==8 && n[1].ins==6 && n[1].insExplicit);
  }
  {
    TEST("E4 apagar não mexe em volume nem efeitos da linha");
    FakeChan c(16,{0},2);
    c.note(0,4,C4,1); c.row(0,4)[PR_COL_VOL]=0x40; c.fx(0,4,0,0x08,0x11); c.fx(0,4,1,0xec,2);
    c.note(0,6,PR_PITCH_OFF);
    auto s0=c.snap();
    PREditCtx x=c.ctx();
    prApplyEdit(x,{4},{},PR_PITCH_OFF);
    CHECK(c.row(0,4)[PR_COL_VOL]==0x40);
    CHECK(c.row(0,4)[PR_COL_FX(0)]==0x08 && c.row(0,4)[PR_COL_FXVAL(0)]==0x11);
    CHECK(c.row(0,4)[PR_COL_FX(1)]==0xec);
    CHECK(pitchAt(c,4)==PR_PITCH_EMPTY && insAt(c,4)==-1 && pitchAt(c,6)==PR_PITCH_EMPTY);
    CHECK((diffRows(s0,c.snap())==std::vector<int>{4,6}));
  }
  {
    TEST("E4 apagar nota com macro release limpa o macro release");
    FakeChan c(16,{0});
    c.note(0,0,C4,1); c.note(0,3,PR_PITCH_MACRO_REL); c.note(0,6,PR_PITCH_OFF);
    PREditCtx x=c.ctx();
    prApplyEdit(x,{0},{},PR_PITCH_OFF);
    CHECK(prReadChannel(c.src()).empty());
    CHECK(pitchAt(c,3)==PR_PITCH_EMPTY && pitchAt(c,6)==PR_PITCH_EMPTY);
  }
  {
    TEST("E4 mover nota (tempo e altura)");
    FakeChan c(16,{0});
    c.note(0,2,C4,3); c.note(0,4,PR_PITCH_OFF);
    auto s0=c.snap();
    PREditCtx x=c.ctx();
    prApplyEdit(x,{2},{PRNoteSpec(6,9,C4+2,3)},PR_PITCH_OFF);
    auto n=prReadChannel(c.src());
    CHECK(n.size()==1 && n[0].start==6 && n[0].end==9 && n[0].pitch==C4+2 && n[0].ins==3);
    CHECK((diffRows(s0,c.snap())==std::vector<int>{2,4,6,9}));
  }
  {
    TEST("E4 redimensionar nota");
    FakeChan c(16,{0});
    c.note(0,2,C4,3); c.note(0,4,PR_PITCH_OFF);
    PREditCtx x=c.ctx();
    prApplyEdit(x,{2},{PRNoteSpec(2,10,C4,3)},PR_PITCH_OFF);
    auto n=prReadChannel(c.src());
    CHECK(n.size()==1 && n[0].start==2 && n[0].end==10 && n[0].endType==PR_END_OFF);
    CHECK(pitchAt(c,4)==PR_PITCH_EMPTY);
  }
  {
    TEST("E4 mover nota para a pattern seguinte (duas posições da order)");
    FakeChan c(8,{0,1});
    c.note(0,6,C4,2); c.note(1,0,PR_PITCH_OFF); // nota 6..8, OFF na linha 0 da pattern 1
    PREditCtx x=c.ctx();
    prApplyEdit(x,{6},{PRNoteSpec(10,13,C4,2)},PR_PITCH_OFF);
    auto n=prReadChannel(c.src());
    CHECK(n.size()==1 && n[0].start==10 && n[0].end==13);
    CHECK(c.row(0,6)[0]==0 && c.row(1,0)[0]==0);
    CHECK(pitchAt(c,10)==C4 && pitchAt(c,13)==PR_PITCH_OFF);
  }
  {
    TEST("E4 duplicar (acrescentar sem remover)");
    FakeChan c(16,{0});
    c.note(0,0,C4,1); c.note(0,2,PR_PITCH_OFF);
    PREditCtx x=c.ctx();
    prApplyEdit(x,{},{PRNoteSpec(8,10,C4,1)},PR_PITCH_OFF);
    auto n=prReadChannel(c.src());
    CHECK(n.size()==2 && n[0].start==0 && n[0].end==2 && n[1].start==8 && n[1].end==10);
  }
  {
    TEST("E4 nova nota que termina onde começa outra não escreve OFF");
    FakeChan c(16,{0});
    c.note(0,4,C4,1); c.note(0,6,PR_PITCH_OFF);
    PREditCtx x=c.ctx();
    prApplyEdit(x,{},{PRNoteSpec(1,4,C4+1,1)},PR_PITCH_OFF);
    auto n=prReadChannel(c.src());
    CHECK(n.size()==2 && n[0].end==4 && n[0].endType==PR_END_NEXT && n[1].start==4);
  }
  {
    TEST("E4 inserir nota com instrumento explícito não muda o timbre das seguintes");
    FakeChan c(16,{0});
    c.note(0,0,C4,3); c.note(0,2,PR_PITCH_OFF);
    c.note(0,8,C4+5);  c.note(0,10,PR_PITCH_OFF); // herda 3
    PREditCtx x=c.ctx();
    prApplyEdit(x,{},{PRNoteSpec(4,6,C4,9)},PR_PITCH_OFF);
    auto n=prReadChannel(c.src());
    CHECK(n.size()==3 && n[2].start==8 && n[2].ins==3);
  }
  {
    TEST("E4 mover várias notas numa edição só");
    FakeChan c(32,{0});
    c.note(0,0,C4,1); c.note(0,2,PR_PITCH_OFF);
    c.note(0,4,C4+2,1); c.note(0,6,PR_PITCH_OFF);
    PREditCtx x=c.ctx();
    prApplyEdit(x,{0,4},{PRNoteSpec(16,18,C4+12,1),PRNoteSpec(20,22,C4+14,1)},PR_PITCH_OFF);
    auto n=prReadChannel(c.src());
    CHECK(n.size()==2 && n[0].start==16 && n[0].pitch==C4+12 && n[1].start==20 && n[1].end==22);
    CHECK(pitchAt(c,0)==PR_PITCH_EMPTY && pitchAt(c,2)==PR_PITCH_EMPTY && pitchAt(c,4)==PR_PITCH_EMPTY && pitchAt(c,6)==PR_PITCH_EMPTY);
  }
  {
    TEST("E4 ida e volta: apagar e recriar igual devolve as mesmas notas");
    FakeChan c(16,{0});
    c.note(0,0,C4,1); c.note(0,3,C4+3,2); c.note(0,7,PR_PITCH_OFF); c.note(0,9,C4,1); c.note(0,12,PR_PITCH_REL);
    auto n0=prReadChannel(c.src());
    PREditCtx x=c.ctx();
    prApplyEdit(x,{3},{PRNoteSpec(3,7,C4+3,2)},PR_PITCH_OFF);
    auto n1=prReadChannel(c.src());
    bool same=n0.size()==n1.size();
    for (size_t i=0; same && i<n0.size(); i++) same=n0[i].start==n1[i].start && n0[i].end==n1[i].end && n0[i].pitch==n1[i].pitch && n0[i].ins==n1[i].ins && n0[i].endType==n1[i].endType;
    CHECK(same);
  }


  {
    TEST("E4 teste aleatório: 50000 edições sorteadas, regras conferidas a cada uma");
    unsigned int seed=12345;
    auto rnd=[&seed](int n) { seed=seed*1103515245u+12345u; return (int)((seed>>16)%(unsigned)n); };
    int bad=0;
    for (int iter=0; iter<50000 && bad<5; iter++) {
      int nOrd=1+rnd(3);
      std::vector<int> ord;
      for (int i=0; i<nOrd; i++) ord.push_back(i); // distinct patterns (repeats are handled by the GUI popup)
      FakeChan c(16,ord,2);
      for (int pat=0; pat<nOrd; pat++) {
        for (int r=0; r<16; r++) {
          int k=rnd(10);
          if (k<3) c.note(pat,r,C4+rnd(24),rnd(3)==0?-1:rnd(8));
          else if (k==3) c.note(pat,r,PR_PITCH_OFF);
          else if (k==4) c.note(pat,r,PR_PITCH_REL);
          else if (k==5) c.note(pat,r,PR_PITCH_MACRO_REL);
          if (rnd(4)==0) c.row(pat,r)[PR_COL_VOL]=rnd(128);
          if (rnd(4)==0) c.fx(pat,r,rnd(2),rnd(256),rnd(256));
          if (rnd(8)==0) c.row(pat,r)[PR_COL_INS]=rnd(8);
        }
      }
      PREditCtx x=c.ctx();
      auto before=prReadRows(x.total,x.effectCols,x.get);
      auto s0=c.snap();
      std::vector<int> rem;
      std::vector<PRNoteSpec> add;
      int kind=rnd(3);
      if ((kind==0 || kind==2) && !before.empty()) rem.push_back(before[rnd(before.size())].start);
      if (kind==1 || kind==2) {
        int a=rnd(x.total); int b=a+1+rnd(8);
        add.push_back(PRNoteSpec(a,b,C4+rnd(24),rnd(8)));
      }
      prApplyEdit(x,rem,add,rnd(2)?PR_PITCH_OFF:PR_PITCH_REL);
      auto after=prReadRows(x.total,x.effectCols,x.get);
      auto s1=c.snap();
      bool ok=true;
      std::string why;
      // 1. volume and effects never change (cols >= 3)
      for (size_t i=0; i<s0.size(); i++) if ((int)(i%DIV_MAX_COLS)>=PR_COL_VOL && s0[i]!=s1[i]) { ok=false; why="volume/efeito mudou"; }
      // 2. added note exists exactly
      int a=-1, b=-1;
      if (!add.empty()) {
        a=add[0].start; b=std::min(add[0].end,x.total);
        bool found=false;
        for (auto& n: after) if (n.start==a && n.end==b && n.pitch==add[0].pitch && n.ins==add[0].ins) found=true;
        if (!found) { ok=false; why="nota acrescentada não confere"; }
      }
      // 3. removed note is gone (unless re-added at same start)
      for (int r: rem) {
        if (r==a) continue;
        for (auto& n: after) if (n.start==r) { ok=false; why="nota removida continua lá"; }
      }
      // 4. untouched notes keep start, pitch, instrument; end only shrinks to a when cut
      for (auto& bn: before) {
        bool edited=false;
        for (int r: rem) if (r==bn.start) edited=true;
        if (a>=0 && bn.start>=a && bn.start<b) edited=true;
        if (edited) continue;
        const PRNote* an=NULL;
        for (auto& n: after) if (n.start==bn.start) an=&n;
        if (an==NULL) { ok=false; why="nota intocada sumiu"; break; }
        if (an->pitch!=bn.pitch) { ok=false; why="altura de nota intocada mudou"; }
        if (an->ins!=bn.ins && bn.ins>=0) { ok=false; why="instrumento efetivo de nota intocada mudou"; }
        int expEnd=bn.end;
        if (a>=0 && bn.start<a && bn.end>a) expEnd=a;
        if (an->end!=expEnd) { ok=false; why="duração de nota intocada mudou"; }
      }
      // 5. no note appears anywhere else (only the added note at a, or a trimmed note moved to b)
      for (auto& an: after) {
        if (an.start==a || an.start==b) continue;
        bool existed=false;
        for (auto& bn: before) if (bn.start==an.start && bn.pitch==an.pitch) existed=true;
        if (!existed) { ok=false; why="nota apareceu do nada"; }
      }
      if (!ok) {
        if (bad==0) {
          printf("    antes: ");
          for (auto& n: before) printf("[%d-%d p%d i%d e%d] ",n.start,n.end,n.pitch,n.ins,(int)n.endType);
          printf("\n    depois: ");
          for (auto& n: after) printf("[%d-%d p%d i%d e%d] ",n.start,n.end,n.pitch,n.ins,(int)n.endType);
          printf("\n");
        }
        bad++;
        printf("    iter %d: %s (rem=%d add=%d..%d)\n",iter,why.c_str(),rem.empty()?-1:rem[0],a,b);
      }
    }
    CHECK(bad==0);
  }


  // ================= E4: preferências por canal (.fur.roll.json) =================
  {
    TEST("E5 trocar instrumento: só a coluna de instrumento da linha inicial muda");
    FakeChan c(16,{0},2);
    c.note(0,0,C4,1); c.row(0,0)[PR_COL_VOL]=0x30; c.fx(0,0,0,0xed,2);
    c.note(0,4,PR_PITCH_REL);
    c.note(0,8,C4+2,1); c.note(0,10,PR_PITCH_OFF);
    auto s0=c.snap();
    PREditCtx x=c.ctx();
    CHECK(prSetInstrument(x,{0},7));
    auto s1=c.snap();
    int diffs=0;
    for (size_t i=0; i<s0.size(); i++) if (s0[i]!=s1[i]) diffs++;
    CHECK(diffs==1);
    CHECK(c.row(0,0)[PR_COL_INS]==7);
    CHECK(c.row(0,0)[PR_COL_VOL]==0x30);
    auto n=prReadChannel(c.src());
    CHECK(n.size()==2 && n[0].ins==7 && n[0].endType==PR_END_REL && n[1].ins==1);
  }
  {
    TEST("E5 trocar instrumento: a nota que herdava mantém o timbre");
    FakeChan c(16,{0});
    c.note(0,0,C4,1); c.note(0,2,PR_PITCH_OFF);
    c.note(0,4,C4+3);  c.note(0,6,PR_PITCH_OFF);   // herda 1
    c.note(0,8,C4+5);  c.note(0,10,PR_PITCH_OFF);  // herda 1
    PREditCtx x=c.ctx();
    CHECK(prSetInstrument(x,{0},9));
    auto n=prReadChannel(c.src());
    CHECK(n.size()==3);
    CHECK(n[0].ins==9);
    CHECK(n[1].ins==1 && n[2].ins==1);
  }
  {
    TEST("E5 trocar instrumento em várias notas de uma vez, inclusive vizinhas");
    FakeChan c(16,{0});
    c.note(0,0,C4,1); c.note(0,2,C4+1); c.note(0,4,C4+2,3); c.note(0,6,PR_PITCH_OFF);
    PREditCtx x=c.ctx();
    CHECK(prSetInstrument(x,{0,2},5));
    auto n=prReadChannel(c.src());
    CHECK(n.size()==3);
    CHECK(n[0].ins==5 && n[1].ins==5 && n[2].ins==3);
  }
  {
    TEST("E5 trocar instrumento: sem mudança real devolve false; ins<0 e nota inexistente são ignorados");
    FakeChan c(16,{0});
    c.note(0,0,C4,4); c.note(0,2,PR_PITCH_OFF);
    PREditCtx x=c.ctx();
    CHECK(!prSetInstrument(x,{0},4));
    CHECK(!prSetInstrument(x,{0},-1));
    CHECK(!prSetInstrument(x,{5},2));
    CHECK(c.row(0,0)[PR_COL_INS]==4);
  }
  {
    TEST("E5 trocar instrumento numa pattern repetida (ordem 0 e 2 usam a mesma)");
    FakeChan c(8,{0,1,0});
    c.note(0,0,C4,1); c.note(0,2,PR_PITCH_OFF);
    c.note(1,0,C4+4,6); c.note(1,2,PR_PITCH_OFF);
    PREditCtx x=c.ctx();
    CHECK(prSetInstrument(x,{16},3)); // a nota da ordem 2 é a mesma pattern da ordem 0
    auto n=prReadChannel(c.src());
    CHECK(n.size()==3 && n[0].ins==3 && n[1].ins==6 && n[2].ins==3);
  }
  {
    TEST("E6 efeito: grava na primeira coluna livre, sem tocar nota, instrumento e volume");
    FakeChan c(16,{0},3);
    c.note(0,0,C4,1); c.row(0,0)[PR_COL_VOL]=0x40; c.fx(0,0,0,0xed,2);
    c.note(0,4,PR_PITCH_OFF);
    auto s0=c.snap();
    PREditCtx x=c.ctx();
    std::vector<int> ref;
    CHECK(prSetEffect(x,{0},0x08,0xf0,&ref));
    CHECK(ref.empty());
    CHECK(c.row(0,0)[PR_COL_FX(0)]==0xed && c.row(0,0)[PR_COL_FXVAL(0)]==2);
    CHECK(c.row(0,0)[PR_COL_FX(1)]==0x08 && c.row(0,0)[PR_COL_FXVAL(1)]==0xf0);
    auto s1=c.snap();
    int diffs=0;
    for (size_t i=0; i<s0.size(); i++) if (s0[i]!=s1[i]) diffs++;
    CHECK(diffs==2);
    auto n=prReadChannel(c.src());
    CHECK(n.size()==1 && n[0].start==0 && n[0].end==4 && n[0].ins==1 && n[0].delayTicks==2);
  }
  {
    TEST("E6 efeito: se a linha já tem o efeito, só o valor muda (não duplica)");
    FakeChan c(16,{0},2);
    c.fx(0,3,1,0x08,0x11);
    PREditCtx x=c.ctx();
    CHECK(prSetEffect(x,{3},0x08,0x22,NULL));
    CHECK(c.row(0,3)[PR_COL_FX(1)]==0x08 && c.row(0,3)[PR_COL_FXVAL(1)]==0x22);
    CHECK(c.row(0,3)[PR_COL_FX(0)]==-1);
    CHECK(!prSetEffect(x,{3},0x08,0x22,NULL)); // nada a mudar
  }
  {
    TEST("E6 efeito: sem coluna livre a linha é recusada e não muda");
    FakeChan c(16,{0},1);
    c.fx(0,2,0,0xec,4);
    c.note(0,5,C4,1);
    auto s0=c.snap();
    PREditCtx x=c.ctx();
    std::vector<int> ref;
    CHECK(prSetEffect(x,{2,5},0x08,0xf0,&ref));
    CHECK(ref.size()==1 && ref[0]==2);
    CHECK(c.row(0,2)[PR_COL_FX(0)]==0xec && c.row(0,2)[PR_COL_FXVAL(0)]==4);
    CHECK(c.row(0,5)[PR_COL_FX(0)]==0x08);
    // coluna com só valor (sem efeito) não conta como livre
    FakeChan d(16,{0},1);
    d.row(0,1)[PR_COL_FXVAL(0)]=0x33;
    PREditCtx y=d.ctx();
    std::vector<int> ref2;
    CHECK(!prSetEffect(y,{1},0x08,0xf0,&ref2));
    CHECK(ref2.size()==1);
  }
  {
    TEST("E6 efeito solto (EC04 sem nota) é lido e editável");
    FakeChan c(16,{0},2);
    c.note(0,0,C4,1); c.note(0,8,PR_PITCH_OFF);
    c.fx(0,6,0,0xec,4);  // linha sem nota
    auto e6=prRowEffects(c.row(0,6),2);
    CHECK(e6.size()==1 && e6[0].col==0 && e6[0].fx==0xec && e6[0].val==4);
    PREditCtx x=c.ctx();
    CHECK(prSetEffectValueWherePresent(x,{6},0xec,0x02));
    CHECK(c.row(0,6)[PR_COL_FXVAL(0)]==2);
    CHECK(!prSetEffectValueWherePresent(x,{0},0xec,0x02)); // linha 0 não tem EC: nada
    CHECK(prRemoveEffect(x,{6},0xec));
    CHECK(c.row(0,6)[PR_COL_FX(0)]==-1 && c.row(0,6)[PR_COL_FXVAL(0)]==-1);
    auto n=prReadChannel(c.src());
    CHECK(n.size()==1 && n[0].end==8);
  }
  {
    TEST("E6 efeito em várias linhas, inclusive repetida e fora da música");
    FakeChan c(8,{0,0},1);
    PREditCtx x=c.ctx();
    std::vector<int> ref;
    CHECK(prSetEffect(x,{1,9,99,-3,1},0x0a,0x0f,&ref)); // 1 e 9 são a mesma célula (pattern repetida)
    CHECK(ref.empty());
    CHECK(c.row(0,1)[PR_COL_FX(0)]==0x0a);
  }
  {
    TEST("E6 volume: grava e apaga só a coluna de volume");
    FakeChan c(16,{0},1);
    c.note(0,0,C4,1); c.fx(0,0,0,0x08,0x11); c.note(0,4,PR_PITCH_OFF);
    auto s0=c.snap();
    PREditCtx x=c.ctx();
    CHECK(prSetVolume(x,{{0,0x7f},{2,0x20},{30,5}}));
    CHECK(c.row(0,0)[PR_COL_VOL]==0x7f && c.row(0,2)[PR_COL_VOL]==0x20);
    auto s1=c.snap();
    int diffs=0;
    for (size_t i=0; i<s0.size(); i++) if (s0[i]!=s1[i]) diffs++;
    CHECK(diffs==2);
    CHECK(prSetVolume(x,{{2,-1}}));
    CHECK(c.row(0,2)[PR_COL_VOL]==-1);
    CHECK(!prSetVolume(x,{{2,-1}}));
  }
  {
    TEST("E6b mover: efeitos e volume da linha inicial vão junto; efeito de música fica");
    FakeChan c(32,{0},3);
    c.note(0,4,C4,1); c.row(0,4)[PR_COL_VOL]=0x50; c.fx(0,4,0,0x03,0x10); c.fx(0,4,1,0x0f,0x06);
    c.note(0,8,PR_PITCH_OFF);
    PREditCtx x=c.ctx();
    std::vector<int> ref;
    CHECK(prMoveNotes(x,{PRMove(4,PRNoteSpec(12,16,C4+2,1),false)},PR_PITCH_OFF,&ref));
    CHECK(ref.empty());
    // linha antiga: sem nota, sem 03, sem volume; 0F continua
    CHECK(c.row(0,4)[PR_COL_VOL]==-1);
    CHECK(c.row(0,4)[PR_COL_FX(0)]==-1);
    CHECK(c.row(0,4)[PR_COL_FX(1)]==0x0f && c.row(0,4)[PR_COL_FXVAL(1)]==0x06);
    // linha nova
    CHECK(c.row(0,12)[PR_COL_VOL]==0x50);
    auto e12=prRowEffects(c.row(0,12),3);
    CHECK(e12.size()==1 && e12[0].fx==0x03 && e12[0].val==0x10);
    auto n=prReadChannel(c.src());
    CHECK(n.size()==1 && n[0].start==12 && n[0].end==16 && n[0].pitch==C4+2);
  }
  {
    TEST("E6b duplicar: original e cópia ficam com os efeitos");
    FakeChan c(32,{0},2);
    c.note(0,0,C4,1); c.fx(0,0,0,0x03,0x20); c.note(0,4,PR_PITCH_OFF);
    PREditCtx x=c.ctx();
    CHECK(prMoveNotes(x,{PRMove(0,PRNoteSpec(8,12,C4,1),true)},PR_PITCH_OFF,NULL));
    CHECK(c.row(0,0)[PR_COL_FX(0)]==0x03);
    CHECK(c.row(0,8)[PR_COL_FX(0)]==0x03 && c.row(0,8)[PR_COL_FXVAL(0)]==0x20);
    auto n=prReadChannel(c.src());
    CHECK(n.size()==2);
  }
  {
    TEST("E6b destino sem coluna livre: o efeito fica na linha antiga e a linha é avisada");
    FakeChan c(32,{0},1);
    c.note(0,0,C4,1); c.fx(0,0,0,0x03,0x20); c.note(0,4,PR_PITCH_OFF);
    c.fx(0,10,0,0x08,0x11);
    PREditCtx x=c.ctx();
    std::vector<int> ref;
    CHECK(prMoveNotes(x,{PRMove(0,PRNoteSpec(10,14,C4,1),false)},PR_PITCH_OFF,&ref));
    CHECK(ref.size()==1 && ref[0]==10);
    CHECK(c.row(0,0)[PR_COL_FX(0)]==0x03 && c.row(0,0)[PR_COL_FXVAL(0)]==0x20);
    CHECK(c.row(0,10)[PR_COL_FX(0)]==0x08);
    // mesmo efeito no destino: o valor da nota vence
    FakeChan d(32,{0},1);
    d.note(0,0,C4,1); d.fx(0,0,0,0x03,0x20); d.note(0,4,PR_PITCH_OFF);
    d.fx(0,10,0,0x03,0x55);
    PREditCtx y=d.ctx();
    std::vector<int> ref2;
    CHECK(prMoveNotes(y,{PRMove(0,PRNoteSpec(10,14,C4,1),false)},PR_PITCH_OFF,&ref2));
    CHECK(ref2.empty() && d.row(0,10)[PR_COL_FXVAL(0)]==0x20);
  }
  {
    TEST("E6b corte EC dentro da nota: anda junto e mantém a distância do fim");
    FakeChan c(32,{0},2);
    c.note(0,0,C4,1); c.fx(0,2,0,0xec,3); c.note(0,4,PR_PITCH_OFF); // nota 0..4, corte na linha 2
    PREditCtx x=c.ctx();
    CHECK(prMoveNotes(x,{PRMove(0,PRNoteSpec(10,14,C4,1),false)},PR_PITCH_OFF,NULL));
    CHECK(c.row(0,2)[PR_COL_FX(0)]==-1);
    CHECK(c.row(0,12)[PR_COL_FX(0)]==0xec && c.row(0,12)[PR_COL_FXVAL(0)]==3);
    auto n=prReadChannel(c.src());
    CHECK(n.size()==1 && n[0].cutRow==12 && n[0].cutTicks==3);
    // redimensionar: o corte acompanha o fim
    PREditCtx y=c.ctx();
    CHECK(prMoveNotes(y,{PRMove(10,PRNoteSpec(10,20,C4,1),false)},PR_PITCH_OFF,NULL));
    auto m=prReadChannel(c.src());
    CHECK(m.size()==1 && m[0].end==20 && m[0].cutRow==18);
    CHECK(c.row(0,12)[PR_COL_FX(0)]==-1);
  }
  {
    TEST("E6b mover várias notas trocando de lugar não perde efeitos");
    FakeChan c(32,{0},1);
    c.note(0,0,C4,1); c.fx(0,0,0,0x03,0x11);
    c.note(0,4,C4+1,1); c.fx(0,4,0,0x03,0x22);
    c.note(0,8,PR_PITCH_OFF);
    PREditCtx x=c.ctx();
    CHECK(prMoveNotes(x,{PRMove(0,PRNoteSpec(4,8,C4,1),false),PRMove(4,PRNoteSpec(8,12,C4+1,1),false)},PR_PITCH_OFF,NULL));
    CHECK(c.row(0,0)[PR_COL_FX(0)]==-1);
    CHECK(c.row(0,4)[PR_COL_FXVAL(0)]==0x11);
    CHECK(c.row(0,8)[PR_COL_FXVAL(0)]==0x22);
  }
  {
    TEST("prefs: ida e volta em JSON (OFF/REL, fold, nomes com acento e aspas)");
    PRPrefs a;
    a.get(0).endRel=true;
    a.get(3).fold=true;
    a.get(9).rowNames[60]="bumbo \"grave\"";
    a.get(9).rowNames[61]="caixa ação";
    a.get(5); // default: not written
    std::string j=a.toJson();
    PRPrefs b;
    CHECK(b.fromJson(j));
    CHECK(b.chans.size()==3);
    CHECK(b.get(0).endRel && !b.get(0).fold);
    CHECK(b.get(3).fold && !b.get(3).endRel);
    CHECK(b.get(9).rowNames[60]=="bumbo \"grave\"" && b.get(9).rowNames[61]=="caixa ação");
  }
  {
    TEST("prefs: JSON inválido é recusado e não apaga o que havia");
    PRPrefs a;
    a.get(1).endRel=true;
    CHECK(!a.fromJson("{ \"channels\": { \"1\": { \"fold\": tru"));
    CHECK(!a.fromJson("lixo"));
    CHECK(a.get(1).endRel);
    CHECK(a.fromJson("{}"));
    CHECK(a.chans.empty());
  }
  {
    TEST("prefs: gravar e ler do disco ao lado da música");
    PRPrefs a;
    a.get(2).endRel=true;
    a.get(2).rowNames[72]="noise periódico";
    std::string song="/tmp/pianoRollPrefsTest.fur";
    CHECK(prPrefsSave(a,song));
    PRPrefs b;
    CHECK(prPrefsLoad(b,song));
    CHECK(b.get(2).endRel && b.get(2).rowNames[72]=="noise periódico");
    CHECK(!prPrefsLoad(b,"/tmp/nao-existe-xyz.fur"));
  }

  printf("\n%d verificações, %d falhas\n",checks,fails);
  if (fails==0) printf("TODOS OS TESTES PASSARAM\n");
  return fails?1:0;
}

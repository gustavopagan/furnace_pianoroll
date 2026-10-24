// unit tests for src/gui/pianoRollModel (fork piano-roll).
// standalone: no ImGui, no engine. build with -DPIANO_ROLL_TESTS=ON
// or by hand: g++ -std=c++14 -I src test/pianoRoll/pianoRollModelTest.cpp src/gui/pianoRollModel.cpp

#include <stdio.h>
#include <string.h>
#include <map>
#include <string>
#include "gui/pianoRollModel.h"

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

  printf("\n%d verificações, %d falhas\n",checks,fails);
  if (fails==0) printf("TODOS OS TESTES PASSARAM\n");
  return fails?1:0;
}

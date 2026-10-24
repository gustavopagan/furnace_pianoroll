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

#include "pianoRollPrefs.h"
#include "../fileutils.h"
#include <stdio.h>
#include <stdlib.h>

// ---------------- writer ----------------

static std::string prJsonEscape(const std::string& s) {
  std::string r="\"";
  for (unsigned char c: s) {
    switch (c) {
      case '"': r+="\\\""; break;
      case '\\': r+="\\\\"; break;
      case '\n': r+="\\n"; break;
      case '\r': r+="\\r"; break;
      case '\t': r+="\\t"; break;
      default:
        if (c<0x20) {
          char buf[8];
          snprintf(buf,8,"\\u%.4x",c);
          r+=buf;
        } else {
          r+=(char)c;
        }
    }
  }
  r+="\"";
  return r;
}

std::string PRPrefs::toJson() const {
  std::string r="{\n  \"version\": 1,\n  \"channels\": {";
  bool first=true;
  for (auto& i: chans) {
    if (i.second.isDefault()) continue;
    r+=first?"\n":",\n";
    first=false;
    r+="    \""+std::to_string(i.first)+"\": { ";
    r+="\"fimPadrao\": ";
    r+=i.second.endRel?"\"REL\"":"\"OFF\"";
    r+=", \"fold\": ";
    r+=i.second.fold?"true":"false";
    r+=", \"nomesDeLinha\": {";
    bool f2=true;
    for (auto& n: i.second.rowNames) {
      r+=f2?" ":", ";
      f2=false;
      r+="\""+std::to_string(n.first)+"\": "+prJsonEscape(n.second);
    }
    r+=f2?"} }":" } }";
  }
  r+=first?"}\n}\n":"\n  }\n}\n";
  return r;
}

// ---------------- reader (small JSON subset: objects, strings, numbers, bools) ----------------

namespace {
struct JVal {
  enum T { NUL, BOOL, NUM, STR, OBJ, ARR } t;
  bool b;
  double n;
  std::string s;
  std::map<std::string,JVal> o;
  JVal(): t(NUL), b(false), n(0) {}
};

struct JParser {
  const std::string& s;
  size_t p;
  bool ok;
  JParser(const std::string& str): s(str), p(0), ok(true) {}
  void ws() { while (p<s.size() && (s[p]==' ' || s[p]=='\t' || s[p]=='\n' || s[p]=='\r')) p++; }
  bool eat(char c) { ws(); if (p<s.size() && s[p]==c) { p++; return true; } return false; }
  std::string str() {
    std::string r;
    if (!eat('"')) { ok=false; return r; }
    while (p<s.size() && s[p]!='"') {
      char c=s[p++];
      if (c=='\\' && p<s.size()) {
        char e=s[p++];
        switch (e) {
          case 'n': r+='\n'; break;
          case 'r': r+='\r'; break;
          case 't': r+='\t'; break;
          case 'u': {
            if (p+4>s.size()) { ok=false; return r; }
            unsigned int cp=(unsigned int)strtoul(s.substr(p,4).c_str(),NULL,16);
            p+=4;
            // encode as UTF-8 (BMP only)
            if (cp<0x80) r+=(char)cp;
            else if (cp<0x800) { r+=(char)(0xc0|(cp>>6)); r+=(char)(0x80|(cp&0x3f)); }
            else { r+=(char)(0xe0|(cp>>12)); r+=(char)(0x80|((cp>>6)&0x3f)); r+=(char)(0x80|(cp&0x3f)); }
            break;
          }
          default: r+=e;
        }
      } else {
        r+=c;
      }
    }
    if (p>=s.size()) { ok=false; return r; }
    p++;
    return r;
  }
  JVal val(int depth=0) {
    JVal v;
    if (depth>16) { ok=false; return v; }
    ws();
    if (p>=s.size()) { ok=false; return v; }
    char c=s[p];
    if (c=='{') {
      p++;
      v.t=JVal::OBJ;
      if (eat('}')) return v;
      do {
        ws();
        std::string k=str();
        if (!ok || !eat(':')) { ok=false; return v; }
        v.o[k]=val(depth+1);
        if (!ok) return v;
      } while (eat(','));
      if (!eat('}')) ok=false;
    } else if (c=='[') {
      p++;
      v.t=JVal::ARR;
      if (eat(']')) return v;
      do { val(depth+1); if (!ok) return v; } while (eat(','));
      if (!eat(']')) ok=false;
    } else if (c=='"') {
      v.t=JVal::STR;
      v.s=str();
    } else if (s.compare(p,4,"true")==0) {
      v.t=JVal::BOOL; v.b=true; p+=4;
    } else if (s.compare(p,5,"false")==0) {
      v.t=JVal::BOOL; v.b=false; p+=5;
    } else if (s.compare(p,4,"null")==0) {
      p+=4;
    } else {
      char* end=NULL;
      v.n=strtod(s.c_str()+p,&end);
      if (end==s.c_str()+p) { ok=false; return v; }
      v.t=JVal::NUM;
      p=end-s.c_str();
    }
    return v;
  }
};
}

bool PRPrefs::fromJson(const std::string& text) {
  JParser jp(text);
  JVal root=jp.val();
  jp.ws();
  if (!jp.ok || jp.p!=text.size() || root.t!=JVal::OBJ) return false;
  PRPrefs out;
  auto chIt=root.o.find("channels");
  if (chIt!=root.o.end() && chIt->second.t==JVal::OBJ) {
    for (auto& c: chIt->second.o) {
      char* end=NULL;
      long ch=strtol(c.first.c_str(),&end,10);
      if (end==c.first.c_str() || *end!=0 || ch<0 || ch>=1024 || c.second.t!=JVal::OBJ) continue;
      PRChanPrefs cp;
      auto f=c.second.o.find("fimPadrao");
      if (f!=c.second.o.end() && f->second.t==JVal::STR) cp.endRel=(f->second.s=="REL");
      f=c.second.o.find("fold");
      if (f!=c.second.o.end() && f->second.t==JVal::BOOL) cp.fold=f->second.b;
      f=c.second.o.find("nomesDeLinha");
      if (f!=c.second.o.end() && f->second.t==JVal::OBJ) {
        for (auto& n: f->second.o) {
          char* e2=NULL;
          long pitch=strtol(n.first.c_str(),&e2,10);
          if (e2==n.first.c_str() || *e2!=0 || pitch<0 || pitch>179 || n.second.t!=JVal::STR) continue;
          cp.rowNames[(int)pitch]=n.second.s;
        }
      }
      out.chans[(int)ch]=cp;
    }
  }
  *this=out;
  return true;
}

std::string prPrefsPath(const std::string& songPath) {
  return songPath+".roll.json";
}

bool prPrefsLoad(PRPrefs& prefs, const std::string& songPath) {
  if (songPath.empty()) return false;
  FILE* f=ps_fopen(prPrefsPath(songPath).c_str(),"rb");
  if (f==NULL) return false;
  std::string text;
  char buf[4096];
  size_t n;
  while ((n=fread(buf,1,sizeof(buf),f))>0) {
    text.append(buf,n);
    if (text.size()>4*1024*1024) break;
  }
  fclose(f);
  return prefs.fromJson(text);
}

bool prPrefsSave(const PRPrefs& prefs, const std::string& songPath) {
  if (songPath.empty()) return false;
  std::string path=prPrefsPath(songPath);
  std::string json=prefs.toJson();
  // write to a temp file first so a crash never leaves a half-written file
  std::string tmp=path+".tmp";
  FILE* f=ps_fopen(tmp.c_str(),"wb");
  if (f==NULL) return false;
  bool ok=fwrite(json.data(),1,json.size(),f)==json.size();
  if (fclose(f)!=0) ok=false;
  if (!ok) { deleteFile(tmp.c_str()); return false; }
  if (fileExists(path.c_str())==1) deleteFile(path.c_str());
  if (!moveFiles(tmp.c_str(),path.c_str())) { deleteFile(tmp.c_str()); return false; }
  return true;
}

#!/usr/bin/env python3
"""Confere as notas do piano roll contra o export de texto do proprio Furnace.

uso: conferir_roll.py <export.txt> <dump.txt> <guiConst.cpp>

export.txt: gerado com `furnace -txtout export.txt musica.fur`
dump.txt:   gerado abrindo a janela Piano Roll com FURNACE_PIANO_ROLL_DUMP=dump.txt
guiConst.cpp: src/gui/guiConst.cpp (tabela de nomes de nota)

A reconstrucao aqui segue a secao 3.2 do plano, escrita de novo em Python a
partir do texto, sem usar nada do modelo C++.
"""
import re
import sys
from collections import defaultdict

OFF, REL, MREL = 253, 254, 255
END = {"song": 0, "next": 1, "off": 2, "rel": 3}


def note_table(guiconst):
    src = open(guiconst, encoding="utf-8").read()
    m = re.search(r"const char\* noteNames\[180\]=\{(.*?)\};", src, re.S)
    names = re.findall(r'"([^"]+)"', m.group(1))
    assert len(names) == 180
    return {n: i for i, n in enumerate(names)}


def parse_export(path):
    lines = open(path, encoding="utf-8").read().split("\n")
    # subsong 0 only
    i = lines.index("## Patterns")
    pat_len = None
    for l in lines:
        mm = re.match(r"- pattern length: (\d+)", l)
        if mm:
            pat_len = int(mm.group(1))
            break
    orders = []  # list of list-of-rows, each row = list of channel cell strings
    cur = None
    for l in lines[i + 1:]:
        if l.startswith("# "):
            break
        if l.startswith("## ") and orders:
            break
        if l.startswith("----- ORDER"):
            cur = []
            orders.append(cur)
            continue
        if cur is not None and re.match(r"^[0-9A-F]{2} \|", l):
            cells = l.split("|")[1:]
            cur.append([c.strip() for c in cells])
    return pat_len, orders


def reconstruct(orders, ntab):
    nch = len(orders[0][0])
    out = defaultdict(list)
    total = 0
    for ch in range(nch):
        notes = []
        cur = None
        last_ins = -1
        r = 0
        for rows in orders:
            for row in rows:
                parts = row[ch].split()
                nn, ins = parts[0], parts[1]
                fx = parts[3:]
                pitch = None
                if nn == "OFF":
                    pitch = OFF
                elif nn == "===":
                    pitch = REL
                elif nn == "REL":
                    pitch = MREL
                elif nn != "...":
                    pitch = ntab[nn]
                ins_v = -1 if ins == ".." else int(ins, 16)

                def fxval(code):
                    for f in fx:
                        if f[:2] == code:
                            v = f[2:]
                            return 0 if v == ".." else int(v, 16)
                    return -1

                if pitch is not None and pitch < 180:
                    if cur:
                        cur["end"], cur["endType"] = r, END["next"]
                        notes.append(cur)
                    cur = dict(start=r, end=0, pitch=pitch,
                               ins=ins_v if ins_v >= 0 else last_ins,
                               insExplicit=1 if ins_v >= 0 else 0,
                               endType=0, macroRel=-1,
                               delay=max(0, fxval("ED")), cutRow=-1, cutTicks=0)
                elif pitch in (OFF, REL):
                    if cur:
                        cur["end"] = r
                        cur["endType"] = END["off"] if pitch == OFF else END["rel"]
                        notes.append(cur)
                        cur = None
                elif pitch == MREL:
                    if cur and cur["macroRel"] < 0:
                        cur["macroRel"] = r
                if ins_v >= 0:
                    last_ins = ins_v
                if cur and cur["cutRow"] < 0:
                    c = fxval("EC")
                    if c > 0:
                        cur["cutRow"], cur["cutTicks"] = r, c
                r += 1
        total = r
        if cur:
            cur["end"], cur["endType"] = total, END["song"]
            notes.append(cur)
        out[ch] = [(n["start"], n["end"], n["pitch"], n["ins"], n["insExplicit"],
                    n["endType"], n["macroRel"], n["delay"], n["cutRow"], n["cutTicks"])
                   for n in notes]
    return nch, out


def parse_dump(path):
    out = defaultdict(list)
    for l in open(path):
        v = [int(x) for x in l.split()]
        out[v[0]].append(tuple(v[1:]))
    return out


def main():
    exp, dump, gc = sys.argv[1:4]
    ntab = note_table(gc)
    pat_len, orders = parse_export(exp)
    nch, ref = reconstruct(orders, ntab)
    got = parse_dump(dump)
    names = sys.argv[4].split(",") if len(sys.argv) > 4 else [str(i) for i in range(nch)]
    bad = 0
    tot = 0
    print(f"orders: {len(orders)}  linhas por pattern: {pat_len}  canais: {nch}")
    for ch in range(nch):
        a, b = ref[ch], got[ch]
        tot += len(a)
        ok = a == b
        nm = names[ch] if ch < len(names) else str(ch)
        print(f"  {nm:<12} texto: {len(a):4d} notas   roll: {len(b):4d} notas   {'OK' if ok else 'DIFERENTE'}")
        if not ok:
            bad += 1
            for x, y in zip(a, b):
                if x != y:
                    print("    primeira diferença: texto", x, " roll", y)
                    break
    print(f"\n{tot} notas conferidas, {bad} canais com diferença")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())

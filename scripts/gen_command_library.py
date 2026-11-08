"""Genere core/sim/CommandLibrary.inc a partir des prereglages Rafale de StreamSim (shared/msfs.js).
Usage : python scripts/gen_command_library.py D:/Repos/StreamDeck/shared/msfs.js
Ne garde que les commandes qu'un contact du doigt peut declencher (interrupteurs, boutons) : pas les molettes ni les selecteurs."""
import re
import sys

src = open(sys.argv[1], encoding="utf-8").read()
src = src[src.index("export const RAFALE_COVERS"):]
entries = []  # (label, type, target, mode)
str_re = r"'((?:[^'\\]|\\.)*)'"
for m in re.finditer(r"\b(rafToggle|rafVar|rafEvent|rafEventToggle|rafFaceToggle|rafPage)\(\s*" + str_re + r"\s*,\s*(\[[^\]]*\]|" + str_re + r")", src):
    kind, label, arg = m.group(1), m.group(2), m.group(3)
    label = label.replace("\\'", "'")
    if kind == "rafToggle":
        entries.append((label, "input", arg.strip("'"), "toggle"))
    elif kind == "rafVar":
        entries.append((label, "var", "L:" + arg.strip("'"), "toggle"))
    elif kind == "rafPage":
        evs = re.findall(r"'([^']+)'", arg)
        entries.append((label, "code", " ".join("(>H:%s)" % e for e in evs), "press"))
    else:
        entries.append((label, "code", "(>H:%s)" % arg.strip("'"), "press"))

seen = set()
out = ["// Genere par scripts/gen_command_library.py depuis StreamSim (shared/msfs.js) : commandes du Rafale declenchables par contact.\n",
       "// { libelle, type, cible, mode }\n"]
for label, typ, target, mode in entries:
    if (typ, target) in seen:
        continue
    seen.add((typ, target))
    out.append('{L"%s", "%s", "%s", "%s"},\n' % (label.replace('"', '\\"'), typ, target, mode))
open("core/sim/CommandLibrary.inc", "w", encoding="utf-8").write("".join(out))
print(len(seen), "commandes")

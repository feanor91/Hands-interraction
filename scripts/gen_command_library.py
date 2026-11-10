"""Genere core/sim/CommandLibrary.inc a partir des prereglages Rafale de StreamSim (shared/msfs.js).
Usage : python scripts/gen_command_library.py D:/Repos/StreamDeck/shared/msfs.js
Chaque entree : { libelle, type de commande (push/toggle/dial), sequence press, sequence inc, sequence dec } (voir core/sim/ZoneControl.h)."""
import re
import sys

src = open(sys.argv[1], encoding="utf-8").read()
src = src[src.index("export const RAFALE_COVERS"):]
S = r"'((?:[^'\\]|\\.)*)'"


def act(typ, mode, value, target, unit="number", mn="", mx="", wrap="0"):
    return "%s|%s|%s|%s|%s|%s|%s|%s" % (typ, mode, value, unit, mn, mx, wrap, target)


def num(v):
    return ("%g" % v)


entries = []  # (label, kind, press, inc, dec, press2, inc2, dec2)

for m in re.finditer(r"\b(rafToggle|rafVar|rafEvent|rafEventToggle|rafFaceToggle|rafPage|rafKnob|rafDial|rafSelector)\(\s*" + S + r"\s*,\s*(\[[^\]]*\]|" + S + r")(.*)", src):
    kind, label, arg, rest = m.group(1), m.group(2).replace("\\'", "'"), m.group(3), m.group(5) or ""
    a = arg.strip("'")
    if kind == "rafToggle":
        entries.append((label, "toggle", act("input", "toggle", 1, a), "", ""))
    elif kind == "rafVar":
        entries.append((label, "toggle", act("var", "toggle", 1, "L:" + a), "", ""))
    elif kind in ("rafEvent", "rafEventToggle", "rafFaceToggle"):
        entries.append((label, "push", act("code", "press", 1, "(>H:%s)" % a), "", ""))
    elif kind == "rafPage":
        evs = re.findall(r"'([^']+)'", arg)
        entries.append((label, "push", act("code", "press", 1, " ".join("(>H:%s)" % e for e in evs)), "", ""))
    elif kind == "rafKnob":
        push = "push: false" not in rest
        press = act("code", "press", 1, "(>H:%s_PUSHED)" % a) if push else ""
        entries.append((label, "dial", press, act("code", "press", 1, "(>H:%s_INC)" % a), act("code", "press", 1, "(>H:%s_DEC)" % a)))
    elif kind == "rafDial":
        mx = re.search(r"max:\s*([0-9.]+)", rest)
        mx = float(mx.group(1)) if mx else 1.0
        step = mx / 20
        entries.append((label, "dial", "", act("var", "add", num(step), "L:" + a, mn="0", mx=num(mx)), act("var", "add", num(-step), "L:" + a, mn="0", mx=num(mx))))
    elif kind == "rafSelector":
        # rafSelector(label, stateVar, [labels], ...) : le 2e argument est la variable, les positions suivent
        labels = re.search(r"\[([^\]]*)\]", rest)
        n = len(re.findall(r"'", labels.group(1))) // 2 if labels else 3
        if labels is None:
            mm = re.search(r"AEC_LABELS", rest)
            n = 4 if mm else 3
        entries.append((label, "dial", "", act("var", "add", 1, "L:" + a, mn="0", mx=str(n - 1)), act("var", "add", -1, "L:" + a, mn="0", mx=str(n - 1))))

# Capot et test court des commandes de vol (codes relevés dans l'Inspector).
MODE, HOOD = "AZP_RAF_HANDLING_FCS_TEST_MODE", "AZP_RAF_HANDLING_FCS_TEST_SWITCH_HOOD"
entries.append(("FCS TEST : capot de protection", "push", act("code", "press", 1, "(L:%s, Enum) 0 == if{ (L:%s, Bool) ! (>L:%s, Bool) }" % (MODE, HOOD, HOOD)), "", ""))
entries.append(("FCS TEST : test court (capot ouvert)", "push",
                act("code", "press", 1, "(L:%s, Bool) if{ 1 (>L:%s, Enum) }" % (HOOD, MODE)) + " >> " + act("delay", "set", 500, "") + " >> " +
                act("code", "press", 1, "0 (>L:%s, Enum)" % MODE), "", ""))

# Commandes ajoutees par AzurPoly (mise a jour de l'avion, https://docs.azurpoly.com/rafale/controls-assignments/#hevents).
H = lambda name: act("code", "press", 1, "(>H:%s)" % name)
entries = [e for e in entries if not e[0].startswith("Molette multifonction")]
entries.append(("Molette multifonction (MFK) : bague + centre", "dial", "", H("AZP_RAF_AVIONICS_MF_KNOB_MODE_INC"), H("AZP_RAF_AVIONICS_MF_KNOB_MODE_DEC"),
                H("AZP_RAF_AVIONICS_MF_KNOB_PUSHED"), H("AZP_RAF_AVIONICS_MF_KNOB_INC"), H("AZP_RAF_AVIONICS_MF_KNOB_DEC")))
for lab, ev in [("Pilote automatique : ALT", "AZP_RAF_AP_ALT_TOGGLE"), ("Pilote automatique : ALT SEL", "AZP_RAF_AP_ALT_SEL_TOGGLE"),
                ("Pilote automatique : FPL", "AZP_RAF_AP_FPL_TOGGLE"), ("Pilote automatique : HOM", "AZP_RAF_AP_HOM_TOGGLE"),
                ("Pilote automatique : ILS", "AZP_RAF_AP_ILS_TOGGLE")]:
    entries.append((lab, "push", H(ev), "", "", "", "", ""))
entries.append(("Commande moteur AEC gauche (cran)", "dial", "", H("AZP_RAF_ENGINE_AEC_LEFT_INC"), H("AZP_RAF_ENGINE_AEC_LEFT_DEC"), "", "", ""))
entries.append(("Commande moteur AEC droite (cran)", "dial", "", H("AZP_RAF_ENGINE_AEC_RIGHT_INC"), H("AZP_RAF_ENGINE_AEC_RIGHT_DEC"), "", "", ""))
# Releves en vol avec le debogueur de MSFS (Behaviors > Mouser) : couvercle de protection de la verriere (1 = ouvert). Le bouton lui-meme n'a pas de commande externe connue.
entries.append(("Verrière : couvercle de protection", "toggle", act("var", "toggle", 1, "L:AZP_RAF_CANOPY_SWITCH_HOOD"), "", ""))
entries = [e + ("",) * (8 - len(e)) for e in entries]

seen = set()
out = ["// Genere par scripts/gen_command_library.py depuis StreamSim (shared/msfs.js) : commandes du Rafale.\n",
       "// { libelle, type de commande, press, inc, dec, press2, inc2, dec2 }\n"]
for label, kind, press, inc, dec, press2, inc2, dec2 in entries:
    key = (kind, press, inc, dec, press2, inc2, dec2)
    if key in seen:
        continue
    seen.add(key)
    esc = lambda t: t.replace("\\", "\\\\").replace('"', '\\"')
    out.append('{L"%s", "%s", "%s", "%s", "%s", "%s", "%s", "%s"},\n' % (esc(label), kind, esc(press), esc(inc), esc(dec), esc(press2), esc(inc2), esc(dec2)))
open("core/sim/CommandLibrary.inc", "w", encoding="utf-8").write("".join(out))
print(len(seen), "commandes :", {k: sum(1 for e in entries if e[1] == k) for k in ("push", "toggle", "dial")})

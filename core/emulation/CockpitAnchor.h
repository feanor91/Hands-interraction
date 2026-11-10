// CockpitAnchor.h — repere du cockpit pour les zones du pochoir (« move cockpit » d'Archan XR).
//
// Les positions des zones sont memorisees RELATIVEMENT a ce repere, et non dans l'espace brut du jeu : quand on recentre la vue dans MSFS
// (touche de recentrage), le cockpit se replace autour de la tete, et il suffit de remettre le repere a la pose de la tete (position et
// lacet, a plat) pour que toutes les zones suivent. Portable, teste.
#pragma once

#include "emulation/PoseMath.h"

namespace hands::emulation {

struct CockpitAnchor {
    Vec3 pos{0, 0, 0};
    Quat q{0, 0, 0, 1};
    bool set = false;  // faux : pas encore de recentrage depuis le lancement (repere = espace brut du jeu)

    // Repere pose a la tete : position de la tete, orientation = lacet seul (axe vertical), sans tangage ni roulis.
    static CockpitAnchor FromHead(const Pose& head) {
        CockpitAnchor a;
        a.pos = head.p;
        const Vec3 fwd = Rotate(head.q, Vec3{0, 0, -1});
        Vec3 fh{fwd.x, 0.0f, fwd.z};
        if (Length(fh) < 0.3f) fh = Vec3{0, 0, -1};  // regard presque vertical : lacet indetermine, on garde l'avant
        a.q = LookAlong(Normalized(fh), Vec3{0, 1, 0});
        a.set = true;
        return a;
    }
    Vec3 ToWorld(Vec3 rel) const { return pos + Rotate(q, rel); }
    Vec3 ToRel(Vec3 world) const { return Rotate(Conjugate(q), world - pos); }
    Quat ToWorldRot(Quat rel) const { return Normalize(Multiply(q, rel)); }
};

}  // namespace hands::emulation

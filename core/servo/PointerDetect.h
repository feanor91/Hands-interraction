// PointerDetect.h — detecte le pointeur de la souris de MSFS (fleche blanche ou main, contour sombre) dans un morceau (ROI) de l'image que le jeu rend
// pour un oeil. Portable (aucune dependance Windows / D3D12), teste sur de vrais recadrages natifs (tests/data).
//
// Principe (valide sur des captures natives de la couche et par des essais de synthese, voir CHANGELOG 0.0.28) :
//   * la plaque de blancheur W = min(R, G, B) ; le remplissage du pointeur est du blanc pur (255) alors que la scene reste <= ~245 ;
//   * graines : pixels W >= tSeed ; regions : composantes 4-connexes de W >= niveau (128, puis 200 / 232 / tSeed si la region deborde dans un fond clair) ;
//   * caracteristiques : aire, boite, elongation, solidite (aire / enveloppe convexe), remplissage, halo sombre (contour noir du pointeur), isolement ;
//   * point chaud = pixel de la region qui minimise x + y (le bout de la fleche, ou le bout de l'index de la main, pointent en haut a gauche) ;
//   * score = produit de termes souples dans [0, 1], porte par un a priori de position facultatif.
// Les coordonnees rendues sont celles du tableau fourni (indice de pixel : le centre du pixel (i, j) est (i + 0.5, j + 0.5)).
#pragma once

#include <cstdint>

namespace hands::servo {

struct DetectParams {
    int tSeed = 246;       // une region doit contenir au moins un pixel de blancheur >= tSeed
    int tFringe = 128;     // niveau le plus bas d'une region (>= 50 % blanc : englobe la frange anticrenelee)
    int tSat = 235;        // pixels « satures » pour la part de saturation
    int aMin = 6;          // aire minimale d'une region
    int aMax = 3000;       // au-dela : panneau / etiquette / ciel
    int sideMin = 3, sideMax = 80;
    int haloRadius = 2;    // le contour sombre se cherche a cette distance (damier) autour de la region
    int haloDark = 90;     // pixel « sombre » : max(R,G,B) <= haloDark
    double haloMinBound = 0.60;  // PORTE : part des pixels du bord de la region ayant un pixel sombre a proximite
    // Contour NOIR net : le curseur de MSFS a un contour noir qui touche le remplissage blanc ; un eclat speculaire sur du metal (verriere, loquet, rails : de loin
    // le principal faux positif) decroit en douceur, sans jamais atteindre le noir contre le blanc. Part des pixels du bord de la region qui ont, a 2 pixels au plus,
    // un pixel dont max(R,G,B) <= outlineDark : curseurs reels 0.72-1.00, eclats mesures 0.00-0.25 (captures du 11 nov. 2026).
    int outlineDark = 30;
    double outlineMin = 0.50;    // PORTE
    double haloRingLo = 0.35, haloRingHi = 0.85, haloFloor = 0.55;
    int peakLo = 244, peakHi = 252;
    double peakFloor = 0.15;
    int isoRadius = 22;
    double solidityGate = 0.95;  // les regions non minuscules (quasi) convexes sont des LED / disques / rectangles
    int tinyArea = 70;           // en dessous, la solidite n'a pas de sens
    bool allowClipped = true;    // regions touchant le bord de la ROI : acceptees mais penalisees
    double clipPenalty = 0.6;
    int maxRegions = 96;         // garde-fou : scene saturee
};

// Reglages de suivi : ROI reduite autour d'un a priori ; graines plus basses, le prior remplace l'indice de saturation.
inline DetectParams TrackingParams() {
    DetectParams p;
    p.tSeed = 205;
    p.peakFloor = 0.6;
    p.aMin = 5;
    p.outlineMin = 0.42;  // contour un peu flou en mouvement : un peu plus tolerant, toujours au-dessus des eclats (<= 0.25)
    return p;
}

struct DetectPrior {
    bool valid = false;
    double x = 0, y = 0;   // position attendue du point chaud (coordonnees de la ROI)
    double sigma = 60.0;   // rayon (1 sigma) en pixels
};

struct PointerCandidate {
    double x = 0, y = 0;          // point chaud (bout du remplissage blanc)
    double score = 0;             // 0..1
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;  // boite (inclusive)
    int area = 0;
    bool hand = false;            // curseur « main » (au-dessus d'un element cliquable) plutot que fleche
    bool clipped = false;         // touche le bord de la ROI
    double peak = 0, solidity = 0, fill = 0, ring = 0, bound = 0, elong = 0, outline = 0;  // diagnostic
};

// `rgba` : octets R, G, B, A par pixel (sRGB), `pitchBytes` : octets par ligne. Renvoie le nombre de candidats ecrits dans `out` (tries par score decroissant,
// au plus `maxOut`) ; 0 = pas de pointeur visible.
int DetectPointer(const uint8_t* rgba, int pitchBytes, int w, int h, const DetectParams& params, const DetectPrior* prior, PointerCandidate* out, int maxOut);

}  // namespace hands::servo

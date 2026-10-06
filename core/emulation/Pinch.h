// Pinch.h — détection du pincement pouce-index avec hystérésis (portable, testable).
//
// Deux seuils distincts évitent le « scintillement » autour d'une valeur unique :
//   - on appuie quand la distance passe SOUS `onDistance` ;
//   - on relâche quand elle repasse AU-DESSUS de `offDistance` (offDistance > onDistance).
// Entre les deux, l'état précédent est conservé.
#pragma once

namespace hands::emulation {

class PinchDetector {
public:
    // `distance` : distance (mètres) entre le bout du pouce et le bout de l'index.
    // Retourne l'état « appuyé » après mise à jour.
    bool Update(float distance, float onDistance, float offDistance) {
        if (offDistance < onDistance) offDistance = onDistance;
        if (pressed_) {
            if (distance > offDistance) pressed_ = false;
        } else {
            if (distance < onDistance) pressed_ = true;
        }
        return pressed_;
    }
    void Reset() { pressed_ = false; }
    bool Pressed() const { return pressed_; }

private:
    bool pressed_ = false;
};

}  // namespace hands::emulation

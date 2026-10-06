#include "TestFramework.h"
#include "emulation/Pinch.h"

using hands::emulation::PinchDetector;

TEST(Pinch_hysteresis_appui_et_relachement) {
    PinchDetector p;
    CHECK(!p.Update(0.060f, 0.022f, 0.035f));  // loin : relâché
    CHECK(!p.Update(0.030f, 0.022f, 0.035f));  // zone intermédiaire : toujours relâché
    CHECK(p.Update(0.020f, 0.022f, 0.035f));   // sous le seuil d'appui : appuyé
    CHECK(p.Update(0.030f, 0.022f, 0.035f));   // zone intermédiaire : reste appuyé
    CHECK(!p.Update(0.040f, 0.022f, 0.035f));  // au-dessus du seuil de relâchement : relâché
}

TEST(Pinch_seuils_inverses_sont_corriges) {
    PinchDetector p;
    CHECK(p.Update(0.010f, 0.030f, 0.020f));  // off < on : off ramené à on
    CHECK(!p.Update(0.031f, 0.030f, 0.020f));
}

TEST(Pinch_Reset) {
    PinchDetector p;
    p.Update(0.0f, 0.02f, 0.03f);
    CHECK(p.Pressed());
    p.Reset();
    CHECK(!p.Pressed());
}

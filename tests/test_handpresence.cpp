#include "TestFramework.h"
#include "emulation/HandPresence.h"

using hands::emulation::HandPresence;

TEST(HandPresence_apparait_tout_de_suite) {
    HandPresence p;
    CHECK(!p.Update(false, 0, 300));
    CHECK(p.Update(true, 10, 300));
    CHECK(p.Present());
}

TEST(HandPresence_maintien_apres_perte) {
    HandPresence p;
    CHECK(p.Update(true, 0, 300));
    CHECK(p.Update(false, 100, 300));   // perte brève : toujours présente (pas de scintillement)
    CHECK(p.Update(false, 300, 300));
    CHECK(!p.Update(false, 301, 300));  // plus de 300 ms sans suivi : absente
    CHECK(!p.Present());
}

TEST(HandPresence_retour_rearme_le_maintien) {
    HandPresence p;
    p.Update(true, 0, 300);
    p.Update(false, 200, 300);
    CHECK(p.Update(true, 250, 300));    // le suivi revient avant la fin du maintien
    CHECK(p.Update(false, 500, 300));   // 250 ms depuis le dernier suivi actif : toujours présente
    CHECK(!p.Update(false, 551, 300));
}

TEST(HandPresence_Reset) {
    HandPresence p;
    p.Update(true, 0, 300);
    p.Reset();
    CHECK(!p.Present());
}

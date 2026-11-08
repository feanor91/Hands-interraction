#include "TestFramework.h"
#include "sim/SimNames.h"

using namespace hands::sim;

TEST(SimNames_evenements) {
    CHECK(IsValidEventName("PARKING_BRAKES"));
    CHECK(IsValidEventName("A32NX.FCU_HDG_PUSH"));
    CHECK(!IsValidEventName("parking_brakes"));  // majuscules exigées (le pont met en majuscules avant)
    CHECK(!IsValidEventName("X"));
    CHECK(!IsValidEventName("AP MASTER"));
}

TEST(SimNames_variables) {
    CHECK(IsValidSimvar("L:AZP_RAF_EFB_ON"));
    CHECK(IsValidSimvar("l:A32NX_X:2"));
    CHECK(IsValidSimvar("GEAR HANDLE POSITION"));
    CHECK(IsValidSimvar("NAV OBS:1"));
    CHECK(!IsValidSimvar("L:"));
    CHECK(!IsValidSimvar("L:a b"));
    CHECK(!IsValidSimvar("gear handle position"));  // la forme canonique est en majuscules
    CHECK(!IsValidSimvar("GEAR:123"));
    CHECK(NormalizeVar("gear handle position") == "GEAR HANDLE POSITION");
    CHECK(NormalizeVar("l:Mon_Var") == "L:Mon_Var");
}

TEST(SimNames_unites_et_inputs) {
    CHECK(IsValidUnit("number"));
    CHECK(IsValidUnit("feet per minute"));
    CHECK(!IsValidUnit(""));
    CHECK(!IsValidUnit("a;b"));
    CHECK(IsValidInputEvent("UNKNOWN_CD_PITOT_COVER"));
    CHECK(IsValidInputEvent("AZP_RAF_ENGINE_AUXILIARY_LEVER_POS:1"));
    CHECK(!IsValidInputEvent("a"));
    CHECK(!IsValidInputEvent("bad name"));
}

TEST(SimNames_nom_de_l_avion) {
    CHECK(AircraftFromPath("SimObjects\\Airplanes\\Asobo_C172\\aircraft.cfg") == "Asobo_C172");
    CHECK(AircraftFromPath("C:\\x\\rafale-c\\config\\aircraft.cfg") == "rafale-c");
    CHECK(AircraftFromPath("C:/x/Diamond_DA62/model/common/aircraft.cfg") == "Diamond_DA62");
    CHECK(AircraftFromPath("pasunchemin") == "pasunchemin");
    CHECK(AircraftFromPath("a\\b\\notes.txt") == "a\\b\\notes.txt");
}

TEST(SimNames_code_avionique) {
    CHECK(NormalizeCode("  (>H:AZP_X)   1  (>L:Y, Boolean)\n") == "(>H:AZP_X) 1 (>L:Y, Boolean)");
    CHECK(!IsCodeTooLong("(>H:X)"));
    CHECK(IsCodeTooLong(std::string(1100, 'a')));
}

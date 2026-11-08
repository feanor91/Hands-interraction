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

#include "sim/ZoneAction.h"

TEST(ZoneAction_requetes) {
    hands::sim::ZoneAction a;
    std::string err;
    CHECK(BuildBridgeRequest(a, &err).empty());  // sans action
    a.type = "event"; a.target = "parking_brakes"; a.value = 1;
    CHECK(BuildBridgeRequest(a, &err) == "event\tPARKING_BRAKES\t1");
    a.type = "var"; a.target = "l:AZP_X"; a.unit = "bool"; a.value = 1; a.mode = "toggle";
    CHECK(BuildBridgeRequest(a, &err) == "togglevar\tL:AZP_X\tbool\t1");
    a.mode = "set"; a.value = 0.5;
    CHECK(BuildBridgeRequest(a, &err) == "setvar\tL:AZP_X\tbool\t0.5");
    a.type = "input"; a.target = "AZP_RAF_SW"; a.mode = "toggle";
    CHECK(BuildBridgeRequest(a, &err) == "toggleinput\tAZP_RAF_SW\t0.5");
    a.type = "code"; a.target = "  (>H:AZP_X)  ";
    CHECK(BuildBridgeRequest(a, &err) == "code\t(>H:AZP_X)");
    a.type = "event"; a.target = "bad name";
    CHECK(BuildBridgeRequest(a, &err).empty() && !err.empty());
    a.type = "zzz";
    CHECK(BuildBridgeRequest(a, &err).empty());
}

#include "sim/ZoneFile.h"

TEST(ZoneFile_aller_retour) {
    hands::sim::ZoneRecord a;
    a.id = "frein_parc"; a.action.type = "event"; a.action.target = "PARKING_BRAKES"; a.action.mode = "press";
    a.hasPos = true; a.x = 0.1234; a.y = -0.5; a.z = 0.75; a.radiusMm = 20;
    hands::sim::ZoneRecord b;
    b.id = "efb"; b.action.type = "var"; b.action.target = "L:AZP_RAF_EFB_ON"; b.action.mode = "toggle"; b.action.unit = "bool"; b.action.value = 1;
    const auto back = hands::sim::ParseZones(hands::sim::SerializeZones({a, b}));
    CHECK(back.size() == 2);
    CHECK(back[0].id == "efb" || back[1].id == "efb");
    for (const auto& r : back) {
        if (r.id == "frein_parc") CHECK(r.hasPos && r.action.target == "PARKING_BRAKES" && r.radiusMm == 20);
        if (r.id == "efb") CHECK(!r.hasPos && r.action.mode == "toggle" && r.action.unit == "bool");
    }
    CHECK(hands::sim::SanitizeZoneId("Frein de Parc.1") == "frein_de_parc1");
}

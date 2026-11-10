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

#include "sim/ZoneFile.h"

using namespace hands::sim;

TEST(ZoneAction_requetes) {
    ZoneAction a;
    std::string err;
    CHECK(BuildBridgeRequest(a, &err).empty());
    a.type = "event"; a.target = "parking_brakes"; a.value = 1;
    CHECK(BuildBridgeRequest(a, &err) == "event\tPARKING_BRAKES\t1");
    a.type = "var"; a.target = "l:AZP_X"; a.unit = "bool"; a.mode = "toggle";
    CHECK(BuildBridgeRequest(a, &err) == "togglevar\tL:AZP_X\tbool\t1");
    a.mode = "add"; a.value = 0.05; a.hasMin = true; a.min = 0; a.hasMax = true; a.max = 1; a.wrap = false;
    CHECK(BuildBridgeRequest(a, &err) == "addvar\tL:AZP_X\tbool\t0.05\t0\t1\t0");
    a.type = "input"; a.target = "AZP_RAF_SW"; a.mode = "add"; a.hasMax = false; a.wrap = true;
    CHECK(BuildBridgeRequest(a, &err) == "addinput\tAZP_RAF_SW\t0.05\t0\t-\t1");
    a.type = "code"; a.target = "  (>H:AZP_X)  ";
    CHECK(BuildBridgeRequest(a, &err) == "code\t(>H:AZP_X)");
    a.type = "event"; a.target = "bad name";
    CHECK(BuildBridgeRequest(a, &err).empty() && !err.empty());
    a.type = "delay";
    CHECK(BuildBridgeRequest(a, &err).empty() && err == "pause");
}

TEST(ZoneControl_sequences) {
    ActionSeq s = ParseSeq("input|toggle|1|number|||0|LANDING_GEAR_PARKINGBRAKE >> delay|set|300|number|||0| >> code|press|1|number|||0|(>H:A) (>L:B, Bool)");
    CHECK(s.size() == 3);
    CHECK(s[0].type == "input" && s[0].mode == "toggle" && s[0].target == "LANDING_GEAR_PARKINGBRAKE");
    CHECK(s[1].type == "delay" && s[1].value == 300);
    CHECK(s[2].type == "code" && s[2].target == "(>H:A) (>L:B, Bool)");
    CHECK(ParseSeq(FormatSeq(s)).size() == 3);
    CHECK(ParseSeq("n'importe quoi").empty());
}

TEST(ZoneFile_aller_retour) {
    ZoneRecord a;
    a.id = "frein_parc";
    a.control.kind = "push";
    a.control.slots["press"] = ParseSeq("event|press|1|number|||0|PARKING_BRAKES");
    a.hasPos = true; a.x = 0.1234; a.y = -0.5; a.z = 0.75; a.geom = {30, 20, 40, 15, 0, 0};
    ZoneRecord b;
    b.id = "aec";
    b.control.kind = "slider"; b.control.sliderMode = "value"; b.control.min = 0; b.control.max = 16383;
    b.control.slots["set"] = ParseSeq("event|set|1|number|||0|THROTTLE_SET");
    b.hasPos = b.hasPos2 = true; b.x = 1; b.y = 2; b.z = 3; b.x2 = 1; b.y2 = 2.2; b.z2 = 3;
    ZoneRecord c;
    c.id = "mode";
    c.control.kind = "switch"; c.control.count = 3; c.control.switchMode = "cycle";
    for (const char* k : {"pos1", "pos2", "pos3"}) c.control.slots[k] = ParseSeq("var|set|1|number|||0|L:X");
    const auto back = ParseZones(SerializeZones({a, b, c}));
    CHECK(back.size() == 3);
    CHECK(back[0].id == "frein_parc" && back[1].id == "aec" && back[2].id == "mode");  // ordre conserve
    CHECK(back[0].hasPos && back[0].control.slots.at("press")[0].target == "PARKING_BRAKES" && back[0].geom.sx == 30 && back[0].geom.sz == 40 && back[0].geom.yaw == 15);
    CHECK(back[1].control.kind == "slider" && back[1].hasPos2 && Complete(back[1]) && !Complete([&] { ZoneRecord r = b; r.hasPos2 = false; return r; }()));
    CHECK(back[2].control.count == 3 && back[2].control.switchMode == "cycle" && !back[2].hasPos);
    CHECK(Validate(back[0].control).empty() && Validate(back[1].control).empty() && Validate(back[2].control).empty());
    CHECK(SanitizeZoneId("Frein de Parc.1") == "frein_de_parc1");
}

TEST(ZoneControl_ancien_format) {
    const auto z = ParseZones("[frein]\naction = event\ntarget = PARKING_BRAKES\nmode = press\nvalue = 1\nx = 1\ny = 2\nz = 3\n");
    CHECK(z.size() == 1 && z[0].control.kind == "push");
    CHECK(z[0].control.slots.at("press").size() == 1 && z[0].control.slots.at("press")[0].target == "PARKING_BRAKES");
}

TEST(ZoneControl_validation) {
    ZoneControl c;
    c.kind = "dial";
    c.slots["inc"] = ParseSeq("code|press|1|number|||0|(>H:X_INC)");
    CHECK(!Validate(c).empty());  // dec manquant
    c.slots["dec"] = ParseSeq("code|press|1|number|||0|(>H:X_DEC)");
    CHECK(Validate(c).empty());
    c.kind = "switch"; c.count = 3;
    CHECK(!Validate(c).empty());
    c.kind = "push";
    CHECK(!Validate(c).empty());  // pas de press
}

TEST(ZoneControl_gestes) {
    int pos, dir;
    NextPosition(3, "bounce", 0, 1, &pos, &dir); CHECK(pos == 1 && dir == 1);
    NextPosition(3, "bounce", 2, 1, &pos, &dir); CHECK(pos == 1 && dir == -1);
    NextPosition(3, "bounce", 0, -1, &pos, &dir); CHECK(pos == 1 && dir == 1);
    NextPosition(3, "cycle", 2, 1, &pos, &dir); CHECK(pos == 0);
    CHECK(PositionOf(37, {0, 25, 37, 50, 63, 75}, 6) == 2);
    CHECK(PositionOf(1.4, {}, 3) == 1 && PositionOf(9, {}, 3) == 2);
    CHECK(LevelToValue(0.5, 0, 16383) == 8192 && LevelToValue(2, 0, 100) == 100);
    CHECK(NotchDelta(0, 1, 10) == 10 && NotchDelta(0.52, 0.48, 10) == 0 && NotchDelta(0.5, 0.2, 10) == -3);
    DialAccumulator d;
    CHECK(d.Feed(10, 25) == 0 && d.Feed(20, 25) == 1 && d.Feed(-60, 25) == -2);
}

TEST(ZoneFile_ancien_rayon_devient_cube) {
    const auto z = ParseZones("[a]\ncontrol = push\npress = event|press|1|number|||0|PARKING_BRAKES\nx = 1\ny = 2\nz = 3\nradius_mm = 20\n");
    CHECK(z.size() == 1 && z[0].geom.sx == 40 && z[0].geom.sy == 40 && z[0].geom.sz == 40);
    double sx, sy, sz;
    DefaultSizeMm("dial", &sx, &sy, &sz);
    CHECK(sx == 50);
}

#include <chrono>
#include <filesystem>
#include <fstream>
#include <thread>

#include "TestFramework.h"
#include "config/Config.h"

using namespace hands::config;

namespace {
std::filesystem::path TempFile(const char* name) {
    return std::filesystem::temp_directory_path() / name;
}
void WriteText(const std::filesystem::path& p, const std::string& s) {
    std::ofstream o(p, std::ios::trunc);
    o << s;
}
}  // namespace

TEST(Config_defauts_si_ini_vide) {
    Config c = Config::FromIni(IniFile::Parse(""));
    CHECK(!c.discoveryMode);
    CHECK_EQ(c.processAllowlist.size(), size_t(1));
    CHECK(c.enableHandTrackingExtension);
    CHECK(c.enabled);
}

TEST(Config_emulation_defauts_et_seuils_corriges) {
    Config d = Config::FromIni(IniFile::Parse(""));
    CHECK(d.emulationEnabled);
    CHECK(d.clickOnMm == 20.0);
    CHECK(d.clickOffMm == 23.0);
    Config c = Config::FromIni(IniFile::Parse("[emulation]\nenabled = false\nclick_on_mm = 30\nclick_off_mm = 10\n"));
    CHECK(!c.emulationEnabled);
    CHECK(c.clickOnMm == 30.0);
    CHECK(c.clickOffMm > c.clickOnMm);
    // anciens noms toujours lus
    CHECK(Config::FromIni(IniFile::Parse("[emulation]\npinch_on_mm = 18\n")).clickOnMm == 18.0);
}

TEST(Config_mains_defauts_et_bornes) {
    Config d = Config::FromIni(IniFile::Parse(""));
    CHECK(d.handsEnabled);
    CHECK(d.handsRenderScale == 0.75);
    Config c = Config::FromIni(IniFile::Parse("[hands]\nenabled = false\nrender_scale = 9\nbone_mm = 0\njoint_scale = 99\n"));
    CHECK(!c.handsEnabled);
    CHECK(c.handsRenderScale == 1.0);
    CHECK(c.handsBoneMm == 1.0);
    CHECK(c.handsJointScale == 5.0);
}

TEST(Config_MatchesProcess_insensible_casse_et_chemin) {
    std::vector<std::string> l{"FlightSimulator2024.exe"};
    CHECK(MatchesProcess(l, "C:\\X\\flightsimulator2024.EXE"));
    CHECK(!MatchesProcess(l, "C:\\X\\FlightSimulator.exe"));
    CHECK(!MatchesProcess({}, "a.exe"));
}

TEST(Config_Reload_garde_ancienne_valeur_si_fichier_absent) {
    auto f = TempFile("hands_cfg_absent.ini");
    std::filesystem::remove(f);
    ConfigStore store(f);
    std::string msg;
    CHECK(!store.Reload(&msg));
    CHECK(!store.Get()->discoveryMode);  // defaut conserve
}

TEST(Config_rechargement_a_chaud) {
    auto f = TempFile("hands_cfg_hot.ini");
    WriteText(f, "[general]\ndiscovery_mode = true\nlog_level = info\n");
    ConfigStore store(f);
    CHECK(store.Reload());
    CHECK(store.Get()->discoveryMode);

    std::atomic<int> notified{0};
    store.StartWatching(std::chrono::milliseconds(50), [&](const Config&, const std::string&) { ++notified; });
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    // Taille differente pour garantir un changement detecte meme si l'horloge du FS est grossiere.
    WriteText(f, "[general]\ndiscovery_mode = false\nlog_level = debug\n# modifie\n");

    bool changed = false;
    for (int i = 0; i < 60 && !changed; ++i) {  // jusqu'a 3 s
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        changed = !store.Get()->discoveryMode;
    }
    store.StopWatching();
    CHECK(changed);
    CHECK(store.Get()->logLevel == hands::log::LogLevel::Debug);
    CHECK(notified.load() >= 1);
}

TEST(Config_click_mode_defaut_et_validation) {
    CHECK_EQ(Config::FromIni(IniFile::Parse("")).clickMode, std::string("any"));
    CHECK_EQ(Config::FromIni(IniFile::Parse("[emulation]\nclick_mode = PINCH\n")).clickMode, std::string("pinch"));
    CHECK_EQ(Config::FromIni(IniFile::Parse("[emulation]\nclick_mode = middle\n")).clickMode, std::string("middle"));
    CHECK_EQ(Config::FromIni(IniFile::Parse("[emulation]\nclick_mode = n_importe_quoi\n")).clickMode, std::string("any"));
    CHECK(!Config::FromIni(IniFile::Parse("")).hideControllerModel);
}

TEST(Config_pointeur_mode_et_bornes) {
    Config d = Config::FromIni(IniFile::Parse(""));
    CHECK_EQ(d.pointerMode, std::string("shoulder"));
    CHECK(d.shoulderSideMm == 180.0 && d.shoulderDropMm == 170.0 && d.shoulderBackMm == 50.0);
    CHECK(d.anchorForwardMm == 70.0);
    CHECK_EQ(d.anchorSource, std::string("fingertip"));
    CHECK_EQ(Config::FromIni(IniFile::Parse("[emulation]\nanchor_source = BONE\n")).anchorSource, std::string("bone"));
    CHECK_EQ(Config::FromIni(IniFile::Parse("[emulation]\nanchor_source = n_importe_quoi\n")).anchorSource, std::string("fingertip"));
    CHECK(Config::FromIni(IniFile::Parse("[emulation]\nanchor_forward_mm = 999\n")).anchorForwardMm == 150.0);
    CHECK_EQ(d.palmAim, std::string("intermediate"));
    CHECK(d.palmFollowHz == 0.6);
    CHECK(d.pointerPitchDeg == 0.0);
    Config c = Config::FromIni(IniFile::Parse("[emulation]\npointer_mode = FINGER\npointer_pitch_deg = 99\npointer_yaw_deg = -99\n"));
    CHECK_EQ(c.pointerMode, std::string("finger"));
    CHECK(c.pointerPitchDeg == 45.0);
    CHECK(c.pointerYawDeg == -45.0);
    CHECK_EQ(Config::FromIni(IniFile::Parse("[emulation]\npointer_mode = bizarre\n")).pointerMode, std::string("shoulder"));
    CHECK_EQ(Config::FromIni(IniFile::Parse("[emulation]\npointer_mode = contact\n")).pointerMode, std::string("contact"));
    CHECK(Config::FromIni(IniFile::Parse("[emulation]\nshoulder_side_mm = 5\n")).shoulderSideMm == 50.0);
    CHECK(Config::FromIni(IniFile::Parse("[emulation]\nshoulder_drop_mm = 9999\n")).shoulderDropMm == 500.0);
    CHECK_EQ(Config::FromIni(IniFile::Parse("[emulation]\npointer_mode = palm\n")).pointerMode, std::string("palm"));
    CHECK_EQ(Config::FromIni(IniFile::Parse("[emulation]\npointer_mode = hand\n")).pointerMode, std::string("hand"));
    CHECK_EQ(Config::FromIni(IniFile::Parse("[emulation]\npalm_aim = TIP\n")).palmAim, std::string("tip"));
    CHECK_EQ(Config::FromIni(IniFile::Parse("[emulation]\npalm_aim = n_importe_quoi\n")).palmAim, std::string("intermediate"));
    CHECK(Config::FromIni(IniFile::Parse("[emulation]\npalm_follow_hz = 99\n")).palmFollowHz == 5.0);
}

TEST(Config_clic_accepte_les_positions_estimees_par_defaut) {
    CHECK(!Config::FromIni(IniFile::Parse("")).clickRequireTracked);
    CHECK(Config::FromIni(IniFile::Parse("[emulation]\nclick_require_tracked = true\n")).clickRequireTracked);
}

TEST(Config_doigt_du_pointeur) {
    CHECK_EQ(Config::FromIni(IniFile::Parse("")).pointerFinger, std::string("index"));
    CHECK_EQ(Config::FromIni(IniFile::Parse("[emulation]\npointer_finger = middle\n")).pointerFinger, std::string("middle"));
    CHECK_EQ(Config::FromIni(IniFile::Parse("[emulation]\npointer_finger = MAJEUR\n")).pointerFinger, std::string("middle"));
    CHECK_EQ(Config::FromIni(IniFile::Parse("[emulation]\npointer_finger = pouce\n")).pointerFinger, std::string("index"));
}

TEST(Config_melange_doigt_et_torse) {
    Config d = Config::FromIni(IniFile::Parse(""));
    CHECK(d.fingerBlend);
    CHECK(d.fingerBlendStartDeg == 30.0 && d.fingerBlendEndDeg == 70.0);
    CHECK(d.torsoFollowS == 3.0);
    Config c = Config::FromIni(IniFile::Parse("[emulation]\nfinger_blend = false\nfinger_blend_start_deg = 50\nfinger_blend_end_deg = 10\ntorso_follow_s = 999\n"));
    CHECK(!c.fingerBlend);
    CHECK(c.fingerBlendEndDeg >= c.fingerBlendStartDeg + 5.0);
    CHECK(c.torsoFollowS == 30.0);
}

TEST(Config_menu_dans_le_casque) {
    Config d = Config::FromIni(IniFile::Parse(""));
    CHECK(d.menuEnabled);
    CHECK_EQ(d.menuHoldMs, 1200);
    Config c = Config::FromIni(IniFile::Parse("[menu]\nenabled = false\nopen_hold_ms = 10\n"));
    CHECK(!c.menuEnabled);
    CHECK_EQ(c.menuHoldMs, 400);
    CHECK_EQ(Config::FromIni(IniFile::Parse("[menu]\nopen_hold_ms = 99999\n")).menuHoldMs, 5000);
}

TEST(Config_captures) {
    Config d = Config::FromIni(IniFile::Parse(""));
    CHECK_EQ(d.captureMax, 16);
    CHECK_EQ(d.captureIntervalMs, 3000);
    CHECK_EQ(d.colorState, std::string("render_target"));
    Config c = Config::FromIni(IniFile::Parse("[touch]\ncapture_max = 999\ncapture_interval_ms = 1\ncolor_state = n_importe_quoi\n"));
    CHECK_EQ(c.captureMax, 100);
    CHECK_EQ(c.captureIntervalMs, 500);
    CHECK_EQ(c.colorState, std::string("render_target"));
    CHECK_EQ(Config::FromIni(IniFile::Parse("[touch]\ncapture_max = 0\n")).captureMax, 0);
}

TEST(Config_sonde_de_profondeur) {
    Config d = Config::FromIni(IniFile::Parse(""));
    CHECK(d.depthProbe);
    CHECK_EQ(d.depthProbeIntervalMs, 500);
    CHECK_EQ(d.depthProbeState, std::string("depth_write"));
    Config c = Config::FromIni(IniFile::Parse("[touch]\ndepth_probe = false\nprobe_interval_ms = 5\ndepth_state = SHADER\n"));
    CHECK(!c.depthProbe);
    CHECK_EQ(c.depthProbeIntervalMs, 100);
    CHECK_EQ(c.depthProbeState, std::string("shader"));
    CHECK_EQ(Config::FromIni(IniFile::Parse("[touch]\ndepth_state = n_importe_quoi\n")).depthProbeState, std::string("depth_write"));
}

TEST(Config_pointeur_partage) {
    Config d = Config::FromIni(IniFile::Parse(""));
    CHECK(!d.sharedPointer);
    CHECK(d.engageMm == 34.0);
    Config c = Config::FromIni(IniFile::Parse("[emulation]\nshared_pointer = true\nengage_mm = 5\n"));
    CHECK(c.sharedPointer);
    CHECK(c.engageMm == 22.0);
    CHECK(Config::FromIni(IniFile::Parse("[emulation]\nengage_mm = 500\n")).engageMm == 80.0);
}

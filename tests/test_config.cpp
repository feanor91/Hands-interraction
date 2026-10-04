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
    CHECK(c.discoveryMode);
    CHECK_EQ(c.processAllowlist.size(), size_t(1));
    CHECK(c.enableHandTrackingExtension);
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
    CHECK(store.Get()->discoveryMode);  // defaut conserve
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

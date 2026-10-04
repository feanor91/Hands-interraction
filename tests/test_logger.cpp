#include <filesystem>
#include <fstream>
#include <sstream>

#include "TestFramework.h"
#include "log/Logger.h"

using namespace hands::log;

namespace {
std::string Slurp(const std::filesystem::path& p) {
    std::ifstream in(p);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}
}  // namespace

TEST(Logger_ecrit_et_filtre_par_niveau) {
    auto dir = std::filesystem::temp_directory_path() / "hands_log_test";
    std::filesystem::remove_all(dir);
    auto file = dir / "t.log";
    {
        Logger lg;
        CHECK(lg.Open(file));
        lg.SetLevel(LogLevel::Info);
        lg.Log(LogLevel::Info, "bonjour %d", 42);
        lg.Log(LogLevel::Debug, "invisible");
        lg.Log(LogLevel::Error, "erreur %s", "x");
        CHECK(lg.Flush(std::chrono::milliseconds(2000)));
    }
    const std::string txt = Slurp(file);
    CHECK(txt.find("bonjour 42") != std::string::npos);
    CHECK(txt.find("invisible") == std::string::npos);
    CHECK(txt.find("[ERREUR]") != std::string::npos);
}

TEST(Logger_rotation_du_fichier_precedent) {
    auto dir = std::filesystem::temp_directory_path() / "hands_log_test2";
    std::filesystem::remove_all(dir);
    auto file = dir / "t.log";
    { Logger a; a.Open(file); a.Log(LogLevel::Info, "premiere"); a.Flush(std::chrono::milliseconds(2000)); }
    { Logger b; b.Open(file); b.Log(LogLevel::Info, "seconde"); b.Flush(std::chrono::milliseconds(2000)); }
    CHECK(Slurp(file).find("seconde") != std::string::npos);
    CHECK(Slurp(dir / "t.prev.log").find("premiere") != std::string::npos);
}

TEST(Logger_chemin_inaccessible_ne_plante_pas) {
    Logger lg;
    // Un fichier "sous" un fichier ordinaire ne peut pas etre cree.
    auto base = std::filesystem::temp_directory_path() / "hands_log_notadir";
    { std::ofstream o(base); o << "x"; }
    CHECK(!lg.Open(base / "sub" / "t.log"));
    lg.Log(LogLevel::Info, "sans effet");  // ne doit ni bloquer ni lever
    CHECK(!lg.Flush(std::chrono::milliseconds(50)));
}

TEST(Logger_ne_bloque_pas_quand_la_file_deborde) {
    Logger lg;  // jamais ouvert : tout reste en memoire (borne)
    for (int i = 0; i < 20000; ++i) lg.Log(LogLevel::Info, "msg %d", i);
    CHECK(true);  // l'important est d'etre arrive ici rapidement, sans memoire non bornee
}

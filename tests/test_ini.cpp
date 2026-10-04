#include "TestFramework.h"
#include "config/IniFile.h"

using hands::config::IniFile;

TEST(Ini_lecture_basique_et_casse) {
    const char* txt =
        "; commentaire\n"
        "[General]\n"
        "Nom = Valeur avec espaces   ; fin de ligne\n"
        "flag = YES\n"
        "n = 42\n"
        "x = 0.25\n"
        "liste = a, b ,c\n"
        "[autre]\n"
        "n = 7\n";
    std::vector<std::string> w;
    IniFile ini = IniFile::Parse(txt, &w);
    CHECK(w.empty());
    CHECK_EQ(ini.GetString("GENERAL", "nom", ""), std::string("Valeur avec espaces"));
    CHECK(ini.GetBool("general", "flag", false));
    CHECK_EQ(ini.GetInt("general", "n", 0), 42);
    CHECK_EQ(ini.GetInt("autre", "n", 0), 7);
    CHECK(ini.GetDouble("general", "x", 0) == 0.25);
    CHECK_EQ(ini.GetList("general", "liste", {}).size(), size_t(3));
}

TEST(Ini_valeurs_invalides_gardent_le_defaut) {
    IniFile ini = IniFile::Parse("[s]\nn = abc\nb = peutetre\nx = 1,5\nn2 = 12abc\n");
    CHECK_EQ(ini.GetInt("s", "n", 5), 5);
    CHECK_EQ(ini.GetInt("s", "n2", 5), 5);
    CHECK_EQ(ini.GetBool("s", "b", true), true);
    CHECK(ini.GetDouble("s", "x", 9.0) == 9.0);  // virgule refusee
    CHECK_EQ(ini.GetInt("s", "absent", 3), 3);
}

TEST(Ini_lignes_mal_formees_signalees_sans_exception) {
    std::vector<std::string> w;
    IniFile ini = IniFile::Parse("[ouverte\nsans_egal\n= valeur\n[ok]\nk=v\n", &w);
    CHECK_EQ(w.size(), size_t(3));
    CHECK_EQ(ini.GetString("ok", "k", ""), std::string("v"));
}

TEST(Ini_bom_utf8_et_fin_de_ligne_windows) {
    IniFile ini = IniFile::Parse("\xEF\xBB\xBF[s]\r\nk = v\r\n");
    CHECK_EQ(ini.GetString("s", "k", ""), std::string("v"));
}

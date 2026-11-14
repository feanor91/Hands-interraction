#include "TestFramework.h"
#include "config/IniEdit.h"
#include "config/IniFile.h"

using namespace hands::config;

TEST(IniEdit_remplace_valeur_existante_et_garde_le_reste) {
    const std::string in = "; haut\n[general]\nenabled = true\nlog_level = info\n[probe]\nenabled = autre\n";
    const std::string out = IniSetValue(in, "GENERAL", "ENABLED", "false");
    CHECK_EQ(out, std::string("; haut\n[general]\nENABLED = false\nlog_level = info\n[probe]\nenabled = autre\n"));
}

TEST(IniEdit_ajoute_la_cle_en_fin_de_section) {
    const std::string in = "[general]\nlog_level = info\n\n[probe]\nx = 1\n";
    const std::string out = IniSetValue(in, "general", "enabled", "true");
    CHECK_EQ(out, std::string("[general]\nlog_level = info\nenabled = true\n\n[probe]\nx = 1\n"));
    CHECK(IniFile::Parse(out).GetBool("general", "enabled", false));
}

TEST(IniEdit_cree_la_section_et_respecte_crlf) {
    const std::string out = IniSetValue("[probe]\r\nx = 1", "general", "enabled", "false");
    CHECK_EQ(out, std::string("[probe]\r\nx = 1\r\n\r\n[general]\r\nenabled = false\r\n"));
    CHECK_EQ(IniSetValue("", "general", "enabled", "true"), std::string("[general]\nenabled = true\n"));
}

TEST(IniEdit_ignore_les_commentaires_qui_ressemblent_a_la_cle) {
    const std::string out = IniSetValue("[general]\n; enabled = true\n", "general", "enabled", "false");
    CHECK_EQ(out, std::string("[general]\n; enabled = true\nenabled = false\n"));
}

TEST(IniMerge_ajoute_les_cles_manquantes_avec_leur_commentaire_sans_toucher_aux_valeurs) {
    const std::string tmpl =
        "; en-tete\n[hands]\n; style des mains\nstyle = skeleton\n; portee\n; plage 0.5 a 2.5\nreach_scale = 1.0\nreach_scale_up = 1.0\n\n[menu]\nx = 1\n";
    const std::string user = "[hands]\nstyle = ghost\n\n[menu]\nx = 5\n";
    std::vector<std::string> added;
    const std::string out = IniMergeMissing(user, tmpl, &added);
    CHECK_EQ(out, std::string("[hands]\nstyle = ghost\n; portee\n; plage 0.5 a 2.5\nreach_scale = 1.0\nreach_scale_up = 1.0\n\n[menu]\nx = 5\n"));
    CHECK_EQ(added.size(), static_cast<size_t>(2));
    CHECK_EQ(added[0], std::string("hands.reach_scale"));
    CHECK_EQ(IniFile::Parse(out).GetString("hands", "style", ""), std::string("ghost"));
}

TEST(IniMerge_rien_ne_manque_fichier_inchange) {
    const std::string user = "[hands]\r\nSTYLE = ghost\r\n; commentaire perso\r\n";
    std::vector<std::string> added;
    CHECK_EQ(IniMergeMissing(user, "[hands]\n; c\nstyle = skeleton\n", &added), user);
    CHECK(added.empty());
}

TEST(IniMerge_section_manquante_ajoutee_entiere_en_crlf) {
    const std::string out = IniMergeMissing("[a]\r\nx = 1", "[a]\nx = 2\n\n[b]\n; doc\ny = 3\n\n");
    CHECK_EQ(out, std::string("[a]\r\nx = 1\r\n\r\n[b]\r\n; doc\r\ny = 3\r\n"));
}

TEST(IniMerge_cle_mise_en_commentaire_par_l_utilisateur_pas_rajoutee) {
    const std::string user = "[hands]\n;reach_scale = 2.0\n";
    CHECK_EQ(IniMergeMissing(user, "[hands]\nreach_scale = 1.0\n"), user);
}

TEST(IniMerge_insertion_dans_plusieurs_sections) {
    const std::string out = IniMergeMissing("[a]\nx = 1\n[b]\ny = 1\n", "[a]\nx = 0\nx2 = 0\n[b]\ny = 0\ny2 = 0\n");
    CHECK_EQ(out, std::string("[a]\nx = 1\nx2 = 0\n[b]\ny = 1\ny2 = 0\n"));
}

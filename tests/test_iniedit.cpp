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

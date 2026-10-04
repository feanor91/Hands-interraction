// IniFile.h — lecteur de fichiers INI minimal (aucune dépendance).
//
// Format accepté :
//     ; commentaire     # commentaire
//     [section]
//     cle = valeur      ; commentaire de fin de ligne accepté
//
// Les noms de sections et de clés ne sont pas sensibles à la casse.
// Un fichier mal formé ne lève JAMAIS d'exception : les lignes invalides sont
// ignorées et décrites dans la liste d'avertissements (utile pour le log).
#pragma once

#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace hands::config {

class IniFile {
public:
    // Analyse le texte complet d'un fichier. `warnings` (facultatif) reçoit un
    // message par ligne ignorée.
    // NOTE C++ : `std::vector<std::string>*` est un POINTEUR possiblement nul
    // (nullptr). C'est l'idiome « paramètre de sortie facultatif ».
    static IniFile Parse(std::string_view text, std::vector<std::string>* warnings = nullptr);

    bool Has(std::string_view section, std::string_view key) const;

    std::string GetString(std::string_view section, std::string_view key, std::string_view def) const;
    // Booléens acceptés : true/false, yes/no, on/off, 1/0.
    bool GetBool(std::string_view section, std::string_view key, bool def) const;
    int GetInt(std::string_view section, std::string_view key, int def) const;
    double GetDouble(std::string_view section, std::string_view key, double def) const;
    // Liste séparée par des virgules.
    std::vector<std::string> GetList(std::string_view section, std::string_view key,
                                     const std::vector<std::string>& def) const;

private:
    static std::string MakeKey(std::string_view section, std::string_view key);
    // Clé interne : "section.cle" en minuscules.
    std::map<std::string, std::string> values_;
};

}  // namespace hands::config

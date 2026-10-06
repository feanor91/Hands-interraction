// IniEdit.h — modification ciblée d'une valeur dans le TEXTE d'un fichier INI.
//
// Sert à l'application de la barre des tâches (basculer `enabled`) : on ne
// réécrit que la ligne concernée, les commentaires et le reste sont conservés.
#pragma once

#include <string>
#include <string_view>

namespace hands::config {

// Retourne `text` où `section`/`key` vaut `value` :
//  - la clé existe dans la section : sa ligne est remplacée (commentaire de fin de ligne perdu) ;
//  - sinon elle est ajoutée à la fin de la section ;
//  - si la section n'existe pas, elle est créée en fin de fichier.
// Les fins de ligne du fichier (\n ou \r\n) sont respectées.
std::string IniSetValue(std::string_view text, std::string_view section, std::string_view key, std::string_view value);

}  // namespace hands::config

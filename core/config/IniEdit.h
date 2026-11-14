// IniEdit.h — modification ciblée d'une valeur dans le TEXTE d'un fichier INI.
//
// Sert à l'application de la barre des tâches (basculer `enabled`) : on ne
// réécrit que la ligne concernée, les commentaires et le reste sont conservés.
#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace hands::config {

// Retourne `text` où `section`/`key` vaut `value` :
//  - la clé existe dans la section : sa ligne est remplacée (commentaire de fin de ligne perdu) ;
//  - sinon elle est ajoutée à la fin de la section ;
//  - si la section n'existe pas, elle est créée en fin de fichier.
// Les fins de ligne du fichier (\n ou \r\n) sont respectées.
std::string IniSetValue(std::string_view text, std::string_view section, std::string_view key, std::string_view value);

// Mise a jour : ajoute a `user` les cles de `tmpl` (le hands.ini livre) qui lui manquent, sans jamais toucher une valeur existante.
//  - une cle manquante est ajoutee en fin de sa section, precedee des lignes de commentaire qui la precedent dans `tmpl` ;
//  - une section manquante est ajoutee entiere en fin de fichier ;
//  - une cle presente en commentaire dans `user` (« ; cle = ... ») est un choix de l'utilisateur : pas ajoutee ;
//  - les cles hors section (avant le premier [..]) sont ignorees ; fins de ligne de `user` respectees.
// `added` (optionnel) recoit « section.cle » pour chaque cle ajoutee. Retourne `user` inchange s'il ne manque rien.
std::string IniMergeMissing(std::string_view user, std::string_view tmpl, std::vector<std::string>* added = nullptr);

}  // namespace hands::config

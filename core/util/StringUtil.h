// StringUtil.h — petits utilitaires de chaînes, sans dépendance.
//
// NOTE C++ (pour un développeur C#) :
//  * `std::string` est une chaîne MUTABLE d'octets (pas d'UTF-16 comme en C#).
//    Dans tout ce projet, les chaînes sont de l'UTF-8.
//  * `std::string_view` est une « vue » en lecture seule sur des caractères
//    qui appartiennent à quelqu'un d'autre (comparable à ReadOnlySpan<char>) :
//    aucune copie, mais il ne faut pas la garder plus longtemps que la chaîne
//    d'origine.
//  * `inline` dans un en-tête : la fonction peut être définie dans plusieurs
//    fichiers .cpp sans erreur d'édition de liens (équivalent d'une méthode
//    statique d'une classe utilitaire).
#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace hands::util {

// Minuscules ASCII uniquement (suffisant pour des noms d'exe et des clés INI).
std::string ToLower(std::string_view s);

// Retire les espaces / tabulations / retours chariot en début et fin.
std::string_view Trim(std::string_view s);

// Découpe sur un séparateur, retire les espaces autour de chaque élément et
// ignore les éléments vides. "a, b,,c" -> {"a","b","c"}.
std::vector<std::string> SplitList(std::string_view s, char sep = ',');

// Compare sans tenir compte de la casse (ASCII).
bool EqualsIgnoreCase(std::string_view a, std::string_view b);

// Extrait le nom de fichier d'un chemin Windows ou POSIX :
// "C:\\Games\\X\\Foo.exe" -> "Foo.exe".
std::string FileNameOf(std::string_view path);

}  // namespace hands::util

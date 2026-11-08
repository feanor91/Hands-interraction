// bridge_client.h — client du tube nomme de HandsBridge.exe (pont SimConnect resident). Envoi asynchrone : le rendu n'attend jamais.
#pragma once

#include <string>

namespace layer {

// Envoie une requete (voir core/sim/ZoneAction.h) au pont sur un fil a part et journalise la reponse. `label` sert aux messages du journal.
void Bridge_Send(const std::string& request, const std::string& label);

// Requete bloquante (a n'appeler que depuis un fil a part) : renvoie la reponse du pont, ou "" s'il est injoignable.
std::string Bridge_Call(const std::string& request, unsigned timeoutMs);

}  // namespace layer

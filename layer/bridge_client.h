// bridge_client.h — client du tube nomme de HandsBridge.exe (pont SimConnect resident).
// Les envois passent par UN fil de travail (file d'attente) : le rendu n'attend jamais, et les sequences gardent leur ordre et leurs pauses.
#pragma once

#include <functional>
#include <string>

#include "sim/ZoneControl.h"

namespace layer {

// Requete bloquante (a n'appeler que depuis un fil a part ou une tache de la file) : reponse du pont, ou "" s'il est injoignable.
std::string Bridge_Call(const std::string& request, unsigned timeoutMs);

// Lit une valeur numerique : `request` = « getvar... » ou « getinput... ». Faux si le pont ou le simulateur ne repond pas.
bool Bridge_ReadNumber(const std::string& request, double* out);

// Ajoute une tache a la file (executee dans l'ordre sur le fil de travail). Faux si la file est pleine (tache ignoree).
bool Bridge_Post(std::function<void()> task);

// Execute une sequence d'actions (pauses comprises) sur le fil de travail ; journalise le resultat sous `label`.
bool Bridge_RunSeq(const hands::sim::ActionSeq& seq, const std::string& label);

// Version synchrone pour une tache deja sur le fil de travail.
void Bridge_RunSeqNow(const hands::sim::ActionSeq& seq, const std::string& label);

}  // namespace layer

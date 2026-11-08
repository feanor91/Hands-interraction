# Pont SimConnect (liaison avec MSFS) : bibliothèque réutilisable par la tray, et outil en ligne de commande pour l'essayer.
# Demande le SDK SimConnect de MSFS (bibliothèque statique) ; sans lui, la cible est simplement ignorée.
set(HANDS_SIMCONNECT_SDK "C:/MSFS 2024 SDK/SimConnect SDK" CACHE PATH "Dossier SimConnect SDK de MSFS (contient include/ et lib/static/)")
if(EXISTS "${HANDS_SIMCONNECT_SDK}/include/SimConnect.h" AND EXISTS "${HANDS_SIMCONNECT_SDK}/lib/static/SimConnect.lib")
  # La bibliotheque statique SimConnect du SDK est compilee avec la CRT dynamique (/MD) : ces cibles-la (et elles seules) ne suivent pas la regle
  # /MT du reste du projet. Elles n'utilisent donc pas hands_core (en /MT) : seul l'en-tete portable core/sim/SimNames.h leur sert.
  add_library(hands_simbridge STATIC ../win/SimBridge.cpp)
  target_compile_definitions(hands_simbridge PUBLIC WIN32_LEAN_AND_MEAN NOMINMAX)
  target_compile_options(hands_simbridge PRIVATE /wd4245)  # avertissements dans SimConnect.h (en-tete du SDK)
  target_include_directories(hands_simbridge PUBLIC ../win ../core "${HANDS_SIMCONNECT_SDK}/include")
  target_link_libraries(hands_simbridge PUBLIC "${HANDS_SIMCONNECT_SDK}/lib/static/SimConnect.lib" ws2_32 shlwapi)
  set_target_properties(hands_simbridge PROPERTIES MSVC_RUNTIME_LIBRARY "MultiThreadedDLL")

  add_executable(simbridge_cli simbridge_cli.cpp)
  target_link_libraries(simbridge_cli PRIVATE hands_simbridge)
  set_target_properties(simbridge_cli PROPERTIES MSVC_RUNTIME_LIBRARY "MultiThreadedDLL")
else()
  message(STATUS "SDK SimConnect introuvable (${HANDS_SIMCONNECT_SDK}) : pont SimConnect non compile.")
endif()

# Couche OpenXR : DLL 64 bits. Nom de DLL = nom de couche (cohérent avec le manifeste).
add_library(XR_APILAYER_HANDS_bare SHARED
  entry.cpp
  layer.cpp
  hooks_probe.cpp)

# XR_NO_PROTOTYPES : on n'appelle JAMAIS les fonctions xr* directement (donc pas
# de dépendance à openxr_loader.lib) ; on passe par les pointeurs de la couche suivante.
target_compile_definitions(XR_APILAYER_HANDS_bare PRIVATE XR_NO_PROTOTYPES WIN32_LEAN_AND_MEAN NOMINMAX)
target_include_directories(XR_APILAYER_HANDS_bare PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_SOURCE_DIR}/third_party/openxr/include)
target_link_libraries(XR_APILAYER_HANDS_bare PRIVATE hands_core)
set_target_properties(XR_APILAYER_HANDS_bare PROPERTIES PREFIX "")

# Rassemble DLL + manifeste + config exemple dans un seul dossier (celui de la DLL),
# prêt pour scripts/install.ps1.
add_custom_command(TARGET XR_APILAYER_HANDS_bare POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          ${CMAKE_SOURCE_DIR}/manifest/XR_APILAYER_HANDS_bare.json
          ${CMAKE_SOURCE_DIR}/config/hands.ini
          $<TARGET_FILE_DIR:XR_APILAYER_HANDS_bare>)

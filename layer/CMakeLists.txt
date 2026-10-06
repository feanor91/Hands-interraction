# Couche OpenXR : DLL 64 bits. Nom de DLL = nom de couche (cohérent avec le manifeste).
add_library(XR_APILAYER_HANDS_bare SHARED
  entry.cpp
  layer.cpp
  hooks_probe.cpp
  emulate.cpp
  hand_renderer.cpp
  hands_overlay.cpp)

# XR_NO_PROTOTYPES : on n'appelle JAMAIS les fonctions xr* directement (donc pas
# de dépendance à openxr_loader.lib) ; on passe par les pointeurs de la couche suivante.
target_compile_definitions(XR_APILAYER_HANDS_bare PRIVATE XR_NO_PROTOTYPES WIN32_LEAN_AND_MEAN NOMINMAX)
target_include_directories(XR_APILAYER_HANDS_bare PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_SOURCE_DIR}/third_party/openxr/include)
target_link_libraries(XR_APILAYER_HANDS_bare PRIVATE hands_core d3d12)
set_target_properties(XR_APILAYER_HANDS_bare PROPERTIES PREFIX "")

# Rassemble DLL + manifeste + config exemple dans un seul dossier (celui de la DLL),
# prêt pour scripts/install.ps1.
add_custom_command(TARGET XR_APILAYER_HANDS_bare POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          ${CMAKE_SOURCE_DIR}/manifest/XR_APILAYER_HANDS_bare.json
          ${CMAKE_SOURCE_DIR}/config/hands.ini
          $<TARGET_FILE_DIR:XR_APILAYER_HANDS_bare>)

# --- Rendu des mains (étape 3a) : test autonome sans OpenXR, rend dans une texture et la relit -------------
add_executable(hand_renderer_test ../tests/hand_renderer_test.cpp hand_renderer.cpp)
target_include_directories(hand_renderer_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(hand_renderer_test PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_libraries(hand_renderer_test PRIVATE hands_core d3d12 dxgi)
if(HANDS_BUILD_TESTS AND NOT CMAKE_CROSSCOMPILING)
  add_test(NAME hand_renderer_test COMMAND hand_renderer_test ${CMAKE_BINARY_DIR}/hand_renderer_test.bmp)
endif()

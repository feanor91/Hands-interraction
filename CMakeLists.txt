cmake_minimum_required(VERSION 3.20)
project(HandsInteraction LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Runtime C++ lié statiquement (/MT) : la DLL est chargée dans le processus du
# jeu, on évite ainsi toute dépendance à la redistribuable Visual C++.
if(MSVC)
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
  add_compile_options(/W4 /permissive- /utf-8)
else()
  # -Wno-missing-field-initializers : les structures OpenXR s'initialisent par `{XR_TYPE_xxx}` (le reste à zéro).
  add_compile_options(-Wall -Wextra -Wno-missing-field-initializers)
endif()

# --- Cœur portable (compilable et testable sous Windows, Linux, ...) ---------
add_library(hands_core STATIC
  core/util/StringUtil.cpp
  core/config/IniFile.cpp
  core/config/Config.cpp
  core/log/Logger.cpp)
target_include_directories(hands_core PUBLIC core)
find_package(Threads REQUIRED)
target_link_libraries(hands_core PUBLIC Threads::Threads)

# --- Tests unitaires du cœur -------------------------------------------------
option(HANDS_BUILD_TESTS "Compiler les tests unitaires" ON)
if(HANDS_BUILD_TESTS AND NOT CMAKE_CROSSCOMPILING)
  enable_testing()
  add_subdirectory(tests)
endif()

# --- Couche OpenXR (Windows uniquement) --------------------------------------
if(WIN32)
  add_subdirectory(layer)
endif()

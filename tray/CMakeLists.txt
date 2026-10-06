# Application de la barre des tâches (Windows) : interrupteur de la couche, installation, journaux.
add_executable(HandsTray WIN32 HandsTray.cpp)
target_compile_definitions(HandsTray PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(HandsTray PRIVATE hands_core user32 gdi32 shell32 advapi32)

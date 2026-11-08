# Application de la barre des tâches (Windows) : interrupteur de la couche, installation, journaux.
add_executable(HandsTray WIN32 HandsTray.cpp ../win/JoyPoll.cpp)
target_compile_definitions(HandsTray PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_include_directories(HandsTray PRIVATE ${CMAKE_SOURCE_DIR}/win)
target_link_libraries(HandsTray PRIVATE hands_core user32 gdi32 shell32 advapi32 dinput8 dxguid)

# Application de la barre des tâches (Windows) : interrupteur de la couche, installation, journaux.
add_executable(HandsTray WIN32 HandsTray.cpp ../win/JoyPoll.cpp)
target_compile_definitions(HandsTray PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_include_directories(HandsTray PRIVATE ${CMAKE_SOURCE_DIR}/win)
target_link_libraries(HandsTray PRIVATE hands_core user32 gdi32 shell32 advapi32 dinput8 dxguid)

# Le pont SimConnect (compile a part en /MD) est lance par la tray : on le copie a cote d'elle.
if(TARGET HandsBridge)
  add_dependencies(HandsTray HandsBridge)
  add_custom_command(TARGET HandsTray POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:HandsBridge> $<TARGET_FILE_DIR:HandsTray>)
endif()

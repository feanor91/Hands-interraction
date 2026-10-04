# Aide de développement : compile la couche pour Windows depuis Linux avec MinGW-w64.
# Sert UNIQUEMENT à détecter les erreurs de compilation/syntaxe sans PC Windows.
# Ce n'est PAS MSVC : la DLL de référence est celle de Visual Studio 2022.
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)
set(CMAKE_C_COMPILER x86_64-w64-mingw32-gcc-posix)
set(CMAKE_CXX_COMPILER x86_64-w64-mingw32-g++-posix)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_EXE_LINKER_FLAGS "-static")
set(CMAKE_SHARED_LINKER_FLAGS "-static")

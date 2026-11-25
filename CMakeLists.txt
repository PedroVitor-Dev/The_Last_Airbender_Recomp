cmake_minimum_required(VERSION 3.25)
project(TheLastAirbenderRecomp VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(MSVC)
    add_compile_options(/W4 /utf-8 /permissive- /Zc:__cplusplus)
else()
    add_compile_options(-Wall -Wextra)
endif()

# Biblioteca comum (ROM, decodificador ARMv4T)
add_library(tla_core STATIC core/src/rom.cpp)
target_include_directories(tla_core PUBLIC core/include)

# Recompilador offline
add_executable(tlarecomp recompiler/src/main.cpp)
target_link_libraries(tlarecomp PRIVATE tla_core)

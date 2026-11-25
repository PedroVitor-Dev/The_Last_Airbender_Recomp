cmake_minimum_required(VERSION 3.25)
project(TheLastAirbenderRecomp VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(MSVC)
    add_compile_options(/W4 /utf-8 /permissive- /Zc:__cplusplus)
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
else()
    add_compile_options(-Wall -Wextra)
endif()

# Biblioteca comum (ROM, decodificador ARMv4T)
add_library(tla_core STATIC
    core/src/rom.cpp
    core/src/armv4t_decode.cpp
    core/src/armv4t_disasm.cpp)
target_include_directories(tla_core PUBLIC core/include)

# Recompilador offline
add_executable(tlarecomp
    recompiler/src/main.cpp
    recompiler/src/analysis.cpp)
target_link_libraries(tlarecomp PRIVATE tla_core)

# Runtime do GBA (hardware + interpretador)
add_library(tla_gba STATIC
    runtime/src/bus.cpp
    runtime/src/cpu.cpp
    runtime/src/io.cpp
    runtime/src/ppu.cpp
    runtime/src/bios.cpp
    runtime/src/gba.cpp)
target_include_directories(tla_gba PUBLIC runtime/include)
target_link_libraries(tla_gba PUBLIC tla_core)

# Frontend
add_executable(tlagba frontend/src/main.cpp)
target_link_libraries(tlagba PRIVATE tla_gba)
if(WIN32)
    target_link_libraries(tlagba PRIVATE winmm)
endif()

# Testes
enable_testing()
add_executable(test_decoder tests/test_decoder.cpp)
target_link_libraries(test_decoder PRIVATE tla_core)
add_test(NAME decoder COMMAND test_decoder)

cmake_minimum_required(VERSION 3.25)
project(TheLastAirbenderRecomp VERSION 0.0.1 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Runtime do C++ estático: o executável distribuído não depende do VC++ Redistributable.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

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
    recompiler/src/analysis.cpp
    recompiler/src/emitter.cpp
    recompiler/src/synth.cpp)
target_link_libraries(tlarecomp PRIVATE tla_core)

# Runtime do GBA (hardware + interpretador)
add_library(tla_gba STATIC
    runtime/src/bus.cpp
    runtime/src/cpu.cpp
    runtime/src/io.cpp
    runtime/src/ppu.cpp
    runtime/src/bios.cpp
    runtime/src/apu.cpp
    runtime/src/state.cpp
    runtime/src/gba.cpp)
target_include_directories(tla_gba PUBLIC runtime/include)
target_link_libraries(tla_gba PUBLIC tla_core)

# Código recompilado (gerado por "tlarecomp emit" em generated/recomp; fora do Git)
file(GLOB TLA_RECOMP_SOURCES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/generated/recomp/*.cpp)
if(TLA_RECOMP_SOURCES)
    add_library(tla_recomp STATIC ${TLA_RECOMP_SOURCES})
    target_link_libraries(tla_recomp PUBLIC tla_gba)
    if(MSVC)
        # Código gerado: rótulos não usados e variáveis-sombra são esperados.
        target_compile_options(tla_recomp PRIVATE /W0 /O2 /bigobj)
    endif()
    message(STATUS "Código recompilado: ${CMAKE_SOURCE_DIR}/generated/recomp")
else()
    add_library(tla_recomp STATIC frontend/src/recomp_stub.cpp)
    target_link_libraries(tla_recomp PUBLIC tla_gba)
    message(STATUS "Sem código recompilado: tlagba usará só o interpretador")
endif()

# Frontend: tlagba (console, para desenvolvimento e testes) e AvatarRecomp (o executável
# distribuído: sem console, ícone, acha a ROM sozinho e mostra erros em caixas de diálogo).
set(TLA_FRONTEND_SOURCES frontend/src/main.cpp frontend/src/audio_xaudio2.cpp frontend/src/config.cpp
    frontend/src/input_xinput.cpp frontend/src/window_win32.cpp frontend/src/menu.cpp)
if(WIN32)
    set(TLA_ICON_PATH ${CMAKE_SOURCE_DIR}/frontend/res/app.ico)
    set(TLA_LAUNCHER_BG ${CMAKE_SOURCE_DIR}/frontend/res/launcher_bg.jpg)
    configure_file(frontend/res/app.rc.in ${CMAKE_BINARY_DIR}/app.rc @ONLY)
    list(APPEND TLA_FRONTEND_SOURCES ${CMAKE_BINARY_DIR}/app.rc)
endif()
foreach(target tlagba AvatarRecomp)
    add_executable(${target} ${TLA_FRONTEND_SOURCES})
    target_link_libraries(${target} PRIVATE tla_gba tla_recomp)
    target_compile_definitions(${target} PRIVATE TLA_VERSION="${PROJECT_VERSION}")
    if(WIN32)
        target_link_libraries(${target} PRIVATE winmm xaudio2 ole32 gdi32 xinput comdlg32 shell32 windowscodecs)
    endif()
endforeach()
if(WIN32)
    set_target_properties(AvatarRecomp PROPERTIES WIN32_EXECUTABLE ON)
    target_compile_definitions(AvatarRecomp PRIVATE TLA_GUI=1)
    if(MSVC)
        target_link_options(AvatarRecomp PRIVATE /ENTRY:mainCRTStartup)
    endif()
endif()

# Teste diferencial recompilado × interpretador
add_executable(tlaverify frontend/src/verify.cpp)
target_link_libraries(tlaverify PRIVATE tla_gba tla_recomp)

# Fuzzer de instruções (código gerado da ROM sintética em generated/synth; ver
# scripts/test_synth.ps1). Executável separado: a tabela gerada tem os mesmos nomes.
file(GLOB TLA_SYNTH_SOURCES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/generated/synth/*.cpp)
if(TLA_SYNTH_SOURCES)
    add_executable(test_synth frontend/src/test_synth.cpp ${TLA_SYNTH_SOURCES})
    target_link_libraries(test_synth PRIVATE tla_gba)
    if(MSVC)
        set_source_files_properties(${TLA_SYNTH_SOURCES} PROPERTIES COMPILE_OPTIONS "/W0;/bigobj")
    endif()
endif()

# Testes
enable_testing()
add_executable(test_decoder tests/test_decoder.cpp)
target_link_libraries(test_decoder PRIVATE tla_core)
add_test(NAME decoder COMMAND test_decoder)
add_executable(test_ppu tests/test_ppu.cpp)
target_link_libraries(test_ppu PRIVATE tla_gba)
add_test(NAME ppu_compose COMMAND test_ppu)
add_executable(test_state tests/test_state.cpp)
target_link_libraries(test_state PRIVATE tla_gba tla_recomp)

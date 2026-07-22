cmake_minimum_required(VERSION 3.15)

# Static CRT (/MT): Steam Deck / Proton prefixes usually do NOT have the MSVC
# runtime redistributables installed, so a /MD build's DLL silently fails to
# load (LoadLibrary can't find VCRUNTIME140/MSVCP140) -- "the mod does nothing
# on Deck". Linking the CRT statically makes the DLL self-contained. Must be
# set before project(); applies to minhook/libER too (all built from source
# here, so the whole link is consistent).
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

project(PersistentBuffs LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# REQUIRED for ELDEN RING ABI compatibility -- libER's STL containers only match
# the game's layout when iterator debugging is disabled (same as the sibling mods).
add_definitions(-D_ITERATOR_DEBUG_LEVEL=0)

# --- MinHook (static), used for function hooking ----------------------------
add_subdirectory(external/minhook)

# --- libER (static): SpEffectParam access for the buff filter ----------------
# Lives as a git submodule under external/libER (same repo/commit the sibling
# mods use):
#   git submodule add https://github.com/Dasaav-dsv/libER external/libER
#   git submodule update --init --recursive
add_subdirectory(external/libER libER)

# --- embed Paramdex names into the DLL (readable logs) -----------------------
# external/Paramdex is a git submodule (soulsmods/Paramdex, --depth 1). The ER
# name lists are converted to byte arrays at build time so the log can print
# "1730:Golden Vow" / weapon names with NO loose files next to the DLL. A
# SpEffectParam.txt placed next to the DLL still works as an override/extension
# (for overhaul mods whose ids Paramdex doesn't know).
set(PARAMDEX_ER_NAMES "${CMAKE_CURRENT_SOURCE_DIR}/external/Paramdex/ER/Names")
set(GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
function(embed_names txt inc var)
    add_custom_command(
        OUTPUT "${GENERATED_DIR}/${inc}"
        COMMAND ${CMAKE_COMMAND}
                -DINPUT=${PARAMDEX_ER_NAMES}/${txt}
                -DOUTPUT=${GENERATED_DIR}/${inc}
                -DVAR=${var}
                -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_names.cmake
        DEPENDS "${PARAMDEX_ER_NAMES}/${txt}"
                "${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_names.cmake"
        COMMENT "Embedding Paramdex ${txt}"
    )
endfunction()
embed_names(SpEffectParam.txt    speffect_names.inc kSpEffectNamesTxt)
embed_names(EquipParamWeapon.txt weapon_names.inc   kWeaponNamesTxt)

# --- the mod DLL ------------------------------------------------------------
add_library(PersistentBuffs SHARED
    src/ini.hpp
    src/scan.hpp
    src/utils.hpp
    src/utils.cpp
    src/config.hpp
    src/config.cpp
    src/offsets.hpp
    src/offsets.cpp
    src/game_access.hpp
    src/game_access.cpp
    src/buff_filters.hpp
    src/buff_filters.cpp
    src/buff_discovery.hpp
    src/buff_discovery.cpp
    src/buff_timing.hpp
    src/buff_timing.cpp
    src/sp_entry.hpp
    src/sp_entry.cpp
    src/weapon_memory.hpp
    src/weapon_memory.cpp
    src/session_store.hpp
    src/session_store.cpp
    src/main.cpp
    "${GENERATED_DIR}/speffect_names.inc"
    "${GENERATED_DIR}/weapon_names.inc"
)

target_include_directories(PersistentBuffs PRIVATE
    src
    external/minhook/include
    "${GENERATED_DIR}"
)

# Link both statically -> a single self-contained PersistentBuffs.dll.
target_link_libraries(PersistentBuffs PRIVATE minhook libER_static)

# Single self-contained DLL named exactly "PersistentBuffs.dll".
set_target_properties(PersistentBuffs PROPERTIES
    PREFIX ""
    OUTPUT_NAME "PersistentBuffs"
)

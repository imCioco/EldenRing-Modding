cmake_minimum_required(VERSION 3.15)
project(InfiniteWeaponBuffs LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# REQUIRED for ELDEN RING ABI compatibility -- libER's STL containers
# only match the game's layout when iterator debugging is disabled.
add_definitions(-D_ITERATOR_DEBUG_LEVEL=0)

# libER lives as a git submodule under external/libER:
#   git submodule add https://github.com/Dasaav-dsv/libER external/libER
#   git submodule update --init --recursive
add_subdirectory("external/libER" "libER")

# --- embed Paramdex names into the DLL (readable logs) -----------------------
# external/Paramdex is a git submodule (soulsmods/Paramdex, --depth 1). The ER
# name lists are converted to byte arrays at build time so the log can print
# "1851:[Weapon] Oath of Vengeance - Damage Buff" / item + spell names with NO
# loose files next to the DLL. Loose <ParamName>.txt files next to the DLL still
# work as overrides/extensions (overhaul mods whose ids Paramdex doesn't know).
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
embed_names(EquipParamGoods.txt  goods_names.inc    kGoodsNamesTxt)
embed_names(Magic.txt            magic_names.inc    kMagicNamesTxt)

add_library(${PROJECT_NAME} SHARED
    src/ini.hpp
    src/utils.hpp
    src/utils.cpp
    src/names.hpp
    src/names.cpp
    src/config.hpp
    src/config.cpp
    src/speffect_lookup.hpp
    src/speffect_lookup.cpp
    src/buff_filters.hpp
    src/buff_filters.cpp
    src/horse_protection.hpp
    src/horse_protection.cpp
    src/dual_wield.hpp
    src/dual_wield.cpp
    src/apply.hpp
    src/apply.cpp
    src/discover.hpp
    src/discover.cpp
    src/main.cpp
    "${GENERATED_DIR}/speffect_names.inc"
    "${GENERATED_DIR}/goods_names.inc"
    "${GENERATED_DIR}/magic_names.inc"
)

target_include_directories(${PROJECT_NAME} PRIVATE src "${GENERATED_DIR}")
target_compile_features(${PROJECT_NAME} PRIVATE cxx_std_20)

# Link libER STATICALLY -> a single self-contained InfiniteWeaponBuffs.dll
# with no libER.dll dependency. (Dynamic linking caused LoadLibrary error
# 126 because libER.dll wasn't on the mod loader's search path.)
target_link_libraries(${PROJECT_NAME} PRIVATE libER_static)

# Final artifact: InfiniteWeaponBuffs.dll
set_target_properties(${PROJECT_NAME} PROPERTIES OUTPUT_NAME "InfiniteWeaponBuffs")

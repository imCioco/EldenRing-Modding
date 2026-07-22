cmake_minimum_required(VERSION 3.15)
project(OmniCaster LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# REQUIRED for ELDEN RING ABI compatibility.
add_definitions(-D_ITERATOR_DEBUG_LEVEL=0)

# libER as a git submodule under external/libER:
#   git submodule update --init --recursive
add_subdirectory("external/libER" "libER")

# ---- embed Paramdex weapon names into the DLL (readable logs) ----
set(NAMES_TXT "${CMAKE_CURRENT_SOURCE_DIR}/external/Paramdex/ER/Names/EquipParamWeapon.txt")
set(NAMES_INC "${CMAKE_CURRENT_BINARY_DIR}/generated/weapon_names.inc")
add_custom_command(
    OUTPUT "${NAMES_INC}"
    COMMAND ${CMAKE_COMMAND}
            -DINPUT=${NAMES_TXT}
            -DOUTPUT=${NAMES_INC}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_names.cmake
    DEPENDS "${NAMES_TXT}" "${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_names.cmake"
    COMMENT "Embedding Paramdex weapon names"
)

add_library(${PROJECT_NAME} SHARED
    src/main.cpp
    src/scaling.cpp
    src/scaling.hpp
    src/requirements.cpp
    src/requirements.hpp
    src/names.cpp
    src/names.hpp
    src/player_stats.cpp
    src/player_stats.hpp
    src/utils.cpp
    src/utils.hpp
    src/ini.hpp
    src/scan.hpp
    "${NAMES_INC}"
)

target_include_directories(${PROJECT_NAME} PRIVATE src "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_compile_features(${PROJECT_NAME} PRIVATE cxx_std_20)

# Static-link libER -> a single self-contained DLL (no libER.dll dependency).
target_link_libraries(${PROJECT_NAME} PRIVATE libER_static)

set_target_properties(${PROJECT_NAME} PROPERTIES OUTPUT_NAME "OmniCaster")

cmake_minimum_required(VERSION 3.15)
project(CustomTalismanEffects LANGUAGES C CXX RC)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ---- version resource ------------------------------------------------------
# release/version.md is the single source of truth (build.bat bumps it BEFORE
# configuring, so the DLL embeds exactly the version that ships). The generated
# VERSIONINFO is what gives the binary an identity -- see
# src/CustomTalismanEffects.rc.in for why that matters (AV false positives).
set(CTE_VERSION "0.0.0")
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/release/version.md")
  file(READ "${CMAKE_CURRENT_SOURCE_DIR}/release/version.md" CTE_VERSION_RAW)
  string(REGEX MATCH "[0-9]+\\.[0-9]+\\.[0-9]+" CTE_VERSION_MATCH "${CTE_VERSION_RAW}")
  if(CTE_VERSION_MATCH)
    set(CTE_VERSION "${CTE_VERSION_MATCH}")
  else()
    message(WARNING "release/version.md has no x.y.z version; falling back to ${CTE_VERSION}")
  endif()
endif()
string(REPLACE "." ";" CTE_VERSION_PARTS "${CTE_VERSION}")
list(GET CTE_VERSION_PARTS 0 CTE_VER_MAJOR)
list(GET CTE_VERSION_PARTS 1 CTE_VER_MINOR)
list(GET CTE_VERSION_PARTS 2 CTE_VER_PATCH)
message(STATUS "CustomTalismanEffects version resource: ${CTE_VERSION}")
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/src/CustomTalismanEffects.rc.in"
               "${CMAKE_CURRENT_BINARY_DIR}/CustomTalismanEffects.rc" @ONLY)

# REQUIRED for ELDEN RING ABI compatibility.
add_definitions(-D_ITERATOR_DEBUG_LEVEL=0)

# ---- third-party (fetched at configure time) ------------------------------
include(FetchContent)

# MinHook: the overlay's detours -- the two DXGI factory creation entry points
# (backend v2's discovery hooks), XInputGetState/Ex and the DirectInput8 device
# vtable (so the game reads a neutral pad while the menu is open), plus
# SetCursorPos/ClipCursor (so the OS cursor is free while the menu is open).
# Pinned: backend v2 is validated against 1.3.3.
FetchContent_Declare(minhook
  GIT_REPOSITORY https://github.com/TsudaKageyu/minhook.git
  GIT_TAG        v1.3.3)

# Dear ImGui: the in-game overlay UI. Source-only; we compile the core + the
# Win32 platform backend straight into the DLL (see the sources below). There is
# no ImGui renderer backend: overlay_d3d12.cpp is a purpose-built consumer of
# the immutable draw packets.
FetchContent_Declare(imgui
  GIT_REPOSITORY https://github.com/ocornut/imgui.git
  GIT_TAG        v1.90.9)

# Both pinned revisions are source-only (neither has a root CMakeLists.txt), so
# MakeAvailable populates them without adding foreign targets -- the supported
# replacement for the deprecated FetchContent_Populate form (policy CMP0169).
FetchContent_MakeAvailable(minhook imgui)

# libER as a git submodule under external/libER (param access), static-linked.
add_subdirectory("external/libER" "libER")

add_library(${PROJECT_NAME} SHARED
    src/main.cpp
    src/log.cpp
    src/offsets.cpp
    src/game_access.cpp
    src/state.cpp
    src/hooks.cpp
    src/messages.cpp
    src/session_store.cpp
    # Backend v2: overlay.cpp owns input + the frontend producer; immutable
    # frame packets cross to the DXGI hook coordinator and private D3D12
    # command-list renderer. The talisman panel itself lives in overlay_ui.cpp
    # and knows nothing about any of that.
    src/overlay.cpp
    # Every process-wide name the backend claims, plus the single-open-menu
    # arbitration that lets a sibling mod embedding this backend (QuestPath)
    # coexist in the same game process.
    src/overlay_coexist.cpp
    src/overlay_present.cpp
    src/overlay_dxgi_shadow.cpp
    src/overlay_d3d12.cpp
    src/overlay_frame.cpp
    src/overlay_ui.cpp
    ${minhook_SOURCE_DIR}/src/buffer.c
    ${minhook_SOURCE_DIR}/src/hook.c
    ${minhook_SOURCE_DIR}/src/trampoline.c
    ${minhook_SOURCE_DIR}/src/hde/hde64.c
    # Dear ImGui core stays statically isolated inside this DLL. The Win32
    # backend supplies platform timing/clipboard support to a message-only HWND;
    # GPU rendering is the packet consumer in overlay_d3d12.cpp.
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp
    # Identity metadata (CompanyName/ProductName/FileVersion/...). Its absence
    # was a direct contributor to the ML-heuristic AV false positives.
    ${CMAKE_CURRENT_BINARY_DIR}/CustomTalismanEffects.rc
)

target_include_directories(${PROJECT_NAME} PRIVATE
    src
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
    ${minhook_SOURCE_DIR}/include
)

# We feed the gamepad ourselves (from the hooked XInput), so disable ImGui's
# built-in Win32 gamepad polling.
target_compile_definitions(${PROJECT_NAME} PRIVATE
    IMGUI_IMPL_WIN32_DISABLE_GAMEPAD
    CTE_VERSION="${CTE_VERSION}")
target_compile_features(${PROJECT_NAME} PRIVATE cxx_std_20)

# Static-link libER (+ MinHook compiled above) -> a single self-contained DLL.
# The overlay renders into the game's own D3D12 backbuffer, so d3d12/dxgi/
# d3dcompiler/ole32 replace the old D3D11 + DirectComposition set. dinput8/
# dxguid are for the DirectInput8 pad-neutralization detours (GUID_SysKeyboard,
# the IIDs).
target_link_libraries(${PROJECT_NAME} PRIVATE
    libER_static d3d12 dxgi d3dcompiler ole32 dinput8 dxguid)

if(MSVC)
  # Exploit-mitigation flags benign commercial software carries and injected
  # payloads typically don't -- one less feature the AV heuristics score
  # against us. These three are MSVC defaults; naming them makes the intent
  # explicit and survives a future toolchain default change.
  target_link_options(${PROJECT_NAME} PRIVATE /DYNAMICBASE /NXCOMPAT /HIGHENTROPYVA)

  # ⚠ Do NOT add /guard:cf here, despite it being the obvious next mitigation.
  # CFG validates every indirect call against a bitmap of legal call targets,
  # and this DLL's whole purpose is calling MinHook trampolines -- code
  # VirtualAlloc'd at runtime, which is by definition absent from that bitmap.
  # Where the host process has CFG enforcement on, the first call through a
  # trampoline fast-fails and takes the game down. Making it safe would mean
  # SetProcessValidCallTargets on every trampoline MinHook builds, i.e.
  # patching the vendored MinHook. Not worth it for a mitigation the audit
  # itself rated "mild".

  set_source_files_properties(
      src/overlay_present.cpp
      src/overlay_dxgi_shadow.cpp
      src/overlay_d3d12.cpp
      src/overlay_frame.cpp
      PROPERTIES COMPILE_OPTIONS "/W4;/permissive-")
endif()

set_target_properties(${PROJECT_NAME} PROPERTIES OUTPUT_NAME "CustomTalismanEffects")

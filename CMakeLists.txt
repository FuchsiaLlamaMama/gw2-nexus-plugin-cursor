# Cursor Finder — a Guild Wars 2 Nexus addon. Split so the logic is testable
# off-game:
#
#   cursor-finder-core — pure C++17 settings + geometry + write-through
#                        persistence (macOS + MSVC).
#   cursor-finder      — the Nexus/ImGui DLL glue (Windows/MSVC only).
#
# The DLL is guarded behind WIN32 so a macOS configure still builds
# cursor-finder-core and its doctest tests (the DLL needs Nexus.h + ImGui +
# Windows.h).
#
# Builds standalone (this directory is the top-level project: dependencies come
# from the submodules/vendored headers below) or as a subdirectory of a larger
# build that already defines imgui, nexus-api, nlohmann-json, doctest and
# shared-core.

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  cmake_minimum_required(VERSION 3.21)
  project(gw2-nexus-plugin-cursor-finder LANGUAGES CXX)

  set(CMAKE_CXX_STANDARD 17)
  set(CMAKE_CXX_STANDARD_REQUIRED ON)

  # Static CRT (/MT): a self-contained DLL that loads without the MSVC
  # redistributable present — important under Wine/CrossOver.
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

  # Nexus loads into the 64-bit game process — x64 only.
  if(MSVC AND NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "Cursor Finder must be built for x64 (configure with -A x64).")
  endif()

  # Dear ImGui (submodule: RaidcoreGG/imgui, v1.80) — pinned to the ImGui the
  # running Nexus host links; a mismatch crashes on the shared context. Compiled
  # statically; the addon adopts Nexus's ImGui context at load.
  add_library(imgui STATIC
    vendor/imgui/imgui.cpp
    vendor/imgui/imgui_draw.cpp
    vendor/imgui/imgui_tables.cpp
    vendor/imgui/imgui_widgets.cpp
  )
  target_include_directories(imgui PUBLIC vendor/imgui)

  # Nexus addon API (submodule: RaidcoreGG/Nexus-API, MIT).
  add_library(nexus-api INTERFACE)
  target_include_directories(nexus-api INTERFACE sdk)

  # Header-only libs, vendored as single headers: nlohmann/json v3.11.3 and
  # doctest v2.4.11 (both MIT).
  add_library(nlohmann-json INTERFACE)
  target_include_directories(nlohmann-json INTERFACE vendor)
  add_library(doctest INTERFACE)
  target_include_directories(doctest INTERFACE vendor)

  enable_testing()

  # Shared helpers (submodule: FuchsiaLlamaMama/gw2-nexus-plugin-shared).
  add_subdirectory(shared)
endif()

# --- cursor-finder-core: platform-independent, unit-tested -------------------
add_library(cursor-finder-core STATIC
  core/cursor_finder_store.cpp
  core/marker.cpp
  core/clip_freeze.cpp
  core/visibility.cpp
)
target_include_directories(cursor-finder-core PUBLIC .)
target_compile_features(cursor-finder-core PUBLIC cxx_std_17)
target_link_libraries(cursor-finder-core PUBLIC shared-core nlohmann-json)

# --- cursor-finder-core tests (doctest, run via ctest) -----------------------
add_executable(cursor-finder-core-tests tests/test_cursor_finder_core.cpp)
target_link_libraries(cursor-finder-core-tests PRIVATE cursor-finder-core doctest)
add_test(NAME cursor-finder-core-tests COMMAND cursor-finder-core-tests)

# --- cursor-finder: the Nexus addon DLL (Windows only) -----------------------
if(WIN32)
  # Textures are embedded as C byte arrays (assets/*_data.h) and loaded via
  # Textures_GetOrCreateFromMemory — no .rc / Windows resource lookup
  # (FindResource by numeric ID fails at runtime under CrossOver/Wine). No
  # bundled font: the panel uses ImGui's default font.
  add_library(cursor-finder SHARED src/entry.cpp)
  target_link_libraries(cursor-finder PRIVATE imgui nexus-api cursor-finder-core shared-core)
  set_target_properties(cursor-finder PROPERTIES
    PREFIX ""            # cursor-finder.dll, not libcursor-finder.dll
    OUTPUT_NAME "cursor-finder"
  )
endif()

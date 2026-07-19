cmake_minimum_required(VERSION 3.21)

# The project name becomes the name of the produced SKSE .dll.
project(CostumeExpansionFW VERSION 1.3.1 LANGUAGES CXX)

# --- Optional auto-deploy --------------------------------------------------
# If SKYRIM_MODS_FOLDER is set, the built .dll is copied to
#   <SKYRIM_MODS_FOLDER>/<PROJECT_NAME>/SKSE/Plugins/
# Left unset during initial toolchain validation (no deploy).
if(DEFINED ENV{SKYRIM_FOLDER} AND IS_DIRECTORY "$ENV{SKYRIM_FOLDER}/Data")
    set(OUTPUT_FOLDER "$ENV{SKYRIM_FOLDER}/Data")
endif()
if(DEFINED ENV{SKYRIM_MODS_FOLDER} AND IS_DIRECTORY "$ENV{SKYRIM_MODS_FOLDER}")
    set(OUTPUT_FOLDER "$ENV{SKYRIM_MODS_FOLDER}/${PROJECT_NAME}")
endif()

# --- The SKSE plugin -------------------------------------------------------
find_package(CommonLibSSE CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(nifly CONFIG REQUIRED)
find_package(pugixml CONFIG REQUIRED)

# --- nifcarrier core (in-proc carrier builder; NIFCARRIER_INPROC.md) --------
# Engine-free static lib linked into BOTH the SKSE DLL and nifcarrier_cli.exe
# so the T0-T3 offline checks keep running outside the game (I-7).
# nifly 1.0.0 headers do not compile as C++20/23 (non-const Matrix operator==
# turns ambiguous against its own reversed candidate), so this lib is the
# C++17 firewall: nifly headers appear ONLY in nifcarrier/*.cpp, never in the
# public header, never in a DLL translation unit.
add_library(nifcarrier_core STATIC
    src/nifcarrier/NifCarrierCore.cpp
)
target_include_directories(nifcarrier_core PUBLIC src)
target_link_libraries(nifcarrier_core PRIVATE nifly pugixml::pugixml nlohmann_json::nlohmann_json bcrypt)
target_compile_features(nifcarrier_core PUBLIC cxx_std_17)

# Offline test host - a DEV tool, never part of the mod package. Nexus zips
# are staged from the deployed mod folder only; keep it that way (review P1-2).
option(CEF_BUILD_NIFCARRIER_CLI "Build the nifcarrier_cli.exe offline test host" ON)
if(CEF_BUILD_NIFCARRIER_CLI)
    add_executable(nifcarrier_cli tools/nifcarrier_cli/main.cpp)
    target_link_libraries(nifcarrier_cli PRIVATE nifcarrier_core)
endif()

add_commonlibsse_plugin(${PROJECT_NAME} SOURCES
    plugin.cpp
    src/SkinRebind.cpp
    src/Commands.cpp
    src/Cosave.cpp
    src/Papyrus.cpp
    src/BoxStore.cpp
    src/PublishStore.cpp
    src/BodyMorph.cpp
    src/Preset.cpp
    src/LoreBox.cpp
    src/NifCarrierBridge.cpp
    src/SmfUI.cpp
)
target_include_directories(${PROJECT_NAME} PRIVATE src)
target_link_libraries(${PROJECT_NAME} PRIVATE nlohmann_json::nlohmann_json nifcarrier_core)
target_compile_features(${PROJECT_NAME} PRIVATE cxx_std_23)
target_precompile_headers(${PROJECT_NAME} PRIVATE PCH.h)

# --- Auto-copy on build (only if OUTPUT_FOLDER resolved above) -------------
if(DEFINED OUTPUT_FOLDER)
    set(DLL_FOLDER "${OUTPUT_FOLDER}/SKSE/Plugins")
    message(STATUS "SKSE plugin output folder: ${DLL_FOLDER}")
    add_custom_command(
        TARGET "${PROJECT_NAME}"
        POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${DLL_FOLDER}"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different "$<TARGET_FILE:${PROJECT_NAME}>" "${DLL_FOLDER}/$<TARGET_FILE_NAME:${PROJECT_NAME}>"
        VERBATIM
    )
    if(CMAKE_BUILD_TYPE STREQUAL "Debug")
        add_custom_command(
            TARGET "${PROJECT_NAME}"
            POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different "$<TARGET_PDB_FILE:${PROJECT_NAME}>" "${DLL_FOLDER}/$<TARGET_PDB_FILE_NAME:${PROJECT_NAME}>"
            VERBATIM
        )
    endif()
endif()

cmake_minimum_required(VERSION 3.21)

# The project name becomes the name of the produced SKSE .dll.
project(CostumeExpansionFW VERSION 1.1.0 LANGUAGES CXX)

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
add_commonlibsse_plugin(${PROJECT_NAME} SOURCES
    plugin.cpp
    src/SkinRebind.cpp
    src/Commands.cpp
    src/Cosave.cpp
    src/Papyrus.cpp
    src/BoxStore.cpp
    src/BodyMorph.cpp
    src/Preset.cpp
    src/LoreBox.cpp
)
target_include_directories(${PROJECT_NAME} PRIVATE src)
target_link_libraries(${PROJECT_NAME} PRIVATE nlohmann_json::nlohmann_json)
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

# SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
# SPDX-License-Identifier: GPL-3.0-or-later

# Chained Echoes 1.41 (0100C510166F0000): Unity asset reader (chained_echoes_unity). Included from
# CMakeLists.txt. Defines:
#   CE_UNITY_SOURCES               reader + bc_decoder + lz4.c
#   CE_UNITY_INCLUDE_DIRECTORIES   their include directories
#   dsmod-ce                       the title module 0100C510166F0000 (EXCLUDE_FROM_ALL): live
#                                  battle readers, HUD switches, pictures composed from the romfs
#   ce-field-tool                  Linux dev CLI (EXCLUDE_FROM_ALL): Field Home pictures offline
#   ce-skills-tool                 Linux dev CLI (EXCLUDE_FROM_ALL): Skills page pictures offline
#   ce-page-tool                   Linux dev CLI (EXCLUDE_FROM_ALL): renders the module's pictures
#                                  offline from a merged romfs (sample battle model) to PNG
#   dsmod-ce-unity-lib             static library of the above (EXCLUDE_FROM_ALL; Android build check)
#   ce-unity-tool                  Linux dev CLI (EXCLUDE_FROM_ALL): survey, bench, families, dump
#   dsmod-ce-unity                 test (BUILD_TESTING, Linux); exit 77 = game data absent (skipped)
# The LZ4 block decoder is compiled in from the emulator's CPM cache, like Wonder's zstd.

enable_language(C)
file(GLOB CE_LZ4_LIB LIST_DIRECTORIES true "${EDEN_CPM_CACHE}/lz4/*/lib")
list(LENGTH CE_LZ4_LIB ce_lz4_count)
if(NOT ce_lz4_count EQUAL 1)
    message(FATAL_ERROR "Expected one lz4 source in ${EDEN_CPM_CACHE}/lz4; configure the compatible Eden Duo checkout first")
endif()
list(GET CE_LZ4_LIB 0 CE_LZ4_LIB)

set(CE_UNITY_SOURCES
    "${CMAKE_CURRENT_LIST_DIR}/chained_echoes_unity.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/chained_echoes_unity.h"
    "${CMAKE_CURRENT_LIST_DIR}/chained_echoes_unity_pins.inc"
    "${EDEN_SOURCE_ROOT}/externals/bc_decoder/bc_decoder.cpp"
    "${CE_LZ4_LIB}/lz4.c")
set(CE_UNITY_INCLUDE_DIRECTORIES "${CE_LZ4_LIB}" "${EDEN_SOURCE_ROOT}/externals/bc_decoder")

add_library(dsmod-ce-unity-lib STATIC EXCLUDE_FROM_ALL ${CE_UNITY_SOURCES})
target_compile_features(dsmod-ce-unity-lib PRIVATE cxx_std_20)
target_include_directories(dsmod-ce-unity-lib PRIVATE "${CMAKE_CURRENT_LIST_DIR}" "${EDEN_SOURCE_ROOT}/src"
    ${CE_UNITY_INCLUDE_DIRECTORIES})
set_target_properties(dsmod-ce-unity-lib PROPERTIES POSITION_INDEPENDENT_CODE ON
    CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN ON)

if(NOT ANDROID)
    add_executable(ce-unity-tool EXCLUDE_FROM_ALL
        "${CMAKE_CURRENT_LIST_DIR}/chained_echoes_unity_tool.cpp" ${CE_UNITY_SOURCES})
    target_compile_features(ce-unity-tool PRIVATE cxx_std_20)
    target_include_directories(ce-unity-tool PRIVATE "${CMAKE_CURRENT_LIST_DIR}" "${EDEN_SOURCE_ROOT}/src"
        "${EDEN_SOURCE_ROOT}/externals/stb" ${CE_UNITY_INCLUDE_DIRECTORIES})

    if(BUILD_TESTING AND COMMAND dsmod_add_test)
        dsmod_add_test(dsmod-ce-parse SOURCES "${CMAKE_CURRENT_LIST_DIR}/ce_parse_test.cpp"
            "${CMAKE_CURRENT_LIST_DIR}/ce_parse.h" COMPILE_OPTIONS -Wall -Wextra)
        dsmod_add_test(dsmod-ce-unity
            SOURCES "${CMAKE_CURRENT_LIST_DIR}/chained_echoes_unity_test.cpp" ${CE_UNITY_SOURCES}
            INCLUDE_DIRECTORIES "${EDEN_SOURCE_ROOT}/externals/stb" ${CE_UNITY_INCLUDE_DIRECTORIES})
        set(CE_UNITY_ROMFS "" CACHE PATH "Merged Chained Echoes 1.41 romfs root (private; empty = skip)")
        set(CE_UNITY_REFERENCE "" CACHE PATH "UnityPy reference set from research tools/unity-reference.py")
        set_tests_properties(dsmod-ce-unity PROPERTIES SKIP_RETURN_CODE 77
            ENVIRONMENT "CE_ROMFS=${CE_UNITY_ROMFS};CE_REFERENCE=${CE_UNITY_REFERENCE}")
    endif()
endif()

# The title module: Unity reader + drawing kit + pages + live reader.
set(CE_MODULE_SOURCES
    "${CMAKE_CURRENT_LIST_DIR}/ce_draw.cpp" "${CMAKE_CURRENT_LIST_DIR}/ce_draw.h"
    "${CMAKE_CURRENT_LIST_DIR}/ce_pages.cpp" "${CMAKE_CURRENT_LIST_DIR}/ce_pages.h"
    "${CMAKE_CURRENT_LIST_DIR}/ce_page_skyarmor.cpp" "${CMAKE_CURRENT_LIST_DIR}/ce_page_skyarmor.h"
    "${CMAKE_CURRENT_LIST_DIR}/ce_page_field.cpp" "${CMAKE_CURRENT_LIST_DIR}/ce_page_field.h"
    "${CMAKE_CURRENT_LIST_DIR}/ce_page_skills.cpp" "${CMAKE_CURRENT_LIST_DIR}/ce_page_skills.h"
    "${CMAKE_CURRENT_LIST_DIR}/ce_page_crystals.cpp" "${CMAKE_CURRENT_LIST_DIR}/ce_page_crystals.h"
    ${CE_UNITY_SOURCES})
dsmod_add_module(dsmod-ce EXCLUDE_FROM_ALL
    TITLE 0100C510166F0000
    SOURCES 0100C510166F0000.cpp ce_reader.cpp ce_reader.h ce_mech_reader.cpp ce_mech_reader.h ce_field_reader.cpp ce_field_reader.h
        ce_skill_reader.cpp ce_skill_reader.h
        ce_crystal_reader.cpp ce_crystal_reader.h
        ${CE_MODULE_SOURCES}
    EXPORTS eden_dsmod_get_module eden_dsmod_get_extensions eden_dsmod_get_font_extensions
        eden_dsmod_get_write_extensions
    INCLUDE_DIRECTORIES ${CE_UNITY_INCLUDE_DIRECTORIES})
if(NOT ANDROID)
    add_executable(ce-page-tool EXCLUDE_FROM_ALL
        "${CMAKE_CURRENT_LIST_DIR}/ce_page_tool.cpp" ${CE_MODULE_SOURCES})
    target_compile_features(ce-page-tool PRIVATE cxx_std_20)
    target_include_directories(ce-page-tool PRIVATE "${CMAKE_CURRENT_LIST_DIR}" "${EDEN_SOURCE_ROOT}/src"
        "${EDEN_SOURCE_ROOT}/externals/stb" ${CE_UNITY_INCLUDE_DIRECTORIES})
    # Sky Armor picture with the accepted mockup's sample (ce_sky_page_tool.cpp)
    add_executable(ce-sky-page-tool EXCLUDE_FROM_ALL
        "${CMAKE_CURRENT_LIST_DIR}/ce_sky_page_tool.cpp" ${CE_MODULE_SOURCES})
    target_compile_features(ce-sky-page-tool PRIVATE cxx_std_20)
    target_include_directories(ce-sky-page-tool PRIVATE "${CMAKE_CURRENT_LIST_DIR}" "${EDEN_SOURCE_ROOT}/src"
        "${EDEN_SOURCE_ROOT}/externals/stb" ${CE_UNITY_INCLUDE_DIRECTORIES})
endif()
if(NOT ANDROID)
    # Field Home slice: offline field pictures with the mockups' sample data (ce-field-tool)
    add_executable(ce-field-tool EXCLUDE_FROM_ALL
        "${CMAKE_CURRENT_LIST_DIR}/ce_field_tool.cpp" ${CE_MODULE_SOURCES})
    target_compile_features(ce-field-tool PRIVATE cxx_std_20)
    target_include_directories(ce-field-tool PRIVATE "${CMAKE_CURRENT_LIST_DIR}" "${EDEN_SOURCE_ROOT}/src"
        "${EDEN_SOURCE_ROOT}/externals/stb" ${CE_UNITY_INCLUDE_DIRECTORIES})
endif()
if(NOT ANDROID)
    # Skills page: offline pictures with the mockups' sample data (ce-skills-tool)
    add_executable(ce-skills-tool EXCLUDE_FROM_ALL
        "${CMAKE_CURRENT_LIST_DIR}/ce_skills_tool.cpp" ${CE_MODULE_SOURCES})
    target_compile_features(ce-skills-tool PRIVATE cxx_std_20)
    target_include_directories(ce-skills-tool PRIVATE "${CMAKE_CURRENT_LIST_DIR}" "${EDEN_SOURCE_ROOT}/src"
        "${EDEN_SOURCE_ROOT}/externals/stb" ${CE_UNITY_INCLUDE_DIRECTORIES})
    # Crystals page: offline pictures with the mockups' sample data (ce-crystals-tool)
    add_executable(ce-crystals-tool EXCLUDE_FROM_ALL
        "${CMAKE_CURRENT_LIST_DIR}/ce_crystals_tool.cpp" ${CE_MODULE_SOURCES})
    target_compile_features(ce-crystals-tool PRIVATE cxx_std_20)
    target_include_directories(ce-crystals-tool PRIVATE "${CMAKE_CURRENT_LIST_DIR}" "${EDEN_SOURCE_ROOT}/src"
        "${EDEN_SOURCE_ROOT}/externals/stb" ${CE_UNITY_INCLUDE_DIRECTORIES})
endif()

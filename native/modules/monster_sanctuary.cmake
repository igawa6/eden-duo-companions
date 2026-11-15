# SPDX-FileCopyrightText: Copyright 2026 Eden DSMod fork
# SPDX-License-Identifier: GPL-3.0-or-later

# Monster Sanctuary (0100A0F00DA68000), update v524288 / Ver 2.1.0.32. Included from
# CMakeLists.txt after chained_echoes.cmake (reuses CE_UNITY_SOURCES: the UnityFS bundle reader,
# LZ4). Defines:
#   dsmod-ms             the title module 0100A0F00DA68000 (EXCLUDE_FROM_ALL): live readers
#   dsmod-ms-publish     test (BUILD_TESTING, Linux): publish invariants without a game
#   dsmod-ms-helper-mailbox  test: the helper mailbox client and the guest .S offsets

set(MS_MODULE_SOURCES ms_art.cpp ms_art.h ms_fx.cpp ms_fx.h ms_mapdraw.cpp ms_mapdraw.h ms_config.h ms_model.h ms_publish.cpp ms_publish.h ms_reader.cpp ms_reader.h)

dsmod_add_module(dsmod-ms EXCLUDE_FROM_ALL
    TITLE 0100A0F00DA68000
    SOURCES 0100A0F00DA68000.cpp ${MS_MODULE_SOURCES} ${CE_UNITY_SOURCES}
    EXPORTS eden_dsmod_get_module eden_dsmod_get_extensions eden_dsmod_get_font_extensions
    INCLUDE_DIRECTORIES ${CE_UNITY_INCLUDE_DIRECTORIES})
target_compile_options(dsmod-ms PRIVATE "-fdebug-prefix-map=$ENV{HOME}=~")

if(BUILD_TESTING AND NOT ANDROID)
    dsmod_add_test(dsmod-ms-module KEEP_ASSERTS SOURCES ms_module_test.cpp)
    target_link_libraries(dsmod-ms-module-test PRIVATE dsmod-ms)
    dsmod_add_test(dsmod-ms-publish KEEP_ASSERTS
        SOURCES ms_publish_test.cpp ms_publish.cpp ms_mapdraw.cpp ms_art.cpp ms_fx.cpp ${CE_UNITY_SOURCES}
        INCLUDE_DIRECTORIES ${CE_UNITY_INCLUDE_DIRECTORIES}
        COMPILE_OPTIONS -Wall -Wextra)
    # the guest-helper mailbox client against a fake mailbox, and the .S offsets it must match
    dsmod_add_test(dsmod-ms-helper-mailbox KEEP_ASSERTS SOURCES ms_helper_mailbox_test.cpp
        COMPILE_OPTIONS -Wall -Wextra)
    target_compile_definitions(dsmod-ms-helper-mailbox-test PRIVATE
        MS_HELPER_SOURCE="${CMAKE_CURRENT_LIST_DIR}/../helper/ms_guest_helper.S")
endif()

if(BUILD_TESTING AND NOT ANDROID)
    # MS_ROMFS: a merged Monster Sanctuary romfs folder (private; empty = the decode part skips)
    set(MS_ROMFS "" CACHE PATH "Merged Monster Sanctuary v524288 romfs root (private; empty = skip)")
    dsmod_add_test(dsmod-ms-art KEEP_ASSERTS
        SOURCES ms_art_test.cpp ms_art.cpp ms_fx.cpp ms_mapdraw.cpp ${CE_UNITY_SOURCES}
        INCLUDE_DIRECTORIES ${CE_UNITY_INCLUDE_DIRECTORIES}
        COMPILE_OPTIONS -Wall)
    target_compile_definitions(dsmod-ms-art-test PRIVATE
        MS_TEST_PACKAGE="${CMAKE_CURRENT_LIST_DIR}/../../packages/MonsterSanctuary/dualscreen")
    set_tests_properties(dsmod-ms-art PROPERTIES SKIP_RETURN_CODE 77)
    # A configured path overrides the environment; an empty cache leaves MS_ROMFS inherited.
    if(MS_ROMFS)
        set_tests_properties(dsmod-ms-art PROPERTIES ENVIRONMENT "MS_ROMFS=${MS_ROMFS}")
    endif()
endif()

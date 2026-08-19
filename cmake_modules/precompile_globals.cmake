set(SYMBOLS_FILE ${PROJECT_SOURCE_DIR}/symbols.syms)
set(GLOBALS_H ${CMAKE_CURRENT_BINARY_DIR}/generated/globals.h)
set(GLOBALS_C ${CMAKE_CURRENT_BINARY_DIR}/generated/globals.c)
set(GLOBALS_H_COG ${PROJECT_SOURCE_DIR}/src/globals.h.cog)
set(GLOBALS_C_COG ${PROJECT_SOURCE_DIR}/src/globals.c.cog)

make_directory(${CMAKE_CURRENT_BINARY_DIR}/generated)
include_directories(${CMAKE_CURRENT_BINARY_DIR}/generated)

if(NOT PLAT_MSVC)
    set(PYTHON_EXE "${CMAKE_CURRENT_BINARY_DIR}/cogapp_venv/bin/python3")
    set(COGAPP_DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/cogapp_venv/bin/cog")
else()
    find_package(Python3 COMPONENTS Interpreter REQUIRED)

    # Print the Python executable path
    message(STATUS "Python executable: ${Python3_EXECUTABLE}")
    set(PYTHON_EXE "${Python3_EXECUTABLE}")
    # cogapp is a pip module run via this same python — there's no separate cog
    # binary to depend on. Leave this EMPTY: passing python.exe as both PYTHON_EXE
    # and COGAPP_DEPENDS lists it twice in the custom-command DEPENDS, which makes
    # MSBuild's CustomBuild tracker report "list of dependencies has changed since
    # the last build" every time — re-running cog and relinking on every build even
    # with no source changes. (AAOpenJKDF2 39c0e27f.)
    set(COGAPP_DEPENDS "")
endif()

list(JOIN EMBEDDED_RESOURCES "+" EMBEDDED_RESOURCES_SEPARATED)

# Bootstrap the cog venv first (non-MSVC) so the generator command below can
# depend on it. On MSVC cog runs via the system python (no venv).
if(NOT PLAT_MSVC)
    add_custom_command(
        OUTPUT ${PYTHON_EXE}
        COMMAND python3 -m venv ${CMAKE_CURRENT_BINARY_DIR}/cogapp_venv
    )
    add_custom_command(
        OUTPUT ${COGAPP_DEPENDS}
        COMMAND ${PYTHON_EXE} -m pip install cogapp
        DEPENDS ${PYTHON_EXE}
    )
endif()

# Generate globals.h and globals.c with cog, both from a single custom command
# (no inter-rule chain). NOTE: the Visual Studio generator re-runs this CustomBuild
# on every build anyway — its tracker reports "list of dependencies has changed
# since the last build" each time, a known quirk we couldn't fully suppress. To
# keep that from forcing a needless full recompile every build, cog writes to a
# .tmp and we copy_if_different into place, so globals.h/.c only change mtime when
# their CONTENT actually changes. Result: a no-source-change build does a fast cog
# pass + a quick relink, with NO recompile of globals.c or of everything that
# includes the (widely-included) generated globals.h. (AAOpenJKDF2 39c0e27f.)
add_custom_command(
    OUTPUT ${GLOBALS_H} ${GLOBALS_C}
    COMMAND ${PYTHON_EXE} -m cogapp -d -D symbols_fpath="${SYMBOLS_FILE}" -D project_root="${PROJECT_SOURCE_DIR}" -D embedded_resources="${EMBEDDED_RESOURCES_SEPARATED}" -o ${GLOBALS_H}.tmp ${GLOBALS_H_COG}
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${GLOBALS_H}.tmp ${GLOBALS_H}
    COMMAND ${PYTHON_EXE} -m cogapp -d -D symbols_fpath="${SYMBOLS_FILE}" -D project_root="${PROJECT_SOURCE_DIR}" -D embedded_resources="${EMBEDDED_RESOURCES_SEPARATED}" -o ${GLOBALS_C}.tmp ${GLOBALS_C_COG}
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${GLOBALS_C}.tmp ${GLOBALS_C}
    DEPENDS ${SYMBOLS_FILE} ${GLOBALS_H_COG} ${GLOBALS_C_COG} ${EMBEDDED_RESOURCES} ${PYTHON_EXE} ${COGAPP_DEPENDS}
)

# Gather the cog generation into one target. Many sith_engine translation units
# include generated/globals.h transitively (via rdMaterial.h etc.), but CMake has
# no way to know that on a clean build, so a high -j build would compile them while
# cog is still writing globals.h and read a truncated header ("unterminated
# #ifndef"). add_dependencies(sith_engine generate_globals) (in CMakeLists.txt)
# makes every sith_engine object wait for this target to finish first.
set_source_files_properties(${GLOBALS_H} ${GLOBALS_C} PROPERTIES GENERATED TRUE)
add_custom_target(generate_globals DEPENDS ${GLOBALS_H} ${GLOBALS_C})

# HACK
list(REMOVE_ITEM ENGINE_SOURCE_FILES ${GLOBALS_C})
list(APPEND ENGINE_SOURCE_FILES ${GLOBALS_C})
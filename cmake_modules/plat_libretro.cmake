# Libretro core platform: builds the engine in its stock desktop configuration
# (windowless — the frontend owns the window/GL context/audio) and packages it as
# a shared-library libretro core. Windows/MSVC first; Linux baseline comes later.
#
# Selected with -DPLAT_LIBRETRO=TRUE. See DESIGN.md.

macro(plat_initialize)
    if(NOT MSVC)
        message(FATAL_ERROR "PLAT_LIBRETRO currently supports MSVC x64 only (Linux planned, see DESIGN.md M3)")
    endif()
    message(STATUS "Target platform: libretro core (Win64 MSVC)")

    set(BIN_NAME "openjkdf2_libretro")

    # The dependency modules (build_sdl_mixer.cmake etc.) branch on PLAT_MSVC for
    # their MSVC-specific handling (patch commands, static lib names). This build
    # IS the MSVC platform plus libretro packaging, so behave as it everywhere
    # downstream. (Safe: the top-level platform dispatch already ran.)
    set(PLAT_MSVC TRUE)

    # Engine config identical to the proven MSVC standalone build (plat_msvc.cmake)...
    add_compile_definitions(WINVER=0x0600 _WIN32_WINNT=0x0600)
    add_compile_definitions(WIN64)
    add_compile_definitions(WIN64_STANDALONE)
    add_compile_definitions(ARCH_64BIT)
    add_compile_definitions(WIN32)

    include(cmake_modules/plat_feat_full_sdl2.cmake)
    set(TARGET_USE_PHYSFS FALSE)
    set(TARGET_USE_CURL FALSE)
    set(TARGET_COMPILE_FREEGLUT TRUE)
    set(TARGET_FIND_OPENAL FALSE)
    set(TARGET_USE_GAMENETWORKINGSOCKETS FALSE)
    set(SDL2_COMMON_LIBS SDL::SDL)

    set(TARGET_WIN32 TRUE)

    # ...plus the libretro platform marker. LIBRETRO_BUILD gates the small
    # engine-side patches (std3D window FBO, keyboard state source, no window
    # recreation).
    set(TARGET_LIBRETRO TRUE)
    add_compile_definitions(LIBRETRO_BUILD)

    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} /std:c11")
endmacro()

macro(plat_specific_deps)
    set(SDL2_COMMON_LIBS SDL::SDL)
endmacro()

macro(plat_link_and_package)
    # libretro naming convention: openjkdf2_libretro.dll, no prefix/version suffix.
    # Exports: the ~25 retro_* entry points carry __declspec(dllexport) via
    # RETRO_API in libretro.h — deliberately NOT WINDOWS_EXPORT_ALL_SYMBOLS
    # (65535-symbol limit and it would leak every engine symbol).
    set_target_properties(${BIN_NAME} PROPERTIES
        PREFIX ""
        OUTPUT_NAME "openjkdf2_libretro"
        ENABLE_EXPORTS FALSE
        WINDOWS_EXPORT_ALL_SYMBOLS FALSE
    )

    set(CMAKE_THREAD_PREFER_PTHREAD TRUE)
    set(THREADS_PREFER_PTHREAD_FLAG TRUE)
    find_package(Threads REQUIRED)
    target_link_libraries(sith_engine PRIVATE Threads::Threads)

    target_link_libraries(sith_engine PRIVATE GLUT::GLUT)
    target_link_libraries(sith_engine PRIVATE GLEW::glew_s)
    target_link_libraries(${BIN_NAME} PRIVATE GLEW::glew_s)
    target_link_libraries(sith_engine PRIVATE ${SDL2_COMMON_LIBS} version imm32 setupapi gdi32 winmm ole32 oleaut32 shell32 user32 crypt32 advapi32)

    if(TARGET_CAN_JKGM)
        target_link_libraries(sith_engine PRIVATE PNG::PNG ZLIB::ZLIB)
    endif()

    if(TARGET_USE_OPENAL)
        target_link_libraries(sith_engine PRIVATE ${SDL_MIXER_DEPS} SDL::Mixer)
        if(OPENAL_COMPILING_FROM_SRC)
            target_link_libraries(sith_engine PRIVATE OpenAL::OpenAL)
        else()
            target_link_libraries(sith_engine PRIVATE ${OPENAL_LIBRARIES})
        endif()
    endif()

    target_link_libraries(sith_engine PRIVATE nlohmann_json::nlohmann_json)
    target_link_libraries(sith_engine PRIVATE opengl32 ws2_32 uuid ole32)

    # --- Performance flags (optimized configs only) -------------------------
    # AAOpenJKDF2 9d3db5ac. /GL + /LTCG: whole-program / link-time code
    # generation (~5-10% in the fork's measurements). sith_engine is an OBJECT
    # library holding all the hot render code; its /GL objects flow straight
    # into the DLL link, so /LTCG on ${BIN_NAME} covers the whole engine.
    # /arch:AVX2 deliberately NOT taken — RetroArch cores run on old hardware;
    # revisit near release with tester data (devdocs/07 §6).
    set(OPENJKDF2_PERF_COPTS $<$<CONFIG:Release,RelWithDebInfo>:/GL>)
    target_compile_options(sith_engine PRIVATE ${OPENJKDF2_PERF_COPTS})
    target_compile_options(${BIN_NAME} PRIVATE ${OPENJKDF2_PERF_COPTS})
    target_link_options(${BIN_NAME} PRIVATE $<$<CONFIG:Release,RelWithDebInfo>:/LTCG>)

    # --- Crash symbols (optimized configs) ----------------------------------
    # AAOpenJKDF2 b924df8d hunk. The VS generator emits no debug info for
    # Release, so a user crash (or our Event Log triage) shows raw offsets.
    # /Zi (compile) + /DEBUG (link) emit openjkdf2_libretro.pdb WITHOUT
    # changing optimization — /GL objects defer debug info to the /LTCG
    # backend, which writes the PDB at link time. Keep the PDB per release so
    # crash dumps symbolicate. (Per-target compiler PDBs: /Zi is parallel-safe
    # here, no /FS needed.)
    set(OPENJKDF2_DEBUG_COPTS $<$<CONFIG:Release,RelWithDebInfo>:/Zi>)
    target_compile_options(sith_engine PRIVATE ${OPENJKDF2_DEBUG_COPTS})
    target_compile_options(${BIN_NAME} PRIVATE ${OPENJKDF2_DEBUG_COPTS})
    target_link_options(${BIN_NAME} PRIVATE $<$<CONFIG:Release,RelWithDebInfo>:/DEBUG>)

    # The core's runtime DLL deps must sit next to it (RetroArch loads the core
    # from its cores dir; document copying these alongside).
    if(TARGET_USE_OPENAL)
        add_custom_command(
            TARGET ${BIN_NAME}
            POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy ${PROJECT_BINARY_DIR}/openal/bin/OpenAL32.dll ${PROJECT_BINARY_DIR}
        )
    endif()
endmacro()

macro(plat_extra_deps)
endmacro()

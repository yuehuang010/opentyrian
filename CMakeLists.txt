# CMake build for OpenTyrian, added alongside the canonical GNU Makefile.
#
# This mirrors the Makefile's build semantics (see /Makefile) rather than
# replacing it: same C standard, same preprocessor defines, same warning
# flags, same default TYRIAN_DIR / OPENTYRIAN_VERSION stamping. Prefer the
# Makefile on Unix if in doubt about behavior; this file exists for
# cross-platform (Windows/vcpkg, IDE) builds.
#
#   cmake -S . -B build
#   cmake --build build -j
#
# Optional HD asset paks (large/slow, off by default):
#
#   cmake --build build --target hd-assets
#   # or bake them into the default build:
#   cmake -S . -B build -DOPENTYRIAN_BUILD_HD_ASSETS=ON

cmake_minimum_required(VERSION 3.16)
project(opentyrian C)

# --- C standard: strict C99, no GNU extensions -------------------------------
# Makefile uses `-std=iso9899:1999`.
set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

# --- Default build type --------------------------------------------------
# Makefile's default build defines NDEBUG (the game uses NDEBUG to skip the
# intro logos); `make debug` is -UNDEBUG -O0 -g3 -Werror. Mirror that: default
# to a Release config (CMake defines NDEBUG for Release out of the box), and
# leave asserts on for Debug. Only meaningful for single-config generators
# (Makefiles/Ninja) -- multi-config generators (Xcode/VS) ignore this.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS "Debug" "Release" "RelWithDebInfo" "MinSizeRel")
endif()

# --- Sources ---------------------------------------------------------------
# Flat src/*.c, no subdirs (53 files). CONFIGURE_DEPENDS re-globs on
# reconfigure so new files are picked up like the Makefile's $(wildcard).
file(GLOB SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/*.c)

add_executable(opentyrian ${SOURCES})

# --- WITH_NETWORK ------------------------------------------------------------
option(WITH_NETWORK "Enable SDL2_net-based network multiplayer" ON)

# --- SDL2 / SDL2_net discovery, cross-platform -------------------------------
# Prefer CONFIG mode (vcpkg, Windows) and fall back to pkg-config (Linux /
# macOS Homebrew), matching how the Makefile shells out to pkg-config.
find_package(SDL2 CONFIG QUIET)
if(SDL2_FOUND)
    set(SDL2_LINK_TARGETS SDL2::SDL2)
    if(TARGET SDL2::SDL2main)
        list(APPEND SDL2_LINK_TARGETS SDL2::SDL2main)
    endif()
else()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(SDL2 REQUIRED IMPORTED_TARGET sdl2)
    set(SDL2_LINK_TARGETS PkgConfig::SDL2)
endif()

if(WITH_NETWORK)
    # Try CONFIG mode for SDL2_net independently of how SDL2 itself was
    # found -- e.g. SDL2 could come from a vcpkg CONFIG package while
    # SDL2_net is only available via pkg-config, or vice versa.
    find_package(SDL2_net CONFIG QUIET)
    if(SDL2_net_FOUND AND TARGET SDL2_net::SDL2_net)
        set(SDL2_NET_LINK_TARGETS SDL2_net::SDL2_net)
    else()
        find_package(PkgConfig REQUIRED)
        pkg_check_modules(SDL2_NET REQUIRED IMPORTED_TARGET SDL2_net)
        set(SDL2_NET_LINK_TARGETS PkgConfig::SDL2_NET)
    endif()
endif()

# --- Preprocessor defines, mirroring ALL_CPPFLAGS -----------------------------
if(WIN32)
    target_compile_definitions(opentyrian PRIVATE TARGET_WIN32)
    # Makefile: TYRIAN_DIR = C:\\TYRIAN (a literal two-backslash make
    # variable); after -DTYRIAN_DIR='"..."' the compiler sees the string
    # literal "C:\\TYRIAN", which the C preprocessor unescapes to C:\TYRIAN.
    # Reproduce the same two-backslash source text here.
    set(TYRIAN_DIR_VALUE "C:\\\\TYRIAN")
else()
    target_compile_definitions(opentyrian PRIVATE TARGET_UNIX)
    # Makefile default: TYRIAN_DIR = $(gamesdir)/tyrian = $(datadir)/games/tyrian
    #                  = $(prefix)/share/games/tyrian
    set(TYRIAN_DIR_VALUE "${CMAKE_INSTALL_PREFIX}/share/games/tyrian")
endif()
target_compile_definitions(opentyrian PRIVATE TYRIAN_DIR="${TYRIAN_DIR_VALUE}")

if(WITH_NETWORK)
    target_compile_definitions(opentyrian PRIVATE WITH_NETWORK)
endif()

# --- Version stamping --------------------------------------------------------
# Mirrors: VCS_IDREV = (git describe --tags || git rev-parse --short HEAD)
# -> -DOPENTYRIAN_VERSION='"..."'. If git is unavailable or both commands
# fail, define nothing -- src/opentyrian_version.h already supplies a
# "v2.1.???" fallback via #ifndef, and we must not touch that header.
find_package(Git QUIET)
set(OPENTYRIAN_VERSION_STR "")
if(GIT_FOUND)
    execute_process(
        COMMAND ${GIT_EXECUTABLE} describe --tags
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        OUTPUT_VARIABLE OPENTYRIAN_VERSION_STR
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
        RESULT_VARIABLE _git_describe_result
    )
    if(NOT _git_describe_result EQUAL 0 OR OPENTYRIAN_VERSION_STR STREQUAL "")
        execute_process(
            COMMAND ${GIT_EXECUTABLE} rev-parse --short HEAD
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
            OUTPUT_VARIABLE OPENTYRIAN_VERSION_STR
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET
            RESULT_VARIABLE _git_rev_parse_result
        )
        if(NOT _git_rev_parse_result EQUAL 0)
            set(OPENTYRIAN_VERSION_STR "")
        endif()
    endif()
endif()
if(NOT OPENTYRIAN_VERSION_STR STREQUAL "")
    target_compile_definitions(opentyrian PRIVATE OPENTYRIAN_VERSION="${OPENTYRIAN_VERSION_STR}")
endif()

# --- Warning flags, GNU/Clang only (mirrors CFLAGS; MSVC untouched) ----------
target_compile_options(opentyrian PRIVATE
    $<$<C_COMPILER_ID:GNU,Clang,AppleClang>:-pedantic>
    $<$<C_COMPILER_ID:GNU,Clang,AppleClang>:-Wall>
    $<$<C_COMPILER_ID:GNU,Clang,AppleClang>:-Wextra>
    $<$<C_COMPILER_ID:GNU,Clang,AppleClang>:-Wno-format-truncation>
    $<$<C_COMPILER_ID:GNU,Clang,AppleClang>:-Wno-missing-field-initializers>
)

# --- stb_vorbis special case (Makefile line 193) -----------------------------
# Vendored single-file decoder has one tautological-pointer-comparison
# warning; isolate the suppression to this file instead of disabling it
# project-wide. GNU/Clang-only flag, so guard it the same way as above.
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    set_source_files_properties(${CMAKE_CURRENT_SOURCE_DIR}/src/stb_vorbis.c
        PROPERTIES COMPILE_OPTIONS "-Wno-tautological-compare")
endif()

# --- Link libraries ----------------------------------------------------------
target_link_libraries(opentyrian PRIVATE ${SDL2_LINK_TARGETS})
if(WITH_NETWORK)
    target_link_libraries(opentyrian PRIVATE ${SDL2_NET_LINK_TARGETS})
endif()
if(NOT WIN32)
    target_link_libraries(opentyrian PRIVATE m)
endif()

# --- HD pipeline kernel library ------------------------------------------------
# tools/hdkernels.c: C ports of the HD pipeline's three hot per-pixel kernels
# (Lanczos upscale, xBRZ upscale, QOI encode/decode), loaded from the Python
# tools via ctypes (tools/hdkernels.py). The tools keep their pure-Python
# implementations and fall back to them when this library is absent, so it is
# purely a speedup -- but every HD-asset step below depends on it, because a
# clean build is ~5x faster with it than without.
#
# The kernels must produce output byte-identical to the Python originals (the
# built paks are hashed as the acceptance test). That is a floating-point
# constraint: forbid fused multiply-add contraction, which would silently
# change the low bit of the Lanczos accumulator and the xBRZ colour-distance
# metric. See the header comment in tools/hdkernels.c.
add_library(hdkernels SHARED ${CMAKE_CURRENT_SOURCE_DIR}/tools/hdkernels.c)
set_target_properties(hdkernels PROPERTIES C_STANDARD 99 C_STANDARD_REQUIRED ON
                                           C_EXTENSIONS OFF
                                           C_VISIBILITY_PRESET hidden)
target_compile_options(hdkernels PRIVATE
    $<$<C_COMPILER_ID:MSVC>:/fp:precise>
    $<$<C_COMPILER_ID:GNU,Clang,AppleClang>:-ffp-contract=off>
    $<$<C_COMPILER_ID:GNU,Clang,AppleClang>:-pedantic>
    $<$<C_COMPILER_ID:GNU,Clang,AppleClang>:-Wall>
    $<$<C_COMPILER_ID:GNU,Clang,AppleClang>:-Wextra>
)
if(NOT WIN32)
    target_link_libraries(hdkernels PRIVATE m)
endif()

# --- HD-asset targets ---------------------------------------------------------
# Drives tools/hd_build.py, the cross-platform orchestrator for the HD asset
# extraction + bundling pipeline (writes tyrian.base / tyrian.hd). Owned by
# a different track of work; we only invoke it here.
#
# Rather than one opaque target that shells out to the whole pipeline, each
# pipeline step gets its own target (`hd_build.py --only <step>`), so the build
# tool schedules them: the extraction steps are mutually independent and run
# concurrently under `--parallel`, and `bundle` is serialized after all of them.
# `hd-assets` stays the documented user-facing entry point.
#
# Note: the step targets deliberately do NOT use USES_TERMINAL. Ninja maps that
# onto the `console` pool, which has depth 1 -- it would serialize every step
# and defeat the whole point. Output is buffered and printed per step instead
# (which is what hd_build.py does internally anyway).
#
# Each step's real outputs are hundreds to thousands of files (comp alone
# writes 11,856 frame assets) -- far too many to enumerate as a custom_command
# OUTPUT list, and CMake needs a concrete OUTPUT to know whether a step is up
# to date. So each step gets a single stamp file as a proxy: the command runs
# the real python step, then touches the stamp. add_custom_target(hd-assets-
# <step> DEPENDS <stamp>) wraps that so the standalone target name still works
# (`cmake --build build --target hd-assets-font`), while ninja/make now has a
# file whose mtime it can compare against DEPENDS to skip an up-to-date step.
find_package(Python3 COMPONENTS Interpreter QUIET)
option(OPENTYRIAN_BUILD_HD_ASSETS "Build the HD asset paks as part of the default build" OFF)
if(Python3_Interpreter_FOUND)
    set(HD_BUILD_SCRIPT ${CMAKE_CURRENT_SOURCE_DIR}/tools/hd_build.py)
    set(HD_STAMP_DIR ${CMAKE_BINARY_DIR}/hd-stamps)
    file(MAKE_DIRECTORY ${HD_STAMP_DIR})
    set(HD_TYRIAN21 ${CMAKE_CURRENT_SOURCE_DIR}/tyrian21)
    set(HD_TOOLS ${CMAKE_CURRENT_SOURCE_DIR}/tools)

    # Configure-time globs for the two steps whose real inputs are open-ended
    # file sets (34 dynamically-loaded enemy sheets for comp/filter; 41
    # committed HD music tracks for music). Deliberately NOT
    # CONFIGURE_DEPENDS: adding or removing a newsh*.shp / hdmusic_*.ogg file
    # needs a `cmake` reconfigure to be picked up by the dependency graph.
    # An acceptable over-declaration tradeoff for this project's fixed asset
    # set, which doesn't change without a source-tree edit anyway.
    file(GLOB HD_NEWSH_FILES ${HD_TYRIAN21}/newsh*.shp)
    file(GLOB HD_MUSIC_FILES ${CMAKE_CURRENT_SOURCE_DIR}/hdmusic/hdmusic_*.ogg)

    # Per-step real inputs, read off each tool's own source (hd_extract.py,
    # hd_extract_comp.py, hd_extract_filter.py, hd_vectorize_font.py,
    # hd_extract_anim.py, stage_music.py) rather than assumed.
    set(HD_DEPS_backdrops
        ${HD_TYRIAN21}/palette.dat ${HD_TYRIAN21}/tyrian.pic ${HD_TYRIAN21}/tyrian.shp
        ${HD_TYRIAN21}/estsc.shp
        ${HD_TYRIAN21}/tshp2.pcx ${HD_TYRIAN21}/shipedit.pcx ${HD_TYRIAN21}/tyrset.pcx
        ${HD_TYRIAN21}/netarena.pcx ${HD_TYRIAN21}/netset.pcx ${HD_TYRIAN21}/netmega.pcx
        ${HD_TYRIAN21}/netfont1.pcx ${HD_TYRIAN21}/netfont2.pcx
        ${HD_TOOLS}/hd_extract.py)
    set(HD_DEPS_comp
        ${HD_TYRIAN21}/palette.dat ${HD_TYRIAN21}/tyrian.shp ${HD_NEWSH_FILES}
        ${HD_TOOLS}/hd_extract_comp.py)
    set(HD_DEPS_filter
        ${HD_TYRIAN21}/palette.dat ${HD_NEWSH_FILES}
        ${HD_TOOLS}/hd_extract_filter.py)
    set(HD_DEPS_font
        ${HD_TYRIAN21}/tyrian.shp
        ${HD_TOOLS}/hd_vectorize_font.py)
    set(HD_DEPS_anim
        ${HD_TYRIAN21}/tyrend.anm
        ${HD_TOOLS}/hd_extract_anim.py)
    set(HD_DEPS_music
        ${HD_MUSIC_FILES}
        ${HD_TOOLS}/stage_music.py)

    # The default (non-opt-in) extraction steps, per `hd_build.py --list`. The
    # opt-in `tiles` / `sfx` steps are not part of the default pipeline.
    set(HD_EXTRACT_STEPS backdrops comp filter font anim music)

    set(HD_EXTRACT_TARGETS "")
    set(HD_STEP_STAMPS "")
    foreach(step IN LISTS HD_EXTRACT_STEPS)
        set(stamp ${HD_STAMP_DIR}/${step}.stamp)
        add_custom_command(
            OUTPUT ${stamp}
            COMMAND ${Python3_EXECUTABLE} ${HD_BUILD_SCRIPT} --only ${step}
            COMMAND ${CMAKE_COMMAND} -E touch ${stamp}
            # $<TARGET_FILE:hdkernels> (not just the add_dependencies below)
            # is what actually makes a kernel rebuild invalidate this step --
            # a DEPENDS entry is compared by ninja/make; a target-order
            # dependency alone does not force a re-run when the step's other
            # inputs are unchanged.
            DEPENDS ${HD_BUILD_SCRIPT} ${HD_DEPS_${step}} $<TARGET_FILE:hdkernels>
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
            COMMENT "HD assets: extracting ${step}"
        )
        add_custom_target(hd-assets-${step} DEPENDS ${stamp})
        # Explicit build-order dependency, kept alongside the DEPENDS above:
        # the tools locate the kernel library by searching <repo>/build*, so
        # it only has to exist before a step runs -- no path is plumbed
        # through -- but scheduling still needs hdkernels built first.
        add_dependencies(hd-assets-${step} hdkernels)
        list(APPEND HD_EXTRACT_TARGETS hd-assets-${step})
        list(APPEND HD_STEP_STAMPS ${stamp})
    endforeach()

    # bundle packs whatever the extraction steps wrote into the data dir, so it
    # is the one ordering constraint in the pipeline: it must not start until
    # every extraction step has finished. Unlike the extraction steps, its
    # real outputs ARE the two enumerable distributable paks, so OUTPUT names
    # them directly instead of going through a stamp.
    set(HD_BUNDLE_OUT ${CMAKE_CURRENT_SOURCE_DIR}/tyrian.base)
    set(HD_BUNDLE_HD_OUT ${CMAKE_CURRENT_SOURCE_DIR}/tyrian.hd)
    add_custom_command(
        OUTPUT ${HD_BUNDLE_OUT} ${HD_BUNDLE_HD_OUT}
        COMMAND ${Python3_EXECUTABLE} ${HD_BUILD_SCRIPT} --bundle-only
        DEPENDS ${HD_BUILD_SCRIPT} ${HD_TOOLS}/mkbundle.py ${HD_STEP_STAMPS}
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        COMMENT "HD assets: bundling tyrian.base + tyrian.hd"
    )
    add_custom_target(hd-assets-bundle DEPENDS ${HD_BUNDLE_OUT} ${HD_BUNDLE_HD_OUT})
    add_dependencies(hd-assets-bundle hdkernels ${HD_EXTRACT_TARGETS})

    # User-facing entry point (`cmake --build build --target hd-assets`), and
    # the ALL hook when the assets are baked into the default build.
    if(OPENTYRIAN_BUILD_HD_ASSETS)
        add_custom_target(hd-assets ALL)
    else()
        add_custom_target(hd-assets)
    endif()
    add_dependencies(hd-assets hd-assets-bundle)
else()
    message(STATUS "Python3 interpreter not found -- skipping hd-assets target")
endif()

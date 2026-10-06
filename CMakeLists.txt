cmake_minimum_required(VERSION 3.21)
project(img2png VERSION 1.5.0 LANGUAGES C CXX)

option(IMG2PNG_GUI "Build the Qt GUI" ON)
option(IMG2PNG_TESTS "Build tests" ON)
option(IMG2PNG_FETCH_DEPS "Download missing dependencies online (last resort)" ON)

# Dependency resolution per library, in order:
#   1. vendored source in thirdparty/<lib>/  (works fully offline)
#   2. local override / prebuilt (see CACHE PATHs below)
#   3. online download via FetchContent (zlib.net / SourceForge / GitHub)
set(LIBPNG_SOURCE_DIR "" CACHE PATH "local libpng source tree")
set(LIBJPEG_PREBUILT_DIR "" CACHE PATH "prebuilt libjpeg-turbo (include/ + lib/)")
set(ZLIB_SOURCE_DIR "" CACHE PATH "local zlib source tree")

include(FetchContent)

# ---------------------------------------------------------------------------
# zlib
# ---------------------------------------------------------------------------
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/zlib/CMakeLists.txt")
    message(STATUS "img2png: using vendored zlib")
    set(ZLIB_BUILD_SHARED OFF CACHE BOOL "" FORCE)
    set(ZLIB_BUILD_STATIC ON CACHE BOOL "" FORCE)
    set(ZLIB_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    add_subdirectory(thirdparty/zlib zlib-build)
    set(ZLIB_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/zlib" "${CMAKE_BINARY_DIR}/zlib-build")
    if(MSVC)
        set(ZLIB_LIBRARY "${CMAKE_BINARY_DIR}/zlib-build/zlibstatic.lib")
    elseif(APPLE OR UNIX)
        set(ZLIB_LIBRARY "${CMAKE_BINARY_DIR}/zlib-build/libz.a")   # OUTPUT_NAME z on unix
    else()
        set(ZLIB_LIBRARY "${CMAKE_BINARY_DIR}/zlib-build/libzlibstatic.a")
    endif()
elseif(ZLIB_SOURCE_DIR)
    message(STATUS "img2png: using local zlib at ${ZLIB_SOURCE_DIR}")
    set(ZLIB_BUILD_SHARED OFF CACHE BOOL "" FORCE)
    set(ZLIB_BUILD_STATIC ON CACHE BOOL "" FORCE)
    add_subdirectory(${ZLIB_SOURCE_DIR} zlib-build)
    set(ZLIB_LIBRARY zlibstatic)
    set(ZLIB_INCLUDE_DIR "${ZLIB_SOURCE_DIR}")
else()
    find_package(ZLIB QUIET)
    if(NOT ZLIB_FOUND AND IMG2PNG_FETCH_DEPS)
        message(STATUS "img2png: fetching zlib")
        set(ZLIB_BUILD_SHARED OFF CACHE BOOL "" FORCE)
        set(ZLIB_BUILD_STATIC ON CACHE BOOL "" FORCE)
        FetchContent_Declare(zlib
            URL https://zlib.net/fossils/zlib-1.3.1.tar.gz
                https://github.com/madler/zlib/archive/refs/tags/v1.3.1.tar.gz)
        FetchContent_MakeAvailable(zlib)
        # satisfy libpng's find_package(ZLIB REQUIRED) with the fetched build:
        # preset what FindZLIB would find, with the real static-lib file
        set(ZLIB_INCLUDE_DIR "${zlib_SOURCE_DIR}" "${zlib_BINARY_DIR}")
        if(MSVC)
            set(ZLIB_LIBRARY "${zlib_BINARY_DIR}/zlibstatic.lib")
        elseif(APPLE OR UNIX)
            set(ZLIB_LIBRARY "${zlib_BINARY_DIR}/libz.a")   # OUTPUT_NAME z on unix
        else()
            set(ZLIB_LIBRARY "${zlib_BINARY_DIR}/libzlibstatic.a")
        endif()
    elseif(NOT ZLIB_FOUND)
        message(FATAL_ERROR "img2png: zlib not found; install it, vendor it under thirdparty/zlib, or enable IMG2PNG_FETCH_DEPS")
    endif()
endif()

# ---------------------------------------------------------------------------
# libpng 1.6.59
# ---------------------------------------------------------------------------
set(PNG_SHARED OFF CACHE BOOL "" FORCE)
set(PNG_TESTS OFF CACHE BOOL "" FORCE)
set(PNG_TOOLS OFF CACHE BOOL "" FORCE)
set(PNG_EXECUTABLES OFF CACHE BOOL "" FORCE)
set(PNG_FRAMEWORK OFF CACHE BOOL "" FORCE)
set(PNG_HARDWARE_OPTIMIZATIONS ON CACHE BOOL "" FORCE)
set(SKIP_INSTALL_ALL ON CACHE BOOL "" FORCE)

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/libpng/CMakeLists.txt")
    message(STATUS "img2png: using vendored libpng")
    add_subdirectory(thirdparty/libpng libpng-build)
elseif(LIBPNG_SOURCE_DIR)
    message(STATUS "img2png: using local libpng at ${LIBPNG_SOURCE_DIR}")
    add_subdirectory(${LIBPNG_SOURCE_DIR} libpng-build)
else()
    if(NOT IMG2PNG_FETCH_DEPS)
        message(FATAL_ERROR "img2png: no libpng; vendor it under thirdparty/libpng, pass -DLIBPNG_SOURCE_DIR, or enable IMG2PNG_FETCH_DEPS")
    endif()
    message(STATUS "img2png: fetching libpng v1.6.59")
    FetchContent_Declare(libpng
        URL https://downloads.sourceforge.net/project/libpng/libpng16/1.6.59/libpng-1.6.59.tar.gz
            https://github.com/pnggroup/libpng/archive/refs/tags/v1.6.59.tar.gz)
    FetchContent_MakeAvailable(libpng)
endif()
if(TARGET zlibstatic)
    add_dependencies(png_static zlibstatic)
endif()
set(IMG2PNG_PNG_TARGET png_static)

# ---------------------------------------------------------------------------
# libjpeg-turbo 3.2.0 (SIMD off -> no NASM needed)
# ---------------------------------------------------------------------------
# Its bundled GNUInstallDirs computes blank install dirs when built as a
# subproject on Windows; preset sane values (we never install it anyway).
set(CMAKE_INSTALL_BINDIR "bin" CACHE PATH "" FORCE)
set(CMAKE_INSTALL_DATAROOTDIR "share" CACHE PATH "" FORCE)
set(CMAKE_INSTALL_DOCDIR "share/doc" CACHE PATH "" FORCE)
set(CMAKE_INSTALL_INCLUDEDIR "include" CACHE PATH "" FORCE)
set(CMAKE_INSTALL_LIBDIR "lib" CACHE PATH "" FORCE)
set(CMAKE_INSTALL_MANDIR "share/man" CACHE PATH "" FORCE)

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/libjpeg-turbo/CMakeLists.txt")
    message(STATUS "img2png: using vendored libjpeg-turbo")
    set(WITH_SIMD OFF CACHE BOOL "" FORCE)
    set(WITH_TURBOJPEG OFF CACHE BOOL "" FORCE)
    set(ENABLE_SHARED OFF CACHE BOOL "" FORCE)
    set(ENABLE_STATIC ON CACHE BOOL "" FORCE)
    set(WITH_TOOLS OFF CACHE BOOL "" FORCE)
    set(WITH_JAVA OFF CACHE BOOL "" FORCE)
    set(WITH_FUZZ OFF CACHE BOOL "" FORCE)
    add_subdirectory(thirdparty/libjpeg-turbo libjpeg-turbo-build)
    set(IMG2PNG_JPEG_TARGET jpeg-static)
elseif(LIBJPEG_PREBUILT_DIR)
    message(STATUS "img2png: using prebuilt libjpeg-turbo at ${LIBJPEG_PREBUILT_DIR}")
    if(NOT EXISTS "${LIBJPEG_PREBUILT_DIR}/lib/libjpeg.a")
        message(FATAL_ERROR "img2png: ${LIBJPEG_PREBUILT_DIR}/lib/libjpeg.a not found")
    endif()
    add_library(img2png_jpeg STATIC IMPORTED)
    set_target_properties(img2png_jpeg PROPERTIES
        IMPORTED_LOCATION "${LIBJPEG_PREBUILT_DIR}/lib/libjpeg.a"
        INTERFACE_INCLUDE_DIRECTORIES "${LIBJPEG_PREBUILT_DIR}/include")
    set(IMG2PNG_JPEG_TARGET img2png_jpeg)
else()
    if(NOT IMG2PNG_FETCH_DEPS)
        message(FATAL_ERROR "img2png: no libjpeg; vendor it under thirdparty/libjpeg-turbo, pass -DLIBJPEG_PREBUILT_DIR, or enable IMG2PNG_FETCH_DEPS")
    endif()
    message(STATUS "img2png: fetching libjpeg-turbo 3.2.0")
    set(WITH_SIMD OFF CACHE BOOL "" FORCE)
    set(WITH_TURBOJPEG OFF CACHE BOOL "" FORCE)
    set(ENABLE_SHARED OFF CACHE BOOL "" FORCE)
    set(ENABLE_STATIC ON CACHE BOOL "" FORCE)
    set(WITH_TOOLS OFF CACHE BOOL "" FORCE)
    set(WITH_JAVA OFF CACHE BOOL "" FORCE)
    set(WITH_FUZZ OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(libjpeg-turbo
        URL https://github.com/libjpeg-turbo/libjpeg-turbo/releases/download/3.2.0/libjpeg-turbo-3.2.0.tar.gz)
    FetchContent_MakeAvailable(libjpeg-turbo)
    set(IMG2PNG_JPEG_TARGET jpeg-static)
endif()

# ---------------------------------------------------------------------------
# extra image codecs: giflib (vendored subset), qoi (header-only),
# libwebp and libtiff (vendored, CMake)
# ---------------------------------------------------------------------------
add_library(giflib_v STATIC
    thirdparty/giflib/dgif_lib.c
    thirdparty/giflib/egif_lib.c
    thirdparty/giflib/gif_err.c
    thirdparty/giflib/gifalloc.c
    thirdparty/giflib/openbsd-reallocarray.c
    thirdparty/giflib/qprintf.c
    thirdparty/giflib/gif_hash.c
)
target_include_directories(giflib_v PUBLIC thirdparty/giflib)

add_library(qoi INTERFACE)
target_include_directories(qoi INTERFACE thirdparty/qoi)

set(WEBP_BUILD_ANIM_UTILS OFF CACHE BOOL "" FORCE)
set(WEBP_BUILD_CWEBP OFF CACHE BOOL "" FORCE)
set(WEBP_BUILD_DWEBP OFF CACHE BOOL "" FORCE)
set(WEBP_BUILD_GIF2WEBP OFF CACHE BOOL "" FORCE)
set(WEBP_BUILD_IMG2WEBP OFF CACHE BOOL "" FORCE)
set(WEBP_BUILD_VWEBP OFF CACHE BOOL "" FORCE)
set(WEBP_BUILD_WEBPINFO OFF CACHE BOOL "" FORCE)
set(WEBP_BUILD_LIBWEBPMUX OFF CACHE BOOL "" FORCE)
set(WEBP_BUILD_WEBPMUX OFF CACHE BOOL "" FORCE)
set(WEBP_BUILD_EXTRAS OFF CACHE BOOL "" FORCE)
add_subdirectory(thirdparty/libwebp libwebp-build)

set(tiff-static ON CACHE BOOL "" FORCE)
set(tiff-tools OFF CACHE BOOL "" FORCE)
set(tiff-tests OFF CACHE BOOL "" FORCE)
set(tiff-contrib OFF CACHE BOOL "" FORCE)
set(tiff-docs OFF CACHE BOOL "" FORCE)
set(tiff-cxx OFF CACHE BOOL "" FORCE)
set(jpeg OFF CACHE BOOL "" FORCE)
set(old-jpeg OFF CACHE BOOL "" FORCE)
set(webp OFF CACHE BOOL "" FORCE)
set(zstd OFF CACHE BOOL "" FORCE)
set(lzma OFF CACHE BOOL "" FORCE)
set(jbig OFF CACHE BOOL "" FORCE)
set(pixarlog OFF CACHE BOOL "" FORCE)
set(lerc OFF CACHE BOOL "" FORCE)
add_subdirectory(thirdparty/tiff tiff-build)

# ---------------------------------------------------------------------------
# modern codecs: libde265 (HEVC), libheif (HEIF container),
# dav1d shim (AV1), libavif (AVIF container)
# ---------------------------------------------------------------------------
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(ENABLE_DECODER OFF CACHE BOOL "" FORCE)
set(ENABLE_ENCODER OFF CACHE BOOL "" FORCE)
set(ENABLE_SDL OFF CACHE BOOL "" FORCE)
set(ENABLE_INTERNAL_DEVELOPMENT_TOOLS OFF CACHE BOOL "" FORCE)
set(WITH_FUZZERS OFF CACHE BOOL "" FORCE)
# static build: without this the headers use __declspec(dllimport)
add_compile_definitions(LIBDE265_STATIC_BUILD)
add_subdirectory(thirdparty/libde265 libde265-build)
# preset what libheif's FindLIBDE265 would find (it does not support
# add_subdirectory integration)
set(LIBDE265_LIBRARY de265)
# FindLIBDE265 checks for a single directory containing libde265/*.h plus
# the generated de265-version.h: build a merged include dir
set(LIBDE265_MERGED_INCLUDE "${CMAKE_BINARY_DIR}/libde265-merged")
file(MAKE_DIRECTORY "${LIBDE265_MERGED_INCLUDE}/libde265")
file(COPY ${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/libde265/libde265/de265.h
     DESTINATION "${LIBDE265_MERGED_INCLUDE}/libde265")
file(COPY "${CMAKE_BINARY_DIR}/libde265-build/libde265/de265-version.h"
     DESTINATION "${LIBDE265_MERGED_INCLUDE}/libde265")
set(LIBDE265_INCLUDE_DIR "${LIBDE265_MERGED_INCLUDE}")

add_subdirectory(thirdparty/dav1d dav1d-build)
# preset what libavif's Finddav1d would find (its SYSTEM mode creates a
# dav1d::dav1d IMPORTED target from these; an ALIAS cannot be used because
# libavif runs target_link_libraries on the target itself)
if(MSVC)
    set(DAV1D_LIBRARY "${CMAKE_BINARY_DIR}/dav1d-build/dav1d.lib")
else()
    set(DAV1D_LIBRARY "${CMAKE_BINARY_DIR}/dav1d-build/libdav1d.a")
endif()
set(DAV1D_INCLUDE_DIR
    "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/dav1d/include")
# libavif SYSTEM codec mode picks up the existing dav1d target
set(AVIF_CODEC_DAV1D "SYSTEM" CACHE STRING "" FORCE)
set(AVIF_BUILD_APPS OFF CACHE BOOL "" FORCE)
set(AVIF_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(AVIF_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(AVIF_LIBYUV OFF CACHE STRING "" FORCE)
add_subdirectory(thirdparty/libavif libavif-build)

set(ENABLE_PLUGIN_LOADING OFF CACHE BOOL "" FORCE)
set(WITH_AOM OFF CACHE BOOL "" FORCE)
set(WITH_DAV1D OFF CACHE BOOL "" FORCE)
set(WITH_EXAMPLES OFF CACHE BOOL "" FORCE)
set(WITH_GDK_PIXBUF OFF CACHE BOOL "" FORCE)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(BUILD_DOCUMENTATION OFF CACHE BOOL "" FORCE)
add_subdirectory(thirdparty/libheif libheif-build)
# upstream's PUBLIC include points at a non-existent generated dir;
# the real headers live in libheif/api/libheif
target_include_directories(heif INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/libheif/libheif/api>
    $<BUILD_INTERFACE:${libheif_BINARY_DIR}>)
# static consumers must not use dllimport on Windows
target_compile_definitions(heif INTERFACE LIBHEIF_STATIC_BUILD)

# ---------------------------------------------------------------------------
# img2png core
# ---------------------------------------------------------------------------
add_library(img2png_core STATIC
    src/image.c
    src/dec/bmp.c
    src/dec/tga.c
    src/dec/pnm.c
    src/dec/pngdec.c
    src/dec/ico.c
    src/dec/jpeg.c
    src/dec/gif.c
    src/dec/qoi.c
    src/dec/webp.c
    src/dec/tiff.c
    src/dec/heif.c
    src/dec/avif.c
    src/enc/pngenc.c
    src/enc/apngenc.c
    src/convert.c
    src/util/threads.c
    src/util/filetime.c
)
target_include_directories(img2png_core PUBLIC src)
target_compile_definitions(img2png_core PUBLIC _CRT_SECURE_NO_WARNINGS)

if(NOT WIN32)
    find_package(Threads REQUIRED)
    target_link_libraries(img2png_core PUBLIC Threads::Threads)
endif()

target_link_libraries(img2png_core PUBLIC ${IMG2PNG_PNG_TARGET} ${IMG2PNG_JPEG_TARGET}
    giflib_v qoi webp webpdemux tiff heif avif)

# libjpeg-turbo's targets don't carry PUBLIC include dirs (upstream expects
# an install layout), so add them directly for source-tree integrations
if(IMG2PNG_JPEG_TARGET STREQUAL "jpeg-static")
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/libjpeg-turbo/CMakeLists.txt")
        target_include_directories(img2png_core PUBLIC
            "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/libjpeg-turbo/src"
            "${CMAKE_BINARY_DIR}/libjpeg-turbo-build")
    else()
        target_include_directories(img2png_core PUBLIC
            "${libjpeg-turbo_SOURCE_DIR}/src" "${libjpeg-turbo_BINARY_DIR}")
    endif()
endif()

# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------
add_executable(img2png src/cli/main.c)
target_link_libraries(img2png PRIVATE img2png_core)

# ---------------------------------------------------------------------------
# GUI (Qt6, optional)
# ---------------------------------------------------------------------------
if(IMG2PNG_GUI)
    find_package(Qt6 COMPONENTS Widgets)
    if(NOT Qt6_FOUND)
        message(STATUS "img2png: Qt6 not found, building CLI only (-DIMG2PNG_GUI=OFF to silence)")
        set(IMG2PNG_GUI OFF)
    else()
        # Qt < 6.3 (e.g. ubuntu-22.04 packages) does not provide this command,
        # so set up what it does for us explicitly:
        set(CMAKE_AUTOMOC ON)
        set(CMAKE_AUTORCC ON)
        set(CMAKE_AUTOUIC ON)
        if(COMMAND qt_standard_project_setup)
            qt_standard_project_setup()
        endif()
        if(APPLE)
            set(IMG2PNG_GUI_MACOSX_BUNDLE MACOSX_BUNDLE)
        endif()
        qt_add_executable(img2png-gui WIN32 ${IMG2PNG_GUI_MACOSX_BUNDLE}
            src/gui/main_gui.cpp
            src/gui/mainwindow.cpp
            src/gui/mainwindow.h
        )
        target_link_libraries(img2png-gui PRIVATE Qt6::Widgets img2png_core)
    endif()
endif()

# ---------------------------------------------------------------------------
# tests
# ---------------------------------------------------------------------------
if(IMG2PNG_TESTS)
    enable_testing()
    add_executable(img2png_selftest tests/selftest.c)
    target_link_libraries(img2png_selftest PRIVATE img2png_core)
endif()

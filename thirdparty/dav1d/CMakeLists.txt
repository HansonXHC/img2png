# Hand-written CMake integration for the vendored dav1d (upstream builds
# with meson).  Builds the C fallback paths (no asm) for both bitdepths
# and produces a static `dav1d` target.

set(DAV1D_SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
set(DAV1D_BIN_DIR "${CMAKE_CURRENT_BINARY_DIR}")

# ---- generated headers ----
file(WRITE "${DAV1D_BIN_DIR}/vcs_version.h"
"#define DAV1D_VERSION \"1.5.4\"\n")

# pure-C build: arch flags stay 0 (all code paths use the C fallbacks;
# src/x86, src/arm ... asm trees are not vendored)
set(_dav1d_sigaction 0)
set(_dav1d_pthread_np 0)
set(_dav1d_dlsym 0)
set(_dav1d_getauxval 0)
set(_dav1d_io_h 0)
set(_dav1d_unistd_h 1)
if(WIN32)
    set(_dav1d_io_h 1)
    set(_dav1d_unistd_h 1)
elseif(UNIX)
    set(_dav1d_sigaction 1)
    set(_dav1d_dlsym 1)
    set(_dav1d_getauxval 1)
endif()

file(WRITE "${DAV1D_BIN_DIR}/config.h" "\
#define CONFIG_8BPC 1
#define CONFIG_16BPC 1
#define HAVE_ASM 0
#define ARCH_AARCH64 0
#define ARCH_ARM 0
#define ARCH_LOONGARCH 0
#define ARCH_LOONGARCH64 0
#define ARCH_PPC64LE 0
#define ARCH_RISCV 0
#define ARCH_X86 0
#define ARCH_X86_64 0
#define ARCH_X86_32 0
#define ENDIANNESS_BIG 0
#define PIC 0
#define TRIM_DSP_FUNCTIONS 0
#define CONFIG_LOG 1
#define HAVE_ALIGNED_ALLOC 1
#define HAVE_MEMALIGN 0
#define HAVE_POSIX_MEMALIGN 1
#define HAVE_CLOCK_GETTIME 1
#define HAVE_DLSYM ${_dav1d_dlsym}
#define HAVE_ELF_AUX_INFO ${_dav1d_sigaction}
#define HAVE_GETAUXVAL ${_dav1d_getauxval}
#define HAVE_SIGACTION ${_dav1d_sigaction}
#define HAVE_PTHREAD_NP_H ${_dav1d_pthread_np}
#define HAVE_PTHREAD_GETAFFINITY_NP 0
#define HAVE_PTHREAD_SETNAME_NP 0
#define HAVE_PTHREAD_SET_NAME_NP 0
#define HAVE_UNISTD_H ${_dav1d_unistd_h}
#define HAVE_SYS_TYPES_H 1
#define HAVE_IO_H ${_dav1d_io_h}
#define HAVE_THREADS 1
#define HAVE_C11_GENERIC 1
")

# ---- source lists (mirroring src/meson.build) ----
set(DAV1D_NEUTRAL_SOURCES
    src/cdf.c
    src/cpu.c
    src/ctx.c
    src/data.c
    src/decode.c
    src/dequant_tables.c
    src/getbits.c
    src/intra_edge.c
    src/itx_1d.c
    src/lf_mask.c
    src/lib.c
    src/log.c
    src/mem.c
    src/msac.c
    src/obu.c
    src/pal.c
    src/picture.c
    src/qm.c
    src/ref.c
    src/refmvs.c
    src/scan.c
    src/tables.c
    src/thread_task.c
    src/warpmv.c
    src/wedge.c
)
set(DAV1D_TMPL_SOURCES
    src/cdef_apply_tmpl.c
    src/cdef_tmpl.c
    src/fg_apply_tmpl.c
    src/filmgrain_tmpl.c
    src/ipred_prepare_tmpl.c
    src/ipred_tmpl.c
    src/itx_tmpl.c
    src/lf_apply_tmpl.c
    src/loopfilter_tmpl.c
    src/looprestoration_tmpl.c
    src/lr_apply_tmpl.c
    src/mc_tmpl.c
    src/recon_tmpl.c
)

# per-bitdepth objects: the same template sources compiled twice with
# BITDEPTH=8 / BITDEPTH=16 (10- and 12-bit share the 16-bit pipeline)
add_library(dav1d_bd8 OBJECT ${DAV1D_TMPL_SOURCES})
target_compile_definitions(dav1d_bd8 PRIVATE BITDEPTH=8)
add_library(dav1d_bd16 OBJECT ${DAV1D_TMPL_SOURCES})
target_compile_definitions(dav1d_bd16 PRIVATE BITDEPTH=16)
foreach(_bd dav1d_bd8 dav1d_bd16)
    target_include_directories(${_bd} PRIVATE
        "${DAV1D_SRC_DIR}" "${DAV1D_SRC_DIR}/include" "${DAV1D_BIN_DIR}")
    if(MSVC)
        target_compile_definitions(${_bd} PRIVATE _CRT_SECURE_NO_WARNINGS)
    endif()
endforeach()

add_library(dav1d STATIC
    ${DAV1D_NEUTRAL_SOURCES}
    $<TARGET_OBJECTS:dav1d_bd8>
    $<TARGET_OBJECTS:dav1d_bd16>
)
if(WIN32)
    # pthread emulation shipped with dav1d for Win32
    target_sources(dav1d PRIVATE src/win32/thread.c)
    target_compile_definitions(dav1d PRIVATE _WIN32_WINNT=0x0601)
    if(MINGW)
        target_compile_definitions(dav1d PRIVATE __USE_MINGW_ANSI_STDIO=1)
    endif()
endif()
target_include_directories(dav1d PUBLIC
    "${DAV1D_SRC_DIR}/include" "${DAV1D_BIN_DIR}")
target_include_directories(dav1d PRIVATE "${DAV1D_SRC_DIR}")
if(NOT WIN32)
    find_package(Threads REQUIRED)
    target_link_libraries(dav1d PUBLIC Threads::Threads)
endif()
# libavif's AVIF_CODEC_DAV1D=SYSTEM path checks for this target
add_library(dav1d::dav1d ALIAS dav1d)

include(FetchContent)
# Release source archives contain the exact pristine GPL decoder sources.
# Normal repository builds still download and verify the fixed archives below.
foreach(dependency a52 dca)
 string(TOUPPER "${dependency}" dependency_upper)
 if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/${dependency}/source/COPYING")
  set(FETCHCONTENT_SOURCE_DIR_${dependency_upper} "${CMAKE_CURRENT_SOURCE_DIR}/third_party/${dependency}/source")
 endif()
endforeach()
FetchContent_Declare(a52 URL https://distfiles.adelielinux.org/source/a52dec/a52dec-0.8.0.tar.gz
 URL_HASH SHA256=03c181ce9c3fe0d2f5130de18dab9bd8bc63c354071515aa56983c74a9cffcc9
 SOURCE_SUBDIR no-upstream-cmake)
FetchContent_Declare(dca URL https://download.videolan.org/pub/videolan/libdca/0.0.7/libdca-0.0.7.tar.bz2
 URL_HASH SHA256=3a0b13815f582c661d2388ffcabc2f1ea82f471783c400f765f2ec6c81065f6a
 SOURCE_SUBDIR no-upstream-cmake)
FetchContent_MakeAvailable(a52 dca)
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/a52-source-path.txt" "${a52_SOURCE_DIR}\n")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/dca-source-path.txt" "${dca_SOURCE_DIR}\n")
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/codec-config")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/codec-config/config.h"
 "#pragma once\n#define HAVE_INTTYPES_H 1\n#define HAVE_STDINT_H 1\n#define LIBA52_DOUBLE 1\n#define LIBDCA_DOUBLE 1\n#define inline __inline\n")
file(READ "${a52_SOURCE_DIR}/liba52/imdct.c" a52_imdct)
string(REPLACE "void a52_imdct_init (" "void ttplayer_a52_imdct_init_impl (" a52_imdct "${a52_imdct}")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/codec-config/imdct.c" "${a52_imdct}")
add_library(a52core STATIC ${a52_SOURCE_DIR}/liba52/bitstream.c "${CMAKE_CURRENT_BINARY_DIR}/codec-config/imdct.c"
 ${a52_SOURCE_DIR}/liba52/bit_allocate.c ${a52_SOURCE_DIR}/liba52/parse.c ${a52_SOURCE_DIR}/liba52/downmix.c)
file(READ "${dca_SOURCE_DIR}/libdca/parse.c" dca_parse)
set(dca_modern_scale "1.4142135623730951 /* sqrt(2) */ * 32768.0")
string(FIND "${dca_parse}" "${dca_modern_scale}" dca_scale_offset)
if(dca_scale_offset LESS 0)
 message(FATAL_ERROR "Pinned libdca QMF normalization patch no longer matches")
endif()
# 5.7.9: 60058f2d -> 60059952, constant 6005f940 is 49152.0.
# LFE uses its separate 8388608 divisor and must not receive this adjustment.
string(REPLACE "${dca_modern_scale}" "49152.0 /* TTPlayer 5.7.9 QMF normalization */" dca_parse "${dca_parse}")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/codec-config/dca_parse.c" "${dca_parse}")
add_library(dcacore STATIC ${dca_SOURCE_DIR}/libdca/bitstream.c "${CMAKE_CURRENT_BINARY_DIR}/codec-config/dca_parse.c" ${dca_SOURCE_DIR}/libdca/downmix.c)
foreach(core a52core dcacore)
 target_include_directories(${core} PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/codec-config")
 target_compile_definitions(${core} PUBLIC LIBA52_DOUBLE LIBDCA_DOUBLE PRIVATE _CRT_SECURE_NO_WARNINGS)
 target_compile_options(${core} PRIVATE /Os /Gy /Gw /GF /fp:precise /arch:SSE2)
endforeach()
target_include_directories(a52core PUBLIC ${a52_SOURCE_DIR}/include PRIVATE ${a52_SOURCE_DIR}/liba52)
target_include_directories(dcacore PUBLIC ${dca_SOURCE_DIR}/include PRIVATE ${dca_SOURCE_DIR}/libdca)
add_library(codec INTERFACE)
target_link_libraries(codec INTERFACE a52core dcacore)

include(FetchContent)
# Release source archives contain the exact pristine GPL decoder sources.
# Normal repository builds still download and verify the fixed archives below.
foreach(dependency a52)
 string(TOUPPER "${dependency}" dependency_upper)
 if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/${dependency}/source/COPYING")
  set(FETCHCONTENT_SOURCE_DIR_${dependency_upper} "${CMAKE_CURRENT_SOURCE_DIR}/third_party/${dependency}/source")
 endif()
endforeach()
FetchContent_Declare(a52 URL https://github.com/user-attachments/files/33714374/a52dec-0.8.0.tar.gz
 URL_HASH SHA256=03c181ce9c3fe0d2f5130de18dab9bd8bc63c354071515aa56983c74a9cffcc9
 SOURCE_SUBDIR no-upstream-cmake)
FetchContent_MakeAvailable(a52)
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/a52-source-path.txt" "${a52_SOURCE_DIR}\n")
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/codec-config")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/codec-config/config.h"
 "#pragma once\n#define HAVE_INTTYPES_H 1\n#define HAVE_STDINT_H 1\n#define LIBA52_DOUBLE 1\n#define inline __inline\n")
file(READ "${a52_SOURCE_DIR}/liba52/imdct.c" a52_imdct)
string(REPLACE "void a52_imdct_init (" "void ttplayer_a52_imdct_init_impl (" a52_imdct "${a52_imdct}")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/codec-config/imdct.c" "${a52_imdct}")
add_library(a52core STATIC ${a52_SOURCE_DIR}/liba52/bitstream.c "${CMAKE_CURRENT_BINARY_DIR}/codec-config/imdct.c"
 ${a52_SOURCE_DIR}/liba52/bit_allocate.c ${a52_SOURCE_DIR}/liba52/parse.c ${a52_SOURCE_DIR}/liba52/downmix.c)
target_include_directories(a52core PUBLIC "${a52_SOURCE_DIR}/include" PRIVATE "${a52_SOURCE_DIR}/liba52" "${CMAKE_CURRENT_BINARY_DIR}/codec-config")
target_compile_definitions(a52core PUBLIC LIBA52_DOUBLE PRIVATE _CRT_SECURE_NO_WARNINGS)
target_compile_options(a52core PRIVATE /Os /Gy /Gw /GF /fp:precise /arch:SSE2)
include("${CMAKE_CURRENT_LIST_DIR}/dts_backend.cmake")
add_library(codec INTERFACE)
target_link_libraries(codec INTERFACE a52core dts_backend)

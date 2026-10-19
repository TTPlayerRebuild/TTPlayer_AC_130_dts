cmake_minimum_required(VERSION 3.24)
project(TtpAc3Dts LANGUAGES C CXX RC)
if(NOT WIN32 OR NOT MSVC OR NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
  message(FATAL_ERROR "ttp_ac3dts requires MSVC and -A Win32")
endif()
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
include(cmake/legacy_windows.cmake)
include(cmake/size_release.cmake)
include(cmake/codec.cmake)
include(cmake/version.cmake)
add_library(ttp_ac3dts SHARED src/plugin.cpp src/reader.cpp src/decoder.cpp src/channel_maps.cpp
  src/ttp_ac3dts.def "${CMAKE_CURRENT_BINARY_DIR}/generated/version.rc")
add_dependencies(ttp_ac3dts ttp_ac3dts_version)
target_include_directories(ttp_ac3dts PUBLIC include PRIVATE src)
target_compile_definitions(ttp_ac3dts PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN
  _WIN32_WINNT=0x0501 _CRT_SECURE_NO_WARNINGS)
target_compile_options(ttp_ac3dts PRIVATE /W4 /permissive- /EHsc /GR- /Os /Gy /Gw /GF
  /utf-8 /fp:precise /arch:SSE2)
target_link_libraries(ttp_ac3dts PRIVATE codec ole32 uuid shlwapi)
target_link_options(ttp_ac3dts PRIVATE /OPT:REF /OPT:ICF /INCREMENTAL:NO /DEBUG:NONE)
set_target_properties(ttp_ac3dts PROPERTIES PREFIX "" OUTPUT_NAME "ttp_ac3dts")
find_package(Python3 REQUIRED COMPONENTS Interpreter)
add_custom_command(TARGET ttp_ac3dts POST_BUILD
  COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/cmake/check_legacy_imports.py"
    "$<TARGET_FILE:ttp_ac3dts>"
    --exports "${ttplayer_yy_thunks_SOURCE_DIR}/Config/x86/5.1.2600.txt"
    --exports "${ttplayer_yy_thunks_SOURCE_DIR}/Config/x86/6.1.7600.txt"
    --report "$<TARGET_FILE_DIR:ttp_ac3dts>/legacy-imports.json" VERBATIM)
install(TARGETS ttp_ac3dts RUNTIME DESTINATION AddIn COMPONENT Runtime)

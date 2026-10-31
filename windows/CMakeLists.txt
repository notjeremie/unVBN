# unVBN for Windows: a WIC codec (unvbn_wic.dll) so Windows can open Vizrt .vbn images, plus tests.
#   cmake -S windows -B build -A x64 -DUNVBN_VERSION=0.3.1 && cmake --build build --config Release
# On macOS/Linux only the portable decoder test builds (used to check against real samples).
cmake_minimum_required(VERSION 3.21)
project(unvbn_windows LANGUAGES CXX)

set(UNVBN_VERSION "0.0.0" CACHE STRING "Version stamped into the DLL and the MSI")
string(REPLACE "." ";" _v "${UNVBN_VERSION}")
list(GET _v 0 UNVBN_VERSION_MAJOR)
list(GET _v 1 UNVBN_VERSION_MINOR)
list(GET _v 2 UNVBN_VERSION_PATCH)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# Static C++ runtime: the DLL is loaded into Explorer and must not depend on a VC++ redistributable.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_library(vbn_decode STATIC src/vbn_decode.cpp)
target_include_directories(vbn_decode PUBLIC src)
if(MSVC)
  target_compile_options(vbn_decode PRIVATE /W4 /WX /permissive-)
else()
  target_compile_options(vbn_decode PRIVATE -Wall -Wextra -Werror)
endif()

add_executable(test_decode test/test_decode.cpp)
target_link_libraries(test_decode PRIVATE vbn_decode)

if(WIN32)
  configure_file(src/version.h.in ${CMAKE_CURRENT_BINARY_DIR}/generated/version.h @ONLY)
  add_library(unvbn_wic SHARED src/wic_codec.cpp src/unvbn_wic.def src/unvbn_wic.rc)
  target_include_directories(unvbn_wic PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/generated)
  target_compile_definitions(unvbn_wic PRIVATE UNICODE _UNICODE)
  target_compile_options(unvbn_wic PRIVATE /W4 /WX /permissive- /guard:cf)
  target_link_options(unvbn_wic PRIVATE /guard:cf /DYNAMICBASE /NXCOMPAT)
  target_link_libraries(unvbn_wic PRIVATE vbn_decode windowscodecs ole32 shell32 advapi32 gdi32 uuid)

  add_executable(wic_test test/wic_test.cpp)
  target_compile_definitions(wic_test PRIVATE UNICODE _UNICODE)
  target_link_options(wic_test PRIVATE /ENTRY:wmainCRTStartup)
  target_link_libraries(wic_test PRIVATE vbn_decode windowscodecs ole32 shell32 shlwapi gdi32 advapi32 uuid)
endif()

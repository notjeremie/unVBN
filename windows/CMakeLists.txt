# unVBN for Windows: a WIC codec (unvbn_wic.dll) so Windows can open Vizrt .vbn images, the export tools
# (unvbn.exe, the CLI, and unvbn-export.exe, the export window), plus tests.
#   cmake -S windows -B build -A x64 -DUNVBN_VERSION=0.3.2 ^
#     -DCMAKE_TOOLCHAIN_FILE=%VCPKG_ROOT%/scripts/buildsystems/vcpkg.cmake -DVCPKG_TARGET_TRIPLET=x64-windows-static
#   cmake --build build --config Release
# vcpkg (manifest vcpkg.json) supplies libvpx and libwebm, linked statically. Use arm64-windows-static for ARM64.
# On macOS/Linux only the portable tests build (decoder against real samples; export core with a fake encoder).
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

# Export core: sequence scanning, naming, the parallel exporter and batch runner, BT.709 conversion.
add_library(vbn_export STATIC src/export/sequence.cpp src/export/exporter.cpp src/export/yuv.cpp)
target_include_directories(vbn_export PUBLIC src)
target_link_libraries(vbn_export PUBLIC vbn_decode)
if(MSVC)
  target_compile_options(vbn_export PRIVATE /W4 /WX /permissive- /utf-8)
  target_compile_definitions(vbn_export PUBLIC UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
else()
  target_compile_options(vbn_export PRIVATE -Wall -Wextra -Werror)
endif()

if(NOT WIN32)
  # Without the Windows encoders: the tests drive the exporter through a recording sink.
  target_sources(vbn_export PRIVATE src/export/sinks_none.cpp)
  find_package(Threads REQUIRED)
  target_link_libraries(vbn_export PUBLIC Threads::Threads)
  add_executable(export_test test/export_test.cpp)
  target_link_libraries(export_test PRIVATE vbn_export)
endif()

if(WIN32)
  configure_file(src/version.h.in ${CMAKE_CURRENT_BINARY_DIR}/generated/version.h @ONLY)
  # What every shipped binary gets: strict warnings, Control Flow Guard, the generated version header.
  function(unvbn_shipped target)
    target_include_directories(${target} PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/generated)
    target_compile_definitions(${target} PRIVATE UNICODE _UNICODE)
    target_compile_options(${target} PRIVATE /W4 /WX /permissive- /guard:cf)
    target_link_options(${target} PRIVATE /guard:cf /DYNAMICBASE /NXCOMPAT)
  endfunction()

  add_library(unvbn_wic SHARED src/wic_codec.cpp src/unvbn_wic.def src/unvbn_wic.rc)
  unvbn_shipped(unvbn_wic)
  target_link_libraries(unvbn_wic PRIVATE vbn_decode windowscodecs ole32 shell32 advapi32 gdi32 uuid)

  add_executable(wic_test test/wic_test.cpp)
  target_compile_definitions(wic_test PRIVATE UNICODE _UNICODE)
  target_link_options(wic_test PRIVATE /ENTRY:wmainCRTStartup)
  target_link_libraries(wic_test PRIVATE vbn_decode windowscodecs ole32 shell32 shlwapi gdi32 advapi32 uuid)

  # Encoders: WIC (PNG), Media Foundation (H.264 MP4), libvpx + libwebm (WebM VP9 + alpha), from vcpkg.
  # vcpkg's imported targets pick the release or debug library to match the configuration.
  find_package(unofficial-libvpx CONFIG REQUIRED)
  find_package(unofficial-libwebm CONFIG REQUIRED)
  # libwebm's headers include each other as "mkvmuxer/...", relative to include/webm.
  find_path(WEBM_INCLUDE_DIR mkvmuxer/mkvmuxer.h PATH_SUFFIXES webm REQUIRED)
  find_path(VPX_INCLUDE_DIR vpx/vpx_encoder.h REQUIRED)
  target_sources(vbn_export PRIVATE src/export/sinks_win.cpp)
  target_include_directories(vbn_export PUBLIC ${WEBM_INCLUDE_DIR})
  target_link_libraries(vbn_export PUBLIC unofficial::libvpx::libvpx unofficial::libwebm::libwebm
                        windowscodecs mfplat mfreadwrite mfuuid ole32 uuid)

  add_executable(unvbn src/cli/main.cpp src/cli/cli.rc)
  unvbn_shipped(unvbn)
  target_compile_options(unvbn PRIVATE /utf-8)
  target_link_libraries(unvbn PRIVATE vbn_export)

  add_executable(unvbn-export WIN32 src/app/app.cpp src/app/app.rc)
  unvbn_shipped(unvbn-export)
  target_compile_options(unvbn-export PRIVATE /utf-8)
  # The manifest comes from app.rc (common controls v6, per-monitor DPI).
  target_link_options(unvbn-export PRIVATE /MANIFEST:NO)
  target_link_libraries(unvbn-export PRIVATE vbn_export comctl32 comdlg32 shell32 ole32 uuid)

  # THIRD_PARTY_NOTICES.txt next to the binaries (the MSI installs it): the licences of what is linked in.
  find_file(VPX_COPYRIGHT copyright PATHS ${VPX_INCLUDE_DIR}/../share/libvpx NO_DEFAULT_PATH REQUIRED)
  find_file(WEBM_COPYRIGHT copyright PATHS ${VPX_INCLUDE_DIR}/../share/libwebm NO_DEFAULT_PATH REQUIRED)
  file(READ ${VPX_COPYRIGHT} _vpx_license)
  file(READ ${WEBM_COPYRIGHT} _webm_license)
  file(GENERATE OUTPUT $<TARGET_FILE_DIR:unvbn>/THIRD_PARTY_NOTICES.txt CONTENT
"unVBN for Windows links the following third-party software into unvbn.exe and unvbn-export.exe.
Both are unmodified upstream releases built by vcpkg.

== libvpx (VP9 encoder) - https://chromium.googlesource.com/webm/libvpx ==

${_vpx_license}

== libwebm (WebM muxer) - https://chromium.googlesource.com/webm/libwebm ==

${_webm_license}
")

  add_executable(export_test test/export_test.cpp test/export_test_win.cpp)
  target_compile_options(export_test PRIVATE /utf-8)
  target_link_libraries(export_test PRIVATE vbn_export)
endif()

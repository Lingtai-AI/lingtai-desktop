cmake_minimum_required(VERSION 3.25)
project(lingtai_desktop LANGUAGES C CXX OBJC OBJCXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_OSX_DEPLOYMENT_TARGET 10.13 CACHE STRING "Minimum macOS version")
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC OFF)

set(LINGTAI_DEPS_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/.deps" CACHE PATH
    "Verified local dependency root created by scripts/bootstrap-deps.sh")
set(QT_ROOT "$ENV{QT_ROOT}" CACHE PATH "Official Qt 6.11.1 macOS prefix")
if(NOT QT_ROOT)
    set(QT_ROOT "$ENV{HOME}/Qt/6.11.1/macos" CACHE PATH "" FORCE)
endif()

set(SRC "${LINGTAI_DEPS_ROOT}/src")
set(DEPS "${LINGTAI_DEPS_ROOT}/third_party")
set(OPENSSL_BUILD "${LINGTAI_DEPS_ROOT}/build/openssl")
set(PARENT_INPUTS "${LINGTAI_DEPS_ROOT}/parent-inputs")
set(cmake_helpers_loc "${LINGTAI_DEPS_ROOT}/cmake_helpers")
set(submodules_loc "${PARENT_INPUTS}")
set(third_party_loc "${DEPS}")
set(QT_VERSION 6.11.1)
set(QT_VERSION_MAJOR 6)
set(DESKTOP_APP_DISABLE_CRASH_REPORTS ON)

foreach(required_path
    "${SRC}/lib_ui/CMakeLists.txt"
    "${SRC}/lib_base/CMakeLists.txt"
    "${SRC}/lib_rpl/CMakeLists.txt"
    "${SRC}/lib_crl/CMakeLists.txt"
    "${SRC}/codegen/codegen/common/CMakeLists.txt"
    "${DEPS}/GSL/include"
    "${DEPS}/expected/include"
    "${DEPS}/range-v3/include"
    "${DEPS}/lz4/lib/lz4.c"
    "${DEPS}/xxHash/xxhash.h"
    "${DEPS}/zlib/contrib/minizip/zip.c"
    "${OPENSSL_BUILD}/include/openssl"
    "${OPENSSL_BUILD}/lib/libssl.a"
    "${OPENSSL_BUILD}/lib/libcrypto.a"
    "${cmake_helpers_loc}/init_target.cmake"
    "${PARENT_INPUTS}/lib_ui"
    "${PARENT_INPUTS}/Resources")
    if(NOT EXISTS "${required_path}")
        message(FATAL_ERROR
            "Missing verified dependency input: ${required_path}\n"
            "Run scripts/bootstrap-deps.sh before configuring.")
    endif()
endforeach()

if(NOT EXISTS "${QT_ROOT}/lib/cmake/Qt6")
    message(FATAL_ERROR
        "QT_ROOT is not an official Qt 6.11.1 macOS prefix: ${QT_ROOT}\n"
        "Set QT_ROOT or use the documented default $HOME/Qt/6.11.1/macos.")
endif()
list(PREPEND CMAKE_PREFIX_PATH "${QT_ROOT}")
find_package(Qt6 6.11.1 EXACT REQUIRED COMPONENTS
    Core Gui Widgets Network Svg OpenGL OpenGLWidgets)
# Qt 6.11.1 ships the private targets as explicit packages required by full lib_ui.
find_package(Qt6CorePrivate 6.11.1 EXACT REQUIRED)
find_package(Qt6GuiPrivate 6.11.1 EXACT REQUIRED)
find_package(Qt6WidgetsPrivate 6.11.1 EXACT REQUIRED)

# Link the platform SDK zlib rather than a package-manager prefix.
add_library(sdk_zlib INTERFACE)
target_link_libraries(sdk_zlib INTERFACE "-lz")
add_library(ZLIB::ZLIB ALIAS sdk_zlib)

# Upstream helpers expect this parent-owned options target.
add_library(common_options INTERFACE)
add_library(desktop-app::common_options ALIAS common_options)
target_compile_definitions(common_options INTERFACE
    $<$<CONFIG:Debug>:DEBUG>
    DESKTOP_APP_DISABLE_CRASH_REPORTS)

# These are the exact checksum-verified files named by the lock manifest.
include("${cmake_helpers_loc}/init_target.cmake")
include("${cmake_helpers_loc}/nice_target_sources.cmake")
include("${cmake_helpers_loc}/generate_target.cmake")
include("${cmake_helpers_loc}/target_prepare_qrc.cmake")

# Minimum faithful dependency target surface required by the complete lib_ui target.
add_library(external_qt INTERFACE)
add_library(desktop-app::external_qt ALIAS external_qt)
target_link_libraries(external_qt INTERFACE
    Qt6::Core Qt6::CorePrivate
    Qt6::Gui Qt6::GuiPrivate
    Qt6::Widgets Qt6::WidgetsPrivate
    Qt6::Network Qt6::Svg Qt6::OpenGL Qt6::OpenGLWidgets
    "-framework Cocoa" "-framework Carbon" "-framework IOKit"
    "-framework LocalAuthentication" "-framework Security"
    "-framework QuartzCore" "-framework Metal")

add_library(external_qt_static_plugins INTERFACE)
add_library(desktop-app::external_qt_static_plugins ALIAS external_qt_static_plugins)
target_link_libraries(external_qt_static_plugins INTERFACE external_qt)

add_library(external_gsl INTERFACE)
add_library(desktop-app::external_gsl ALIAS external_gsl)
target_include_directories(external_gsl SYSTEM INTERFACE "${DEPS}/GSL/include")

add_library(external_ranges INTERFACE)
add_library(desktop-app::external_ranges ALIAS external_ranges)
target_include_directories(external_ranges SYSTEM INTERFACE "${DEPS}/range-v3/include")

add_library(external_expected INTERFACE)
add_library(desktop-app::external_expected ALIAS external_expected)
target_include_directories(external_expected SYSTEM INTERFACE "${DEPS}/expected/include")

add_library(external_openssl INTERFACE)
add_library(desktop-app::external_openssl ALIAS external_openssl)
target_include_directories(external_openssl SYSTEM INTERFACE "${OPENSSL_BUILD}/include")
target_link_libraries(external_openssl INTERFACE
    "${OPENSSL_BUILD}/lib/libssl.a"
    "${OPENSSL_BUILD}/lib/libcrypto.a")

add_library(external_crash_reports INTERFACE)
add_library(desktop-app::external_crash_reports ALIAS external_crash_reports)

add_library(external_xxhash INTERFACE)
add_library(desktop-app::external_xxhash ALIAS external_xxhash)
target_include_directories(external_xxhash SYSTEM INTERFACE "${DEPS}/xxHash")
target_compile_definitions(external_xxhash INTERFACE XXH_INLINE_ALL)

add_library(external_lz4_bundled STATIC
    "${DEPS}/lz4/lib/lz4.c"
    "${DEPS}/lz4/lib/lz4frame.c"
    "${DEPS}/lz4/lib/lz4hc.c"
    "${DEPS}/lz4/lib/xxhash.c")
init_target(external_lz4_bundled "(external)")
target_include_directories(external_lz4_bundled PUBLIC "${DEPS}/lz4/lib")
add_library(external_lz4 INTERFACE)
add_library(desktop-app::external_lz4 ALIAS external_lz4)
target_link_libraries(external_lz4 INTERFACE external_lz4_bundled)

add_library(external_minizip STATIC
    "${DEPS}/zlib/contrib/minizip/ioapi.c"
    "${DEPS}/zlib/contrib/minizip/zip.c"
    "${DEPS}/zlib/contrib/minizip/unzip.c")
init_target(external_minizip "(external)")
target_include_directories(external_minizip PUBLIC "${DEPS}/zlib/contrib")
target_link_libraries(external_minizip PUBLIC ZLIB::ZLIB)
add_library(external_zlib INTERFACE)
add_library(desktop-app::external_zlib ALIAS external_zlib)
target_link_libraries(external_zlib INTERFACE ZLIB::ZLIB external_minizip)

add_library(external_jpeg INTERFACE)
add_library(desktop-app::external_jpeg ALIAS external_jpeg)
target_include_directories(external_jpeg SYSTEM INTERFACE "${QT_ROOT}/include/QtJpeg")
target_link_libraries(external_jpeg INTERFACE "${QT_ROOT}/lib/libQt6BundledLibjpeg.a")

# Exact pinned toolkit targets, in upstream dependency order.
add_subdirectory("${SRC}/lib_rpl" lib_rpl)
add_subdirectory("${SRC}/lib_crl" lib_crl)
add_subdirectory("${SRC}/lib_base" lib_base)

# Only the exact code generators needed by lib_ui are added.
add_subdirectory("${SRC}/codegen/codegen/common" codegen_common)
add_subdirectory("${SRC}/codegen/codegen/emoji" codegen_emoji)
add_subdirectory("${SRC}/codegen/codegen/style" codegen_style)

# Build the complete pinned lib_ui target without patches or source overlays.
add_subdirectory("${SRC}/lib_ui" lib_ui)
target_compile_options(lib_ui PRIVATE $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-weak>)

add_executable(lingtai_desktop_smoke
    src/main.cpp
    src/crl_integration.cpp)
init_target(lingtai_desktop_smoke)
target_link_libraries(lingtai_desktop_smoke PRIVATE desktop-app::lib_ui)

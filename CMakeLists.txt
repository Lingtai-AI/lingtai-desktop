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

add_library(lingtai_desktop_core STATIC
    src/project_attachment.cpp
    src/workspace_selection.cpp)
init_target(lingtai_desktop_core)
target_include_directories(lingtai_desktop_core PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src")

add_library(lingtai_desktop_native_shell STATIC
    src/native_shell.cpp
    src/native_window_background.mm
    src/project_bootstrap.cpp
    src/ui/agent_roster.cpp
    src/ui/conversation_surface.cpp)
init_target(lingtai_desktop_native_shell)
target_include_directories(lingtai_desktop_native_shell PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_options(lingtai_desktop_native_shell PRIVATE
    -Wall -Wextra -Werror -pedantic
    $<$<COMPILE_LANG_AND_ID:CXX,AppleClang>:-Wno-unused-parameter>
    $<$<COMPILE_LANG_AND_ID:CXX,AppleClang>:-Wno-deprecated-this-capture>
    $<$<COMPILE_LANG_AND_ID:CXX,GNU>:-Wno-unused-parameter>)
target_link_libraries(lingtai_desktop_native_shell
    PUBLIC lingtai_desktop_core
    PRIVATE desktop-app::lib_ui
        Qt6::Core
        lingtai_desktop_agent_projection lingtai_desktop_conversation
        lingtai_desktop_mail_publisher lingtai_desktop_agent_sleep
        lingtai_desktop_agent_launch
        lingtai_desktop_agent_preset_summary)

# Internal POSIX descriptor/no-follow primitives. No Qt, no project model, and
# deliberately no link edge of its own: it is the shared safety mechanism every
# descriptor-anchored reader must not re-derive, not a filesystem framework.
# Definitions live in one substitutable link-time unit so the contract can be
# exercised against a scaffold independently of any consumer.
add_library(lingtai_desktop_posix_primitives STATIC
    src/posix_descriptor_primitives.cpp)
init_target(lingtai_desktop_posix_primitives)
target_include_directories(lingtai_desktop_posix_primitives PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_options(lingtai_desktop_posix_primitives PRIVATE
    -Wall -Wextra -Werror -pedantic)

add_library(lingtai_desktop_agent_projection STATIC
    src/agent_projection.cpp)
init_target(lingtai_desktop_agent_projection)
target_include_directories(lingtai_desktop_agent_projection PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_options(lingtai_desktop_agent_projection PRIVATE
    -Wall -Wextra -Werror -pedantic)
target_link_libraries(lingtai_desktop_agent_projection
    PUBLIC lingtai_desktop_core
    PRIVATE Qt6::Core lingtai_desktop_posix_primitives)

# Pure route resolution: no Qt, no filesystem, and deliberately no link edge to
# discovery, so a second manifest or status read is structurally impossible.
add_library(lingtai_desktop_direct_route STATIC
    src/direct_conversation_route.cpp)
init_target(lingtai_desktop_direct_route)
target_include_directories(lingtai_desktop_direct_route PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_options(lingtai_desktop_direct_route PRIVATE
    -Wall -Wextra -Werror -pedantic)
target_link_libraries(lingtai_desktop_direct_route
    PUBLIC lingtai_desktop_core)

# Pure composer classification: it recognizes one TUI-shaped leading-slash
# command without owning dispatch, widgets, filesystem access, or side effects.
add_library(lingtai_desktop_slash_command STATIC
    src/slash_command.cpp)
init_target(lingtai_desktop_slash_command)
target_include_directories(lingtai_desktop_slash_command PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_options(lingtai_desktop_slash_command PRIVATE
    -Wall -Wextra -Werror -pedantic)
target_link_libraries(lingtai_desktop_native_shell PRIVATE
    lingtai_desktop_slash_command)

# The one current conversation owner: it turns an already-accepted route into
# the direct rows the selected-Agent surface renders. It reads the human's own
# mailbox and nothing else, and it owns no presentation.
add_library(lingtai_desktop_conversation STATIC
    src/direct_conversation_history.cpp)
init_target(lingtai_desktop_conversation)
target_include_directories(lingtai_desktop_conversation PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_options(lingtai_desktop_conversation PRIVATE
    -Wall -Wextra -Werror -pedantic)
target_link_libraries(lingtai_desktop_conversation
    PUBLIC lingtai_desktop_direct_route
    PRIVATE Qt6::Core lingtai_desktop_posix_primitives)

# The one current publisher owner: it turns an already-accepted route and a
# validated plain-text body into one exclusively created human outbox leaf. It
# writes only that leaf and never the target inbox or human sent.
add_library(lingtai_desktop_mail_publisher STATIC
    src/direct_mail_publisher.cpp)
init_target(lingtai_desktop_mail_publisher)
target_include_directories(lingtai_desktop_mail_publisher PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_options(lingtai_desktop_mail_publisher PRIVATE
    -Wall -Wextra -Werror -pedantic)
target_link_libraries(lingtai_desktop_mail_publisher
    PUBLIC lingtai_desktop_direct_route
    PRIVATE Qt6::Core lingtai_desktop_posix_primitives)

# The one Presets summary reader owner: it turns the selected Agent's own
# kernel-published `system/manifest.resolved.json` into a bounded, stateless,
# read-only policy/effective projection. It reads no other source (init.json
# is opened only for an mtime staleness comparison), never dereferences an
# allowed ref, and owns no presentation.
add_library(lingtai_desktop_agent_preset_summary STATIC
    src/agent_preset_summary.cpp)
init_target(lingtai_desktop_agent_preset_summary)
target_include_directories(lingtai_desktop_agent_preset_summary PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_options(lingtai_desktop_agent_preset_summary PRIVATE
    -Wall -Wextra -Werror -pedantic)
target_link_libraries(lingtai_desktop_agent_preset_summary
    PUBLIC lingtai_desktop_core
    PRIVATE Qt6::Core lingtai_desktop_posix_primitives)

# The one Request sleep leaf owner: it turns an already-accepted project
# attachment and an exact selected Agent key into one local `.sleep` marker
# write, plus a best-effort target-side `sleep_received` observation of the
# selected Agent's own `logs/events.jsonl`. It owns no eligibility policy,
# UI, timer, or command framework.
add_library(lingtai_desktop_agent_sleep STATIC
    src/agent_sleep.cpp)
init_target(lingtai_desktop_agent_sleep)
target_include_directories(lingtai_desktop_agent_sleep PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_options(lingtai_desktop_agent_sleep PRIVATE
    -Wall -Wextra -Werror -pedantic)
target_link_libraries(lingtai_desktop_agent_sleep
    PUBLIC lingtai_desktop_core
    PRIVATE Qt6::Core lingtai_desktop_posix_primitives)

# The one Start Agent leaf owner: it turns an already-accepted project
# attachment, an exact selected Agent key, and one Desktop-supplied fallback
# interpreter into one detached, shell-free `<python> -m lingtai run <dir>`
# start. It owns no eligibility policy, UI, timer, PID tracking, or command
# framework.
add_library(lingtai_desktop_agent_launch STATIC
    src/agent_launch.cpp)
init_target(lingtai_desktop_agent_launch)
target_include_directories(lingtai_desktop_agent_launch PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_options(lingtai_desktop_agent_launch PRIVATE
    -Wall -Wextra -Werror -pedantic)
target_link_libraries(lingtai_desktop_agent_launch
    PUBLIC lingtai_desktop_core
    PRIVATE Qt6::Core)

add_executable(lingtai_agent_projection_test
    tests/agent_projection_test.cpp)
init_target(lingtai_agent_projection_test)
target_compile_options(lingtai_agent_projection_test PRIVATE
    -Wall -Wextra -Werror -pedantic)
target_link_libraries(lingtai_agent_projection_test PRIVATE
    lingtai_desktop_agent_projection)
add_executable(lingtai_direct_conversation_route_test
    tests/direct_conversation_route_test.cpp)
init_target(lingtai_direct_conversation_route_test)
target_compile_options(lingtai_direct_conversation_route_test PRIVATE
    -Wall -Wextra -Werror -pedantic)
target_link_libraries(lingtai_direct_conversation_route_test PRIVATE
    lingtai_desktop_direct_route)
add_executable(lingtai_slash_command_test
    tests/slash_command_test.cpp)
init_target(lingtai_slash_command_test)
target_include_directories(lingtai_slash_command_test PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_options(lingtai_slash_command_test PRIVATE
    -Wall -Wextra -Werror -pedantic)
target_link_libraries(lingtai_slash_command_test PRIVATE
    lingtai_desktop_slash_command)
add_executable(lingtai_direct_conversation_history_test
    tests/direct_conversation_history_test.cpp)
init_target(lingtai_direct_conversation_history_test)
target_compile_options(lingtai_direct_conversation_history_test PRIVATE
    -Wall -Wextra -Werror -pedantic)
target_link_libraries(lingtai_direct_conversation_history_test PRIVATE
    lingtai_desktop_conversation)
add_executable(lingtai_direct_mail_publisher_test
    tests/direct_mail_publisher_test.cpp)
init_target(lingtai_direct_mail_publisher_test)
target_compile_options(lingtai_direct_mail_publisher_test PRIVATE
    -Wall -Wextra -Werror -pedantic)
target_link_libraries(lingtai_direct_mail_publisher_test PRIVATE
    lingtai_desktop_mail_publisher)
add_executable(lingtai_agent_preset_summary_test
    tests/agent_preset_summary_test.cpp)
init_target(lingtai_agent_preset_summary_test)
target_compile_options(lingtai_agent_preset_summary_test PRIVATE
    -Wall -Wextra -Werror -pedantic)
target_link_libraries(lingtai_agent_preset_summary_test PRIVATE
    lingtai_desktop_agent_preset_summary)
add_executable(lingtai_agent_sleep_test
    tests/agent_sleep_test.cpp)
init_target(lingtai_agent_sleep_test)
target_compile_options(lingtai_agent_sleep_test PRIVATE
    -Wall -Wextra -Werror -pedantic)
target_link_libraries(lingtai_agent_sleep_test PRIVATE
    lingtai_desktop_agent_sleep)
add_executable(lingtai_posix_descriptor_primitives_test
    tests/posix_descriptor_primitives_test.cpp)
init_target(lingtai_posix_descriptor_primitives_test)
target_compile_options(lingtai_posix_descriptor_primitives_test PRIVATE
    -Wall -Wextra -Werror -pedantic)
target_link_libraries(lingtai_posix_descriptor_primitives_test PRIVATE
    lingtai_desktop_posix_primitives)
add_executable(lingtai_workspace_selection_test
    tests/workspace_selection_test.cpp)
init_target(lingtai_workspace_selection_test)
target_compile_options(lingtai_workspace_selection_test PRIVATE
    -Wall -Wextra -Werror -pedantic)
target_link_libraries(lingtai_workspace_selection_test PRIVATE
    lingtai_desktop_core)
add_executable(lingtai_project_attachment_test
    tests/project_attachment_test.cpp)
init_target(lingtai_project_attachment_test)
target_include_directories(lingtai_project_attachment_test PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_options(lingtai_project_attachment_test PRIVATE
    -Wall -Wextra -Werror -pedantic)
target_link_libraries(lingtai_project_attachment_test PRIVATE
    lingtai_desktop_core)
add_executable(lingtai_agent_command_runner_test
    tests/agent_command_runner_test.cpp)
init_target(lingtai_agent_command_runner_test)
target_include_directories(lingtai_agent_command_runner_test PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_options(lingtai_agent_command_runner_test PRIVATE
    -Wall -Wextra -Werror -pedantic)
target_link_libraries(lingtai_agent_command_runner_test PRIVATE
    Qt6::Core)
enable_testing()
add_test(NAME project_attachment
    COMMAND lingtai_project_attachment_test
        "${CMAKE_CURRENT_BINARY_DIR}/project-attachment-fixture")
add_test(NAME agent_projection
    COMMAND lingtai_agent_projection_test
        "${CMAKE_CURRENT_BINARY_DIR}/agent-projection-fixture")
add_test(NAME direct_conversation_route
    COMMAND lingtai_direct_conversation_route_test
        "${CMAKE_CURRENT_BINARY_DIR}/direct-conversation-route-fixture")
add_test(NAME slash_command
    COMMAND lingtai_slash_command_test)
add_test(NAME direct_conversation_history
    COMMAND lingtai_direct_conversation_history_test
        "${CMAKE_CURRENT_BINARY_DIR}/direct-conversation-history-fixture")
add_test(NAME direct_mail_publisher
    COMMAND lingtai_direct_mail_publisher_test
        "${CMAKE_CURRENT_BINARY_DIR}/direct-mail-publisher-fixture")
add_test(NAME agent_preset_summary
    COMMAND lingtai_agent_preset_summary_test
        "${CMAKE_CURRENT_BINARY_DIR}/agent-preset-summary-fixture")
add_test(NAME agent_sleep
    COMMAND lingtai_agent_sleep_test
        "${CMAKE_CURRENT_BINARY_DIR}/agent-sleep-fixture")
add_test(NAME posix_descriptor_primitives
    COMMAND lingtai_posix_descriptor_primitives_test
        "${CMAKE_CURRENT_BINARY_DIR}/posix-descriptor-primitives-fixture")
add_test(NAME workspace_selection
    COMMAND lingtai_workspace_selection_test
        "${CMAKE_CURRENT_BINARY_DIR}/workspace-selection-fixture")
add_test(NAME agent_command_runner
    COMMAND lingtai_agent_command_runner_test
        "${CMAKE_CURRENT_BINARY_DIR}/agent-command-runner-fixture")

add_executable(lingtai_desktop_smoke
    src/main.cpp
    src/crl_integration.cpp)
init_target(lingtai_desktop_smoke)
target_link_libraries(lingtai_desktop_smoke PRIVATE
    lingtai_desktop_native_shell
    desktop-app::lib_ui)

find_package(Python3 REQUIRED COMPONENTS Interpreter)
add_test(NAME native_shell
    COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_native_shell.py"
        --executable $<TARGET_FILE:lingtai_desktop_smoke>)

add_executable(lingtai_native_shell_test
    tests/native_shell_test.cpp
    src/crl_integration.cpp)
init_target(lingtai_native_shell_test)
target_compile_options(lingtai_native_shell_test PRIVATE
    -Wall -Wextra -Werror -pedantic
    $<$<COMPILE_LANG_AND_ID:CXX,AppleClang>:-Wno-unused-parameter>
    $<$<COMPILE_LANG_AND_ID:CXX,AppleClang>:-Wno-deprecated-this-capture>
    $<$<COMPILE_LANG_AND_ID:CXX,GNU>:-Wno-unused-parameter>)
target_link_libraries(lingtai_native_shell_test PRIVATE
    lingtai_desktop_native_shell
    desktop-app::lib_ui)

file(MAKE_DIRECTORY
    "${CMAKE_CURRENT_BINARY_DIR}/native-shell-no-write-fixture")
add_test(NAME native_shell_behavior
    COMMAND lingtai_native_shell_test
        "${CMAKE_CURRENT_BINARY_DIR}/native-shell-no-write-fixture")
if(APPLE)
    set_tests_properties(native_shell_behavior PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=cocoa")
else()
    set_tests_properties(native_shell_behavior PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
endif()

add_executable(lingtai_native_shell_destinations_test
    tests/native_shell_destinations_test.cpp
    src/crl_integration.cpp)
init_target(lingtai_native_shell_destinations_test)
target_compile_options(lingtai_native_shell_destinations_test PRIVATE
    -Wall -Wextra -Werror -pedantic
    $<$<COMPILE_LANG_AND_ID:CXX,AppleClang>:-Wno-unused-parameter>
    $<$<COMPILE_LANG_AND_ID:CXX,AppleClang>:-Wno-deprecated-this-capture>
    $<$<COMPILE_LANG_AND_ID:CXX,GNU>:-Wno-unused-parameter>)
target_link_libraries(lingtai_native_shell_destinations_test PRIVATE
    lingtai_desktop_native_shell
    desktop-app::lib_ui)
add_test(NAME native_shell_destinations
    COMMAND lingtai_native_shell_destinations_test)
if(APPLE)
    set_tests_properties(native_shell_destinations PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=cocoa")
else()
    set_tests_properties(native_shell_destinations PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
endif()

add_executable(lingtai_agent_roster_presentation_test
    tests/agent_roster_presentation_test.cpp
    src/crl_integration.cpp)
init_target(lingtai_agent_roster_presentation_test)
target_compile_options(lingtai_agent_roster_presentation_test PRIVATE
    -Wall -Wextra -Werror -pedantic
    $<$<COMPILE_LANG_AND_ID:CXX,AppleClang>:-Wno-unused-parameter>
    $<$<COMPILE_LANG_AND_ID:CXX,AppleClang>:-Wno-deprecated-this-capture>
    $<$<COMPILE_LANG_AND_ID:CXX,GNU>:-Wno-unused-parameter>)
add_executable(lingtai_native_shell_presets_test
    tests/native_shell_presets_test.cpp
    src/crl_integration.cpp)
init_target(lingtai_native_shell_presets_test)
target_compile_options(lingtai_native_shell_presets_test PRIVATE
    -Wall -Wextra -Werror -pedantic
    $<$<COMPILE_LANG_AND_ID:CXX,AppleClang>:-Wno-unused-parameter>
    $<$<COMPILE_LANG_AND_ID:CXX,AppleClang>:-Wno-deprecated-this-capture>
    $<$<COMPILE_LANG_AND_ID:CXX,GNU>:-Wno-unused-parameter>)
target_link_libraries(lingtai_agent_roster_presentation_test PRIVATE
    lingtai_desktop_native_shell
    desktop-app::lib_ui)
add_test(NAME agent_roster_presentation
    COMMAND lingtai_agent_roster_presentation_test)
if(APPLE)
    set_tests_properties(agent_roster_presentation PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=cocoa")
else()
    set_tests_properties(agent_roster_presentation PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
endif()
target_link_libraries(lingtai_native_shell_presets_test PRIVATE
    lingtai_desktop_native_shell
    desktop-app::lib_ui)
file(MAKE_DIRECTORY
    "${CMAKE_CURRENT_BINARY_DIR}/native-shell-presets-fixture")
add_test(NAME native_shell_presets
    COMMAND lingtai_native_shell_presets_test
        "${CMAKE_CURRENT_BINARY_DIR}/native-shell-presets-fixture")
if(APPLE)
    set_tests_properties(native_shell_presets PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=cocoa")
else()
    set_tests_properties(native_shell_presets PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
endif()
add_executable(lingtai_conversation_surface_typography_test
    tests/conversation_surface_typography_test.cpp
    src/ui/conversation_surface.cpp)
init_target(lingtai_conversation_surface_typography_test)
# The real palette globals the surface references come from the already-owned
# lib_ui dependency; only the direct source include is added here.
target_include_directories(lingtai_conversation_surface_typography_test PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_options(lingtai_conversation_surface_typography_test PRIVATE
    -Wall -Wextra -Werror -pedantic
    $<$<COMPILE_LANG_AND_ID:CXX,AppleClang>:-Wno-unused-parameter>
    $<$<COMPILE_LANG_AND_ID:CXX,AppleClang>:-Wno-deprecated-this-capture>
    $<$<COMPILE_LANG_AND_ID:CXX,GNU>:-Wno-unused-parameter>)
target_link_libraries(lingtai_conversation_surface_typography_test PRIVATE
    desktop-app::lib_ui
    Qt6::Widgets Qt6::Gui Qt6::Core)
add_test(NAME conversation_surface_typography
    COMMAND lingtai_conversation_surface_typography_test)
if(APPLE)
    set_tests_properties(conversation_surface_typography PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=cocoa")
else()
    set_tests_properties(conversation_surface_typography PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
endif()

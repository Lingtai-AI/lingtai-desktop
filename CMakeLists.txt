cmake_minimum_required(VERSION 3.25)
if(NOT DEFINED CMAKE_OSX_DEPLOYMENT_TARGET OR
        CMAKE_OSX_DEPLOYMENT_TARGET STREQUAL "")
    set(CMAKE_OSX_DEPLOYMENT_TARGET "13.0" CACHE STRING
        "Minimum macOS version" FORCE)
endif()
if(CMAKE_OSX_DEPLOYMENT_TARGET VERSION_LESS "13.0")
    message(FATAL_ERROR
        "Qt 6.11.1 requires macOS 13.0 or newer; "
        "CMAKE_OSX_DEPLOYMENT_TARGET=${CMAKE_OSX_DEPLOYMENT_TARGET} is unsupported.")
endif()
project(lingtai_desktop VERSION 0.1.6 LANGUAGES C CXX OBJC OBJCXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
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
    src/runtime_options.cpp
    src/workspace_selection.cpp)
init_target(lingtai_desktop_core)
target_include_directories(lingtai_desktop_core PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src")

# Qt-free local attachment selection/preflight facts. It reads selected source
# files without writing or authorizing publication; the later publisher owns
# revalidation and copying.
add_library(lingtai_desktop_attachment_selection STATIC
    src/attachment_selection.cpp)
init_target(lingtai_desktop_attachment_selection)
target_include_directories(lingtai_desktop_attachment_selection PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_options(lingtai_desktop_attachment_selection PRIVATE
    -Wall -Wextra -Werror -pedantic)

# Read-only Desktop owner for the global saved/template preset catalog. The
# caller injects the global root; missing directories are empty and unreadable
# directory scans return typed evidence without bootstrapping any files.
add_library(lingtai_desktop_preset_catalog STATIC
    src/preset_catalog.cpp)
init_target(lingtai_desktop_preset_catalog)
target_include_directories(lingtai_desktop_preset_catalog PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_options(lingtai_desktop_preset_catalog PRIVATE
    -Wall -Wextra -Werror -pedantic)
target_link_libraries(lingtai_desktop_preset_catalog PRIVATE Qt6::Core)

add_library(lingtai_desktop_native_shell STATIC
    src/attachment_thumbnail.cpp
    src/native_shell.cpp
    src/shell_host.cpp
    src/agent_detail_view.cpp
    src/preset_catalog_presentation.cpp
    src/preset_editor_model.cpp
    src/preset_editor_page.cpp
    src/agent_presets_page.cpp
    src/agent_config_page.cpp
    src/setup_toggle.cpp
    src/setup_style.cpp
    src/credentials_model.cpp
    src/credentials_page.cpp
    src/codex_credentials_strip.cpp
    src/codex_oauth.cpp
    src/project_setup_wizard.cpp
    src/kanban_page.cpp
    src/native_window_background.mm
    src/ui/agent_roster.cpp
    src/ui/conversation_surface.cpp
    src/ui/slash_command_card.cpp)
init_target(lingtai_desktop_native_shell)
qt_add_resources(lingtai_desktop_native_shell "lingtai_startup_assets"
    PREFIX "/lingtai/startup"
    BASE "${CMAKE_CURRENT_SOURCE_DIR}/resources/startup"
    FILES
        resources/startup/lingtai-logo-light-4096.png
        resources/startup/lingtai-logo-dark-4096.png)
qt_add_resources(lingtai_desktop_native_shell "lingtai_macos_assets"
    PREFIX "/lingtai/macos"
    BASE "${CMAKE_CURRENT_SOURCE_DIR}/resources/macos"
    FILES
        resources/macos/AppIcon-1024.png)
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
        Qt6::Network
        lingtai_desktop_agent_projection lingtai_desktop_conversation
        lingtai_desktop_mail_publisher lingtai_desktop_agent_lifecycle
        lingtai_desktop_agent_prompt_actions
        lingtai_desktop_agent_preset_summary
        lingtai_desktop_agent_setup_store
        lingtai_desktop_preset_catalog
        lingtai_desktop_project_creation
        lingtai_desktop_kanban)

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
# mailbox, descriptor-validates attachment metadata through the shared
# classifier owner, and owns fresh current-entry action revalidation; it reads
# no attachment content and owns neither presentation nor external launching.
add_library(lingtai_desktop_conversation STATIC
    src/direct_conversation_attachment_actions.cpp
    src/direct_conversation_history.cpp
    src/message_reactions.cpp
    src/injected_mail_journal.cpp
    src/conversation_unread.cpp
    src/conversation_session.cpp)
init_target(lingtai_desktop_conversation)
target_include_directories(lingtai_desktop_conversation PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_options(lingtai_desktop_conversation PRIVATE
    -Wall -Wextra -Werror -pedantic)
target_link_libraries(lingtai_desktop_conversation
    PUBLIC lingtai_desktop_direct_route
    PRIVATE Qt6::Core lingtai_desktop_attachment_selection
        lingtai_desktop_posix_primitives)

# The one current publisher owner: it turns an already-accepted route and a
# validated body and optional accepted attachment facts into one exclusively
# created human outbox leaf. It writes only that leaf and never the target
# inbox or human sent.
add_library(lingtai_desktop_mail_publisher STATIC
    src/direct_mail_publisher.cpp)
init_target(lingtai_desktop_mail_publisher)
target_include_directories(lingtai_desktop_mail_publisher PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_options(lingtai_desktop_mail_publisher PRIVATE
    -Wall -Wextra -Werror -pedantic)
target_link_libraries(lingtai_desktop_mail_publisher
    PUBLIC lingtai_desktop_direct_route lingtai_desktop_attachment_selection
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

add_library(lingtai_desktop_agent_setup_store STATIC
    src/agent_setup_store.cpp)
init_target(lingtai_desktop_agent_setup_store)
target_include_directories(lingtai_desktop_agent_setup_store PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_options(lingtai_desktop_agent_setup_store PRIVATE
    -Wall -Wextra -Werror -pedantic)
target_link_libraries(lingtai_desktop_agent_setup_store
    PUBLIC lingtai_desktop_core Qt6::Core
    PRIVATE lingtai_desktop_posix_primitives)

# The localized first-boot greeting/playbook is compiled into this Desktop
# owner; creation has no runtime content lookup or external product dependency.
add_library(lingtai_desktop_project_creation STATIC
    src/project_creation.cpp
    src/project_creation_resources.cpp)
init_target(lingtai_desktop_project_creation)
target_include_directories(lingtai_desktop_project_creation PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_options(lingtai_desktop_project_creation PRIVATE
    -Wall -Wextra -Werror -pedantic)
target_link_libraries(lingtai_desktop_project_creation
    PUBLIC lingtai_desktop_agent_setup_store lingtai_desktop_preset_catalog
        Qt6::Core
    PRIVATE lingtai_desktop_posix_primitives)

add_library(lingtai_desktop_kanban STATIC
    src/kanban_model.cpp)
init_target(lingtai_desktop_kanban)
target_include_directories(lingtai_desktop_kanban PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_options(lingtai_desktop_kanban PRIVATE
    -Wall -Wextra -Werror -pedantic)
target_link_libraries(lingtai_desktop_kanban
    PUBLIC lingtai_desktop_core lingtai_desktop_agent_projection
        lingtai_desktop_agent_preset_summary
    PRIVATE Qt6::Core lingtai_desktop_posix_primitives "-lsqlite3")

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
    PRIVATE Qt6::Core lingtai_desktop_agent_signal
        lingtai_desktop_posix_primitives)

# Descriptor-relative lifecycle signal writer shared by sleep and the complete
# Desktop-owned lifecycle controller. It owns only the fixed marker leaves and
# their fixed contents; target/phase policy stays in agent_lifecycle.
add_library(lingtai_desktop_agent_signal STATIC
    src/agent_signal.cpp)
init_target(lingtai_desktop_agent_signal)
target_include_directories(lingtai_desktop_agent_signal PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_options(lingtai_desktop_agent_signal PRIVATE
    -Wall -Wextra -Werror -pedantic)
target_link_libraries(lingtai_desktop_agent_signal
    PUBLIC lingtai_desktop_core
    PRIVATE lingtai_desktop_posix_primitives)

# The one in-conversation prompt/inquiry/goal owner: TUI `/btw`, `/insights`,
# `/molt`, `/export`, and `/goal` write `.prompt`, `.inquiry`, or one
# `goal.request` system notification without opening another page.
add_library(lingtai_desktop_agent_prompt_actions STATIC
    src/agent_prompt_actions.cpp)
init_target(lingtai_desktop_agent_prompt_actions)
target_include_directories(lingtai_desktop_agent_prompt_actions PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_options(lingtai_desktop_agent_prompt_actions PRIVATE
    -Wall -Wextra -Werror -pedantic)
target_link_libraries(lingtai_desktop_agent_prompt_actions
    PUBLIC lingtai_desktop_core
    PRIVATE Qt6::Core lingtai_desktop_posix_primitives)

# The one direct Agent launcher: it turns an already-accepted project
# attachment, an exact selected Agent key, and one Desktop-supplied fallback
# interpreter into one detached, shell-free `<python> -m lingtai run <dir>`
# start and returns detached PID/log facts. It owns no eligibility policy, UI,
# wait/escalation timer, or command framework.
add_library(lingtai_desktop_agent_launch STATIC
    src/agent_launch.cpp)
init_target(lingtai_desktop_agent_launch)
target_include_directories(lingtai_desktop_agent_launch PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_options(lingtai_desktop_agent_launch PRIVATE
    -Wall -Wextra -Werror -pedantic)
target_link_libraries(lingtai_desktop_agent_launch
    PUBLIC lingtai_desktop_core
    PRIVATE Qt6::Core lingtai_desktop_posix_primitives)

# Desktop-owned lifecycle orchestration: exact process observation/signaling,
# advisory-lease observation, preset mutation, launch/heartbeat observation,
# and the nonblocking per-target operation state machine.
add_library(lingtai_desktop_agent_lifecycle STATIC
    src/agent_lifecycle.cpp
    src/agent_process.cpp)
init_target(lingtai_desktop_agent_lifecycle)
target_include_directories(lingtai_desktop_agent_lifecycle PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_options(lingtai_desktop_agent_lifecycle PRIVATE
    -Wall -Wextra -Werror -pedantic)
target_link_libraries(lingtai_desktop_agent_lifecycle
    PUBLIC lingtai_desktop_core
    PRIVATE Qt6::Core
        lingtai_desktop_agent_signal
        lingtai_desktop_agent_sleep
        lingtai_desktop_agent_launch
        lingtai_desktop_posix_primitives)

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
add_executable(lingtai_direct_conversation_attachment_actions_test
    tests/direct_conversation_attachment_actions_test.cpp)
init_target(lingtai_direct_conversation_attachment_actions_test)
target_compile_options(lingtai_direct_conversation_attachment_actions_test PRIVATE
    -Wall -Wextra -Werror -pedantic)
target_link_libraries(lingtai_direct_conversation_attachment_actions_test PRIVATE
    lingtai_desktop_conversation)
add_executable(lingtai_message_reactions_test
    tests/message_reactions_test.cpp)
init_target(lingtai_message_reactions_test)
target_compile_options(lingtai_message_reactions_test PRIVATE
    -Wall -Wextra -Werror -pedantic)
target_link_libraries(lingtai_message_reactions_test PRIVATE
    lingtai_desktop_conversation)
add_executable(lingtai_conversation_unread_test
    tests/conversation_unread_test.cpp)
init_target(lingtai_conversation_unread_test)
target_compile_options(lingtai_conversation_unread_test PRIVATE
    -Wall -Wextra -Werror -pedantic)
target_link_libraries(lingtai_conversation_unread_test PRIVATE
    lingtai_desktop_conversation)
add_executable(lingtai_conversation_session_test
    tests/conversation_session_test.cpp)
init_target(lingtai_conversation_session_test)
target_compile_options(lingtai_conversation_session_test PRIVATE
    -Wall -Wextra -Werror -pedantic)
target_link_libraries(lingtai_conversation_session_test PRIVATE
    lingtai_desktop_conversation)
add_test(NAME conversation_session COMMAND lingtai_conversation_session_test)
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
add_executable(lingtai_agent_setup_store_test
    tests/agent_setup_store_test.cpp)
init_target(lingtai_agent_setup_store_test)
target_compile_options(lingtai_agent_setup_store_test PRIVATE
    -Wall -Wextra -Werror -pedantic)
target_link_libraries(lingtai_agent_setup_store_test PRIVATE
    lingtai_desktop_agent_setup_store Qt6::Core)

add_executable(lingtai_preset_catalog_test
    tests/preset_catalog_test.cpp)
init_target(lingtai_preset_catalog_test)
target_compile_options(lingtai_preset_catalog_test PRIVATE
    -Wall -Wextra -Werror -pedantic)
target_link_libraries(lingtai_preset_catalog_test PRIVATE
    lingtai_desktop_preset_catalog Qt6::Core)

add_executable(lingtai_project_creation_test
    tests/project_creation_test.cpp)
init_target(lingtai_project_creation_test)
target_compile_options(lingtai_project_creation_test PRIVATE
    -Wall -Wextra -Werror -pedantic)
target_compile_definitions(lingtai_project_creation_test PRIVATE
    LINGTAI_PROJECT_CREATION_FIXTURE_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}/tests/resources/project_creation\")
target_link_libraries(lingtai_project_creation_test PRIVATE
    lingtai_desktop_project_creation lingtai_desktop_agent_projection
    lingtai_desktop_slash_command)


add_executable(lingtai_agent_detail_view_test
    tests/agent_detail_view_test.cpp
    src/crl_integration.cpp)
init_target(lingtai_agent_detail_view_test)
target_compile_options(lingtai_agent_detail_view_test PRIVATE
    -Wall -Wextra -Werror -pedantic
    $<$<COMPILE_LANG_AND_ID:CXX,AppleClang>:-Wno-unused-parameter>
    $<$<COMPILE_LANG_AND_ID:CXX,AppleClang>:-Wno-deprecated-this-capture>
    $<$<COMPILE_LANG_AND_ID:CXX,GNU>:-Wno-unused-parameter>
    $<$<COMPILE_LANG_AND_ID:CXX,GNU>:-Wno-deprecated-this-capture>)
target_include_directories(lingtai_agent_detail_view_test PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(lingtai_agent_detail_view_test PRIVATE
    lingtai_desktop_native_shell
    desktop-app::lib_ui
    Qt6::Widgets Qt6::Gui Qt6::Core)
add_test(NAME agent_detail_view COMMAND lingtai_agent_detail_view_test)
if(APPLE)
    set_tests_properties(agent_detail_view PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=cocoa")
else()
    set_tests_properties(agent_detail_view PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
endif()

add_executable(lingtai_ui_test_harness_test
    tests/ui_test_harness_test.cpp
    src/crl_integration.cpp)
init_target(lingtai_ui_test_harness_test)
target_compile_options(lingtai_ui_test_harness_test PRIVATE
    -Wall -Wextra -Werror -pedantic
    $<$<COMPILE_LANG_AND_ID:CXX,AppleClang>:-Wno-unused-parameter>
    $<$<COMPILE_LANG_AND_ID:CXX,AppleClang>:-Wno-deprecated-this-capture>
    $<$<COMPILE_LANG_AND_ID:CXX,GNU>:-Wno-unused-parameter>
    $<$<COMPILE_LANG_AND_ID:CXX,GNU>:-Wno-deprecated-this-capture>)
target_include_directories(lingtai_ui_test_harness_test PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests")
target_compile_definitions(lingtai_ui_test_harness_test PRIVATE
    LINGTAI_UI_TEST_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures")
target_link_libraries(lingtai_ui_test_harness_test PRIVATE
    lingtai_desktop_native_shell
    desktop-app::lib_ui
    Qt6::Widgets Qt6::Gui Qt6::Core)
add_test(NAME ui_test_harness COMMAND lingtai_ui_test_harness_test)
if(APPLE)
    set_tests_properties(ui_test_harness PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=cocoa")
else()
    set_tests_properties(ui_test_harness PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
endif()

add_executable(lingtai_ui_agent_detail_pages_test
    tests/ui/agent_detail_pages_test.cpp
    src/crl_integration.cpp)
init_target(lingtai_ui_agent_detail_pages_test)
target_compile_options(lingtai_ui_agent_detail_pages_test PRIVATE
    -Wall -Wextra -Werror -pedantic
    $<$<COMPILE_LANG_AND_ID:CXX,AppleClang>:-Wno-unused-parameter>
    $<$<COMPILE_LANG_AND_ID:CXX,AppleClang>:-Wno-deprecated-this-capture>
    $<$<COMPILE_LANG_AND_ID:CXX,GNU>:-Wno-unused-parameter>
    $<$<COMPILE_LANG_AND_ID:CXX,GNU>:-Wno-deprecated-this-capture>)
target_include_directories(lingtai_ui_agent_detail_pages_test PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests")
target_compile_definitions(lingtai_ui_agent_detail_pages_test PRIVATE
    LINGTAI_UI_TEST_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures")
target_link_libraries(lingtai_ui_agent_detail_pages_test PRIVATE
    lingtai_desktop_native_shell
    desktop-app::lib_ui
    Qt6::Widgets Qt6::Gui Qt6::Core)
add_test(NAME ui_agent_detail_pages
    COMMAND lingtai_ui_agent_detail_pages_test)
if(APPLE)
    set_tests_properties(ui_agent_detail_pages PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=cocoa")
else()
    set_tests_properties(ui_agent_detail_pages PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
endif()

add_library(lingtai_ui_test_support STATIC
    tests/ui/UiTestFonts.cpp)
init_target(lingtai_ui_test_support)
target_include_directories(lingtai_ui_test_support PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/tests")
target_compile_definitions(lingtai_ui_test_support PUBLIC
    LINGTAI_UI_TEST_FONTS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/fonts")
target_link_libraries(lingtai_ui_test_support PUBLIC
    Qt6::Gui Qt6::Core)

add_library(lingtai_visual_test_utils STATIC
    tests/visual/VisualTestUtils.cpp)
init_target(lingtai_visual_test_utils)
target_include_directories(lingtai_visual_test_utils PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/tests")
target_link_libraries(lingtai_visual_test_utils PUBLIC
    Qt6::Widgets Qt6::Gui Qt6::Core)

add_executable(lingtai_native_shell_visual_test
    tests/visual/native_shell_visual_test.cpp
    tests/visual/VisualScenario.cpp
    src/crl_integration.cpp)
init_target(lingtai_native_shell_visual_test)
target_compile_options(lingtai_native_shell_visual_test PRIVATE
    -Wall -Wextra -Werror -pedantic
    $<$<COMPILE_LANG_AND_ID:CXX,AppleClang>:-Wno-unused-parameter>
    $<$<COMPILE_LANG_AND_ID:CXX,AppleClang>:-Wno-deprecated-this-capture>
    $<$<COMPILE_LANG_AND_ID:CXX,GNU>:-Wno-unused-parameter>
    $<$<COMPILE_LANG_AND_ID:CXX,GNU>:-Wno-deprecated-this-capture>)
target_include_directories(lingtai_native_shell_visual_test PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests")
target_compile_definitions(lingtai_native_shell_visual_test PRIVATE
    LINGTAI_UI_TEST_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures"
    LINGTAI_VISUAL_BASELINE_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/tests/visual/baselines")
target_link_libraries(lingtai_native_shell_visual_test PRIVATE
    lingtai_ui_test_support
    lingtai_visual_test_utils
    lingtai_desktop_native_shell
    desktop-app::lib_ui
    Qt6::Widgets Qt6::Gui Qt6::Core)
set(LINGTAI_VISUAL_SURFACES
    startup-idle
    setup-preset
    setup-agents
    setup-review
    conversation
    conversation-attachments
    conversation-attachments-narrow
    composer-attachments
    presets
    kanban
    empty-conversation)
set(LINGTAI_VISUAL_THEMES light dark)
set(LINGTAI_VISUAL_TESTS "")
foreach(surface IN LISTS LINGTAI_VISUAL_SURFACES)
    foreach(theme IN LISTS LINGTAI_VISUAL_THEMES)
        string(REPLACE "-" "_" surface_underscore "${surface}")
        set(visual_test_name "visual_${surface_underscore}_${theme}")
        add_test(
            NAME ${visual_test_name}
            COMMAND lingtai_native_shell_visual_test
                --surface=${surface} --theme=${theme})
        if(APPLE)
            set_tests_properties(${visual_test_name} PROPERTIES
                ENVIRONMENT "QT_QPA_PLATFORM=cocoa")
        else()
            set_tests_properties(${visual_test_name} PROPERTIES
                ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
        endif()
        list(APPEND LINGTAI_VISUAL_TESTS ${visual_test_name})
    endforeach()
endforeach()

add_executable(lingtai_kanban_model_test
    tests/kanban_model_test.cpp)
init_target(lingtai_kanban_model_test)
target_compile_options(lingtai_kanban_model_test PRIVATE
    -Wall -Wextra -Werror -pedantic)
target_link_libraries(lingtai_kanban_model_test PRIVATE
    lingtai_desktop_kanban)
add_executable(lingtai_agent_sleep_test
    tests/agent_sleep_test.cpp)
init_target(lingtai_agent_sleep_test)
target_compile_options(lingtai_agent_sleep_test PRIVATE
    -Wall -Wextra -Werror -pedantic)
target_link_libraries(lingtai_agent_sleep_test PRIVATE
    lingtai_desktop_agent_sleep)
add_executable(lingtai_agent_lifecycle_test
    tests/agent_lifecycle_test.cpp)
init_target(lingtai_agent_lifecycle_test)
target_compile_options(lingtai_agent_lifecycle_test PRIVATE
    -Wall -Wextra -Werror -pedantic)
target_link_libraries(lingtai_agent_lifecycle_test PRIVATE
    lingtai_desktop_agent_lifecycle Qt6::Core)
add_executable(lingtai_agent_lifecycle_real_smoke
    tests/agent_lifecycle_real_smoke.cpp)
init_target(lingtai_agent_lifecycle_real_smoke)
target_compile_options(lingtai_agent_lifecycle_real_smoke PRIVATE
    -Wall -Wextra -Werror -pedantic)
target_link_libraries(lingtai_agent_lifecycle_real_smoke PRIVATE
    lingtai_desktop_agent_lifecycle
    lingtai_desktop_project_creation)
add_executable(lingtai_agent_prompt_actions_test
    tests/agent_prompt_actions_test.cpp)
init_target(lingtai_agent_prompt_actions_test)
target_compile_options(lingtai_agent_prompt_actions_test PRIVATE
    -Wall -Wextra -Werror -pedantic)
target_link_libraries(lingtai_agent_prompt_actions_test PRIVATE
    lingtai_desktop_agent_prompt_actions
    Qt6::Core)
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
add_executable(lingtai_attachment_selection_test
    tests/attachment_selection_test.cpp)
init_target(lingtai_attachment_selection_test)
target_compile_options(lingtai_attachment_selection_test PRIVATE
    -Wall -Wextra -Werror -pedantic)
target_link_libraries(lingtai_attachment_selection_test PRIVATE
    lingtai_desktop_attachment_selection)
enable_testing()
add_test(NAME project_attachment
    COMMAND lingtai_project_attachment_test
        "${CMAKE_CURRENT_BINARY_DIR}/project-attachment-fixture")
add_test(NAME attachment_selection
    COMMAND lingtai_attachment_selection_test
        "${CMAKE_CURRENT_BINARY_DIR}/attachment-selection-fixture")
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
add_test(NAME direct_conversation_attachment_actions
    COMMAND lingtai_direct_conversation_attachment_actions_test
        "${CMAKE_CURRENT_BINARY_DIR}/direct-conversation-attachment-actions-fixture")
add_test(NAME message_reactions
    COMMAND lingtai_message_reactions_test)
add_test(NAME conversation_unread
    COMMAND lingtai_conversation_unread_test)
add_test(NAME direct_mail_publisher
    COMMAND lingtai_direct_mail_publisher_test
        "${CMAKE_CURRENT_BINARY_DIR}/direct-mail-publisher-fixture")
add_test(NAME agent_preset_summary
    COMMAND lingtai_agent_preset_summary_test
        "${CMAKE_CURRENT_BINARY_DIR}/agent-preset-summary-fixture")
add_test(NAME agent_setup_store
    COMMAND lingtai_agent_setup_store_test
        "${CMAKE_CURRENT_BINARY_DIR}/agent-setup-store-fixture")
add_test(NAME preset_catalog
    COMMAND lingtai_preset_catalog_test
        "${CMAKE_CURRENT_BINARY_DIR}/preset-catalog-fixture")
add_test(NAME project_creation
    COMMAND lingtai_project_creation_test
        "${CMAKE_CURRENT_BINARY_DIR}/project-creation-fixture")
add_test(NAME kanban_model
    COMMAND lingtai_kanban_model_test
        "${CMAKE_CURRENT_BINARY_DIR}/kanban-model-fixture")
add_test(NAME agent_sleep
    COMMAND lingtai_agent_sleep_test
        "${CMAKE_CURRENT_BINARY_DIR}/agent-sleep-fixture")
add_test(NAME agent_lifecycle
    COMMAND lingtai_agent_lifecycle_test
        "${CMAKE_CURRENT_BINARY_DIR}/agent-lifecycle-fixture")
add_test(NAME agent_prompt_actions
    COMMAND lingtai_agent_prompt_actions_test
        "${CMAKE_CURRENT_BINARY_DIR}/agent-prompt-actions-fixture")
add_test(NAME posix_descriptor_primitives
    COMMAND lingtai_posix_descriptor_primitives_test
        "${CMAKE_CURRENT_BINARY_DIR}/posix-descriptor-primitives-fixture")
add_test(NAME workspace_selection
    COMMAND lingtai_workspace_selection_test
        "${CMAKE_CURRENT_BINARY_DIR}/workspace-selection-fixture")
add_executable(lingtai_desktop_smoke MACOSX_BUNDLE
    src/main.cpp
    src/crl_integration.cpp
    "${CMAKE_CURRENT_SOURCE_DIR}/resources/macos/AppIcon.icns")
set_source_files_properties(
    "${CMAKE_CURRENT_SOURCE_DIR}/resources/macos/AppIcon.icns"
    PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
init_target(lingtai_desktop_smoke)
set_target_properties(lingtai_desktop_smoke PROPERTIES
    OUTPUT_NAME LingTai
    MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/cmake/macos/Info.plist.in"
    MACOSX_BUNDLE_BUNDLE_NAME "LingTai"
    MACOSX_BUNDLE_GUI_IDENTIFIER "ai.lingtai.desktop"
    MACOSX_BUNDLE_ICON_FILE AppIcon.icns
    MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
    MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}.${PROJECT_VERSION_PATCH}"
    MACOSX_BUNDLE_INFO_STRING "LingTai Desktop")
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
    lingtai_ui_test_support
    lingtai_desktop_native_shell
    desktop-app::lib_ui)

file(MAKE_DIRECTORY
    "${CMAKE_CURRENT_BINARY_DIR}/native-shell-no-write-fixture")
set(NATIVE_SHELL_JOURNEYS
    semantics
    bootstrap
    setup
    roster
    conversation
    lifecycle
    layout
    theme
    composer
    paste
    outgoing
    kanban)
foreach(journey IN LISTS NATIVE_SHELL_JOURNEYS)
    file(MAKE_DIRECTORY
        "${CMAKE_CURRENT_BINARY_DIR}/native-shell-${journey}-fixture")
    add_test(NAME native_shell_${journey}
        COMMAND lingtai_native_shell_test
            "${CMAKE_CURRENT_BINARY_DIR}/native-shell-${journey}-fixture"
            --journey=${journey})
    if(APPLE)
        set_tests_properties(native_shell_${journey} PROPERTIES
            ENVIRONMENT "QT_QPA_PLATFORM=cocoa")
    else()
        set_tests_properties(native_shell_${journey} PROPERTIES
            ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    endif()
endforeach()

file(MAKE_DIRECTORY
    "${CMAKE_CURRENT_BINARY_DIR}/native-shell-new-window-bootstrap-fixture")
add_test(NAME native_shell_new_window_bootstrap
    COMMAND lingtai_native_shell_test
        "${CMAKE_CURRENT_BINARY_DIR}/native-shell-new-window-bootstrap-fixture"
        --new-window-bootstrap-only)
if(APPLE)
    set_tests_properties(native_shell_new_window_bootstrap PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=cocoa")
else()
    set_tests_properties(native_shell_new_window_bootstrap PROPERTIES
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

add_executable(lingtai_native_shell_presets_test
    tests/native_shell_presets_test.cpp
    src/crl_integration.cpp)
init_target(lingtai_native_shell_presets_test)
target_compile_options(lingtai_native_shell_presets_test PRIVATE
    -Wall -Wextra -Werror -pedantic
    $<$<COMPILE_LANG_AND_ID:CXX,AppleClang>:-Wno-unused-parameter>
    $<$<COMPILE_LANG_AND_ID:CXX,AppleClang>:-Wno-deprecated-this-capture>
    $<$<COMPILE_LANG_AND_ID:CXX,GNU>:-Wno-unused-parameter>)
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
    src/attachment_thumbnail.cpp
    src/ui/conversation_surface.cpp
    src/message_reactions.cpp)
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
    lingtai_desktop_conversation
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

add_executable(lingtai_conversation_surface_scroll_test
    tests/conversation_surface_scroll_test.cpp
    src/attachment_thumbnail.cpp
    src/ui/conversation_surface.cpp
    src/message_reactions.cpp)
init_target(lingtai_conversation_surface_scroll_test)
target_include_directories(lingtai_conversation_surface_scroll_test PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_options(lingtai_conversation_surface_scroll_test PRIVATE
    -Wall -Wextra -Werror -pedantic
    $<$<COMPILE_LANG_AND_ID:CXX,AppleClang>:-Wno-unused-parameter>
    $<$<COMPILE_LANG_AND_ID:CXX,AppleClang>:-Wno-deprecated-this-capture>
    $<$<COMPILE_LANG_AND_ID:CXX,GNU>:-Wno-unused-parameter>)
target_link_libraries(lingtai_conversation_surface_scroll_test PRIVATE
    lingtai_desktop_conversation
    desktop-app::lib_ui
    Qt6::Widgets Qt6::Gui Qt6::Core)
add_test(NAME conversation_surface_scroll
    COMMAND lingtai_conversation_surface_scroll_test)
if(APPLE)
    set_tests_properties(conversation_surface_scroll PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=cocoa")
else()
    set_tests_properties(conversation_surface_scroll PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
endif()

# CTest labels for UI testing workflow (see tests/UI_TESTING_PLAN.md).
set(LINGTAI_UNIT_TESTS
    project_attachment
    attachment_selection
    agent_projection
    direct_conversation_route
    direct_conversation_attachment_actions
    slash_command
    direct_conversation_history
    message_reactions
    conversation_unread
    conversation_session
    direct_mail_publisher
    agent_preset_summary
    preset_catalog
    kanban_model
    agent_sleep
    agent_lifecycle
    agent_prompt_actions
    posix_descriptor_primitives
    workspace_selection)
foreach(test_name IN LISTS LINGTAI_UNIT_TESTS)
    set_tests_properties(${test_name} PROPERTIES LABELS "unit")
endforeach()

set(LINGTAI_UI_TESTS
    native_shell
    native_shell_semantics
    native_shell_bootstrap
    native_shell_roster
    native_shell_conversation
    native_shell_lifecycle
    native_shell_layout
    native_shell_theme
    native_shell_composer
    native_shell_outgoing
    native_shell_kanban
    native_shell_destinations
    native_shell_presets
    conversation_surface_typography
    conversation_surface_scroll
    agent_detail_view
    ui_test_harness
    ui_agent_detail_pages)
foreach(test_name IN LISTS LINGTAI_UI_TESTS)
    set_tests_properties(${test_name} PROPERTIES LABELS "ui")
endforeach()

foreach(test_name IN LISTS LINGTAI_VISUAL_TESTS)
    set_tests_properties(${test_name} PROPERTIES LABELS "ui;visual")
endforeach()

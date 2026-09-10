cmake_minimum_required(VERSION 3.14)
project(LogosctlCLI LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Find Qt packages
find_package(QT NAMES Qt6 Qt5 REQUIRED COMPONENTS Core RemoteObjects)
find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Core RemoteObjects)

set(CMAKE_AUTOMOC ON)

# Set output directories
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# ── logos-liblogos dependency ─────────────────────────────────────────────────
# LOGOS_LIBLOGOS_ROOT must point to the installed logos-liblogos tree
# (containing lib/liblogos_core.{so,dylib} and include/logos_core.h)
if(NOT DEFINED LOGOS_LIBLOGOS_ROOT OR LOGOS_LIBLOGOS_ROOT STREQUAL "")
    message(FATAL_ERROR "LOGOS_LIBLOGOS_ROOT is not set. Pass -DLOGOS_LIBLOGOS_ROOT=<path>")
endif()
get_filename_component(LOGOS_LIBLOGOS_ROOT "${LOGOS_LIBLOGOS_ROOT}" ABSOLUTE)

message(STATUS "Using logos-liblogos at ${LOGOS_LIBLOGOS_ROOT}")

find_library(LOGOS_CORE_LIBRARY
    NAMES logos_core
    PATHS "${LOGOS_LIBLOGOS_ROOT}/lib"
    NO_DEFAULT_PATH
)

if(NOT LOGOS_CORE_LIBRARY)
    message(FATAL_ERROR "liblogos_core not found in ${LOGOS_LIBLOGOS_ROOT}/lib")
endif()

add_library(logos_core SHARED IMPORTED)
set_target_properties(logos_core PROPERTIES
    INTERFACE_INCLUDE_DIRECTORIES "${LOGOS_LIBLOGOS_ROOT}/include;${LOGOS_LIBLOGOS_ROOT}/include/cpp"
)

if(WIN32)
    # A PE shared library is TWO files: liblogos_core.dll (the runtime image)
    # and liblogos_core.dll.a (link-time import stubs). CMake wants them in
    # different properties — IMPORTED_LOCATION is the DLL, IMPORTED_IMPLIB is
    # the import library — and putting the import library in IMPORTED_LOCATION
    # alone is not a hard error. CMake only WARNS
    #     IMPORTED_IMPLIB not set for imported target "logos_core"
    # then substitutes `logos_core-NOTFOUND` into the link line, and the build
    # dies much later and far from the cause:
    #     ninja: error: 'logos_core-NOTFOUND', needed by 'bin/logosctl.exe'
    #
    # find_library's Windows-GNU suffix list is ".dll.a;.a;.lib" and never
    # matches a bare .dll, so LOGOS_CORE_LIBRARY above IS the import library
    # and the DLL has to be located separately with find_file.
    find_file(LOGOS_CORE_RUNTIME
        NAMES liblogos_core.dll logos_core.dll
        PATHS "${LOGOS_LIBLOGOS_ROOT}/lib" "${LOGOS_LIBLOGOS_ROOT}/bin"
        NO_DEFAULT_PATH
    )
    if(NOT LOGOS_CORE_RUNTIME)
        message(FATAL_ERROR
            "liblogos_core.dll not found under ${LOGOS_LIBLOGOS_ROOT} "
            "(the import library ${LOGOS_CORE_LIBRARY} is present, so the DLL "
            "is missing from the package rather than from the build)")
    endif()
    set_target_properties(logos_core PROPERTIES
        IMPORTED_LOCATION "${LOGOS_CORE_RUNTIME}"
        IMPORTED_IMPLIB   "${LOGOS_CORE_LIBRARY}"
    )
else()
    set_target_properties(logos_core PROPERTIES
        IMPORTED_LOCATION "${LOGOS_CORE_LIBRARY}"
    )
endif()

# ── SDK stack (imported targets) ──────────────────────────────────────────────
# logos-qt-host (from logos-plugin-qt) is the Qt HOST RUNTIME: LogosAPI,
# LogosAPIProvider and the LogosProviderObject base the in-process core
# service derives. It chains logos-protocol (transports, bridge helpers,
# logos::transportSetToJsonString). The Qt-free logos-cpp-sdk is header-only.
#
# This used to be logos-qt-sdk::logos_qt_sdk. The host runtime moved out of
# logos-qt-sdk into logos-plugin-qt; nothing else logos-qt-sdk ships (the Qt
# consumer emitter, the LpBridge headers, LogosUiPluginContext) is used here,
# so logos-qt-sdk is not a dependency of this repo at all any more.
if(NOT DEFINED LOGOS_PROTOCOL_ROOT AND DEFINED ENV{LOGOS_PROTOCOL_ROOT})
    set(LOGOS_PROTOCOL_ROOT "$ENV{LOGOS_PROTOCOL_ROOT}")
endif()
if(NOT DEFINED LOGOS_QT_HOST_ROOT AND DEFINED ENV{LOGOS_QT_HOST_ROOT})
    set(LOGOS_QT_HOST_ROOT "$ENV{LOGOS_QT_HOST_ROOT}")
endif()
# Fail loudly rather than letting find_package fall through to a system
# prefix (or, worse, silently skipping a target-guarded block later on).
if(NOT DEFINED LOGOS_QT_HOST_ROOT OR LOGOS_QT_HOST_ROOT STREQUAL "")
    message(FATAL_ERROR
        "LOGOS_QT_HOST_ROOT is not set. Pass -DLOGOS_QT_HOST_ROOT=<path to the "
        "logos-qt-host prefix from logos-plugin-qt>, or set it in the environment.")
endif()
find_package(logos-protocol REQUIRED
    PATHS "${LOGOS_PROTOCOL_ROOT}/lib/cmake/logos-protocol" NO_DEFAULT_PATH)
find_package(logos-qt-host REQUIRED
    PATHS "${LOGOS_QT_HOST_ROOT}/lib/cmake/logos-qt-host" NO_DEFAULT_PATH)
if(NOT TARGET logos-qt-host::logos_qt_host_shared)
    message(FATAL_ERROR
        "find_package(logos-qt-host) succeeded but did not define "
        "logos-qt-host::logos_qt_host_shared — refusing to build a logoscore "
        "without it. That target arrives with logos-plugin-qt#22; building "
        "against an older Qt host runtime would silently fall back to the "
        "static archive and give this image its own TokenManager.")
endif()

# ── nlohmann_json (header-only) ───────────────────────────────────────────────
find_package(nlohmann_json REQUIRED)

# ── OpenSSL (TokenStore::hashToken uses SHA-256 to digest auth tokens at
# rest, replacing the previous 64-bit FNV-1a — collision-resistant by
# design so two distinct tokens can never validate to the same name).
find_package(OpenSSL REQUIRED)

# ── stduuid (header-only) ────────────────────────────────────────────────────
find_package(stduuid REQUIRED)

# ── fmt (formatting library) ─────────────────────────────────────────────────
find_package(fmt QUIET)
if(NOT fmt_FOUND)
    message(STATUS "fmt not found via find_package, using FetchContent")
    include(FetchContent)
    FetchContent_Declare(fmt
        GIT_REPOSITORY https://github.com/fmtlib/fmt
        GIT_TAG 10.2.1
    )
    FetchContent_MakeAvailable(fmt)
else()
    message(STATUS "Using system fmt")
endif()

# ── spdlog ───────────────────────────────────────────────────────────────────
# Its rotating file sink already implements the size cap and keep-N retention
# the daemon log needs, and liblogos logs through spdlog anyway.
find_package(spdlog REQUIRED)

# ── yaml-cpp ─────────────────────────────────────────────────────────────────
# The user-editable config files are YAML; everything machine-owned
# (state.json, tokens.json, the auto token) stays JSON. See src/yaml_json.h.
find_package(yaml-cpp REQUIRED)

# ── CLI11 (header-only) ──────────────────────────────────────────────────────
find_package(CLI11 QUIET)
if(NOT CLI11_FOUND)
    message(STATUS "CLI11 not found via find_package, using FetchContent")
    include(FetchContent)
    FetchContent_Declare(CLI11
        GIT_REPOSITORY https://github.com/CLIUtils/CLI11
        GIT_TAG v2.4.2
    )
    FetchContent_MakeAvailable(CLI11)
else()
    message(STATUS "Using system CLI11")
endif()

# ── logosctl executable ──────────────────────────────────────────────────────
set(SHARED_SOURCES
    src/config.cpp
    src/paths.cpp
    src/yaml_json.cpp

    # Daemon path
    src/daemon/daemon.cpp
    src/daemon/daemon_state.cpp
    src/daemon/access_policy_arg.cpp
    src/daemon/port_allocator.cpp
    src/daemon/token_store.cpp
    src/daemon/log_sink.cpp
    src/daemon/package_bootstrap.cpp

    # Client path
    src/client/client.cpp
    src/client/client_state.cpp
    src/client/output.cpp
    src/client/commands/command.cpp
    src/client/commands/status_command.cpp
    src/client/commands/load_module_command.cpp
    src/client/commands/unload_module_command.cpp
    src/client/commands/reload_module_command.cpp
    src/client/commands/list_modules_command.cpp
    src/client/commands/module_info_command.cpp
    src/client/commands/call_command.cpp
    src/client/commands/watch_command.cpp
    src/client/commands/stats_command.cpp
    src/client/commands/stop_command.cpp
    src/client/commands/issue_token_command.cpp
    src/client/commands/revoke_token_command.cpp
    src/client/commands/list_tokens_command.cpp
    src/client/commands/package_command.cpp
    src/client/commands/catalog_command.cpp
    src/client/commands/config_command.cpp

    # The Web container's desktop backend: spawn a page host, speak the web
    # transport to it over a loopback socket. No browser in THIS binary -- see
    # src/web/webhost_view.h for why the page runs in a child process.
    src/web/webhost_view.cpp

    # Core service (in-process daemon module)
    src/core_service/core_service_impl.cpp
    src/core_service/call_envelope.cpp
    src/core_service/core_service_dispatch.cpp
    src/core_service/package_ops.cpp
)

# Two front-ends over one shared runtime. logoscore is the tool that exists
# today, kept building unchanged while logosctl is validated; logosctl is the
# merged CLI. They differ only in main.cpp and the Config flavor it sets --
# everything below src/daemon, src/client and src/core_service is shared.
add_executable(logosctl  src/main.cpp        ${SHARED_SOURCES})
add_executable(logoscore src/main_legacy.cpp ${SHARED_SOURCES})

foreach(_tgt logosctl logoscore)
target_link_libraries(${_tgt} PRIVATE
    logos_core
    nlohmann_json::nlohmann_json
    stduuid
    CLI11::CLI11
    OpenSSL::Crypto
    fmt::fmt
    yaml-cpp::yaml-cpp
    spdlog::spdlog
    Qt${QT_VERSION_MAJOR}::Core
    Qt${QT_VERSION_MAJOR}::RemoteObjects
)

# Link the Qt host runtime (provides LogosAPI, LogosAPIProvider,
# LogosProviderObject). It PUBLIC-links logos-protocol, which is where the
# bridge helpers and logos::transportSetToJsonString come from. Both
# front-ends need it: the shared core_service is what derives from
# LogosProviderObject.
target_link_libraries(${_tgt} PRIVATE logos-qt-host::logos_qt_host_shared)

# The SHARED Qt host runtime, not the static archive.
#
# This binary used to empty the static archives and take the runtime from
# liblogos_core, which had absorbed both of them (cmake/LogosSharedFromDll.cmake,
# now deleted). It links the libraries that OWN those types instead:
# logos_qt_host_shared owns LogosAPI and PUBLIC-links logos_protocol_shared,
# which owns TokenManager and LogosAPIClient.
#
# LOGOS_SHARED_USE_DLL is gone with the shim. It made this image declare the
# runtime types __declspec(dllimport) so ld would not pull the archive member
# that redefines them -- necessary while the definitions sat in an archive this
# image also linked. There is no second static copy left, so the import is what
# the link naturally produces.
#
# The invariant is unchanged and is asserted directly rather than through a
# compile flag: see nix/symbol-gate.nix.

if(WIN32)
    # PortAllocator's probe socket. Winsock is not in the default link set the
    # way libc's sockets are on POSIX, so without this the build fails at link
    # with undefined `socket`/`bind`/`getsockname`/`WSAStartup`.
    target_link_libraries(${_tgt} PRIVATE ws2_32)
endif()

target_include_directories(${_tgt} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${LOGOS_CPP_SDK_ROOT}/include/cpp
    ${LOGOS_QT_HOST_ROOT}/include
    ${LOGOS_QT_HOST_ROOT}/include/cpp
    ${LOGOS_PROTOCOL_ROOT}/include
    ${LOGOS_PROTOCOL_ROOT}/include/cpp
    ${LOGOS_LIBLOGOS_ROOT}/include
    ${LOGOS_LIBLOGOS_ROOT}/include/cpp
)

# RPATH so the binary finds liblogos_core at runtime.
# PE has no RPATH at all -- an import table carries DLL BASE NAMES and Windows
# resolves them from the executable's own directory -- so there is nothing to
# set here and the packaging stages the DLLs into bin/ instead.
if(WIN32)
    # nothing to do
elseif(APPLE)
    set(_rpath "@loader_path/../lib")
    if(DEFINED Qt${QT_VERSION_MAJOR}_DIR)
        get_filename_component(_qt_lib_dir "${Qt${QT_VERSION_MAJOR}_DIR}/../.." REALPATH)
        list(APPEND _rpath "${_qt_lib_dir}")
    endif()
    list(REMOVE_DUPLICATES _rpath)
    set_target_properties(${_tgt} PROPERTIES
        INSTALL_RPATH "${_rpath}"
        BUILD_WITH_INSTALL_RPATH TRUE)
else()
    set_target_properties(${_tgt} PROPERTIES
        INSTALL_RPATH "$ORIGIN/../lib"
        BUILD_WITH_INSTALL_RPATH TRUE)
endif()

endforeach()

# ── The webview host ──────────────────────────────────────────────────────────
#
# A SEPARATE BINARY in a SEPARATE PROJECT (src/webhost), and OFF by default. It
# is the only thing in this repo that links Qt WebEngine, and Chromium is not
# something a headless CLI should carry to run a module that is not a page:
# `logoscore` built without it behaves exactly as it does today, and a `web`
# module reports the missing bridge by name.
#
# When it IS built here it lands in bin/ beside the daemon, which is the first
# place the backend looks. The `webhost` nix output builds src/webhost on its
# own instead, and `LOGOSCORE_WEBHOST` points the daemon at it.
option(LOGOSCORE_WITH_WEBENGINE
       "Build logoscore-webhost, the Qt WebEngine page host for `--container web`"
       OFF)

if(LOGOSCORE_WITH_WEBENGINE)
    if(WIN32)
        message(FATAL_ERROR
            "LOGOSCORE_WITH_WEBENGINE is not supported on Windows: the backend "
            "that spawns this host is POSIX (see src/web/webhost_view.cpp).")
    endif()
    add_subdirectory(src/webhost)
endif()

# ── Tests ─────────────────────────────────────────────────────────────────────
#
# Not built for a Windows host. The suites are POSIX by construction, not by
# accident: test_integration.cpp drives the daemon over AF_UNIX (sys/un.h) and
# reaps it with fork/waitpid, and test_cli.cpp shells out through
# popen/WEXITSTATUS. There is nothing to gate at the case level -- the
# translation units do not compile.
#
# Skipping the whole block (rather than just the test targets) is deliberate
# for a second reason: `find_package(GTest)` falling through to the
# FetchContent branch would try to DOWNLOAD googletest, which in a Nix sandbox
# fails as an opaque "downloading ... failed" long after the real cause.
if(NOT WIN32)
    find_package(GTest QUIET)

    if(NOT GTest_FOUND)
        message(STATUS "GoogleTest not found via find_package, using FetchContent")
        include(FetchContent)
        FetchContent_Declare(googletest
            URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.zip
        )
        FetchContent_MakeAvailable(googletest)
    else()
        message(STATUS "Using system GoogleTest")
    endif()

    enable_testing()
    include(GoogleTest)

    add_subdirectory(tests)
endif()

# ── Install ───────────────────────────────────────────────────────────────────

install(TARGETS logosctl logoscore
    RUNTIME DESTINATION bin
)

cmake_minimum_required(VERSION 3.16)
project(usb-monitor-controller LANGUAGES CXX VERSION 0.1.0
        DESCRIPTION "C++ controller for the small USB LCD screens of turing-smart-screen-python")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

option(LCDCTL_BUILD_CLI "Build the lcd-cli command line tool" ON)
option(LCDCTL_BUILD_TESTS "Build the self-test executable" ON)
option(LCDCTL_ENABLE_LIBUSB "Build native-USB (TUR_USB) support when libusb is found" ON)

find_package(PkgConfig REQUIRED)
pkg_check_modules(PNG REQUIRED IMPORTED_TARGET libpng)
pkg_check_modules(JPEG REQUIRED IMPORTED_TARGET libjpeg)
pkg_check_modules(FREETYPE REQUIRED IMPORTED_TARGET freetype2)

# yaml-cpp is used by the theme engine (res/themes/*/theme.yaml), the daemon
# configuration and the Qt editor. It ships a CMake config on Homebrew.
find_package(yaml-cpp CONFIG QUIET)
if(NOT yaml-cpp_FOUND)
    find_package(yaml-cpp QUIET)
endif()
if(NOT yaml-cpp_FOUND)
    message(FATAL_ERROR
        "yaml-cpp is required for the theme engine. Install it with `brew install yaml-cpp`.")
endif()
message(STATUS "lcdctl: yaml-cpp ${yaml-cpp_VERSION}")
if(LCDCTL_ENABLE_LIBUSB)
    pkg_check_modules(LIBUSB IMPORTED_TARGET libusb-1.0)
endif()
pkg_check_modules(OPENSSL IMPORTED_TARGET openssl)

add_library(lcdctl STATIC
    src/types.cpp
    src/logging.cpp
    src/paths.cpp
    src/image.cpp
    src/serialize.cpp
    src/font.cpp
    src/draw.cpp
    src/theme.cpp
    src/theme_render.cpp
    src/sensors.cpp
    src/sensors_linux.cpp
    src/settings.cpp
    src/monitor.cpp
    src/transport.cpp
    src/lcd_comm.cpp
    src/console.cpp
    src/simulate.cpp
    src/simulate_rev_c.cpp
    src/drivers/rev_a.cpp
    src/drivers/rev_b.cpp
    src/drivers/rev_c.cpp
    src/drivers/rev_d.cpp
    src/drivers/weact.cpp
    src/drivers/turing_usb.cpp
    src/factory.cpp
)
target_include_directories(lcdctl PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(lcdctl PUBLIC PkgConfig::PNG PkgConfig::JPEG PkgConfig::FREETYPE yaml-cpp::yaml-cpp)
if(UNIX AND NOT APPLE)
    target_link_libraries(lcdctl PUBLIC ${CMAKE_DL_LIBS})
endif()

find_package(Threads REQUIRED)
target_link_libraries(lcdctl PUBLIC Threads::Threads)

if(LIBUSB_FOUND)
    target_link_libraries(lcdctl PUBLIC PkgConfig::LIBUSB)
    target_compile_definitions(lcdctl PUBLIC LCDCTL_WITH_LIBUSB=1)
    message(STATUS "lcdctl: libusb found (${LIBUSB_VERSION})")
else()
    message(STATUS "lcdctl: libusb NOT found (native USB disabled)")
endif()

if(OPENSSL_FOUND)
    target_link_libraries(lcdctl PUBLIC PkgConfig::OPENSSL)
    target_compile_definitions(lcdctl PUBLIC LCDCTL_WITH_OPENSSL=1)
    message(STATUS "lcdctl: OpenSSL found (${OPENSSL_VERSION})")
else()
    message(STATUS "lcdctl: OpenSSL NOT found (TUR_USB encryption unavailable)")
endif()

if(LIBUSB_FOUND AND OPENSSL_FOUND)
    message(STATUS "lcdctl: native USB (TUR_USB) support ENABLED")
else()
    message(STATUS "lcdctl: native USB (TUR_USB) support DISABLED")
endif()

# Absolute source dir, used as a fallback when looking for res/fonts in a dev build.
target_compile_definitions(lcdctl PRIVATE LCDCTL_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")

# Bundled resources (fonts + themes) copied next to the binaries, so the tools
# work from build/ without pointing at the source tree.
add_custom_target(lcdctl-resources ALL
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_CURRENT_SOURCE_DIR}/res/fonts" "${CMAKE_CURRENT_BINARY_DIR}/res/fonts"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_CURRENT_SOURCE_DIR}/res/themes" "${CMAKE_CURRENT_BINARY_DIR}/res/themes"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_CURRENT_SOURCE_DIR}/res/icons" "${CMAKE_CURRENT_BINARY_DIR}/res/icons"
    COMMENT "Copying res/fonts, res/themes and res/icons next to the binaries")

if(LCDCTL_BUILD_CLI)
    add_executable(lcd-cli src/main.cpp)
    target_link_libraries(lcd-cli PRIVATE lcdctl)
    add_dependencies(lcd-cli lcdctl-resources)
endif()

if(LCDCTL_BUILD_TESTS)
    add_executable(lcd-selftest tests/simulate_roundtrip.cpp)
    target_link_libraries(lcd-selftest PRIVATE lcdctl)
    enable_testing()
    add_test(NAME simulate_roundtrip COMMAND lcd-selftest)

    add_executable(lcd-theme-selftest tests/theme_selftest.cpp)
    target_link_libraries(lcd-theme-selftest PRIVATE lcdctl)
    add_dependencies(lcd-theme-selftest lcdctl-resources)
    add_test(NAME theme_selftest COMMAND lcd-theme-selftest)
endif()

# Theme tooling: inspect, validate and render themes without hardware.
add_executable(lcd-theme src/tools/theme_cli.cpp)
target_link_libraries(lcd-theme PRIVATE lcdctl)
add_dependencies(lcd-theme lcdctl-resources)

# Daemon (tray icon) and visual theme editor: Qt6 Widgets on KDE.
option(LCDCTL_BUILD_GUI "Build the Qt6 tray daemon and theme editor" ON)
if(LCDCTL_BUILD_GUI)
    # Prefer the distribution Qt (KDE runs against it, and Homebrew's qtbase on
    # Linux ships an incomplete CMake package: missing share/qt/libexec/syncqt).
    # Hide Homebrew from the search while looking for Qt, then restore it: the
    # other dependencies (yaml-cpp) still come from Homebrew.
    if(NOT DEFINED Qt6_DIR)
        foreach(candidate "/usr/lib64/cmake/Qt6" "/usr/lib/cmake/Qt6" "/usr/lib/x86_64-linux-gnu/cmake/Qt6")
            if(EXISTS "${candidate}")
                set(Qt6_DIR "${candidate}" CACHE PATH "Qt6 CMake package" FORCE)
                break()
            endif()
        endforeach()
    endif()
    set(_lcdctl_saved_prefix "${CMAKE_PREFIX_PATH}")
    set(_lcdctl_saved_env "$ENV{CMAKE_PREFIX_PATH}")
    if(DEFINED CMAKE_PREFIX_PATH)
        list(FILTER CMAKE_PREFIX_PATH EXCLUDE REGEX "linuxbrew|homebrew|Homebrew")
    endif()
    set(ENV{CMAKE_PREFIX_PATH} "")
    set(CMAKE_FIND_USE_PACKAGE_REGISTRY OFF)
    find_package(Qt6 COMPONENTS Widgets QUIET)
    set(CMAKE_FIND_USE_PACKAGE_REGISTRY ON)
    set(ENV{CMAKE_PREFIX_PATH} "${_lcdctl_saved_env}")
    set(CMAKE_PREFIX_PATH "${_lcdctl_saved_prefix}")
    if(Qt6_FOUND)
        message(STATUS "lcdctl: Qt ${Qt6Widgets_VERSION} found (tray daemon + theme editor)")
    else()
        message(STATUS "lcdctl: Qt6 NOT found (lcd-monitor builds headless only)")
    endif()
endif()

# Qt needs its include directories first: Homebrew dependencies (yaml-cpp) also
# ship a /include directory, and Qt headers are included as <QtCore/...>.
function(lcdctl_enable_qt target)
    target_link_libraries(${target} PRIVATE Qt6::Widgets)
    target_compile_definitions(${target} PRIVATE LCDCTL_WITH_QT=1)
    get_target_property(_qt_include_dirs Qt6::Widgets INTERFACE_INCLUDE_DIRECTORIES)
    if(_qt_include_dirs)
        target_include_directories(${target} BEFORE PRIVATE ${_qt_include_dirs})
    endif()
    # Homebrew's lib directory must not win over the system Qt libraries.
    get_target_property(_qt_core_file Qt6::Core IMPORTED_LOCATION)
    if(_qt_core_file)
        get_filename_component(_qt_lib_dir "${_qt_core_file}" DIRECTORY)
        target_link_directories(${target} BEFORE PRIVATE ${_qt_lib_dir})
        set_target_properties(${target} PROPERTIES BUILD_RPATH "${_qt_lib_dir}")
    endif()
    set_target_properties(${target} PROPERTIES
        AUTOMOC ON
        POSITION_INDEPENDENT_CODE ON)
    target_compile_options(${target} PRIVATE -fPIC -fno-pie)
    target_link_options(${target} PRIVATE -no-pie)
endfunction()

add_executable(lcd-monitor src/tools/monitor_main.cpp)
target_link_libraries(lcd-monitor PRIVATE lcdctl)
add_dependencies(lcd-monitor lcdctl-resources)
if(LCDCTL_BUILD_GUI AND Qt6_FOUND)
    lcdctl_enable_qt(lcd-monitor)
endif()

if(LCDCTL_BUILD_GUI AND Qt6_FOUND)
    add_executable(lcd-theme-editor src/tools/theme_editor.cpp)
    target_link_libraries(lcd-theme-editor PRIVATE lcdctl)
    add_dependencies(lcd-theme-editor lcdctl-resources)
    lcdctl_enable_qt(lcd-theme-editor)
endif()

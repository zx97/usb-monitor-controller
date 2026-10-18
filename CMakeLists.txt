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
option(LCDCTL_ENABLE_LIBUSB "Build native-USB (TUR_USB) support when libusb is found" ON)

find_package(PkgConfig REQUIRED)
pkg_check_modules(PNG REQUIRED IMPORTED_TARGET libpng)
pkg_check_modules(JPEG REQUIRED IMPORTED_TARGET libjpeg)
pkg_check_modules(FREETYPE REQUIRED IMPORTED_TARGET freetype2)
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
    src/transport.cpp
    src/lcd_comm.cpp
    src/drivers/rev_a.cpp
    src/drivers/rev_b.cpp
    src/drivers/rev_c.cpp
    src/drivers/rev_d.cpp
    src/drivers/weact.cpp
    src/drivers/turing_usb.cpp
    src/factory.cpp
)
target_include_directories(lcdctl PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(lcdctl PUBLIC PkgConfig::PNG PkgConfig::JPEG PkgConfig::FREETYPE)

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

if(LCDCTL_BUILD_CLI)
    add_executable(lcd-cli src/main.cpp)
    target_link_libraries(lcd-cli PRIVATE lcdctl)

    # Make the bundled font available next to the built binary for `lcd-cli text`.
    add_custom_command(TARGET lcd-cli POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory
                "$<TARGET_FILE_DIR:lcd-cli>/res/fonts"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${CMAKE_CURRENT_SOURCE_DIR}/res/fonts/RobotoMono-Regular.ttf"
                "$<TARGET_FILE_DIR:lcd-cli>/res/fonts/RobotoMono-Regular.ttf")
endif()
